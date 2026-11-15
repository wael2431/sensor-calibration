# Modular sensor calibration

## Separation of responsibilities

- `calibtools.py`: protocol frames, calibration blob encoding/decoding, fixed-point evaluation, firmware-compatible channel validation. It contains no fitting algorithm.
- `calibration_method_polyfit.py`: the current NumPy polynomial fitting implementation, extracted into a replaceable method module.
- `host_sender.py`: collects CSV data, calls a chosen method to create channel dictionaries, writes `cal.json`, and sends that JSON calibration to the device.

## Use the current method

```bash
python host_sender.py fit --csv data.csv --out cal.json --method polyfit --degree 3
python host_sender.py -p /dev/ttyUSB0 send --cal cal.json --revision 1
```

`fit` is offline and does not need a serial port. `send` uses the same JSON channel format and firmware calibration blob as before.

## Add a new method

Create a file next to `host_sender.py` named `calibration_method_<name>.py`. It must expose:

```python
def fit_channel(a, ref, degree=3, margin=0.05, holdout=4):
    # Fit your method and return (channel_dict, stats_dict)
    ...
```

Then select it using `--method <name>`. For example, `--method symbolic` loads `calibration_method_symbolic.py`.

The method must return a channel dictionary in the firmware's existing format:

```python
{
    "a_min": int,  # input lower bound, sensor units (0.01 °C / 0.01 %RH)
    "a_max": int,  # input upper bound
    "y_min": int,  # output clamp lower bound
    "y_max": int,  # output clamp upper bound
    "c": [c0, c1, c2, c3],  # four signed int32 Q16.16 coefficients
}
```

These four coefficients still encode the firmware's cubic-or-lower polynomial using Horner evaluation. A new method can change how the coefficients are found, but cannot return a general expression unless the firmware format/evaluator is changed too. `host_sender.py` validates the returned channel before saving it.

For consistent reporting, `stats_dict` should contain `rmse_test` and `rmse_test_identity`; the current method also returns `n`, `degree`, `rmse_train`, and `max_abs_test`.

Keep these four files together in the same directory. NumPy is required by the default `polyfit` method.
