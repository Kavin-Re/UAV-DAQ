# UAV-DAQ: STM32 Flight Data Acquisition System

**UAV-DAQ** is a standalone, high-speed embedded system designed for real-time sensor data acquisition, synchronous SD card logging, and telemetry streaming. This project utilizes an STM32 (ARM Cortex-M4) to bridge the gap between high-frequency flight physics and deterministic data analysis.

---

## Technical Overview: System Architecture
The system employs a multi-layered architecture to ensure deterministic data handling and zero-loss logging during high-vibration flight environments:

*   **Data Acquisition:** High-speed sampling of flight metrics via I2C/SPI interfaces.
*   **Local Storage:** FATFS-based logging to an onboard SD card via SDIO for post-flight analysis.
*   **Telemetry Link:** Real-time data streaming via high-baud rate UART for live ground monitoring.
*   **Ground Station:** Python-based parsing and visualization of telemetry streams.

---

## Performance and Validation
The system has been validated using live ArduPilot drone log data with the following benchmarks:

| Metric                | Measured Value |
|----------------------|----------------|
| Telemetry Update Rate | 5 Hz           |
| OLED Display Latency | ~50 ms         |
| CSV Parse Accuracy   | 100%           |
| Flash Memory Footprint| ~45 KB         |
| RAM Utilization      | ~3 KB          |

---

## Repository Structure
The project is organized into a modular hierarchy to separate hardware-specific drivers from ground-station logic:

*   /firmware: STM32 core logic, CMSIS-HAL implementation, and Makefile.
*   /firmware/drivers: Custom hardware abstraction for SD card (FAT32) integration.
*   /scripts: Python-based ground station utilities for real-time stream formatting.
*   /data: Sample telemetry logs (CSV) captured during hardware verification.
*   /docs: Technical reports, hardware pinout diagrams, and project schematics.

---

## Technical Specifications
*   **Microcontroller:** STM32F401RE (ARM Cortex-M4).
*   **Filesystem:** FAT32 (via FatFS middleware).
*   **Display:** 0.96" I2C OLED (SSD1306).
*   **Protocol:** Custom packet-based telemetry over UART (9600 baud).

---

## Installation and Deployment

### 1. Firmware Compilation
The firmware is built using the GNU Arm Embedded Toolchain.
```bash
cd firmware
make all
```

### 2. Ground Station Initialization
Ensure the STM32 is connected via a serial-to-USB bridge and mapped correctly.
```bash
# Execute the telemetry listener
python3 scripts/telemetry_streamer.py --port /dev/ttyUSB0
```

---
**Author: Kavin-Re**
*Electronics Engineering | Embedded Systems | UAV Avionics*

