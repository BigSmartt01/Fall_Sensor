# Quantization Calibration

[← Documentation index](README.md) · [TFLite inference](TFLITE_INFERENCE.md)

Use this when Python int8 accuracy is good but the ESP32 behaves differently, or when validating a newly exported model.

## Formulas

```
int8  = round(float / scale) + zero_point
float = (int8 - zero_point) * scale
```

Firmware loads scales from the model in `initializeInference()` — you usually **do not** hand-edit constants in `inference.cpp`.

## Read parameters from `.tflite`

```bash
cd firmware
python extract_quantization.py ../tests/three_classes/fall_cnn_qat_int8.tflite
```

Or in Python:

```python
import tensorflow as tf

i = tf.lite.Interpreter(model_path="tests/three_classes/fall_cnn_qat_int8.tflite")
i.allocate_tensors()
for d in i.get_input_details() + i.get_output_details():
    print(d["name"], d["shape"], d["quantization"])
```

Pass the **`.tflite`** file, not the `.h` header.

## Shapes

| Tensor | Shape |
|--------|--------|
| Input | `[1, 512, 1]` |
| Output | `[1, 1]` |

One input scale for the full window — not per-axis accel/gyro.

## Match training decisions

`train_qat.py` and firmware both use: fall if **int8 output > 0**. Do not add a separate 0.7 float threshold without retraining.

## Desktop sanity check

```python
import numpy as np
import tensorflow as tf

interpreter = tf.lite.Interpreter(
    model_path="tests/three_classes/fall_cnn_qat_int8.tflite"
)
interpreter.allocate_tensors()
inp = interpreter.get_input_details()[0]
out = interpreter.get_output_details()[0]
scale, zp = inp["quantization"]

X = np.fromfile("tests/three_classes/x_test_3", dtype=np.float32).reshape(-1, 512)
y = np.fromfile("tests/three_classes/y_test_3", dtype=np.uint8)

sample_int8 = (X[0].reshape(1, 512, 1) / scale + zp).astype(np.int8)
interpreter.set_tensor(inp["index"], sample_int8)
interpreter.invoke()
raw = interpreter.get_tensor(out["index"])[0, 0]
print("pred:", 1 if raw > 0 else 0, "label:", y[0])
```

## Device: clipping check

If many of the 512 int8 inputs are **±127**, fix `featureForInference()` (units / axis) before tweaking scales. See [TFLITE_INFERENCE.md](TFLITE_INFERENCE.md).

## Troubleshooting

| Symptom | Action |
|---------|--------|
| Inputs always ±127 | Align feature with `x_train_3`; verify input scale |
| NN always normal | Wait 10+ s for window; fix feature pipeline |
| Python OK, MCU bad | Preprocessing mismatch, not only quant params |
| `AllocateTensors()` fail | Increase `kTensorArenaSize` |

## Retrain / redeploy

1. `tests/three_classes/train_qat.py`
2. `tests/three_classes/tflite_to_header.py`
3. Rebuild firmware — [FIRMWARE_QUICKSTART.md](FIRMWARE_QUICKSTART.md)
