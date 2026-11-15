#!/usr/bin/env python3
"""Python tests: frames, blob, polynomial fit, fixed-point evaluation."""
import math
import os
import random
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "host_sender"))
import calibtools as ct  # noqa: E402


class TestFraming(unittest.TestCase):
    def test_cobs_roundtrip(self):
        rng = random.Random(1)
        for _ in range(2000):
            data = bytes(rng.choice([0, 1, 0x55, 0xFF]) if rng.random() < 0.5 else rng.randrange(256)
                         for _ in range(rng.randrange(0, 600)))
            enc = ct.cobs_encode(data)
            self.assertNotIn(0, enc)
            self.assertEqual(ct.cobs_decode(enc), data)

    def test_frame_roundtrip_and_bad_crc(self):
        f = ct.build_frame(ct.MSG_CALIB, 7, b"\x00\x01\x02" * 10)
        self.assertEqual(f[-1], 0)
        self.assertEqual(ct.parse_frame(f[:-1]), (ct.MSG_CALIB, 7, b"\x00\x01\x02" * 10))
        bad = bytearray(f[:-1])
        bad[5] ^= 0x10
        with self.assertRaises(ValueError):
            ct.parse_frame(bytes(bad))

    def test_decode_measure(self):
        p = struct.pack("<QIBBBB6i", 1234567, 3, 3, 3, 1, 3, 2150, 5500, 2160, 5480, 2200, 5400)
        m = ct.decode_measure(p)
        self.assertEqual((m["rev"], m["a"], m["est"], m["ref"], m["oor"]),
                         (3, [2150, 5500], [2160, 5480], [2200, 5400], [True, False]))
        p = struct.pack("<QIBBBB6i", 0, 0, 1, 1, 0, 0, 100, 0, 100, 0, 0, 0)
        m = ct.decode_measure(p)
        self.assertEqual((m["a"], m["ref"]), ([100, None], [None, None]))

    def test_measure_from_c(self):
        path = os.environ.get("MEASURE_BIN")
        if not path:
            self.skipTest("MEASURE_BIN not set (run by run_tests.sh)")
        with open(path, "rb") as fh:
            data = fh.read()
        mtype, seq, payload = ct.parse_frame(data[:-1])
        m = ct.decode_measure(payload)
        self.assertEqual((mtype, seq, len(payload)), (ct.MSG_MEASURE, 9, 40))
        self.assertEqual((m["rev"], m["a"], m["est"], m["ref"], m["oor"]),
                         (3, [2150, 5500], [2160, 5480], [2200, 5400], [True, False]))


class TestCalibration(unittest.TestCase):
    def test_blob_roundtrip(self):
        chans = ct.neutral_channels()
        blob = ct.build_calibration(5, chans)
        self.assertEqual(len(blob), ct.CAL_BLOB_LEN)
        self.assertEqual(ct.parse_calibration(blob), (5, chans))
        with self.assertRaises(ValueError):
            ct.parse_calibration(ct.build_calibration(5, chans, bad_crc=True))

    def test_neutral_is_identity(self):
        for ch in ct.neutral_channels():
            self.assertIsNone(ct.validate_channel(ch))
            for a in range(ch["a_min"], ch["a_max"] + 1, 97):
                y, oor = ct.eval_fixed(ch, a)
                self.assertFalse(oor)
                self.assertLessEqual(abs(y - a), 1)

    def test_out_of_range_is_clamped_and_flagged(self):
        ch = ct.neutral_channels()[1]                    # humidity 0..10000
        self.assertEqual(ct.eval_fixed(ch, -500), (0, True))
        self.assertEqual(ct.eval_fixed(ch, 12000), (10000, True))

    def test_validation_rejects_bad_channels(self):
        ch = dict(ct.neutral_channels()[0])
        self.assertEqual(ct.validate_channel({**ch, "a_max": ch["a_min"]}), -7)
        self.assertEqual(ct.validate_channel({**ch, "y_min": ch["y_max"]}), -7)
        self.assertEqual(ct.validate_channel({**ch, "c": [0, 0, 0, 2 ** 31 - 1]}), -8)

    def test_horner_known_value(self):
        # y = 10 + 20 x at x = 0.5
        self.assertEqual((ct.horner_q16([10 << 16, 20 << 16, 0, 0], 32768) + 32768) >> 16, 20)


class TestFit(unittest.TestCase):
    @staticmethod
    def synth(n=400, seed=7):
        """Input = DHT11 (biased, quantized to whole degrees); target = BME280 (true value + fine noise)."""
        rng = random.Random(seed)
        a, ref = [], []
        for _ in range(n):
            t = rng.randint(1000, 3500)                                    # true temperature: 10..35 °C
            ref.append(int(t + rng.gauss(0, 5)))                          # BME280: fine
            dht = 0.93 * t + 250 + 0.00002 * (t - 2200) ** 2 + rng.gauss(0, 40)   # DHT11: gain, bias, curvature
            a.append(int(round(dht / 100.0)) * 100)                       # 1 °C resolution
        return a, ref

    def test_fit_beats_identity_and_matches_float(self):
        a, ref = self.synth()
        ch, st = ct.fit_channel(a, ref, degree=3, margin=0.05)
        self.assertIsNone(ct.validate_channel(ch))
        # the remaining error comes from quantization (±0.5 °C) and DHT11 noise: ~0.5 °C
        self.assertLess(st["rmse_test"], 90)
        self.assertLess(st["rmse_test"], 0.75 * st["rmse_test_identity"])
        # the fixed-point version follows the floating-point polynomial within ~1 unit (same bounds as the firmware)
        span = ch["a_max"] - ch["a_min"]
        coef = [c / ct.ONE for c in ch["c"]]
        for v in range(ch["a_min"] - 200, ch["a_max"] + 201, 50):
            x = min(1.0, max(0.0, (v - ch["a_min"]) / span))
            yf = coef[0] + coef[1] * x + coef[2] * x * x + coef[3] * x ** 3
            yf = min(ch["y_max"], max(ch["y_min"], yf))
            self.assertLessEqual(abs(ct.eval_fixed(ch, v)[0] - yf), 1.0)

    def test_lower_degrees_zero_high_coeffs(self):
        a, ref = self.synth()
        ch, _ = ct.fit_channel(a, ref, degree=1)
        self.assertEqual(ch["c"][2:], [0, 0])

    def test_rejects_degenerate_input(self):
        with self.assertRaises(ValueError):
            ct.fit_channel([2000] * 50, [2100] * 50)                       # zero input range
        with self.assertRaises(ValueError):
            ct.fit_channel([1, 2, 3], [1, 2, 3])                           # too few points
        with self.assertRaises(ValueError):                                # only 2 input values
            ct.fit_channel([2000, 2100] * 30, list(range(60)), degree=3)

    def test_fit_blob_accepted_by_validation(self):
        a, ref = self.synth()
        ch, _ = ct.fit_channel(a, ref)
        blob = ct.build_calibration(1, [ch, ct.neutral_channels()[1]])
        self.assertEqual(ct.parse_calibration(blob)[1][0], ch)


if __name__ == "__main__":
    unittest.main(verbosity=2)
