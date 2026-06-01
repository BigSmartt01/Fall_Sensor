# TFLite Micro Inference

[← Documentation index](README.md) · [Quick start](FIRMWARE_QUICKSTART.md) · [Quantization](QUANTIZATION_CALIBRATION.md)

Hybrid fall detection: the **rule state machine** runs continuously; the **1D-CNN** optionally confirms candidates in `STATE_POST_IMPACT_STILLNESS` when a full window exists.

## Model (`tests/three_classes/train_qat.py`)

| Property | Value |
|----------|--------|
| Artifact | `fall_cnn_qat_int8.tflite` (17 KB) → `firmware/include/fall_cnn_qat_int8_model.h` |
| Input | `[1, 512, 1]` float32 → int8 (one feature per timestep) |
| Output | `[1, 1]` sigmoid → int8 |
| Labels | `y_train_3`: 0 normal, 1 fall |
| Decision | Fall if **int8 output > 0** (`predictedClass == 1`) |

Not a 6-DOF snapshot classifier. Needs **512 samples** (~10.24 s @ 50 Hz).

Training windows (`x_train_3`) are float32, roughly **[-2.7, 1.9]**. Default firmware feature: **`accelY / 9.81` (g)** in `featureForInference()` — must match offline preprocessing.

## API (`firmware/include/inference.h`)

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

Quantization scale / zero_point are read from the model in `initializeInference()`.

## Runtime flow

```
each 50 Hz tick:
  pushInferenceSample(feature)
  if window full: runInference()
  state machine → POST_IMPACT_STILLNESS:
    ruleBasedFall && (!inferenceValid || predictedClass == 1)
```

If init fails, rules alone can still confirm (`inferenceValid` false).

## Memory

| Resource | Size |
|----------|------|
| Tensor arena | 32 KB (`kTensorArenaSize`) |
| Model (flash) | ~17 KB |
| Ring buffer | 2 KB |

## Regenerate embedded model

```bash
python tests/three_classes/tflite_to_header.py \
  tests/three_classes/fall_cnn_qat_int8.tflite \
  -o firmware/include/fall_cnn_qat_int8_model.h
```

## Optional: inspect quantization

```bash
cd firmware
python extract_quantization.py ../tests/three_classes/fall_cnn_qat_int8.tflite
```

See [QUANTIZATION_CALIBRATION.md](QUANTIZATION_CALIBRATION.md) for validation and debugging.

## References

- [TensorFlow Lite for Microcontrollers](https://www.tensorflow.org/lite/microcontrollers)
- [TFLite quantization spec](https://www.tensorflow.org/lite/performance/quantization_spec)
