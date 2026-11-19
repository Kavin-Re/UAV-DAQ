# Legacy project documents

These files are the original write-ups from the October 2025 university
mini-project. They are kept unchanged (apart from removing two stray preamble
lines) as a record of how the project started. **They are not the current
documentation** - use the top-level [README](../../README.md),
[HARDWARE.md](../HARDWARE.md), [PROTOCOL.md](../PROTOCOL.md) and
[FIRMWARE_AUDIT.md](../FIRMWARE_AUDIT.md) instead.

| File | What it is |
|---|---|
| `University_Mini_Project_Submission.md` | Report submitted for the mini-project |
| `Drone_Telemetry_Project_Report.md` | Shorter technical report |
| `Source_Code_Package_README.md` | README of the original source archive |
| `Hardware_Schematic.txt` | ASCII block diagram, signal flow, power budget |
| `Pinout_Diagram.txt` | ASCII pinout and peripheral summary |

## Known differences from the code in this repository

Read the legacy documents with these corrections in mind:

| Legacy documents say | What the repository shows |
|---|---|
| Wire format `Altitude,Speed,Voltage` parsed with `strtok`/`atof` | The committed firmware expects 7 fields (`TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage`) and ignores 3-field lines |
| Speed in km/h | `data/telemetry_stream.csv` holds `sqrt(VN^2+VE^2)` in m/s |
| System clock 84 MHz from an 8 MHz HSE via PLL | The `.ioc` selects the 16 MHz HSI with the PLL unused. The quoted SPI rates (62.5 kHz = /256, 8 MHz = /2) only add up with 16 MHz |
| "I2C timing 0x20404768" | The STM32F4 I2C peripheral has no timing register; the value is ignored by the F4 code generator |
| SD logging "debug phase", `f_mount` returns `FR_DISK_ERR` | Root cause found in `user_diskio.c` (single-sector reads always returned an error) - see FIRMWARE_AUDIT.md. Still untested on hardware |
| Status "fully functional / production-ready / ready for deployment" | Bench prototype on a breadboard; SD logging never worked in the documented tests |
| `main.c` is ~600 lines | The committed file was 215 lines and was missing the CubeMX init functions |
| Arduino header map lists D0=PD10, D1=PD11, D7=PC1, D9=PC0, D10=PA5, D11=PA4, D12=PA3, D13=PA2 | Those rows are wrong for a NUCLEO-F401RE (the LQFP64 part has no PD10/PD11). Only the pins this project uses were checked - see HARDWARE.md |
| "PA5/PA6/PA7 are not on the headers" | They are (Arduino D13/D12/D11); PB3/PB4/PB5 is simply the alternative SPI1 mapping that was chosen |
| "LQFP64 (100-pin ...)" | LQFP64 has 64 pins |
| Power budget, thermal, EMC and "safety" sections | Datasheet-style estimates; nothing in the repository records a measurement |
| Flash ~45 KB, RAM ~3 KB, OLED ~50 ms, 100 % parse accuracy | Reported values; no map file, size output, scope capture or test log is in the repository |
