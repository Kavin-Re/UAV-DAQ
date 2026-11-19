"""Telemetry wire formats for UAV-DAQ.

Three encodings are implemented. docs/PROTOCOL.md is the normative description.

* **v1** - the ASCII line the committed firmware parses::

      TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\\n

* **v1-legacy** - the 3-column line used by the original project scripts and by
  ``data/telemetry_stream.csv`` (``Altitude,Speed,Voltage\\n``).  The committed
  firmware does *not* accept it; it is supported here for old captures only.
* **v2** - a framed binary format with CRC-16/CCITT-FALSE.  Roadmap: implemented
  on the Python side only, the firmware does not speak it yet.

v1 lines may carry an optional NMEA-style checksum suffix (``*HH``).  That
suffix is a ground-station extension: the firmware's ``sscanf`` ignores it, it
does not verify it.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------

#: Longest line the firmware buffers (RX_BUFFER_SIZE - 1 in main.c).
MAX_LINE_LEN = 127

V1_HEADER = "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage"


@dataclass(frozen=True)
class Sample:
    """One telemetry record.

    ``time_ms`` is the log time (milliseconds since autopilot boot).  It is
    ``None`` for v1-legacy lines, which carry no timestamp.
    Speed is ground speed in m/s (sqrt(VN^2 + VE^2) from the EKF).
    """

    time_ms: int | None
    altitude_m: float
    speed_mps: float
    voltage_v: float

    @property
    def hms(self) -> tuple[int, int, int]:
        """Hours, minutes, seconds derived from ``time_ms`` (0,0,0 if unknown)."""
        total = (self.time_ms or 0) // 1000
        return total // 3600, (total % 3600) // 60, total % 60


class ParseError(ValueError):
    """A received line or frame could not be decoded."""


class ChecksumError(ParseError):
    """Checksum or CRC did not match."""


class HeaderLine(ParseError):
    """The line is a CSV header, not data (harmless, counted separately)."""


# --------------------------------------------------------------------------
# v1 ASCII
# --------------------------------------------------------------------------


def xor_checksum(text: str) -> int:
    """NMEA-style checksum: XOR of every character code in *text*."""
    value = 0
    for byte in text.encode("ascii"):
        value ^= byte
    return value


def encode_v1(sample: Sample, *, checksum: bool = False) -> str:
    """Format *sample* as a v1 line (without the trailing newline)."""
    if sample.time_ms is None:
        raise ValueError("v1 lines need a timestamp")
    hours, minutes, seconds = sample.hms
    body = (
        f"{sample.time_ms},{hours},{minutes},{seconds},"
        f"{sample.altitude_m:.2f},{sample.speed_mps:.2f},{sample.voltage_v:.2f}"
    )
    if checksum:
        body += f"*{xor_checksum(body):02X}"
    return body


def encode_legacy(sample: Sample) -> str:
    """Format *sample* as a v1-legacy 3-column line (no trailing newline)."""
    return f"{sample.altitude_m:.2f},{sample.speed_mps:.1f},{sample.voltage_v:.3f}"


def _to_float(text: str, name: str) -> float:
    try:
        value = float(text)
    except ValueError:
        raise ParseError(f"{name}: not a number: {text!r}") from None
    if not math.isfinite(value):
        raise ParseError(f"{name}: non-finite value {text!r}")
    return value


def _to_uint(text: str, name: str, limit: int = 0xFFFFFFFF) -> int:
    stripped = text.strip()
    if not stripped.isascii() or not stripped.isdigit():
        raise ParseError(f"{name}: not an unsigned integer: {text!r}")
    value = int(stripped)
    if value > limit:
        raise ParseError(f"{name}: {value} out of range")
    return value


def parse_line(line: str | bytes, *, require_checksum: bool = False) -> Sample:
    """Decode one v1 or v1-legacy line.

    Raises :class:`HeaderLine` for a CSV header, :class:`ChecksumError` for a
    bad or (with *require_checksum*) missing ``*HH`` suffix and
    :class:`ParseError` for anything else that is malformed.
    """
    if isinstance(line, (bytes, bytearray)):
        try:
            line = bytes(line).decode("ascii")
        except UnicodeDecodeError:
            raise ParseError("non-ASCII bytes in line") from None
    text = line.strip("\r\n \t")
    if not text:
        raise ParseError("empty line")
    if len(text) > MAX_LINE_LEN:
        raise ParseError(f"line longer than {MAX_LINE_LEN} characters")

    body, star, suffix = text.rpartition("*")
    if star:
        if len(suffix) != 2:
            raise ChecksumError(f"malformed checksum suffix {suffix!r}")
        try:
            expected = int(suffix, 16)
        except ValueError:
            raise ChecksumError(f"malformed checksum suffix {suffix!r}") from None
        if not body.isascii():
            raise ParseError("non-ASCII characters in line")
        actual = xor_checksum(body)
        if actual != expected:
            raise ChecksumError(f"checksum mismatch: got {suffix}, computed {actual:02X}")
        text = body
    elif require_checksum:
        raise ChecksumError("checksum required but missing")

    fields = [part.strip() for part in text.split(",")]
    if len(fields) in (3, 7) and all(part[:1].isalpha() for part in fields):
        raise HeaderLine(text)

    if len(fields) == 7:
        time_ms = _to_uint(fields[0], "TimeMS")
        _to_uint(fields[1], "Hour")
        minutes = _to_uint(fields[2], "Min")
        seconds = _to_uint(fields[3], "Sec")
        if minutes > 59 or seconds > 59:
            raise ParseError(f"Min/Sec out of range: {minutes}:{seconds}")
        values = fields[4:]
    elif len(fields) == 3:
        time_ms = None
        values = fields
    else:
        raise ParseError(f"expected 7 (v1) or 3 (legacy) fields, got {len(fields)}")

    return Sample(
        time_ms,
        _to_float(values[0], "Altitude"),
        _to_float(values[1], "Speed"),
        _to_float(values[2], "Voltage"),
    )


@dataclass
class DecodeStats:
    """Counters shared by the stream decoders."""

    ok: int = 0
    headers: int = 0
    malformed: int = 0
    checksum_errors: int = 0
    overflows: int = 0
    dropped_bytes: int = 0
    sequence_gaps: int = 0

    @property
    def rejected(self) -> int:
        return self.malformed + self.checksum_errors + self.overflows


@dataclass
class LineDecoder:
    """Incremental v1 decoder: feed raw bytes, get complete :class:`Sample` s.

    Bad lines never raise; they are counted in :attr:`stats` and reported
    through :attr:`errors` (only the first ``max_errors`` are kept).
    """

    require_checksum: bool = False
    stats: DecodeStats = field(default_factory=DecodeStats)
    errors: list[str] = field(default_factory=list)
    max_errors: int = 100
    _buffer: bytearray = field(default_factory=bytearray, repr=False)
    _discarding: bool = field(default=False, repr=False)

    def feed(self, data: bytes) -> list[Sample]:
        samples: list[Sample] = []
        for byte in data:
            if byte == 0x0A:  # '\n' terminates a line
                if self._discarding:
                    self._discarding = False
                else:
                    self._finish_line(samples)
                self._buffer.clear()
            elif self._discarding:
                self.stats.dropped_bytes += 1
            elif len(self._buffer) >= MAX_LINE_LEN:
                # Over-long line: drop it entirely and resynchronise on '\n'.
                self.stats.overflows += 1
                self.stats.dropped_bytes += len(self._buffer) + 1
                self._note(f"line longer than {MAX_LINE_LEN} bytes discarded")
                self._buffer.clear()
                self._discarding = True
            else:
                self._buffer.append(byte)
        return samples

    def _finish_line(self, out: list[Sample]) -> None:
        raw = bytes(self._buffer)
        if not raw.strip():
            return  # blank line / bare CR: ignore silently
        try:
            out.append(parse_line(raw, require_checksum=self.require_checksum))
            self.stats.ok += 1
        except HeaderLine:
            self.stats.headers += 1
        except ChecksumError as exc:
            self.stats.checksum_errors += 1
            self._note(f"{exc}: {raw!r}")
        except ParseError as exc:
            self.stats.malformed += 1
            self._note(f"{exc}: {raw!r}")

    def _note(self, message: str) -> None:
        if len(self.errors) < self.max_errors:
            self.errors.append(message)


# --------------------------------------------------------------------------
# v2 binary (roadmap; Python side only)
# --------------------------------------------------------------------------

V2_SOF = b"\xa5\x5a"
V2_VERSION = 2
V2_MSG_TELEMETRY = 0x1
V2_MAX_PAYLOAD = 64
_V2_TELEMETRY = struct.Struct("<IiHH")  # time_ms, alt_cm, speed_cm_s, voltage_mV
_V2_OVERHEAD = len(V2_SOF) + 3 + 2


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _clamp(value: float, low: int, high: int, name: str) -> int:
    scaled = round(value)
    if not low <= scaled <= high:
        raise ValueError(f"{name} out of range for v2 frame: {value}")
    return scaled


def encode_v2(sample: Sample, seq: int = 0) -> bytes:
    """Build one v2 telemetry frame."""
    if sample.time_ms is None:
        raise ValueError("v2 frames need a timestamp")
    payload = _V2_TELEMETRY.pack(
        sample.time_ms & 0xFFFFFFFF,
        _clamp(sample.altitude_m * 100, -(2**31), 2**31 - 1, "altitude"),
        _clamp(sample.speed_mps * 100, 0, 0xFFFF, "speed"),
        _clamp(sample.voltage_v * 1000, 0, 0xFFFF, "voltage"),
    )
    header = bytes(((V2_VERSION << 4) | V2_MSG_TELEMETRY, seq & 0xFF, len(payload)))
    crc = crc16_ccitt(header + payload)
    return V2_SOF + header + payload + struct.pack("<H", crc)


@dataclass
class FrameDecoder:
    """Incremental v2 decoder with resynchronisation on the SOF marker."""

    stats: DecodeStats = field(default_factory=DecodeStats)
    errors: list[str] = field(default_factory=list)
    max_errors: int = 100
    _buffer: bytearray = field(default_factory=bytearray, repr=False)
    _last_seq: int | None = field(default=None, repr=False)

    def feed(self, data: bytes) -> list[Sample]:
        self._buffer.extend(data)
        samples: list[Sample] = []
        buf = self._buffer
        while True:
            start = buf.find(V2_SOF)
            if start < 0:
                # Keep a trailing 0xA5 in case the SOF is split across reads.
                keep = 1 if buf.endswith(V2_SOF[:1]) else 0
                self.stats.dropped_bytes += len(buf) - keep
                del buf[: len(buf) - keep]
                break
            if start:
                self.stats.dropped_bytes += start
                del buf[:start]
            if len(buf) < 5:
                break
            ver_type, seq, length = buf[2], buf[3], buf[4]
            if ver_type >> 4 != V2_VERSION or length > V2_MAX_PAYLOAD:
                self._reject("bad version or length", malformed=True)
                continue
            total = _V2_OVERHEAD + length
            if len(buf) < total:
                break
            (crc,) = struct.unpack_from("<H", buf, total - 2)
            if crc != crc16_ccitt(bytes(buf[2 : total - 2])):
                self._reject("CRC mismatch", malformed=False)
                continue
            payload = bytes(buf[5 : 5 + length])
            del buf[:total]
            if ver_type & 0x0F != V2_MSG_TELEMETRY or length != _V2_TELEMETRY.size:
                self.stats.malformed += 1
                self._note(f"unsupported message type {ver_type & 0x0F} / length {length}")
                continue
            if self._last_seq is not None and seq != (self._last_seq + 1) & 0xFF:
                self.stats.sequence_gaps += 1
            self._last_seq = seq
            time_ms, alt_cm, speed_cm_s, voltage_mv = _V2_TELEMETRY.unpack(payload)
            samples.append(Sample(time_ms, alt_cm / 100, speed_cm_s / 100, voltage_mv / 1000))
            self.stats.ok += 1
        return samples

    def _reject(self, reason: str, *, malformed: bool) -> None:
        if malformed:
            self.stats.malformed += 1
        else:
            self.stats.checksum_errors += 1
        self._note(reason)
        # Skip only the first SOF byte so a real frame hiding inside is found.
        self.stats.dropped_bytes += 1
        del self._buffer[:1]

    def _note(self, message: str) -> None:
        if len(self.errors) < self.max_errors:
            self.errors.append(message)
