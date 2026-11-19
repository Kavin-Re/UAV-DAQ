# UAV-DAQ telemetry protocol

| Version | Encoding | Integrity | Status |
|---|---|---|---|
| **v1** | ASCII line, 7 comma-separated fields | optional XOR checksum `*HH` | Implemented in firmware and ground station |
| v1-legacy | ASCII line, 3 fields | none | Old captures only; **not accepted by the firmware** |
| **v2** | Binary frame | CRC-16/CCITT-FALSE | Roadmap. Implemented and tested in the Python ground station only |

Direction of the link today: **PC -> STM32** only. The ground station replays
a recorded ArduPilot flight log; the firmware never transmits on the UART.

## Physical layer

| Parameter | Value |
|---|---|
| Interface | USART1, PA10 = RX (Arduino D2), PA9 = TX (D8, unused by the firmware) |
| Levels | 3.3 V CMOS through a CP2102 USB-UART bridge (HW-558A) |
| Framing | 9600 baud, 8 data bits, no parity, 1 stop bit, no flow control |
| Throughput | 960 bytes/s. A typical 32-character v1 line occupies the wire for about 33 ms |

## v1 - ASCII line

```
TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage[*HH]<LF>
```

Example: `114805,0,1,54,536.49,0.00,24.12` or, with checksum,
`114805,0,1,54,536.49,1.25,24.12*29`.

| # | Field | Type | Unit | Source in the ArduPilot log |
|---|---|---|---|---|
| 1 | `TimeMS` | unsigned decimal, 0 ... 4294967295 | ms since autopilot boot | `TimeUS / 1000` |
| 2 | `Hour` | unsigned decimal | h | derived from `TimeMS` |
| 3 | `Min` | unsigned decimal, 0 ... 59 | min | derived from `TimeMS` |
| 4 | `Sec` | unsigned decimal, 0 ... 59 | s | derived from `TimeMS` |
| 5 | `Altitude` | decimal, sign allowed | m | `AHR2.Alt` |
| 6 | `Speed` | decimal | m/s | `sqrt(XKF1.VN^2 + XKF1.VE^2)` |
| 7 | `Voltage` | decimal | V | `BAT.Volt` |

Rules (as implemented in `firmware/Core/Src/telemetry.c` and
`ground_station/uavdaq_gs/protocol.py`):

* A line ends with LF (`0x0A`). A CR before it and blanks around fields are tolerated.
* A line may be at most 127 characters. Longer lines are discarded up to the next LF.
* Exactly seven fields. Anything else - including a CSV header line - is dropped
  without any reply. Non-finite numbers (`nan`, `inf`) are rejected.
* `Hour`/`Min`/`Sec` are redundant with `TimeMS`. They are range-checked but not
  cross-checked; the OLED shows them, rates are computed from `TimeMS`.
* The sender formats the three measurements with two decimals. The ground
  station sends one line every 200 ms by default (5 lines/s).

### Optional checksum

`*HH` = two hexadecimal digits holding the XOR of every character before the
`*` (the scheme NMEA 0183 uses, without the leading `$`).

* Lines without a checksum are accepted (backwards compatible).
* Lines with a wrong or malformed checksum are dropped.
* The ground station can demand a checksum (`--require-checksum`); the firmware
  has no such switch yet.
* An 8-bit XOR catches every single-bit error and every odd number of bit
  errors in one bit position, but misses many multi-character errors (for
  example two characters with the same bit flipped). It is a low-cost
  improvement, not a replacement for a CRC - hence v2.

### Behaviour of the originally committed firmware

For anyone comparing with the first commit (`9857044`): that version parsed
with `sscanf("%lu,%lu,%lu,%lu,%f,%f,%f")`, so it also accepted trailing junk
after the seventh field (a `*HH` suffix was ignored, not verified), parsed a
truncated 127-character fragment of an over-long line, and had no range checks.

### SD card log

The firmware appends every accepted record to `telemetry.csv` in the root of a
FAT volume, using the same seven columns:

```
TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage      <- written once, when the file is empty
114805,0,1,54,536.49,0.00,24.12                 <- "%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f\n"
```

### v1-legacy (3 columns)

```
Altitude,Speed,Voltage<LF>        e.g.  536.49,0.0,24.122
```

This is the format of `data/telemetry_stream.csv` and of the first project
scripts and reports. It carries no timestamp. The firmware in this repository
rejects it (wrong field count); the ground station still reads it so old
captures can be replayed and converted.

## v2 - framed binary (proposal)

Goals: detect corruption reliably, detect lost frames, resynchronise after
garbage, and roughly halve the bytes per record (19 instead of ~32).

```
 0      1      2          3     4     5 ... 5+LEN-1   5+LEN  6+LEN
+------+------+----------+-----+-----+---------------+------+------+
| 0xA5 | 0x5A | VER|TYPE | SEQ | LEN |    payload    | CRC  | CRC  |
+------+------+----------+-----+-----+---------------+ low  | high +
  start of frame  \________ covered by the CRC _____/
```

| Field | Size | Meaning |
|---|---|---|
| SOF | 2 | `0xA5 0x5A` |
| VER\|TYPE | 1 | high nibble = protocol version (2), low nibble = message type |
| SEQ | 1 | increments by one per frame, wraps 255 -> 0; a gap means lost frames |
| LEN | 1 | payload length, 0 ... 64 |
| payload | LEN | depends on TYPE |
| CRC | 2 | CRC-16/CCITT-FALSE over VER\|TYPE, SEQ, LEN and payload; little-endian |

CRC parameters: polynomial `0x1021`, initial value `0xFFFF`, no reflection, no
final XOR. Check value: `CRC("123456789") = 0x29B1`.

All multi-byte integers are little-endian (the native order of the Cortex-M4).

### Message type 0x1 - TELEMETRY (LEN = 12)

| Offset | Type | Field | Scaling | Range |
|---|---|---|---|---|
| 0 | uint32 | `time_ms` | 1 ms | 49.7 days |
| 4 | int32 | `altitude_cm` | 0.01 m | about +/-21 000 km |
| 8 | uint16 | `speed_cm_s` | 0.01 m/s | 0 ... 655.35 m/s |
| 10 | uint16 | `voltage_mv` | 0.001 V | 0 ... 65.535 V |

Worked example - time 16 909 060 ms, altitude 1.00 m, speed 2.00 m/s, 3.000 V, SEQ 7:

```
A5 5A | 21 | 07 | 0C | 04 03 02 01 | 64 00 00 00 | C8 00 | B8 0B | 20 D1
```

CRC over `21 07 0C ... B8 0B` = `0xD120`, sent low byte first.

### Receiver algorithm

1. Search for `A5 5A`; count skipped bytes.
2. Read VER|TYPE, SEQ, LEN. If the version is not 2 or LEN > 64, skip one byte and go to 1.
3. Wait for LEN + 2 more bytes; verify the CRC. On mismatch skip **one** byte
   and go to 1 (a genuine frame may start inside the damaged one).
4. Deliver the payload; compare SEQ with the previous frame to count gaps.

There is no byte stuffing: `A5 5A` may occur inside a payload. A false start is
rejected by the CRC and costs at most one frame, which is acceptable at this
frame size.

### Status and open points

* `encode_v2` / `FrameDecoder` in `ground_station/uavdaq_gs/protocol.py`
  implement the above; the tests include an exhaustive single-bit-error check.
  Try it without hardware: `python -m uavdaq_gs monitor --replay ../data/telemetry.csv --wire v2 --corrupt 0.05 --quiet --every 50`.
* The firmware does not decode v2. A C decoder plus interrupt- or DMA-driven
  reception is the next step (see the roadmap in the README).
* Reserved for later: message types for status/ack (MCU -> PC), a 16-bit SEQ
  if rates rise, and a higher baud rate.

## Ground-station decoding statistics

`uavdaq-gs monitor` reports, for either version: samples decoded, lines/frames
rejected (malformed, checksum/CRC, over-long), header lines skipped, bytes
dropped while resynchronising and - v2 only - sequence gaps.
