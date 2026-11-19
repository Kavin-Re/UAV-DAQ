"""Tests for file readers and the command-line front end."""

import contextlib
import csv
import io
import tempfile
import unittest
from pathlib import Path

from uavdaq_gs import cli
from uavdaq_gs.protocol import ParseError, Sample, encode_v1
from uavdaq_gs.sources import detect_format, merge_mavlog_rows, read_samples

REPO = Path(__file__).resolve().parents[2]
FLIGHT = REPO / "data" / "telemetry.csv"
LEGACY = REPO / "data" / "telemetry_stream.csv"

MAVLOG = """"TimeUS","TimeS","Date","Time","AHR2.Alt","XKF1.VN","XKF1.VE","BAT.Volt"
"1000000","1.0","d","t","","3.0","4.0",""
"1100000","1.1","d","t","10.5","","",""
"1200000","1.2","d","t","","","","12.6"
"bogus","1.2","d","t","99","","",""
"1300000","1.3","d","t","","0.0","0.0",""
"""


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def write(self, name: str, text: str) -> Path:
        path = self.tmp / name
        path.write_text(text, encoding="ascii")
        return path

    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()


class SourceTests(TempDirCase):
    def test_forward_fill_merge(self):
        rows = csv.DictReader(io.StringIO(MAVLOG))
        self.assertEqual(
            list(merge_mavlog_rows(rows)),
            [Sample(1200, 10.5, 5.0, 12.6), Sample(1300, 10.5, 0.0, 12.6)],
        )

    def test_detect_format(self):
        self.assertEqual(detect_format('"TimeUS","TimeS"'), "mavlog")
        self.assertEqual(detect_format("536.49,0.0,24.122\n"), "legacy")
        self.assertEqual(detect_format("TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n"), "v1")
        self.assertEqual(detect_format("1,0,0,0,1.0,2.0,3.0*4F\n"), "v1")

    def test_v1_file_skips_header_and_bad_lines(self):
        path = self.write("s.csv", "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n1000,0,0,1,1.00,2.00,3.00\n1,oops\n\n")
        self.assertEqual(list(read_samples(path)), [Sample(1000, 1.0, 2.0, 3.0)])
        with self.assertRaises(ParseError):
            list(read_samples(path, strict=True))

    def test_missing_columns_reported(self):
        path = self.write("m.csv", '"TimeUS","AHR2.Alt"\n"1","2"\n')
        with self.assertRaises(ParseError):
            list(read_samples(path))

    def test_empty_file(self):
        self.assertEqual(list(read_samples(self.write("e.csv", ""))), [])

    @unittest.skipUnless(FLIGHT.is_file() and LEGACY.is_file(), "sample data not present")
    def test_reproduces_committed_stream_file(self):
        """data/telemetry_stream.csv is data/telemetry.csv forward-filled."""
        merged = list(read_samples(FLIGHT))
        legacy = list(read_samples(LEGACY))
        self.assertEqual(len(merged), len(legacy))
        self.assertEqual(len(merged), 72040)
        for ours, theirs in zip(merged, legacy, strict=True):
            self.assertEqual(round(ours.altitude_m, 2), theirs.altitude_m)
            self.assertEqual(round(ours.speed_mps, 1), theirs.speed_mps)
            self.assertEqual(round(ours.voltage_v, 3), theirs.voltage_v)


class CliTests(TempDirCase):
    def test_replay_logs_csv(self):
        source = self.write("in.csv", MAVLOG)
        log = self.tmp / "out.csv"
        code, out, err = self.run_cli("monitor", "--replay", source, "--log", log)
        self.assertEqual(code, 0)
        self.assertIn("ALT    10.50 m", out)
        self.assertIn("decoded 2 sample(s); rejected 0", err)
        self.assertEqual(
            log.read_text().splitlines(),
            [
                "TimeMS,Hour,Min,Sec,Altitude_m,Speed_mps,Voltage_V",
                "1200,0,0,1,10.50,5.00,12.600",
                "1300,0,0,1,10.50,0.00,12.600",
            ],
        )

    def test_replay_all_wire_formats_agree(self):
        source = self.write("in.csv", MAVLOG)
        logs = []
        for wire in cli.WIRE_FORMATS:
            log = self.tmp / f"{wire}.csv"
            self.run_cli("monitor", "--replay", source, "--wire", wire, "--log", log, "--quiet")
            logs.append([line.split(",")[4:] for line in log.read_text().splitlines()])
        self.assertEqual(logs[0], logs[1])
        self.assertEqual(logs[0], logs[2])

    def test_replay_with_corruption_never_passes_bad_checksum(self):
        lines = [encode_v1(Sample(i * 200, 100 + i, 1.0, 12.0)) for i in range(400)]
        source = self.write("in.csv", "\n".join(lines) + "\n")
        for wire, extra in (("v1", ["--checksum", "--require-checksum"]), ("v2", [])):
            log = self.tmp / f"c-{wire}.csv"
            _, _, err = self.run_cli(
                "monitor", "--replay", source, "--wire", wire, *extra, "--corrupt", "0.3", "--log", log, "--quiet"
            )
            rows = log.read_text().splitlines()[1:]
            self.assertLess(len(rows), 400, err)
            self.assertGreater(len(rows), 200, err)
            valid = {f"{i * 200},{100 + i}.00" for i in range(400)}
            for row in rows:
                time_ms, _h, _m, _s, alt, speed, volt = row.split(",")
                self.assertIn(f"{time_ms},{alt}", valid)
                self.assertEqual((speed, volt), ("1.00", "12.000"))

    def test_limit_and_every(self):
        source = self.write("in.csv", "\n".join(f"{i},0,0,0,{i}.0,0.0,1.0" for i in range(100)) + "\n")
        log = self.tmp / "o.csv"
        self.run_cli("monitor", "--replay", source, "--every", "10", "--limit", "3", "--log", log, "--quiet")
        self.assertEqual([r.split(",")[0] for r in log.read_text().splitlines()[1:]], ["0", "10", "20"])

    def test_stream_dry_run_prints_v1_lines(self):
        source = self.write("in.csv", MAVLOG)
        code, out, err = self.run_cli("stream", "--dry-run", "--input", source)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[0], "[1] sent: 1200,0,0,1,10.50,5.00,12.60")
        self.assertIn("2 packet(s)", err)

    def test_run_stream_writes_to_port_and_paces(self):
        written, naps = [], []
        count = cli.run_stream(
            [Sample(0, 1, 2, 3), Sample(200, 1, 2, 3)],
            written.append,
            rate_hz=5,
            verbose=False,
            sleep=naps.append,
        )
        self.assertEqual(count, 2)
        self.assertEqual(written[1], b"200,0,0,0,1.00,2.00,3.00\n")
        self.assertEqual(naps, [0.2, 0.2])

    def test_convert(self):
        source = self.write("in.csv", MAVLOG)
        target = self.tmp / "stream.csv"
        self.run_cli("convert", source, target)
        text = target.read_text().splitlines()
        self.assertEqual(text[0], "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage")
        self.assertEqual(list(read_samples(target)), [Sample(1200, 10.5, 5.0, 12.6), Sample(1300, 10.5, 0.0, 12.6)])

    def test_argument_errors(self):
        for argv in (
            ["stream"],
            ["monitor"],
            ["monitor", "--replay", "x", "--port", "y"],
            ["monitor", "--replay", "x", "--every", "0"],
        ):
            with self.subTest(argv=argv), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                cli.main(argv)
        with self.assertRaises(SystemExit):
            cli.main(["monitor", "--replay", str(self.tmp / "missing.csv")])


if __name__ == "__main__":
    unittest.main()
