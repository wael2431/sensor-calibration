# calib_sensor_node: BME280 = f(DHT11), degree-3 polynomial (ESP32-C6, ESP-IDF >= 5.2 + Rust)

The firmware reads the **DHT11** and derives the value the **BME280** would report:

    x = clamp((a - a_min) / (a_max - a_min), 0, 1)       a = DHT11 value (0.01 °C or 0.01 %RH)
    y = c0 + x*(c1 + x*(c2 + x*c3))                      Horner, Q16.16, 64-bit intermediates
    y = clamp(y, y_min, y_max)                           y = estimated BME280 value

Two channels: 0 = temperature, 1 = humidity. The polynomial is computed **on the PC** and sent to the ESP32.

| Part | Role |
|---|---|
| PC (`tools/host_sender`) | collects (DHT11, BME280) pairs, fits the polynomial, sends the 80-byte blob |
| Rust (`calib_rs`) | **validation only**: length, magic, version, CRC32, ranges, polynomial that does not diverge |
| C (`calib_store`) | NVS storage, switching the active calibration, **Horner evaluation** (`calib_eval.c`) |

## Usage
```
# 1) Collect: BME280 plugged in (APP_COLLECT_REF=y, the default). Vary temperature and humidity.
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 collect --csv data.csv --duration 7200
# 2) Offline fit (compares degrees 1, 2 and 3 on held-out data)
python tools/host_sender/host_sender.py fit --csv data.csv --out cal.json --degree 3
# 3) Send: ACK = active and saved, NACK = rejected (error code shown)
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 send --cal cal.json --revision 1
# 4) Check: estimated vs measured BME280
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 listen
```
In production: `idf.py menuconfig` -> Application -> Sensors -> disable "Collect mode"; the BME280 can be unplugged.

## Build
```
rustup target add riscv32imac-unknown-none-elf     # once
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py menuconfig
idf.py build flash monitor
```
BLE in addition to UART: `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" reconfigure build`

Default wiring: DHT11 on GPIO10 (+ 4.7-10 kOhm pull-up to 3V3); BME280 on I2C SDA=GPIO18 SCL=GPIO19 (address 0x76);
UART1 TX=GPIO6 RX=GPIO7, 115200 8N1 to a USB-serial adapter (TX<->RX crossed, common GND).

## Tests (on a PC, no board and no ESP-IDF needed)
```
./tests/run_tests.sh      # Rust (cargo test), Python (unittest), C (gcc) and cross-checks between the three
pip install -r tools/host_sender/requirements.txt
```

## Limitations
- **The DHT11 sets the pace**: at most one read per second, ~25 ms blocking. Default period: 2 s
  (`APP_ACQ_PERIOD_MS`, minimum 1000 ms). Estimates are refreshed at that rate.
- **The input is coarse**: the DHT11 only reports whole values (1 °C, 1 %RH), accuracy ~±2 °C and ±5 %RH.
  The polynomial corrects a systematic bias or gain, not detail the DHT11 never measured:
  the remaining error is about the size of the quantization (~0.5 °C). On simulated data, degrees 1, 2 and 3
  give almost the same result; `fit` compares them so you can pick the simplest.
- The fit needs enough distinct input values (at least degree + 2): cover a wide range.
- The polynomial is only valid over the fitted input range: outside it, `x` is clamped and the measurement
  is flagged `est_oor`.
- Reading the DHT11 masks interrupts for ~4 ms (bit-banging): keep an eye on this if BLE is active.
- Not verified here: ESP-IDF build and Rust cross-compilation, DHT11 timing, BME280 and BLE on hardware.
  The BME280 temperature compensation is checked against the Bosch datasheet example; humidity follows
  the Bosch reference formula but has no numeric reference value.
- If the linker reports duplicate symbols (compiler_builtins / libgcc): add
  `-Wl,--allow-multiple-definition` via `target_link_options` in `main/CMakeLists.txt`.
