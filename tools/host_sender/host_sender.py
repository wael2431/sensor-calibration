#!/usr/bin/env python3
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
import struct
import sys
import time

import calibtools as ct

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
    except KeyboardInterrupt:
        pass
    return 0


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


if __name__ == "__main__":
    sys.exit(main())
