# Machine Learning & On-Device Inference

[← Project README](../README.md) · [Errors and fixes](ERRORS_AND_FIXES.md)

Hybrid fall detection: a **rule-based state machine** runs at 50 Hz; a **quantized 1D-CNN** (TFLite Micro) optionally confirms candidates in `STATE_POST_IMPACT_STILLNESS` when a 512-sample window is ready.

---

## Model

Trained with `tests/three_classes/train_qat.py`, exported as `fall_cnn_qat_int8.tflite`, embedded in `firmware/include/fall_model.h`.

| Property | Value |
|----------|--------|
| Size | ~17 KB flash |
| Input | `[1, 512, 1]` float32 → **int8** (one feature per timestep) |
| Output | `[1, 1]` sigmoid → **int8** |
| Labels | `y_train_3`: 0 = normal, 1 = fall |
| Decision | Fall if **int8 output > 0** (`predictedClass == 1`) |

Not a 6-DOF instant classifier. Needs **512 samples** (~10.24 s @ 50 Hz) before the NN can run.

Training windows (`x_train_3`) are float32, roughly **[-2.7, 1.9]**. Firmware default feature in `featureForInference()`:

```cpp
return s.accelY / 9.81f;  // g — must match offline preprocessing
```

---

## Firmware API

`firmware/include/inference.h`:

```cpp
constexpr int kInferenceWindowSize = 512;

struct InferenceOutput {
  float fallProbability;
  float normalProbability;
  uint8_t predictedClass;   // 1 = fall (int8 > 0)
};

bool initializeInference();
void pushInferenceSample(float feature);
bool inferenceWindowReady();
bool runInference(InferenceOutput& output);
```

Scale and zero_point are read from the model in `initializeInference()` — do not hand-edit quant constants in normal use.

### Runtime flow

```
each 50 Hz tick:
  pushInferenceSample(feature)
  if window full: runInference()
  POST_IMPACT_STILLNESS:
    ruleBasedFall && (!inferenceValid || predictedClass == 1)
```

If `initializeInference()` fails, rules alone can still confirm.

### Memory (typical)

| Resource | Size |
|----------|------|
| Tensor arena | 60 KB (`kTensorArenaSize` in `inference.cpp`) |
| Model (flash) | ~17 KB |
| Ring buffer | 2 KB |

If `AllocateTensors()` fails, increase `kTensorArenaSize`.

---

## Quantization

### Formulas

```
int8  = round(float / scale) + zero_point
float = (int8 - zero_point) * scale
```

### Inspect parameters (desktop)

```bash
cd firmware
python extract_quantization.py ../tests/three_classes/fall_cnn_qat_int8.tflite
```

Or:

```python
import tensorflow as tf

i = tf.lite.Interpreter(model_path="tests/three_classes/fall_cnn_qat_int8.tflite")
i.allocate_tensors()
for d in i.get_input_details() + i.get_output_details():
    print(d["name"], d["shape"], d["quantization"])
```

Pass the **`.tflite`** file, not the `.h` header. One input scale applies to the full 512-sample window.

### Desktop sanity check

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

### On-device debugging

| Symptom | Action |
|---------|--------|
| Many inputs clipped to ±127 | Fix `featureForInference()` (units/axis), not only scale |
| NN always normal | Wait ~10 s for window; align feature with `x_train_3` |
| Python OK, MCU bad | Preprocessing mismatch |
| `AllocateTensors()` fail | Increase tensor arena |

Do not use a 0.7 float probability threshold unless you retrain with that rule.

---

## Training pipeline (summary)

**Dataset:** SisFall-derived windows in `tests/three_classes/` (`x_train_3`, `y_train_3`, etc.).

**Architecture** (lightweight QAT CNN):

```
Input (512, 1) → Conv1D(8,5) → Conv1D(16,3) → Conv1D(32,3) → Dense(16) → sigmoid
```

**Train and export:**

```bash
cd tests/three_classes
python train_qat.py
```

**Embed for firmware:**

```bash
python tests/three_classes/tflite_to_header.py \
  tests/three_classes/fall_cnn_qat_int8.tflite \
  -o firmware/include/fall_model.h
```

Or `python tests/three_classes/convert_to_header.py` if that script targets `fall_model.h` in your tree.

Then rebuild:

```bash
cd firmware
platformio run -e esp32dev
```

### Build note

Do **not** add `-DTF_LITE_STATIC_MEMORY` to `platformio.ini` when using `tanakamasayuki/TensorFlowLite_ESP32` — it breaks the build (see [ERRORS_AND_FIXES.md](ERRORS_AND_FIXES.md) §18).

---

## References

- [TensorFlow Lite for Microcontrollers](https://www.tensorflow.org/lite/microcontrollers)
- [TFLite quantization spec](https://www.tensorflow.org/lite/performance/quantization_spec)
- [TF Model Optimization](https://www.tensorflow.org/model_optimization)
