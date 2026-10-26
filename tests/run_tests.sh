#!/usr/bin/env bash
# Runs every test that can run on a PC (no board, no ESP-IDF): Rust, Python, C.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cd "$ROOT"

echo "== Rust: cargo test"
(cd components/calib_rs && cargo test --quiet 2>&1 | tail -4)

echo "== Rust: static library for the PC"
(cd components/calib_rs && cargo build --release --quiet)

echo "== Test vectors (Python)"
python3 tests/gen_vectors.py "$TMP/vectors.txt" "$TMP/blob.bin"

echo "== C on the PC (gcc)"
gcc -Wall -Wextra -O1 \
    -Icomponents/protocol -Icomponents/calib_rs/include -Icomponents/calib_store -Icomponents/sensors \
    tests/host/test_host.c components/protocol/cobs.c components/protocol/protocol.c \
    components/calib_store/calib_eval.c components/sensors/bme280_comp.c \
    components/calib_rs/target/release/libcalib_rs.a -ldl -lpthread -o "$TMP/test_host"
"$TMP/test_host" "$TMP/vectors.txt" "$TMP/blob.bin" "$TMP/measure.bin"

echo "== Python: unittest"
MEASURE_BIN="$TMP/measure.bin" python3 tests/test_python.py 2>&1 | tail -8

rm -rf components/calib_rs/target components/calib_rs/Cargo.lock
echo; echo "ALL TESTS PASS"
