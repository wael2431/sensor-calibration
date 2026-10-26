#!/usr/bin/env python3
"""PC tool: collect (DHT11, BME280) pairs, fit BME280 = f(DHT11), send the calibration.

Workflow:
    1) python host_sender.py -p /dev/ttyUSB0 collect --csv data.csv --duration 3600
    2) python host_sender.py fit --csv data.csv --out cal.json --degree 3        (no serial port needed)
    3) python host_sender.py -p /dev/ttyUSB0 send --cal cal.json --revision 1    (waits for ACK/NACK)
    4) python host_sender.py -p /dev/ttyUSB0 listen                              (compares estimate and BME280)

Others: `send --neutral` (restores the identity polynomial), `send --bad-crc` (must give a NACK).
"""
import argparse
import csv
import importlib
import re
import json
import struct
import sys
import time

import calibtools as ct


class FrameReader:
    def __init__(self, ser):
        self.ser, self.buf = ser, bytearray()

    def frames(self, timeout):
        """Generator of valid (type, seq, payload); `timeout` in seconds, no limit if None."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while deadline is None or time.monotonic() < deadline:
            for b in self.ser.read(256):
                if b != 0:
                    self.buf.append(b)
                    continue
                block, self.buf = bytes(self.buf), bytearray()
                if not block:
                    continue
                try:
                    yield ct.parse_frame(block)
                except ValueError as e:
                    print(f"[frame ignored: {e}]", file=sys.stderr)


def fmt(vals, unit):
    return " ".join("   n/a " if v is None else f"{v / 100:7.2f}" for v in vals) + f" {unit}"


def cmd_listen(reader, args):
    try:
        for mtype, seq, payload in reader.frames(None):
            if mtype != ct.MSG_MEASURE:
                continue
            m = ct.decode_measure(payload)
            line = (f"t={m['ts']:9.2f}s rev={m['rev']}  DHT11 T/H: {fmt(m['a'], '')}"
                    f"| BME280 estimated: {fmt(m['est'], '')}")
            if any(v is not None for v in m["ref"]):
                line += f"| BME280 measured: {fmt(m['ref'], '')}"
            if any(m["oor"]):
                line += "  [outside calibration range]"
            print(line)
    except KeyboardInterrupt:
        pass
    return 0


def cmd_collect(reader, args):
    n = 0
    t_end = None if args.duration is None else time.monotonic() + args.duration
    with open(args.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_s", "a_temp", "a_hum", "ref_temp", "ref_hum", "est_temp", "est_hum"])
        try:
            for mtype, seq, payload in reader.frames(None):
                if t_end is not None and time.monotonic() > t_end:
                    break
                if mtype != ct.MSG_MEASURE:
                    continue
                m = ct.decode_measure(payload)
                if m["a_valid"] != 3 or m["ref_valid"] != 3:     # complete pairs (DHT11 and BME280)
                    continue
                w.writerow([f"{m['ts']:.3f}", *m["a"], *m["ref"], *[("" if v is None else v) for v in m["est"]]])
                f.flush()
                n += 1
                print(f"\r{n} pairs collected", end="", file=sys.stderr)
        except KeyboardInterrupt:
            pass
    print(f"\n{n} pairs -> {args.csv}", file=sys.stderr)
    return 0


def load_calibration_method(name):
    """Load calibration_method_<name>.py, which must expose fit_channel()."""
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
        raise ValueError("method name must contain only letters, digits, and underscores")
    module_name = f"calibration_method_{name}"
    try:
        module = importlib.import_module(module_name)
    except ModuleNotFoundError as exc:
        if exc.name == module_name:
            raise ValueError(
                f"calibration method '{name}' not found; expected a file named {module_name}.py"
            ) from exc
        raise
    if not callable(getattr(module, "fit_channel", None)):
        raise ValueError(f"{module_name}.py must expose a callable fit_channel()")
    return module


def validate_method_channel(ch):
    """Check a plugin result before saving it for later firmware transmission."""
    required = ("a_min", "a_max", "y_min", "y_max", "c")
    if not isinstance(ch, dict) or any(key not in ch for key in required):
        raise ValueError("fit method returned an invalid channel dictionary")
    if not isinstance(ch["c"], (list, tuple)) or len(ch["c"]) != 4:
        raise ValueError("fit method must return exactly four Q16.16 coefficients")
    for key in required[:-1]:
        if not isinstance(ch[key], int) or isinstance(ch[key], bool):
            raise ValueError(f"fit method field {key!r} must be an integer")
        if not ct.I32_MIN <= ch[key] <= ct.I32_MAX:
            raise ValueError(f"fit method field {key!r} does not fit int32")
    for index, coefficient in enumerate(ch["c"]):
        if not isinstance(coefficient, int) or isinstance(coefficient, bool):
            raise ValueError(f"coefficient c{index} must be an integer in Q16.16")
        if not ct.I32_MIN <= coefficient <= ct.I32_MAX:
            raise ValueError(f"coefficient c{index} does not fit int32 Q16.16")
    error = ct.validate_channel(ch)
    if error is not None:
        raise ValueError(f"calibration rejected by firmware validation: {ct.ERRORS[error]}")


def _format_stat(stats, key):
    value = stats.get(key)
    return f"{value:8.2f}" if isinstance(value, (int, float)) else "     n/a"


def cmd_fit(args, method):
    with open(args.csv) as fh:
        rows = list(csv.DictReader(fh))
    channels, stats = [], []
    for i, (ca, cr) in enumerate([("a_temp", "ref_temp"), ("a_hum", "ref_hum")]):
        a = [int(r[ca]) for r in rows]
        ref = [int(r[cr]) for r in rows]
        print(f"\n== {ct.CH_NAMES[i]}  ({len(a)} points)")
        for deg in (1, 2, 3):
            try:
                _, s = method.fit_channel(a, ref, degree=deg, margin=args.margin, holdout=args.holdout)
                mark = " <- selected" if deg == args.degree else ""
                test_rmse = _format_stat(s, "rmse_test")
                identity_rmse = _format_stat(s, "rmse_test_identity")
                print(f"  degree {deg}: test RMSE {test_rmse}  (uncalibrated: {identity_rmse}){mark}")
            except ValueError as e:
                print(f"  degree {deg}: {e}")
        ch, s = method.fit_channel(a, ref, degree=args.degree, margin=args.margin, holdout=args.holdout)
        validate_method_channel(ch)
        channels.append(ch)
        stats.append(s)
        print(f"  input range [{ch['a_min']}, {ch['a_max']}]  output bounds [{ch['y_min']}, {ch['y_max']}]")
    with open(args.out, "w") as fh:
        json.dump({"channels": channels, "stats": stats}, fh, indent=2)
    print(f"\ncalibration -> {args.out}")
    return 0


def cmd_send(reader, ser, args):
    if args.neutral:
        channels = ct.neutral_channels()
    else:
        with open(args.cal) as fh:
            channels = json.load(fh)["channels"]
    blob = ct.build_calibration(args.revision, channels, args.bad_crc)
    ser.reset_input_buffer()
    ser.write(ct.build_frame(ct.MSG_CALIB, 1, blob))
    for mtype, seq, payload in reader.frames(args.timeout):
        if mtype == ct.MSG_ACK:
            _, _, rev = struct.unpack("<BBI", payload)
            print(f"ACK: calibration active, revision {rev}")
            return 0
        if mtype == ct.MSG_NACK:
            _, _, err = struct.unpack("<BBi", payload)
            print(f"NACK: error {err} ({ct.ERRORS.get(err, '?')})")
            return 1
    print("No reply (timeout)")
    return 2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("listen", help="show DHT11, BME280 estimate and (in collect mode) the measured BME280")

    c = sub.add_parser("collect", help="record (DHT11, BME280) pairs to a CSV file: a = DHT11, ref = BME280")
    c.add_argument("--csv", required=True)
    c.add_argument("--duration", type=float, help="duration in seconds (default: until Ctrl+C)")

    f = sub.add_parser("fit", help="fit calibration using a selected method (offline, no serial port)")
    f.add_argument("--csv", required=True)
    f.add_argument("--out", default="cal.json")
    f.add_argument("--degree", type=int, default=3, help="maximum polynomial degree for methods that use it")
    f.add_argument("--method", default="polyfit", help="load calibration_method_<name>.py (default: polyfit)")
    f.add_argument("--holdout", type=int, default=4, help="keep every Nth sample for testing (default: 4)")
    f.add_argument("--margin", type=float, default=0.05, help="margin on the input range (default 5 %%)")

    s = sub.add_parser("send", help="send a calibration and wait for ACK/NACK")
    s.add_argument("--cal", help="JSON file produced by `fit`")
    s.add_argument("--neutral", action="store_true", help="identity polynomial")
    s.add_argument("--revision", type=int, required=True)
    s.add_argument("--bad-crc", action="store_true", help="corrupt the CRC (NACK test)")
    s.add_argument("--timeout", type=float, default=3.0)

    args = ap.parse_args()
    if args.cmd == "fit":
        try:
            method = load_calibration_method(args.method)
        except (ImportError, ValueError) as exc:
            ap.error(str(exc))
        return cmd_fit(args, method)
    if args.cmd == "send" and not (args.cal or args.neutral):
        ap.error("send: --cal or --neutral is required")
    if not args.port:
        ap.error("-p/--port is required for this command")

    import serial  # late import: `fit` works without pyserial
    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        reader = FrameReader(ser)
        if args.cmd == "listen":
            return cmd_listen(reader, args)
        if args.cmd == "collect":
            return cmd_collect(reader, args)
        return cmd_send(reader, ser, args)


if __name__ == "__main__":
    sys.exit(main())
