"""Command-line ground station for UAV-DAQ.

Sub-commands
------------
``stream``   send a flight log to the STM32 over a serial port (PC -> MCU).
``monitor``  decode a telemetry stream (serial port, or ``--replay`` of a file
             with no hardware attached), validate it, log it to CSV, plot it.
``convert``  turn a ``mavlogdump.py`` CSV into a clean v1 stream file.
"""

from __future__ import annotations

import argparse
import csv
import random
import sys
import time
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path
from typing import IO, Any

from . import __version__
from .protocol import (
    V1_HEADER,
    DecodeStats,
    FrameDecoder,
    LineDecoder,
    Sample,
    encode_legacy,
    encode_v1,
    encode_v2,
)
from .sources import read_samples

DEFAULT_BAUD = 9600
LOG_HEADER = ("TimeMS", "Hour", "Min", "Sec", "Altitude_m", "Speed_mps", "Voltage_V")
WIRE_FORMATS = ("v1", "v1-legacy", "v2")


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------


def encode_wire(sample: Sample, wire: str, *, checksum: bool = False, seq: int = 0) -> bytes:
    """Serialise *sample* in the chosen wire format, terminator included."""
    if wire == "v2":
        return encode_v2(sample, seq)
    if wire == "v1-legacy":
        return (encode_legacy(sample) + "\n").encode("ascii")
    return (encode_v1(sample, checksum=checksum) + "\n").encode("ascii")


def select(samples: Iterable[Sample], every: int = 1, limit: int | None = None) -> Iterator[Sample]:
    """Keep every *every*-th sample, at most *limit* of them."""
    sent = 0
    for index, sample in enumerate(samples):
        if index % every:
            continue
        if limit is not None and sent >= limit:
            return
        sent += 1
        yield sample


def open_serial(port: str, baud: int, timeout: float = 1.0) -> Any:
    """Open a serial port, importing pyserial only when it is needed."""
    try:
        import serial  # type: ignore[import-untyped]
    except ImportError:
        raise SystemExit(
            "pyserial is required for serial-port access: pip install pyserial\n"
            "(no hardware? try: uavdaq-gs monitor --replay data/telemetry.csv)"
        ) from None
    try:
        return serial.Serial(port, baud, timeout=timeout)
    except serial.SerialException as exc:
        raise SystemExit(f"could not open serial port {port}: {exc}") from None


def corrupt(data: bytes, probability: float, rng: random.Random) -> bytes:
    """Flip one random bit in *data* with the given probability (fault injection)."""
    if probability <= 0 or not data or rng.random() >= probability:
        return data
    damaged = bytearray(data)
    damaged[rng.randrange(len(damaged))] ^= 1 << rng.randrange(8)
    return bytes(damaged)


class CsvLog:
    """Append decoded samples to a CSV file (header written once)."""

    def __init__(self, handle: IO[str]):
        self._writer = csv.writer(handle, lineterminator="\n")
        self._writer.writerow(LOG_HEADER)

    def write(self, sample: Sample) -> None:
        hours, minutes, seconds = sample.hms
        self._writer.writerow(
            (
                "" if sample.time_ms is None else sample.time_ms,
                hours,
                minutes,
                seconds,
                f"{sample.altitude_m:.2f}",
                f"{sample.speed_mps:.2f}",
                f"{sample.voltage_v:.3f}",
            )
        )


def format_summary(stats: DecodeStats) -> str:
    return (
        f"decoded {stats.ok} sample(s); rejected {stats.rejected} "
        f"(malformed {stats.malformed}, checksum/CRC {stats.checksum_errors}, "
        f"overflow {stats.overflows}); headers skipped {stats.headers}; "
        f"bytes dropped {stats.dropped_bytes}; sequence gaps {stats.sequence_gaps}"
    )


# --------------------------------------------------------------------------
# Plotting (optional dependency)
# --------------------------------------------------------------------------


class Plot:
    """Three stacked panels sharing a time axis (one y-scale per measure)."""

    PANELS = (("Altitude (m)", "altitude_m"), ("Ground speed (m/s)", "speed_mps"), ("Battery (V)", "voltage_v"))

    def __init__(self, *, live: bool, save: str | None):
        try:
            import matplotlib

            if not live:
                matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            raise SystemExit("plotting needs matplotlib: pip install matplotlib") from None
        self._plt = plt
        self._live = live
        self._save = save
        self._x: list[float] = []
        self._y: list[list[float]] = [[] for _ in self.PANELS]
        self._fig, self._axes = plt.subplots(len(self.PANELS), 1, sharex=True, figsize=(9, 7))
        self._lines = []
        for axis, (title, _attr) in zip(self._axes, self.PANELS, strict=True):
            (line,) = axis.plot([], [], color="#2a78d6", linewidth=2)
            axis.set_title(title, loc="left", fontsize=10)
            axis.grid(True, color="#d9d8d2", linewidth=0.6)
            for side in ("top", "right"):
                axis.spines[side].set_visible(False)
            self._lines.append(line)
        self._axes[-1].set_xlabel("Log time (s)")
        self._fig.suptitle("UAV-DAQ telemetry")
        self._last_draw = 0.0
        if live:
            plt.ion()
            plt.show(block=False)

    def add(self, sample: Sample) -> None:
        self._x.append((sample.time_ms if sample.time_ms is not None else len(self._x) * 200) / 1000)
        for series, (_title, attr) in zip(self._y, self.PANELS, strict=True):
            series.append(getattr(sample, attr))
        if self._live and time.monotonic() - self._last_draw > 0.2:
            self._redraw()
            self._plt.pause(0.001)
            self._last_draw = time.monotonic()

    def _redraw(self) -> None:
        for axis, line, series in zip(self._axes, self._lines, self._y, strict=True):
            line.set_data(self._x, series)
            axis.relim()
            axis.autoscale_view()

    def finish(self) -> None:
        self._redraw()
        if self._save:
            self._fig.savefig(self._save, dpi=120)
            print(f"plot saved to {self._save}", file=sys.stderr)
        if self._live:
            self._plt.ioff()
            self._plt.show()


# --------------------------------------------------------------------------
# Sub-commands
# --------------------------------------------------------------------------


def run_stream(
    samples: Iterable[Sample],
    write: Callable[[bytes], Any],
    *,
    wire: str = "v1",
    checksum: bool = False,
    rate_hz: float = 5.0,
    verbose: bool = True,
    sleep: Callable[[float], None] = time.sleep,
    out: IO[str] | None = None,
) -> int:
    """Send *samples* through *write* at *rate_hz* lines per second."""
    period = 1.0 / rate_hz if rate_hz > 0 else 0.0
    count = 0
    for count, sample in enumerate(samples, 1):
        packet = encode_wire(sample, wire, checksum=checksum, seq=count - 1)
        write(packet)
        if verbose:
            shown = packet.hex() if wire == "v2" else packet.decode("ascii").strip()
            print(f"[{count}] sent: {shown}", file=out or sys.stdout)
        if period:
            sleep(period)
    return count


def run_monitor(
    chunks: Iterable[bytes],
    *,
    wire: str = "v1",
    require_checksum: bool = False,
    on_sample: Callable[[Sample], None] | None = None,
    limit: int | None = None,
) -> LineDecoder | FrameDecoder:
    """Decode an iterable of raw byte chunks; call *on_sample* for each sample."""
    decoder: LineDecoder | FrameDecoder
    decoder = FrameDecoder() if wire == "v2" else LineDecoder(require_checksum=require_checksum)
    seen = 0
    for chunk in chunks:
        for sample in decoder.feed(chunk):
            seen += 1
            if on_sample:
                on_sample(sample)
            if limit is not None and seen >= limit:
                return decoder
    return decoder


def _serial_chunks(port: Any) -> Iterator[bytes]:
    while True:
        data = port.read(256)
        if data:
            yield data


def _replay_chunks(args: argparse.Namespace) -> Iterator[bytes]:
    """File -> encoder -> (optional bit errors) -> bytes, as if from a UART."""
    rng = random.Random(args.seed)
    period = 1.0 / args.rate if args.rate > 0 else 0.0
    samples = select(read_samples(args.replay), args.every)
    for seq, sample in enumerate(samples):
        if sample.time_ms is None and args.wire != "v1-legacy":
            # Legacy captures have no timestamp: synthesise one at 5 Hz.
            sample = Sample(seq * 200, sample.altitude_m, sample.speed_mps, sample.voltage_v)
        packet = encode_wire(sample, args.wire, checksum=args.checksum, seq=seq)
        yield corrupt(packet, args.corrupt, rng)
        if period:
            time.sleep(period)


def cmd_monitor(args: argparse.Namespace) -> int:
    if args.replay and not Path(args.replay).is_file():
        raise SystemExit(f"replay file not found: {args.replay}")
    port = None if args.replay else open_serial(args.port, args.baud)
    chunks = _replay_chunks(args) if args.replay else _serial_chunks(port)
    plot = Plot(live=args.plot, save=args.save_plot) if (args.plot or args.save_plot) else None
    log_handle = open(args.log, "w", newline="", encoding="ascii") if args.log else None
    log = CsvLog(log_handle) if log_handle else None

    def on_sample(sample: Sample) -> None:
        if log:
            log.write(sample)
        if plot:
            plot.add(sample)
        if not args.quiet:
            hours, minutes, seconds = sample.hms
            print(
                f"T {hours:02d}:{minutes:02d}:{seconds:02d}  ALT {sample.altitude_m:8.2f} m  "
                f"SPD {sample.speed_mps:6.2f} m/s  BAT {sample.voltage_v:6.2f} V"
            )

    decoder: LineDecoder | FrameDecoder | None = None
    try:
        decoder = run_monitor(
            chunks,
            wire=args.wire,
            require_checksum=args.require_checksum,
            on_sample=on_sample,
            limit=args.limit,
        )
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        if log_handle:
            log_handle.close()
        if port is not None:
            port.close()
    if decoder is not None:
        for message in decoder.errors[: args.show_errors]:
            print(f"rejected: {message}", file=sys.stderr)
        print(format_summary(decoder.stats), file=sys.stderr)
    if args.log:
        print(f"log written to {args.log}", file=sys.stderr)
    if plot:
        plot.finish()
    return 0


def cmd_stream(args: argparse.Namespace) -> int:
    if not Path(args.input).is_file():
        raise SystemExit(f"input file not found: {args.input}")
    samples = select(read_samples(args.input), args.every, args.limit)
    if args.dry_run:
        count = run_stream(samples, lambda _b: None, wire=args.wire, checksum=args.checksum, rate_hz=0)
        print(f"dry run: {count} packet(s) encoded, nothing sent", file=sys.stderr)
        return 0
    port = open_serial(args.port, args.baud)
    print(f"streaming {args.input} to {args.port} at {args.baud} baud, {args.rate} Hz", file=sys.stderr)
    time.sleep(1.0)  # let the USB-UART bridge settle
    count = 0
    try:
        count = run_stream(
            samples,
            port.write,
            wire=args.wire,
            checksum=args.checksum,
            rate_hz=args.rate,
            verbose=not args.quiet,
        )
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
    finally:
        port.close()
    print(f"done, {count} packet(s) sent", file=sys.stderr)
    return 0


def cmd_convert(args: argparse.Namespace) -> int:
    if not Path(args.input).is_file():
        raise SystemExit(f"input file not found: {args.input}")
    count = 0
    with open(args.output, "w", newline="", encoding="ascii") as handle:
        if args.wire == "v1":
            handle.write(V1_HEADER + "\n")
        for sample in select(read_samples(args.input), args.every):
            if args.wire == "v1-legacy":
                handle.write(encode_legacy(sample) + "\n")
            else:
                handle.write(encode_v1(sample, checksum=args.checksum) + "\n")
            count += 1
    print(f"wrote {count} record(s) to {args.output}", file=sys.stderr)
    return 0


# --------------------------------------------------------------------------
# Argument parsing
# --------------------------------------------------------------------------


def _positive_int(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="uavdaq-gs",
        description="UAV-DAQ ground station: stream, decode, log and plot telemetry.",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser, wires: tuple[str, ...] = WIRE_FORMATS) -> None:
        p.add_argument(
            "--wire", choices=wires, default="v1", help="wire format (default: v1, what the firmware parses)"
        )
        p.add_argument("--checksum", action="store_true", help="append the optional *HH XOR checksum to v1 lines")
        p.add_argument("--every", type=_positive_int, default=1, metavar="N", help="use every N-th record of the input")

    stream = sub.add_parser("stream", help="send a log file to the STM32 over UART")
    stream.add_argument("--port", help="serial port, e.g. COM3 or /dev/ttyUSB0")
    stream.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    stream.add_argument("--input", default="data/telemetry.csv", help="mavlogdump CSV, v1 or legacy file")
    stream.add_argument("--rate", type=float, default=5.0, help="lines per second (default 5)")
    stream.add_argument("--limit", type=_positive_int, help="stop after N records")
    stream.add_argument("--dry-run", action="store_true", help="encode and print, do not open a port")
    stream.add_argument("--quiet", action="store_true")
    common(stream)
    stream.set_defaults(func=cmd_stream)

    monitor = sub.add_parser("monitor", help="decode, validate, log and plot a telemetry stream")
    source = monitor.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="serial port to listen on")
    source.add_argument("--replay", metavar="FILE", help="no hardware: run FILE through encoder and decoder")
    monitor.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    monitor.add_argument("--rate", type=float, default=0.0, help="replay pacing in Hz (0 = as fast as possible)")
    monitor.add_argument("--limit", type=_positive_int, help="stop after N decoded samples")
    monitor.add_argument("--require-checksum", action="store_true", help="reject v1 lines without a valid *HH")
    monitor.add_argument(
        "--corrupt",
        type=float,
        default=0.0,
        metavar="P",
        help="replay only: flip one bit per packet with probability P",
    )
    monitor.add_argument("--seed", type=int, default=0, help="seed for --corrupt (default 0)")
    monitor.add_argument("--log", metavar="CSV", help="write decoded samples to this CSV file")
    monitor.add_argument("--plot", action="store_true", help="live plot (needs matplotlib)")
    monitor.add_argument("--save-plot", metavar="PNG", help="save the plot to a file (needs matplotlib)")
    monitor.add_argument("--show-errors", type=int, default=5, metavar="N", help="print the first N rejected packets")
    monitor.add_argument("--quiet", action="store_true", help="do not print every sample")
    common(monitor)
    monitor.set_defaults(func=cmd_monitor)

    convert = sub.add_parser("convert", help="mavlogdump CSV -> clean stream file")
    convert.add_argument("input")
    convert.add_argument("output")
    common(convert, wires=("v1", "v1-legacy"))
    convert.set_defaults(func=cmd_convert)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "stream" and not args.dry_run and not args.port:
        parser.error("stream: --port is required unless --dry-run is given")
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
