# Fall Sensor — Documentation

| Guide | Description |
|-------|-------------|
| [FIRMWARE_QUICKSTART.md](FIRMWARE_QUICKSTART.md) | Build, flash, wire MPU6050, test falls, tune thresholds |
| [TFLITE_INFERENCE.md](TFLITE_INFERENCE.md) | Model I/O, inference API, hybrid rules + NN flow |
| [QUANTIZATION_CALIBRATION.md](QUANTIZATION_CALIBRATION.md) | Verify int8 scales; debug Python vs device |
| [ERRORS_AND_FIXES.md](ERRORS_AND_FIXES.md) | Development issues and fixes log |

Project overview: [../README.md](../README.md)

## Architecture (summary)

```
fallDetectionTask (50 Hz)
  ├─ MPU6050 → rule state machine (freefall → impact → stillness → orientation)
  ├─ pushInferenceSample → 512-sample ring buffer
  └─ POST_IMPACT_STILLNESS: confirm if rules pass AND (NN off OR predictedClass==1)

activityTriggerTask → alert on EventGroup bit
```

**Key paths**

| Path | Role |
|------|------|
| `firmware/src/main.cpp` | State machine + `featureForInference()` |
| `firmware/src/inference.cpp` | TFLite Micro |
| `firmware/include/fall_cnn_qat_int8_model.h` | Embedded model |
| `firmware/platformio.ini` | `esp32-c3`, `esp32dev` |
| `firmware/extract_quantization.py` | Print quant params from `.tflite` |
| `tests/three_classes/train_qat.py` | Train / export |
| `tests/three_classes/tflite_to_header.py` | `.tflite` → C header |

**After retraining:** export `.tflite` → run `tflite_to_header.py` → align `featureForInference()` with `x_train_3` → build & test.

**Limits:** ~10.24 s NN warm-up (512 samples @ 50 Hz); feature pipeline must match training data.
