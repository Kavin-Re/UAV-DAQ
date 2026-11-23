# UAV-DAQ: STM32 telemetry display and logger (bench prototype)

> **Status: frozen, 2026-11-23.** First STM32 project (October 2025), kept as the record of where the work
> started. **Not done:** SD-card logging never worked, and the firmware cannot be built from this repository
> as it stands (see Known defects). This README describes only what the code in this repository
> supports.

An STM32F401RE (NUCLEO-F401RE) receives telemetry as text lines over USART1 at 9600 baud, parses them, shows
altitude, speed, battery voltage and their rates of change on a 0.96" SSD1306 OLED (I2C), and tries to append
each record to a CSV file on an SD card through FatFs over SPI. The data is a recorded ArduPilot flight log
(`data/telemetry.csv`), replayed from a PC by `scripts/telemetry_streamer.py`.

## What it is not

* **Not a sensor DAQ.** The firmware reads no sensors; every value comes from the replayed log.
* **Not a telemetry transmitter.** The link is PC to MCU only; the firmware never transmits.
* **Not high-speed or deterministic.** 9600 baud, one line every 0.2 s from the PC script (5 Hz), and a
  polling loop that waits 10 ms after each received byte (`main.c:213`).
* **Not SDIO, not zero-loss.** The SD card is driven over SPI1 (`firmware/drivers/user_diskio.c`), and
  logging failed in testing.

## Known defects

* SD logging: in `firmware/drivers/user_diskio.c` the single-sector read and write paths return `RES_ERROR`
  even when the transfer succeeds (the sector `count` is never cleared, lines about 316-324 and 359-367), so
  mounting the card fails.
* Reception: one byte is polled, then `HAL_Delay(10)` (`main.c:213`); at 9600 baud a byte arrives about every
  1 ms, so bytes are lost while the loop waits.
* Build: `firmware/Core/Src/main.c` stops at line 215; `SystemClock_Config`, the `MX_*_Init` functions and
  `Error_Handler` are declared but not defined, and the ST HAL, CMSIS, FatFs and linker script are not in the
  repository. Regenerate them from `firmware/STM32 Telemetry.ioc` with STM32CubeMX before building.
* Format mismatch: `scripts/telemetry_streamer.py` sends the 3-field lines of `data/telemetry_stream.csv`
  (altitude, speed, voltage), while the committed firmware parses 7 fields
  (`TimeMS,Hour,Min,Sec,Alt,Speed,Volt`, `main.c:165`), so the two in this repository do not work together
  without changes.
* `scripts/telemetry_streamer.py` has the port hard-coded (`COM3`) and reads `telemetry_stream.csv` from the
  current folder; edit `SERIAL_PORT` and run it from `data/`.

## Figures from the original report

Recorded in October 2025 on an earlier firmware revision; no logs, map files or captures were kept, so they
cannot be reproduced from this repository.

| Metric | Reported value | Note |
|---|---|---|
| Telemetry update rate | 5 Hz | The PC script's send rate, not an MCU measurement |
| OLED display latency | ~50 ms | Method not recorded |
| Flash / RAM | ~45 KB / ~3 KB | No build output in the repository |
| SD logging | Did not work | See Known defects |

## Repository Structure
The project is organized into a modular hierarchy to separate hardware-specific drivers from ground-station logic:

*   /firmware: STM32 core logic, CMSIS-HAL implementation, and Makefile.
*   /firmware/drivers: SD card over SPI for FatFs (see Known defects).
*   /scripts: Python-based ground station utilities for real-time stream formatting.
*   /data: A recorded ArduPilot flight log exported to CSV, used as the replayed input.
*   /docs: Technical reports, hardware pinout diagrams, and project schematics.

---

## Technical Specifications
*   **Microcontroller:** STM32F401RE (ARM Cortex-M4).
*   **Filesystem:** FAT32 (via FatFS middleware).
*   **Display:** 0.96" I2C OLED (SSD1306).
*   **Link:** one plain CSV line per record over UART at 9600 baud (no framing or checksum).

---

## Installation and Deployment

### 1. Firmware
The repository does not build as it stands (see Known defects). Generate the project from
`firmware/STM32 Telemetry.ioc` in STM32CubeIDE or CubeMX, add the sources from `firmware/Core` and
`firmware/drivers`, then build and flash through the on-board ST-LINK.

### 2. Ground Station Initialization
Ensure the STM32 is connected via a serial-to-USB bridge and mapped correctly.
```bash
# Sends the replayed log to the board; set SERIAL_PORT in the script first (default COM3)
cd data && python3 ../scripts/telemetry_streamer.py
```

---
**Author: Kavin-Re**
*Embedded systems*

