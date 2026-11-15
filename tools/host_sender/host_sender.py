#!/usr/bin/env python3
<<<<<<< HEAD
"""PC tool for the DHT11 -> BME280 calibration node. Two commands:

  collect   Read the two sensors (DHT11 and BME280) and append them to a CSV file.
            Works with only one sensor connected: the missing sensor's cells are left empty.
            python host_sender.py -p /dev/ttyUSB0 collect --csv data.csv

  run       Send a calibration, wait for ACK/NACK, then show the two sensor values and the
            value calculated with the calibration, until Ctrl+C.
            python host_sender.py -p /dev/ttyUSB0 run --cal calibration.json

The firmware must be built with "Collect mode" enabled (the default) so that it sends both sensors.
CSV values are in degC and %RH. Calibration file format: see README.md / calibration.example.json.
"""
import argparse
import csv
import os
=======
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
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84
import struct
import sys
import time

import calibtools as ct

<<<<<<< HEAD
CSV_HEADER = ["timestamp_s", "dht11_temp_c", "dht11_hum_pct", "bme280_temp_c", "bme280_hum_pct"]
RUN_CSV_HEADER = ["timestamp_s", "revision", "dht11_temp_c", "dht11_hum_pct", "bme280_temp_c", "bme280_hum_pct",
                  "calculated_temp_c", "calculated_hum_pct", "out_of_range"]
ALL_CH = 0x03


class FrameReader:
    """Reassembles COBS frames from a byte stream. Never drops bytes between two calls."""

    def __init__(self, ser):
        self.ser = ser
        self.pending = bytearray()

    def next_frame(self, deadline=None):
        """Returns the next valid (type, seq, payload), or None once `deadline` (time.monotonic) passes."""
        while True:
            end = self.pending.find(0)
            if end >= 0:
                block = bytes(self.pending[:end])
                del self.pending[:end + 1]
                if not block:
                    continue
                try:
                    return ct.parse_frame(block)
                except ValueError as e:
                    print(f"[frame ignored: {e}]", file=sys.stderr)
                    continue
            if len(self.pending) > 4096:          # garbage without delimiter: resynchronize
                self.pending.clear()
            if deadline is not None and time.monotonic() > deadline:
                return None
            self.pending += self.ser.read(256)


def csv_row(m, both_only=False):
    """CSV row for one MEASURE (degC / %RH).

    A value that was not read (sensor absent or read failed) is left as an empty cell.
    Returns None if nothing was read at all, or, with `both_only`, unless both sensors were read.
    """
    if both_only and (m["a_valid"] != ALL_CH or m["ref_valid"] != ALL_CH):
        return None
    values = m["a"] + m["ref"]                      # None for values that were not read
    if all(v is None for v in values):
        return None
    return [f"{m['ts']:.3f}"] + ["" if v is None else f"{v / 100:.2f}" for v in values]

def run_csv_row(m):
    """CSV row for `run`: DHT11, BME280 and calculated values (degC / %RH), revision and out-of-range flags.

    A value that is not available leaves an empty cell. Returns None if the sample holds no value at all."""
    vals = m["a"] + m["ref"] + m["est"]
    if all(v is None for v in vals):
        return None
    flags = "".join(n for n, oor in zip("TH", m["oor"]) if oor)      # "T", "H", "TH" or ""
    return [f"{m['ts']:.3f}", str(m["rev"])] + ["" if v is None else f"{v / 100:.2f}" for v in vals] + [flags]


def open_run_csv(path, overwrite=False):
    """Opens the CSV file for `run` and writes the header if the file is new. Returns the open file.

    Raises ValueError if the file already exists with other columns (for example a `collect` file)."""
    new_file = overwrite or not os.path.exists(path) or os.path.getsize(path) == 0
    if not new_file:
        with open(path, newline="") as f:
            if next(csv.reader(f), []) != RUN_CSV_HEADER:
                raise ValueError(f"{path} already exists with other columns: use another file or --overwrite")
    f = open(path, "w" if overwrite else "a", newline="")
    if new_file:
        csv.writer(f).writerow(RUN_CSV_HEADER)
        f.flush()
    return f

def _fmt(v):
    return "   n/a" if v is None else f"{v / 100:6.2f}"


def format_measure(m):
    """One display line: DHT11 (input), BME280 (measured), calculated BME280 (estimate)."""
    line = (f"t={m['ts']:9.2f}s rev={m['rev']} | DHT11  T={_fmt(m['a'][0])} H={_fmt(m['a'][1])}"
            f" | BME280 T={_fmt(m['ref'][0])} H={_fmt(m['ref'][1])}"
            f" | calculated T={_fmt(m['est'][0])} H={_fmt(m['est'][1])}")
    flags = "".join(n for n, oor in zip("TH", m["oor"]) if oor)
    if flags:
        line += f"  [DHT11 outside calibrated range: {flags}]"
    return line


def cmd_collect(ser, args):
    reader = FrameReader(ser)
    mode = "w" if args.overwrite else "a"
    new_file = args.overwrite or not os.path.exists(args.csv) or os.path.getsize(args.csv) == 0
    t_end = None if args.duration is None else time.monotonic() + args.duration
    n = n_dht = n_bme = 0
    with open(args.csv, mode, newline="") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow(CSV_HEADER)
        try:
            while True:
                frame = reader.next_frame(t_end)
                if frame is None:
                    break
                if frame[0] != ct.MSG_MEASURE:
                    continue
                row = csv_row(ct.decode_measure(frame[2]), args.both_only)
                if row is None:                    # nothing read on this sample (or --both-only and one is missing)
                    continue
                w.writerow(row)
                f.flush()
                n += 1
                n_dht += row[1] != ""
                n_bme += row[3] != ""
                print(f"\r{n} rows -> {args.csv}", end="", file=sys.stderr)
        except KeyboardInterrupt:
            pass
    print(f"\n{n} rows written to {args.csv} (with a DHT11 value: {n_dht}, with a BME280 value: {n_bme})",
          file=sys.stderr)
    for name, count in (("DHT11", n_dht), ("BME280", n_bme)):
        if n and count == 0:
            print(f"Warning: no {name} value was received: is the {name} connected and working?", file=sys.stderr)
    return 0


def cmd_run(ser, channels, revision, args, csv_file=None):
    reader = FrameReader(ser)
    ser.reset_input_buffer()
    ser.write(ct.build_frame(ct.MSG_CALIB, 1, ct.build_calibration(revision, channels)))

    deadline = time.monotonic() + args.timeout
    while True:                                    # wait for the answer, ignoring measurements meanwhile
        frame = reader.next_frame(deadline)
        if frame is None:
            print("No reply from the device (timeout)")
            return 2
        mtype, _, payload = frame
        if mtype == ct.MSG_ACK:
            _, _, rev = struct.unpack("<BBI", payload)
            print(f"ACK: calibration active and saved (revision {rev})")
            break
        if mtype == ct.MSG_NACK:
            _, _, err = struct.unpack("<BBi", payload)
            print(f"NACK: calibration rejected, error {err} ({ct.ERRORS.get(err, '?')})")
            return 1

    writer = csv.writer(csv_file) if csv_file else None
    if writer:
        print(f"Saving every measurement to {csv_file.name}")

    try:
        while True:
            frame = reader.next_frame(None)
            if frame[0] == ct.MSG_MEASURE:
                m = ct.decode_measure(frame[2])
                print(format_measure(m))
                row = run_csv_row(m) if writer else None
                if row:
                    writer.writerow(row)
                    csv_file.flush()
=======

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
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84
    except KeyboardInterrupt:
        pass
    return 0


<<<<<<< HEAD
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True, help="serial port, e.g. /dev/ttyUSB0 or COM3")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("collect", help="read the two sensors and write them to a CSV file")
    c.add_argument("--csv", required=True, help="output file (rows are appended if it already exists)")
    c.add_argument("--overwrite", action="store_true", help="start the file again instead of appending")
    c.add_argument("--both-only", action="store_true",
                   help="only write rows where both sensors were read (default: write what was read, "
                        "leaving empty cells)")
    c.add_argument("--duration", type=float, help="duration in seconds (default: until Ctrl+C)")

    r = sub.add_parser("run", help="send a calibration, then show both sensors and the calculated value")
    r.add_argument("--cal", required=True, help="calibration file (JSON)")
    r.add_argument("--revision", type=int, help="revision number (default: current Unix time)")
    r.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for ACK/NACK (default 3)")
    r.add_argument("--csv", help="also save every measurement (both sensors and the calculated value) to this CSV file")
    r.add_argument("--overwrite", action="store_true", help="with --csv: start the file again instead of appending")
    args = ap.parse_args()

    channels = revision = csv_file = None
    if args.cmd == "run":                          # check the file before touching the serial port
        try:
            channels = ct.load_calibration(args.cal)
        except ValueError as e:
            print(f"Calibration file error: {e}", file=sys.stderr)
            return 3
        revision = args.revision if args.revision is not None else int(time.time())
        if not 0 <= revision < 2 ** 32:
            ap.error("--revision must fit in 32 bits")
        if args.csv:                               # also before the serial port: fail early on a bad path
            try:
                csv_file = open_run_csv(args.csv, args.overwrite)
            except (OSError, ValueError) as e:
                print(f"CSV file error: {e}", file=sys.stderr)
                return 3    

    import serial  # late import so the helper functions can be tested without pyserial
    try:
        with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
            if args.cmd == "collect":
                return cmd_collect(ser, args)
            return cmd_run(ser, channels, revision, args, csv_file)
    finally:
        if csv_file:
            csv_file.close()    
=======
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
>>>>>>> d39c724111eeb73e4dfc10bed5ba550955f2ee84


if __name__ == "__main__":
    sys.exit(main())
