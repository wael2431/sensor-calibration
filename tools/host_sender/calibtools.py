"""PC-side functions (no serial port): frames, calibration blob, calibration file, fixed-point evaluation.

Model (identical to the firmware): for each channel, the BME280 value is estimated from the DHT11:
    x = clamp((a - a_min) / (a_max - a_min), 0, 1)      Q16.16
    y = c0 + x*(c1 + x*(c2 + x*c3))                     Horner, c[k] in Q16.16
    y = clamp(round(y), y_min, y_max)
Firmware units: 0.01 °C (channel 0, temperature) and 0.01 %RH (channel 1, humidity).
Calibration files use physical units (°C, %RH) and are converted by `load_calibration`.
"""
import math
import struct
import zlib

# --------------------------------------------------------------------- constants
MSG_MEASURE, MSG_CALIB, MSG_ACK, MSG_NACK = 0x01, 0x10, 0x20, 0x21
CAL_MAGIC, CAL_VERSION, CAL_CHANNELS = 0x324C4143, 2, 2
CAL_BLOB_LEN = 80
Q = 16
ONE = 1 << Q
I32_MIN, I32_MAX = -(2 ** 31), 2 ** 31 - 1
ERRORS = {-1: "NULL", -2: "LEN", -3: "MAGIC", -4: "VERSION", -5: "CRC", -6: "CHANNELS",
          -7: "RANGE (invalid range or bounds)", -8: "DIVERGES (polynomial out of bounds)",
          -100: "NVS", -101: "UNKNOWN_TYPE"}


# ------------------------------------------------------------------------- COBS
def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_idx, code = 0, 1
    for b in data:
        if b == 0:
            out[code_idx] = code
            code_idx = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx = len(out)
                out.append(0)
                code = 1
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            raise ValueError("0x00 inside a COBS block")
        n = code - 1
        if i + n > len(data):
            raise ValueError("truncated COBS block")
        out += data[i:i + n]
        i += n
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


# ----------------------------------------------------------------------- frames
def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF          # CRC32 IEEE, same as cal_crc32 (Rust)


def build_frame(msg_type: int, seq: int, payload: bytes) -> bytes:
    raw = struct.pack("<BBH", msg_type, seq, len(payload)) + payload
    raw += struct.pack("<I", crc32(raw))
    return cobs_encode(raw) + b"\x00"


def parse_frame(block: bytes):
    """Returns (type, seq, payload) or raises ValueError."""
    raw = cobs_decode(block)
    if len(raw) < 8:
        raise ValueError("frame too short")
    mtype, seq, plen = struct.unpack_from("<BBH", raw)
    if len(raw) != 4 + plen + 4:
        raise ValueError("inconsistent length")
    if crc32(raw[:-4]) != struct.unpack_from("<I", raw, len(raw) - 4)[0]:
        raise ValueError("CRC invalide")
    return mtype, seq, raw[4:4 + plen]


def decode_measure(payload: bytes) -> dict:
    """MEASURE (40 bytes) -> dict. List entries are None for invalid channels."""
    ts_us, rev, a_v, est_v, est_oor, ref_v = struct.unpack_from("<QIBBBB", payload)
    a = struct.unpack_from("<2i", payload, 16)
    est = struct.unpack_from("<2i", payload, 24)
    ref = struct.unpack_from("<2i", payload, 32)
    pick = lambda vals, mask: [v if mask & (1 << i) else None for i, v in enumerate(vals)]
    return {"ts": ts_us / 1e6, "rev": rev, "a": pick(a, a_v), "est": pick(est, est_v),
            "ref": pick(ref, ref_v), "oor": [bool(est_oor & (1 << i)) for i in range(2)],
            "a_valid": a_v, "ref_valid": ref_v}


# ------------------------------------------------------- calibration (blob, v2)
def neutral_channels():
    """Identity polynomial (same default as the firmware)."""
    return [
        {"a_min": -4000, "a_max": 8500, "y_min": -4000, "y_max": 8500,
         "c": [-4000 * ONE, 12500 * ONE, 0, 0]},
        {"a_min": 0, "a_max": 10000, "y_min": 0, "y_max": 10000,
         "c": [0, 10000 * ONE, 0, 0]},
    ]


def build_calibration(revision: int, channels, bad_crc: bool = False) -> bytes:
    blob = struct.pack("<IBBHI", CAL_MAGIC, CAL_VERSION, CAL_CHANNELS, 0, revision)
    for ch in channels:
        blob += struct.pack("<8i", ch["a_min"], ch["a_max"], ch["y_min"], ch["y_max"], *ch["c"])
    crc = crc32(blob) ^ (0xDEADBEEF if bad_crc else 0)
    blob += struct.pack("<I", crc)
    assert len(blob) == CAL_BLOB_LEN
    return blob


def parse_calibration(blob: bytes):
    """Decodes a blob (PC side, for tests). Returns (revision, channels)."""
    if len(blob) != CAL_BLOB_LEN:
        raise ValueError("length")
    magic, ver, nch, _, rev = struct.unpack_from("<IBBHI", blob)
    if magic != CAL_MAGIC or ver != CAL_VERSION or nch != CAL_CHANNELS:
        raise ValueError("header")
    if crc32(blob[:-4]) != struct.unpack_from("<I", blob, CAL_BLOB_LEN - 4)[0]:
        raise ValueError("CRC")
    chans = []
    for i in range(CAL_CHANNELS):
        v = struct.unpack_from("<8i", blob, 12 + 32 * i)
        chans.append({"a_min": v[0], "a_max": v[1], "y_min": v[2], "y_max": v[3], "c": list(v[4:])})
    return rev, chans


# ------------------------------------------------ evaluation (mirror of calib_eval.c)
def horner_q16(c, x):
    r = c[3]
    for k in (2, 1, 0):
        r = ((r * x) >> Q) + c[k]
    return r


def eval_fixed(ch, a: int):
    """Returns (y, out_of_range). Same integers and rounding as calib_eval.c."""
    span = ch["a_max"] - ch["a_min"]
    d = a - ch["a_min"]
    oor = False
    if d < 0:
        d, oor = 0, True
    elif d > span:
        d, oor = span, True
    x = (d << Q) // span
    y = (horner_q16(ch["c"], x) + (ONE >> 1)) >> Q
    return max(ch["y_min"], min(ch["y_max"], y)), oor


def validate_channel(ch):
    """Mirror of the Rust validation. Returns None if valid, otherwise the (negative) error code."""
    span_a = ch["a_max"] - ch["a_min"]
    if span_a <= 0 or span_a > I32_MAX:
        return -7
    if ch["y_min"] >= ch["y_max"]:
        return -7
    span_y = ch["y_max"] - ch["y_min"]
    lo, hi = ch["y_min"] - span_y, ch["y_max"] + span_y
    for k in range(33):
        x = k * ONE // 32
        y = (horner_q16(ch["c"], x) + (ONE >> 1)) >> Q
        if y < lo or y > hi:
            return -8
    return None


# ------------------------------------------------- calibration file (physical units)
CHANNEL_LABELS = ["temperature", "humidity"]


def _poly(c, x):
    return c[0] + x * (c[1] + x * (c[2] + x * c[3]))


def channel_from_spec(spec: dict) -> dict:
    """Converts one channel of a calibration file (physical units: °C or %RH) into the firmware's integers.

    spec keys:
      "c"       1 to 4 coefficients c0..c3 (missing ones are 0)
      "form"    "raw" (default): y = c0 + c1*a + c2*a^2 + c3*a^3, with a = DHT11 value, y = BME280 estimate
                "normalized":    y = c0 + c1*x + c2*x^2 + c3*x^3, with x = (a - a_min) / (a_max - a_min)
      "a_min", "a_max"  input range over which the polynomial is valid (DHT11 values)
      "y_min", "y_max"  optional clamp of the output (both or neither; default: polynomial range +/- 10 %)
    Raises ValueError with a readable message if the channel is unusable.
    """
    form = spec.get("form", "raw")
    if form not in ("raw", "normalized"):
        raise ValueError(f"'form' must be 'raw' or 'normalized', not {form!r}")
    for key in ("a_min", "a_max"):
        if key not in spec:
            raise ValueError(f"missing '{key}'")
    coeffs = spec.get("c")
    if not isinstance(coeffs, list) or not 1 <= len(coeffs) <= 4:
        raise ValueError("'c' must be a list of 1 to 4 coefficients")
    try:
        a_min, a_max = float(spec["a_min"]), float(spec["a_max"])
        p = [float(v) for v in coeffs] + [0.0] * (4 - len(coeffs))
        y_given = ("y_min" in spec, "y_max" in spec)
        y_lo = float(spec["y_min"]) if y_given[0] else None
        y_hi = float(spec["y_max"]) if y_given[1] else None
    except (TypeError, ValueError):
        raise ValueError("a_min, a_max, y_min, y_max and the coefficients must be numbers")
    if not all(math.isfinite(v) for v in [a_min, a_max, *p]):
        raise ValueError("values must be finite numbers")
    if not a_max > a_min:
        raise ValueError("a_max must be greater than a_min")
    if y_given[0] != y_given[1]:
        raise ValueError("give both y_min and y_max, or neither")

    span = a_max - a_min
    if form == "raw":
        # substitute a = a_min + span * x and expand: c_j = sum_k p_k * C(k,j) * a_min^(k-j) * span^j
        c = [sum(p[k] * math.comb(k, j) * a_min ** (k - j) * span ** j for k in range(j, 4)) for j in range(4)]
    else:
        c = p

    if y_lo is None:
        ys = [_poly(c, k / 64.0) for k in range(65)]
        lo, hi = min(ys), max(ys)
        pad = 0.1 * max(hi - lo, 1.0)
        y_lo, y_hi = lo - pad, hi + pad

    cq = []
    for j, v in enumerate(c):
        q = int(round(v * 100 * ONE))
        if not I32_MIN <= q <= I32_MAX:
            raise ValueError(f"coefficient c{j} = {v:.3f} (after conversion) does not fit the Q16.16 format "
                             "(max about +/-327 in the normalized form): check the input range and the coefficients")
        cq.append(q)
    ch = {"a_min": int(round(a_min * 100)), "a_max": int(round(a_max * 100)),
          "y_min": int(math.floor(y_lo * 100)), "y_max": int(math.ceil(y_hi * 100)), "c": cq}
    if not all(I32_MIN <= ch[k] <= I32_MAX for k in ("a_min", "a_max", "y_min", "y_max")):
        raise ValueError("a_min/a_max/y_min/y_max are too large")
    err = validate_channel(ch)
    if err is not None:
        raise ValueError(f"rejected by the firmware checks: {ERRORS[err]}")
    return ch


def load_calibration(path: str):
    """Reads a calibration file (JSON) and returns the two channels as firmware integers."""
    import json

    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        raise ValueError(f"cannot read {path}: {e}")
    chans = data.get("channels") if isinstance(data, dict) else None
    if not isinstance(chans, list) or len(chans) != CAL_CHANNELS:
        raise ValueError(f"{path}: 'channels' must be a list of {CAL_CHANNELS} entries (temperature, humidity)")
    out = []
    for i, spec in enumerate(chans):
        try:
            out.append(channel_from_spec(spec))
        except ValueError as e:
            raise ValueError(f"{path}: channel {i} ({CHANNEL_LABELS[i]}): {e}")
    return out
