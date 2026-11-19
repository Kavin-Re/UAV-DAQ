"""Unit tests for the wire-format codecs (standard library only)."""

import unittest

from uavdaq_gs.protocol import (
    MAX_LINE_LEN,
    V1_HEADER,
    ChecksumError,
    FrameDecoder,
    HeaderLine,
    LineDecoder,
    ParseError,
    Sample,
    crc16_ccitt,
    encode_legacy,
    encode_v1,
    encode_v2,
    parse_line,
    xor_checksum,
)

SAMPLE = Sample(114805, 536.49, 1.25, 24.12)


class V1LineTests(unittest.TestCase):
    def test_encode_matches_firmware_format(self):
        self.assertEqual(encode_v1(SAMPLE), "114805,0,1,54,536.49,1.25,24.12")

    def test_round_trip(self):
        self.assertEqual(parse_line(encode_v1(SAMPLE)), SAMPLE)

    def test_bytes_crlf_and_spaces_accepted(self):
        self.assertEqual(parse_line(b"114805, 0, 1, 54, 536.49, 1.25, 24.12\r\n"), SAMPLE)

    def test_negative_altitude(self):
        self.assertEqual(parse_line("1000,0,0,1,-3.50,0.00,11.10").altitude_m, -3.5)

    def test_legacy_three_columns(self):
        sample = parse_line("536.49,0.0,24.122")
        self.assertIsNone(sample.time_ms)
        self.assertEqual((sample.altitude_m, sample.speed_mps, sample.voltage_v), (536.49, 0.0, 24.122))
        self.assertEqual(encode_legacy(sample), "536.49,0.0,24.122")

    def test_header_is_reported_separately(self):
        with self.assertRaises(HeaderLine):
            parse_line(V1_HEADER)

    def test_malformed_lines_rejected(self):
        bad = [
            "",
            "   ",
            "1,2,3,4",  # wrong field count
            "1,2,3,4,5,6,7,8",
            "114805,0,1,54,536.49,abc,24.12",  # not a number
            "114805,0,1,54,536.49,,24.12",  # empty field
            "-5,0,1,54,536.49,1.25,24.12",  # negative time
            "1.5,0,1,54,536.49,1.25,24.12",  # fractional time
            "114805,0,61,54,536.49,1.25,24.12",  # minutes out of range
            "114805,0,1,60,536.49,1.25,24.12",  # seconds out of range
            "99999999999,0,1,54,536.49,1.25,24.12",  # > uint32
            "114805,0,1,54,nan,1.25,24.12",
            "114805,0,1,54,536.49,inf,24.12",
            b"114805,0,1,54,536\xff49,1.25,24.12",
            "1," * 100 + "1",  # over-long
        ]
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ParseError):
                parse_line(line)

    def test_hms_derived_from_time(self):
        self.assertEqual(Sample(3_725_999, 0, 0, 0).hms, (1, 2, 5))
        self.assertEqual(Sample(None, 0, 0, 0).hms, (0, 0, 0))


class ChecksumTests(unittest.TestCase):
    def test_known_xor(self):
        # Classic NMEA example sentence body.
        self.assertEqual(xor_checksum("GPGLL,5300.97914,N,00259.98174,E,125926,A"), 0x28)

    def test_checksummed_round_trip(self):
        line = encode_v1(SAMPLE, checksum=True)
        self.assertRegex(line, r"\*[0-9A-F]{2}$")
        self.assertEqual(parse_line(line, require_checksum=True), SAMPLE)

    def test_single_character_corruption_detected(self):
        line = encode_v1(SAMPLE, checksum=True)
        for index in range(len(line) - 3):
            damaged = line[:index] + chr(ord(line[index]) ^ 0x01) + line[index + 1 :]
            with self.subTest(index=index), self.assertRaises(ParseError):
                parse_line(damaged)

    def test_bad_suffix(self):
        for line in ("1,0,0,0,1.0,1.0,1.0*", "1,0,0,0,1.0,1.0,1.0*Z9", "1,0,0,0,1.0,1.0,1.0*123"):
            with self.subTest(line=line), self.assertRaises(ChecksumError):
                parse_line(line)

    def test_required_checksum_missing(self):
        with self.assertRaises(ChecksumError):
            parse_line(encode_v1(SAMPLE), require_checksum=True)


class LineDecoderTests(unittest.TestCase):
    def test_split_across_chunks(self):
        decoder = LineDecoder()
        data = (encode_v1(SAMPLE) + "\r\n").encode()
        out = []
        for index in range(len(data)):
            out += decoder.feed(data[index : index + 1])
        self.assertEqual(out, [SAMPLE])
        self.assertEqual(decoder.stats.ok, 1)

    def test_garbage_is_counted_not_raised(self):
        decoder = LineDecoder()
        stream = (
            V1_HEADER.encode()
            + b"\n\n\r\nhello,world\n\xff\xfe\x00\n1,2,3\n"
            + encode_v1(SAMPLE).encode()
            + b"\n114805,0,1,54,536.49"  # truncated, no newline yet
        )
        out = decoder.feed(stream)
        self.assertEqual(out[-1], SAMPLE)
        self.assertEqual(len(out), 2)  # "1,2,3" is a valid legacy line
        self.assertEqual(decoder.stats.headers, 1)
        self.assertEqual(decoder.stats.malformed, 2)  # "hello,world" and the binary junk
        self.assertEqual(len(decoder.errors), 2)

    def test_overlong_line_discarded_then_resync(self):
        decoder = LineDecoder()
        out = decoder.feed(b"9" * (MAX_LINE_LEN + 50) + b",1,2\n" + encode_v1(SAMPLE).encode() + b"\n")
        self.assertEqual(out, [SAMPLE])
        self.assertEqual(decoder.stats.overflows, 1)
        self.assertGreater(decoder.stats.dropped_bytes, MAX_LINE_LEN)

    def test_require_checksum(self):
        decoder = LineDecoder(require_checksum=True)
        good = encode_v1(SAMPLE, checksum=True).encode() + b"\n"
        out = decoder.feed(encode_v1(SAMPLE).encode() + b"\n" + good)
        self.assertEqual(out, [SAMPLE])
        self.assertEqual(decoder.stats.checksum_errors, 1)

    def test_error_list_is_bounded(self):
        decoder = LineDecoder(max_errors=3)
        decoder.feed(b"1,2\n" * 10)
        self.assertEqual(decoder.stats.malformed, 10)
        self.assertEqual(len(decoder.errors), 3)


class V2FrameTests(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)
        self.assertEqual(crc16_ccitt(b""), 0xFFFF)

    def test_frame_layout(self):
        frame = encode_v2(Sample(0x01020304, 1.0, 2.0, 3.0), seq=7)
        self.assertEqual(len(frame), 19)
        self.assertEqual(frame[:5], bytes((0xA5, 0x5A, 0x21, 7, 12)))
        self.assertEqual(frame[5:17], bytes.fromhex("0403020164000000c800b80b"))
        self.assertEqual(int.from_bytes(frame[17:], "little"), crc16_ccitt(frame[2:17]))

    def test_round_trip_with_quantisation(self):
        decoder = FrameDecoder()
        (out,) = decoder.feed(encode_v2(Sample(114805, -12.346, 1.254, 24.1226), seq=0))
        self.assertEqual(out, Sample(114805, -12.35, 1.25, 24.123))

    def test_byte_at_a_time_and_noise_between_frames(self):
        decoder = FrameDecoder()
        stream = b"\x00\xa5\x13" + encode_v2(SAMPLE, 0) + b"\xa5\x5a\xff" + encode_v2(SAMPLE, 1) + b"\xa5"
        out = []
        for index in range(len(stream)):
            out += decoder.feed(stream[index : index + 1])
        self.assertEqual(out, [SAMPLE, SAMPLE])
        self.assertEqual(decoder.stats.sequence_gaps, 0)
        self.assertGreaterEqual(decoder.stats.dropped_bytes, 5)

    def test_every_single_bit_error_is_rejected(self):
        frame = encode_v2(SAMPLE, 3)
        for bit in range(len(frame) * 8):
            damaged = bytearray(frame)
            damaged[bit // 8] ^= 1 << (bit % 8)
            decoder = FrameDecoder()
            with self.subTest(bit=bit):
                self.assertEqual(decoder.feed(bytes(damaged) + b"\x00" * 80), [])

    def test_recovers_after_corrupt_frame_and_counts_gap(self):
        decoder = FrameDecoder()
        bad = bytearray(encode_v2(SAMPLE, 1))
        bad[8] ^= 0x40
        out = decoder.feed(encode_v2(SAMPLE, 0) + bytes(bad) + encode_v2(SAMPLE, 2))
        self.assertEqual(len(out), 2)
        self.assertEqual(decoder.stats.checksum_errors, 1)
        self.assertEqual(decoder.stats.sequence_gaps, 1)

    def test_sequence_wraps(self):
        decoder = FrameDecoder()
        decoder.feed(encode_v2(SAMPLE, 255) + encode_v2(SAMPLE, 256))
        self.assertEqual(decoder.stats.sequence_gaps, 0)

    def test_out_of_range_values_refused(self):
        with self.assertRaises(ValueError):
            encode_v2(Sample(1, 0.0, 700.0, 12.0))
        with self.assertRaises(ValueError):
            encode_v2(Sample(None, 0.0, 1.0, 12.0))


if __name__ == "__main__":
    unittest.main()
