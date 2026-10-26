"""PC-side protocol and firmware-compatible calibration utilities (no serial port).

Model (identical to the firmware): for each channel, the BME280 value is estimated from the DHT11:
    x = clamp((a - a_min) / (a_max - a_min), 0, 1)      Q16.16
    y = c0 + x*(c1 + x*(c2 + x*c3))                     Horner, c[k] in Q16.16
    y = clamp(round(y), y_min, y_max)
Units: 0.01 °C (channel 0, temperature) and 0.01 %RH (channel 1, humidity).
"""
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
CH_NAMES = ["temperature (0.01 °C)", "humidity (0.01 %RH)"]


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

