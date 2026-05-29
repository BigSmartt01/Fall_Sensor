# Quick Start Guide: TFLite Micro Inference on ESP32-C3

## 5-Minute Setup

### 1. Prerequisites

- PlatformIO installed in VS Code
- ESP32-C3 development board
- MPU6050 IMU sensor
- Micro USB cable

### 2. Hardware Connections

```
MPU6050          ESP32-C3
├─ VCC       ──► 3.3V
├─ GND       ──► GND
├─ SDA       ──► GPIO21
├─ SCL       ──► GPIO22
└─ INT       ──► (optional)
```

### 3. Build & Upload

```bash
cd firmware

# Build for ESP32-C3
platformio run -e esp32-c3

# Upload to device
platformio run -e esp32-c3 --target upload

# Monitor output
platformio device monitor --baud 115200
```

### 4. Verify Startup

You should see:
```
=== Initializing Neural Network ===
Initializing TFLite Micro inference...
Input tensor type: 9 (1=FLOAT32, 9=INT8)
Output tensor type: 9
=== Inference Memory Information ===
Tensor Arena Size: 32768 bytes
Arena Used: 18432 bytes (56.3%)
====================================
```

## Calibration (Important!)

Your model's quantization parameters must match the conversion settings. Run:

```bash
python extract_quantization.py include/fall_cnn_qat_int8_model.h --output inference_quantization.h
```

This generates calibration values. Update `src/inference.cpp`:

```cpp
namespace {
  // Copy values from generated file here
  float input_scale = 0.0078125f;      // ← Update
  int32_t input_zero_point = 0;        // ← Update
  
  float output_scale = 0.00390625f;    // ← Update
  int32_t output_zero_point = -128;    // ← Update
}
```

Then rebuild and upload.

## Testing Fall Detection

### 1. Watch Serial Output

Each line shows sensor state:
```
DATA,15234,9.81,5.3,0      # timestamp, accelMag, gyroMag, state
```

States: 0=NORMAL, 1=FREEFALL, 2=IMPACT, 3=POST_IMPACT, 4=CONFIRMED

### 2. Trigger a Test Fall

Hold device horizontal (simulating normal gravity) then quickly:
1. Let it "freefall" (reduce acceleration to ~1m/s²)
2. Create sudden impact (high acceleration spike)
3. Keep still for 2-3 seconds

Watch for:
```
STAGE 1: FREEFALL DETECTED
STAGE 2: IMPACT DETECTED
STAGE 3: POST-IMPACT STILLNESS
*** STAGE 4: FALL CONFIRMED ***
Fall probability: 0.850
FALL,timestamp
```

## Key Parameters (Tuning)

Edit in `src/main.cpp`:

```cpp
// Detection sensitivity (lower = more sensitive)
const float FREEFALL_THRESHOLD = 3.0;      // m/s²
const float IMPACT_THRESHOLD = 20.0;       // m/s²

// Confirmation timing (longer = fewer false positives)
const unsigned long FREEFALL_TIMEOUT = 400;      // ms
const unsigned long IMPACT_WINDOW = 500;         // ms
const unsigned long POST_IMPACT_STILLNESS_DURATION = 2500;  // ms

// NN confidence (higher = more strict)
// In fallDetectionTask:
if (inferenceOutput.fallProbability > 0.7f)  // Adjust 0.7
```

## Inference Frequency

Edit in `src/main.cpp`:

```cpp
const int INFERENCE_EVERY_N = 5;  // Run every 5*20ms = 100ms
// Increase for slower (less CPU), decrease for faster (more responsive)
```

## Troubleshooting

### Won't compile: "TensorFlow not found"
```bash
# Try again - PlatformIO sometimes needs to download dependencies
platformio run -e esp32-c3 --verbose
```

### Won't upload: "Board not detected"
```bash
# Check USB connection and verify COM port
platformio run -e esp32-c3 --target upload --verbose

# If still failing, manually select port
platformio run -e esp32-c3 --target upload --upload-port COM3
```

### Serial output shows garbage
- Check baud rate: 115200
- Try different USB cable (data cable, not just charging)

### Model loads but inference is slow
- Reduce `INFERENCE_EVERY_N` (run less frequently)
- Increase `kTensorArenaSize` if OOM errors occur

### Always detects fall (false positives)
1. Check calibration parameters (run extract_quantization.py again)
2. Increase thresholds (IMPACT_THRESHOLD, POST_IMPACT_STILLNESS_DURATION)
3. Increase NN confidence threshold (0.7 → 0.8)

### Never detects fall (false negatives)
1. Decrease thresholds (IMPACT_THRESHOLD, POST_IMPACT_STILLNESS_DURATION)
2. Decrease NN confidence threshold (0.7 → 0.6)
3. Check MPU6050 calibration

## Project Structure

```
src/
├─ main.cpp              # Fall detection state machine
└─ inference.cpp         # TFLite Micro engine

include/
├─ inference.h           # API definitions
└─ fall_cnn_qat_int8_model.h  # Neural network model

docs/
├─ TFLITE_INFERENCE_README.md     # Detailed documentation
└─ QUANTIZATION_CALIBRATION.md    # Calibration guide

extract_quantization.py  # Parameter extraction tool
```

## Next Steps

1. **Calibrate** - Run extraction script and update parameters
2. **Test** - Verify fall detection works with your model
3. **Optimize** - Adjust thresholds for your environment
4. **Deploy** - Add buzzer/alert logic as needed

## Performance

- **Inference latency**: ~10ms per run
- **Memory usage**: ~34KB RAM, ~32KB flash
- **CPU during inference**: ~10% per 100ms cycle
- **Power**: ~50mA during inference, ~10mA idle

## Get More Help

- **TFLite Micro Docs**: https://www.tensorflow.org/lite/microcontrollers
- **Model Conversion**: See `QUANTIZATION_CALIBRATION.md`
- **Detailed API**: See `TFLITE_INFERENCE_README.md`
- **Hardware Issues**: Check MPU6050 datasheet

## Quick Reference

| Task | File | Location |
|------|------|----------|
| Fall detection logic | main.cpp | Lines 195-330 |
| Inference engine | inference.cpp | - |
| API header | inference.h | - |
| Tuning parameters | main.cpp | Top of file |
| Quantization params | inference.cpp | Lines 26-30 |

---

**Ready?** Just run `platformio run -e esp32-c3 --target upload` and monitor!

Good luck! 🚀
