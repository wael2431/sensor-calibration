# calib_sensor_node: BME280 = f(DHT11) on an ESP32-C6 (ESP-IDF >= 5.2 + Rust)

The firmware reads the **DHT11** and derives the value the **BME280** would report, with a degree-3 polynomial:

    x = clamp((a - a_min) / (a_max - a_min), 0, 1)       a = DHT11 value
    y = c0 + c1*x + c2*x^2 + c3*x^3                      Horner, Q16.16, 64-bit intermediates
    y = clamp(y, y_min, y_max)                           y = calculated BME280 value

Two channels: 0 = temperature, 1 = humidity. The coefficients come from you: **finding them is not part of this
project** (symbolic regression or any other method). You write them to a small JSON file and the tool sends them.

| Part | Role |
|---|---|
| PC (`tools/host_sender`) | the two commands below: collect data, send a calibration |
| Rust (`calib_rs`) | **validation only**: length, magic, version, CRC32, ranges, polynomial that does not diverge |
| C (`calib_store`) | NVS storage, switching the active calibration, **Horner evaluation** (`calib_eval.c`) |

## The two commands

Both need the firmware built with **Collect mode** on (default), so that it sends both sensors.

### 1. `collect`: read the two sensors and write a CSV file
```
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 collect --csv data.csv
```
Stop with Ctrl+C, or use `--duration SECONDS`. Rows are **appended** if the file exists (`--overwrite` starts it
again). A row is written only when both sensors were read. Values are in degC and %RH:
```
timestamp_s,dht11_temp_c,dht11_hum_pct,bme280_temp_c,bme280_hum_pct
0.200,20.20,50.40,20.70,49.40
```
`timestamp_s` is the time since the device booted. The DHT11 can be read once per second at most (default period: 2 s),
so expect one row every 2 s. Vary temperature and humidity during the collection to cover the range you need.

### 2. `run`: update the calibration, then show both sensors and the calculated value
```
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 run --cal calibration.json
```
It checks the file, sends the calibration, waits for the answer, then prints until Ctrl+C:
```
ACK: calibration active and saved (revision 1)
t=  1.40s rev=1 | DHT11  T= 21.40 H= 52.80 | BME280 T= 21.90 H= 51.80 | calculated T= 21.53 H= 51.91
```
- `ACK`: the device validated the calibration, saved it to flash and uses it now. It survives a reboot.
- `NACK`: rejected, with the reason (for example CRC, RANGE, DIVERGES); the previous calibration stays active.
- `--revision N` sets the revision number (default: current Unix time). It is sent back with every measurement.
- A DHT11 value outside `[a_min, a_max]` is flagged `[DHT11 outside calibrated range]` and the input is clamped.

## Calibration file (what your own fitting script should write)

JSON with exactly two channels, **temperature first, then humidity**, in physical units (degC, %RH):
```json
{
  "channels": [
    { "form": "raw", "a_min": 0.0,  "a_max": 50.0, "c": [-0.30, 1.02] },
    { "form": "raw", "a_min": 20.0, "a_max": 90.0, "c": [-3.0, 1.04], "y_min": 0.0, "y_max": 100.0 }
  ]
}
```
See `tools/host_sender/calibration.example.json`.

| Key | Meaning |
|---|---|
| `c` | 1 to 4 coefficients `c0..c3` (missing ones are 0) |
| `form` | `"raw"` (default): `y = c0 + c1*a + c2*a^2 + c3*a^3`, with `a` the DHT11 value in degC or %RH. `"normalized"`: same polynomial in `x = (a - a_min) / (a_max - a_min)` |
| `a_min`, `a_max` | **required**: the DHT11 range the polynomial is valid for (use the range of your collected data) |
| `y_min`, `y_max` | optional output limits, both or neither (default: the polynomial's range over `[a_min, a_max]` +/- 10 %) |

The tool converts to the firmware's fixed-point format and applies the same checks as the firmware, so a bad file is
reported on the PC before anything is sent. Only **polynomials of degree <= 3** are supported: a formula with
`sin`, `exp` or `log` has to be approximated by a cubic over `[a_min, a_max]` first. In the normalized form the
coefficients must stay within about +/-327 (a limit of the Q16.16 format).

## Build and flash
```
rustup target add riscv32imac-unknown-none-elf     # once
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py menuconfig                                   # Application: period, sensors, UART/BLE
idf.py build flash monitor
pip install -r tools/host_sender/requirements.txt   # PC tool (pyserial)
```
BLE in addition to UART: `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" reconfigure build`
(the PC tool talks UART).

Default wiring: DHT11 on GPIO10 (+ 4.7-10 kOhm pull-up to 3V3); BME280 on I2C SDA=GPIO18 SCL=GPIO19 (address 0x76);
UART1 TX=GPIO6 RX=GPIO7, 115200 8N1 to a USB-serial adapter (TX<->RX crossed, common GND).

**Production:** in `idf.py menuconfig` -> Application -> Sensors, disable "Collect mode". Only the DHT11 is read and
the BME280 can be unplugged; the calculated value is still sent (the BME280 columns of `run` then show `n/a`).

## Tests (on a PC, no board and no ESP-IDF needed)
```
./tests/run_tests.sh      # Rust (cargo test), Python, C (gcc), cross-checks, and both commands end to end
```
The end-to-end test runs `collect` and `run` against a simulated device on a pseudo-terminal (Linux/macOS, needs
pyserial; skipped otherwise).

## Limitations
- **The DHT11 sets the pace**: at most one read per second, ~25 ms blocking (`APP_ACQ_PERIOD_MS`, default 2 s, minimum 1 s).
- **The input is coarse**: the DHT11 only reports whole values (1 degC, 1 %RH), accuracy about +/-2 degC and +/-5 %RH.
  A polynomial can correct a systematic bias or gain, not detail the DHT11 never measured.
- The polynomial is only valid over the `[a_min, a_max]` you give: outside it the input is clamped.
- Reading the DHT11 masks interrupts for ~4 ms (bit-banging): keep an eye on this if BLE is active.
- Not verified here: ESP-IDF build and Rust cross-compilation, DHT11 timing, BME280 and BLE on hardware.
  The BME280 temperature compensation is checked against the Bosch datasheet example; humidity follows
  the Bosch reference formula but has no numeric reference value.
- If the linker reports duplicate symbols (compiler_builtins / libgcc): add
  `-Wl,--allow-multiple-definition` via `target_link_options` in `main/CMakeLists.txt`.
