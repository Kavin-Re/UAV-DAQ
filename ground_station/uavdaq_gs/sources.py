"""Readers that turn files on disk into :class:`~uavdaq_gs.protocol.Sample` s.

Supported inputs (auto-detected from the first line):

* ``mavlogdump.py --format csv`` output with the columns
  ``TimeUS, AHR2.Alt, XKF1.VN, XKF1.VE, BAT.Volt`` - e.g. ``data/telemetry.csv``.
  ArduPilot logs each message type on its own row ("staggered" CSV), so rows are
  merged by carrying the last known value of every field forward.
* v1 lines (7 columns, optional header, optional ``*HH`` checksum).
* v1-legacy lines (3 columns, no timestamp) - e.g. ``data/telemetry_stream.csv``.
"""

from __future__ import annotations

import csv
import math
from collections.abc import Iterable, Iterator
from pathlib import Path

from .protocol import HeaderLine, ParseError, Sample, parse_line

MAVLOG_COLUMNS = ("TimeUS", "AHR2.Alt", "XKF1.VN", "XKF1.VE", "BAT.Volt")


def _float_or_none(text: str | None) -> float | None:
    if text is None or not text.strip():
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def merge_mavlog_rows(rows: Iterable[dict[str, str]]) -> Iterator[Sample]:
    """Forward-fill staggered mavlogdump rows into complete samples.

    One sample is produced per input row once altitude, speed and voltage have
    each been seen at least once.  Rows with a missing or invalid ``TimeUS``
    are skipped.
    """
    altitude = speed = voltage = None
    for row in rows:
        time_us = _float_or_none(row.get("TimeUS"))
        if time_us is None or time_us < 0:
            continue
        alt = _float_or_none(row.get("AHR2.Alt"))
        if alt is not None:
            altitude = alt
        v_north = _float_or_none(row.get("XKF1.VN"))
        v_east = _float_or_none(row.get("XKF1.VE"))
        if v_north is not None and v_east is not None:
            speed = math.hypot(v_north, v_east)
        volt = _float_or_none(row.get("BAT.Volt"))
        if volt is not None:
            voltage = volt
        if altitude is None or speed is None or voltage is None:
            continue
        yield Sample(int(time_us // 1000), altitude, speed, voltage)


def detect_format(first_line: str) -> str:
    """Return ``"mavlog"``, ``"v1"`` or ``"legacy"`` for a file's first line."""
    if "TimeUS" in first_line:
        return "mavlog"
    body = first_line.strip().rpartition("*")[0] or first_line.strip()
    return "legacy" if body.count(",") == 2 else "v1"


def read_samples(path: str | Path, *, strict: bool = False) -> Iterator[Sample]:
    """Yield samples from *path*, whatever supported format it is in.

    Malformed lines are skipped unless *strict* is set.
    """
    path = Path(path)
    with path.open(newline="", encoding="ascii", errors="replace") as handle:
        first = handle.readline()
        if not first:
            return
        handle.seek(0)
        if detect_format(first) == "mavlog":
            reader = csv.DictReader(handle)
            missing = [c for c in MAVLOG_COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                raise ParseError(f"{path}: missing mavlogdump columns {missing}")
            yield from merge_mavlog_rows(reader)
            return
        for number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                yield parse_line(line)
            except HeaderLine:
                continue
            except ParseError as exc:
                if strict:
                    raise ParseError(f"{path}:{number}: {exc}") from None
