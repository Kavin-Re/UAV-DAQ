# Firmware audit

Code review of the firmware as first committed (`9857044`), the fixes applied
afterwards, and what is still open. Line numbers refer to the files **at that
first commit** (`git show 9857044:<path>`).

> **Verification status.** No hardware and no ARM toolchain were available for
> this review. Each fix was checked by reading the code and by host-side unit
> tests (`firmware/tests`: fake HAL, fake FatFs, behavioural SD card model,
> AddressSanitizer + UBSan). Every firmware change below therefore **needs
> hardware re-test** before any claim about on-target behaviour is made.

## Summary

| ID | Area | Severity | Finding | Status |
|---|---|---|---|---|
| A1 | Build | blocker | `main.c` ends after `main()`; clock/peripheral init and `Error_Handler` undefined | Reconstructed from `.ioc` |
| A2 | Build | blocker | HAL, CMSIS, FatFs, `fatfs.c`, `ffconf.h`, linker script, IDE project missing; Makefile points at non-existent paths | Documented; Makefile rewritten with a guard |
| B1 | UART RX | high | `HAL_Delay(10)` between single-byte polls at 9600 baud overruns the receiver | Fixed |
| B2 | UART RX | medium | Over-long line: first 127 characters parsed as if complete | Fixed |
| B3 | UART RX | medium | Overrun never detected; damaged lines can parse as valid data | Fixed (line dropped) |
| C1 | Parser | medium | `sscanf("%f")` needs `-u _scanf_float` with newlib-nano, accepts trailing junk, no validation, no integrity check | Replaced by strict `strtoul`/`strtof` parser with optional XOR checksum |
| C2 | Display | low | First record and backwards timestamps give meaningless rates | Fixed |
| C3 | Memory safety | high | `sprintf` into 32- and 100-byte stack buffers with unbounded `%f` | Fixed (`snprintf`) |
| D1 | SD driver | blocker | Single-sector read and write always return `RES_ERROR` | Fixed |
| D2 | SD driver | high | Multi-sector write sends the token but not the data | Fixed |
| D3 | SD driver | high | Multi-sector read releases CS after the first block | Fixed |
| D4 | SD driver | high | Ready/busy polled while the card is de-selected | Fixed |
| D5 | SD driver | medium | Timeouts roughly 10x the documented values (5 s, 10 s) | Fixed |
| D6 | SD driver | medium | `HAL_SPI_Receive` clocks stale buffer contents out on MOSI | Fixed |
| D7 | SD driver | low | Failed re-initialisation keeps the old "ready" state; CMD12 stuff byte not skipped | Fixed |
| D8 | SD driver | low | SDv1 / MMC initialisation incomplete, no CMD16 | Open (commented) |
| E1 | Logging | medium | Mount retried on every record; all FatFs return codes ignored; OLED shows `LOGGING: OK` after write failures | Fixed |
| F1 | Docs | - | README claims that the code does not support | README rewritten |

## Details

### A1 - `main.c` was truncated
`main.c:67-71` declares `SystemClock_Config`, `MX_GPIO_Init`, `MX_I2C1_Init`,
`MX_USART1_UART_Init`, `MX_SPI1_Init`; `main.h:53` declares `Error_Handler`.
None is defined anywhere (the file stops at line 215). The definitions were
re-created from the `.ioc` in CubeMX style and the application calls were moved
into `USER CODE` blocks so that regenerating from the `.ioc` keeps them. The
`.ioc` stores no I2C speed (CubeMX default 100 kHz assumed, matching the legacy
docs) and selects a 16 MHz HSI system clock, not the 84 MHz the legacy docs
describe. TODO(verify on hardware): regenerate with CubeMX and diff.

### A2 - the repository is not self-contained
The original `Makefile` lists three HAL sources under `Drivers/` and FatFs
under `Middlewares/FatFs/src` - neither directory exists - and omits the
start-up file, `stm32f4xx_hal_msp.c`, `sysmem.c`, most HAL modules, FPU flags
and float-printf support; it names a linker script that is not in the
repository. See `firmware/README.md` for the honest build procedure.

### B1 - receiver overrun by construction
`main.c:210-214` runs `Telemetry_ReceiveAndParse(); HAL_Delay(10);` and
`main.c:154` fetches **one** byte per call. USART1 has a single-byte data
register and no FIFO. At 9600 baud a byte arrives every 1.04 ms, so during each
10-11 ms delay about ten bytes arrive and all but the first are lost. With the
seven-field parser such fragments fail to parse; with the earlier `atof` parser
described in the legacy docs they would have produced plausible-looking
numbers. The delay was removed: `HAL_UART_Receive` already blocks for up to
100 ms when idle. This is an analysis result - it contradicts the "100 % parse
accuracy" in the legacy report, which presumably came from a different build.

### B2 / B3 - damaged lines
`main.c:156` terminated a line when the buffer filled and parsed the fragment.
Over-long lines are now skipped up to the next LF. The overrun flag is sampled
before each read; if set, the rest of the current line is discarded and counted
(`TelemetryStats_t.lines_rejected`). Bytes are still lost while the OLED and SD
card are being updated - see "Open items".

### C1 - parser
`main.c:165` used `sscanf("%lu,%lu,%lu,%lu,%f,%f,%f")`. Under newlib-nano (the
CubeIDE default) `%f` conversions in `scanf` silently fail unless
`-u _scanf_float` is linked; the legacy docs mention only `-u _printf_float`.
The new `Telemetry_ParseLine()` uses `strtoul`/`strtof`, requires exactly seven
fields, rejects negative/oversized integers, minutes/seconds above 59,
non-finite floats and trailing junk, and verifies an optional `*HH` checksum
(docs/PROTOCOL.md). Wire compatibility: every line the ground station
generates is accepted by both the old and the new parser.

### C2 - rates
`main.c:170-175`: for the first record the "previous" sample is all zeros, so
the display showed altitude / time-since-boot as a climb rate; a timestamp going
backwards wrapped the unsigned subtraction. Rates are now zero until two
records with increasing timestamps exist.

### C3 - stack buffer overflows
`main.c:105` formats four `%lu` and three `%.2f` into `logBuffer[100]`;
`main.c:130-140` format two floats into `lineBuffer[32]`. `%.2f` of a large
float needs up to 42 characters and the parser accepted any float, so one
hostile or corrupted line (`1,0,0,1,3e38,3e38,3e38`) overflowed both. All
formatting uses `snprintf`; an over-long log record is skipped and counted, an
over-long display row is truncated. Covered by
`test_extreme_values_do_not_overflow_buffers` under ASan.

### D1 - why `f_mount` returned `FR_DISK_ERR`
`user_diskio.c:316-324` (read) and `:359-367` (write): the single-sector branch
never clears `count`, and the function ends with
`return count ? RES_ERROR : RES_OK;` - so every single-sector transfer reports
failure even when the card answered correctly. `f_mount` starts by reading
sector 0. This matches the symptom in the legacy reports ("f_mount() returned
1 (FR_DISK_ERR)") and is the most likely reason SD logging never worked.
The return value of `sd_rcvr_datablock` / `sd_xmit_datablock` was also ignored.

### D2 - D7 - remaining SD driver defects
* `:181` tests `token != WRITE_MULTIPLE_BLOCK` where the reference
  implementation tests for the stop token `0xFD`: multi-block writes sent `0xFC`
  with no data.
* `:161` raises CS inside `sd_rcvr_datablock`, ending a CMD18 read after one block.
* `:109` waits for "ready" before `:111` pulls CS low. A de-selected card does
  not drive MISO, so the wait saw the pull-up and returned at once; the busy
  phase after a write was never honoured. Commands now start with
  de-select / select / wait-ready, and `CTRL_SYNC` selects the card first.
* `:88` (`tmr = 5000`, comment "500 ms") and `:146` (`tmr = 10000`, comment
  "1000 ms") loop with `HAL_Delay(1)`, i.e. 5 s and 10 s. Now tick-based:
  500 ms ready timeout, 200 ms data-token timeout.
* `:156` `HAL_SPI_Receive` in 2-line master mode transmits the receive buffer;
  the buffer is filled with `0xFF` first.
* `:258` only ever clears `STA_NOINIT`; `:312` did not skip the stuff byte after CMD12.

With the SD card model, the original driver fails 27 of 51 checks and the fixed
driver passes all of them (`make -C firmware/tests`).

### E1 - logging state
`main.c:100-103` called `Mount_SD_Card()` for every record while unmounted;
a failing mount blocks for seconds and starves the UART. Retries are now spaced
`TELEMETRY_SD_RETRY_MS` (5 s) apart. `main.c:86` set `is_mounted` even when the
log file could not be opened, and `main.c:108-114` ignored the results of
`f_open`/`f_write`/`f_sync`/`f_close`, so the OLED kept showing `LOGGING: OK`.
A failed or short write now clears `is_mounted` (shown as `LOGGING: FAIL` from
the next record on) and schedules a re-mount.

## Open items (not changed)

| Item | Why it matters | Suggested direction |
|---|---|---|
| Polled UART, blocking OLED and SD in one loop | Any line that arrives during the roughly 0.1 s+ of display/SD work is lost; the maximum reliable line rate is unknown | Interrupt or DMA reception into a ring buffer; log from a buffer |
| OLED refresh time | 8 pages x (128 data + overhead) is about 1.1 kB per frame, i.e. roughly 100 ms at 100 kHz by calculation - longer than the "~50 ms" in the legacy report | Measure with a scope / GPIO toggle; 400 kHz I2C; partial updates |
| `HAL_MAX_DELAY` in `ssd1306.c:14,19` and in the SD data transfers | A missing OLED or stuck bus hangs the firmware forever | Finite timeouts, error path, watchdog |
| Open-append-sync-close per record | Safe against power loss but slow and wear-intensive | Keep the file open, `f_sync` periodically |
| D8: SDv1 / MMC cards | Initialisation is incomplete; only SDv2/SDHC is expected to work | Port the full elm-chan init sequence |
| Fast SPI prescaler fixed at /2 | Correct at 16 MHz; would be 42 MHz at an 84 MHz system clock | Derive the prescaler from PCLK2 |
| No CRC on SD data, no CMD59 | Bit errors on breadboard wiring go unnoticed | Enable CRC in SPI mode |
| `Error_Handler()` is a silent infinite loop | No indication of a failed init | Blink LD2 / show on OLED |
| `HSE_VALUE` is 25 MHz in `stm32f4xx_hal_conf.h:99` and in the `.ioc` | Wrong for the Nucleo (8 MHz from the ST-LINK); harmless while the HSI is used | Correct before enabling the HSE/PLL |
| All SSD1306 fonts and `ssd1306_tests.c` are compiled in | Wasted flash | Trim `ssd1306_conf.h`, exclude the test file |
| Firmware never transmits | No acknowledgement, no status, nothing for a ground station to monitor | Status/echo frames (protocol v2) |
