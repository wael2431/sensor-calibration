# calib_sensor_node

**A test bench for calibrating low-cost sensors with different algorithms, running in real time on microcontrollers.**

Cheap sensors are often biased, non-linear or sensitive to other quantities. A better sensor can be used as a
reference to learn a correction, and the corrected value can then be computed on the device itself. This project
provides the whole loop (collect data, fit a model, send it to the device, run it, compare it with the reference)
so that different calibration algorithms can be tried on the same footing. Its goal is to show, and to inspire,
that a poor sensor can be improved by calibration.

The example in this repository estimates what a **BME280** (reference) would report from a **DHT11** (low-cost
sensor), for temperature and humidity.

## How it works

```
 1. COLLECT         2. FIT (off-device)          3. DEPLOY               4. RUN (on the device)
 cheap sensor  +    any algorithm you like  -->  a compact, validated    cheap sensor only:
 reference sensor   (PC, Python, notebooks...)   model sent to the MCU   reference = f(cheap)
 -> CSV file                                     and saved in flash      compared with the reference
```

The device only *evaluates* a model; learning it is done on a PC, so the fitting code is deliberately not part of
this project. Once a calibration is saved in flash, the reference sensor can be unplugged.

## Algorithms

The aim is to compare approaches. Only the first one is implemented so far.

| Algorithm | Status | Needs on the device |
|---|---|---|
| **Polynomial** (degree <= 3) | implemented | integer arithmetic only |
| **Symbolic regression** | to be tested | a polynomial approximation of the found formula today; the formula itself needs floating point or a CORDIC unit if it uses `sin`, `exp`, `log`... |
| **PINN** (physics-informed neural network) | to be tested | a small neural-network runtime: FPU, or an NPU for larger models |
| **KAN** (Kolmogorov-Arnold network) | to be tested | spline evaluation, possibly turned into lookup tables; an FPU or NPU for larger networks |

## Why a polynomial first

The first target is the **ESP32-C6**, which has no floating-point unit and no DSP or neural accelerator. A
low-degree polynomial in fixed-point arithmetic is cheap there (a few multiplications per value) and easy to
validate before it is applied. This is a choice made for today's hardware, not a limit of the approach: other
hardware allows other models.

| Hardware feature | What it makes possible |
|---|---|
| No FPU, no accelerator (ESP32-C6) | fixed-point polynomial, lookup tables |
| FPU | floating-point formulas from symbolic regression, small float networks |
| CORDIC unit (found on some MCUs) | formulas with trigonometric, logarithmic or exponential terms, in hardware |
| DSP instructions | faster multiply-accumulate: bigger polynomials, spline or network layers |
| NPU | neural-network inference: PINN, KAN, larger models |

## Architecture

The firmware runs on **FreeRTOS** (through ESP-IDF) as three tasks connected by queues:

```
sensors --> [acquisition task] --queue--> [processing task] --queue--> [communication task] --> UART / BLE
                                                  ^                             |
                                          active calibration  <--  new calibration received, validated, saved
```

- **Acquisition**: reads the sensors at a fixed period.
- **Processing**: evaluates the calibration model on each sample.
- **Communication**: sends the measurements, receives calibrations and answers ACK or NACK.
- A calibration is checked (a small Rust library does the validation), saved to non-volatile memory, and only then
  replaces the active one. A bad calibration is rejected and the previous one keeps running.
- The protocol, the model evaluator and the BME280 compensation are plain C with no ESP-IDF calls, which keeps the
  core portable. Sensor drivers and the transport (UART, optional BLE) are the parts to adapt for another board.

## Quick start

Build and flash (ESP-IDF 5.2 or newer, Rust with the `riscv32imac-unknown-none-elf` target):
```
idf.py set-target esp32c6
idf.py menuconfig          # sampling period, sensors, UART / BLE
idf.py build flash monitor
pip install -r tools/host_sender/requirements.txt
```
Default wiring: DHT11 on GPIO10 (with a 4.7-10 kOhm pull-up), BME280 on I2C (SDA GPIO18, SCL GPIO21),.

Two commands (use your own serial port, for example `COM3` on Windows):
```
# 1. Read both sensors and save them to a CSV file (Ctrl+C to stop)
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 collect --csv data.csv

# 2. Send a calibration, then show both sensors and the calculated value (add --csv to also save them)
python tools/host_sender/host_sender.py -p /dev/ttyUSB0 run --cal calibration.json
```
`collect` still works if one sensor is missing (the missing values are left empty). Both commands need the
firmware's "Collect mode" on, which is the default; turn it off for production so that only the cheap sensor is read.

The calibration is a small JSON file written by your own fitting code, in physical units, temperature first and
humidity second:
```json
{ "channels": [
    { "a_min": 0.0,  "a_max": 50.0, "c": [-0.30, 1.02] },
    { "a_min": 20.0, "a_max": 90.0, "c": [-3.0, 1.04] }
] }
```
`c` holds the polynomial coefficients (`y = c0 + c1*a + c2*a^2 + c3*a^3`, `a` being the cheap sensor's value) and
`a_min`, `a_max` is the input range the model is valid for. See `tools/host_sender/calibration.example.json`
for all options.

## What calibration can and cannot do

Calibration can remove a systematic bias, correct a gain error or a non-linearity, and compensate for the
influence of another quantity if the model has it as an input. It cannot recover information the cheap sensor never
measured: the DHT11 only reports whole degrees and whole percent, so some error remains whatever the algorithm.

## Status

- Verified on a PC with unit tests and with both commands run end to end against a simulated device
  (`./tests/run_tests.sh`).
- The firmware build and the behaviour on real hardware (sensor timing, flash storage, BLE) still have to be validated.
- The DHT11 limits the sampling to about one measurement every 2 seconds.
