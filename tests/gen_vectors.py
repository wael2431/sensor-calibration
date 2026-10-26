#!/usr/bin/env python3
"""Generates test vectors for the C code (calib_eval + Rust validation) from the Python mirror.

Usage: gen_vectors.py vectors.txt blob.bin
vectors.txt format:
    CASE a_min a_max y_min y_max c0 c1 c2 c3 valid N
    a expected_y out_of_range        (N lines; N = 0 if not valid)
"""
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "host_sender"))
import calibtools as ct  # noqa: E402

rng = random.Random(1234)


def wild_channel():
    a_min = rng.randint(-2 ** 31, 2 ** 31 - 1)
    a_max = rng.randint(-2 ** 31, 2 ** 31 - 1)
    y_min = rng.randint(-2 ** 31, 2 ** 31 - 1)
    y_max = rng.randint(-2 ** 31, 2 ** 31 - 1)
    return {"a_min": a_min, "a_max": a_max, "y_min": y_min, "y_max": y_max,
            "c": [rng.randint(-2 ** 31, 2 ** 31 - 1) for _ in range(4)]}


def plausible_channel():
    a_min = rng.randint(-5000, 5000)
    a_max = a_min + rng.randint(50, 20000)
    y_min = rng.randint(-5000, 5000)
    y_max = y_min + rng.randint(50, 20000)
    sy = y_max - y_min
    return {"a_min": a_min, "a_max": a_max, "y_min": y_min, "y_max": y_max,
            "c": [rng.randint(y_min, y_max) * ct.ONE,
                  rng.randint(-sy, sy) * ct.ONE,
                  rng.randint(-sy // 2, sy // 2) * ct.ONE,
                  rng.randint(-sy // 2, sy // 2) * ct.ONE]}


def inputs(ch):
    span = ch["a_max"] - ch["a_min"]
    out = [ch["a_min"], ch["a_max"], ch["a_min"] - 1, ch["a_max"] + 1, 0, -2 ** 31, 2 ** 31 - 1]
    for _ in range(300):
        out.append(rng.randint(ch["a_min"] - span // 2, ch["a_max"] + span // 2))
    return [max(-2 ** 31, min(2 ** 31 - 1, a)) for a in out]


cases = [ct.neutral_channels()[0], ct.neutral_channels()[1]]
cases += [plausible_channel() for _ in range(300)] + [wild_channel() for _ in range(300)]

n_valid = 0
with open(sys.argv[1], "w") as f:
    for ch in cases:
        err = ct.validate_channel(ch)
        valid = err is None
        n_valid += valid
        pts = inputs(ch) if valid else []
        f.write("CASE %d %d %d %d %d %d %d %d %d %d\n" %
                (ch["a_min"], ch["a_max"], ch["y_min"], ch["y_max"], *ch["c"], int(valid), len(pts)))
        for a in pts:
            y, oor = ct.eval_fixed(ch, a)
            f.write("%d %d %d\n" % (a, y, int(oor)))

with open(sys.argv[2], "wb") as f:
    pair = [cases[2], cases[3]]
    if any(ct.validate_channel(c) is not None for c in pair):
        pair = ct.neutral_channels()
    f.write(ct.build_calibration(42, pair))
print(f"{len(cases)} cases ({n_valid} valid, {len(cases) - n_valid} invalid)")
