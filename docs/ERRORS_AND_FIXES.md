# Fall Sensor - Errors and Fixes Log

## Overview

This document details issues encountered during development of the fall detection firmware, visualization stack, ML pipeline, hardware migration, and build system. Entries are ordered chronologically.

---

## Firmware Issues (MPU6050 / PlatformIO era)

### 1. Shared RTOS Variables - Race Condition

**Issue**: Unsafe access to shared state between FreeRTOS tasks using only `volatile bool`.

**Fix**: FreeRTOS EventGroup for thread-safe inter-task signaling.

```cpp
EventGroupHandle_t eventGroup = NULL;
#define FALL_DETECTED_BIT     (1 << 0)
#define ACTIVITY_TRIGGERED_BIT (1 << 1)

eventGroup = xEventGroupCreate();
xEventGroupSetBits(eventGroup, FALL_DETECTED_BIT);

EventBits_t uxBits = xEventGroupWaitBits(
    eventGroup, ACTIVITY_TRIGGERED_BIT,
    pdTRUE, pdFALSE, 100 / portTICK_PERIOD_MS
);
```

---

### 2. Freefall Timeout - Incorrect Duration

**Fix**: Reduced from 5000ms to 400ms. Real freefall from standing height lasts 150-550ms.

---

### 3. Gravity Vector - Stale Baseline

**Fix**: Continuous low-pass filter update during STATE_NORMAL (alpha=0.05) instead of one-shot capture at freefall onset.

---

### 4. Switch Case Variable Declarations - Scope Violation

**Fix**: Wrap case body in `{}` to avoid undefined behavior from cross-case variable visibility in C++ switch statements.

---

### 5. MPU6050 testConnection() False Negative on ESP32-S3

**Fix**: 500ms boot delay before Wire.begin(), treat testConnection() as non-blocking warning.

---

### 6. I2C Error 263 During Fast Serial Printing

**Issue**: `[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error 263` - Serial.print() blocking starved the I2C read mid-transaction.

**Fix**: Gate all serial prints behind a 200ms interval check.

---

## Visualization Issues

### 7. Serial Output Format - Inconsistent Parsing

**Fix**: Single CSV-style format with DATA and FALL prefixes:
```
DATA,<timestamp_ms>,<accelMag>,<gyroMag>,<state>
FALL,<timestamp_ms>
```

---

### 8. Python Bridge - DATA Line Parser Off by One

**Fix**: `len(parts) == 5`, not 4 - the DATA line splits into 5 fields including the prefix.

---

### 9. PyQtGraph - Fall Marker Compresses Plot View

**Fix**: Track each marker line by absolute sample index, recompute relative position each refresh, remove the line from the plot once it scrolls off screen, and lock the x-axis range with hard `setLimits()`.

---

### 10. PyQtGraph - No Way to Inspect Fall Events During Live Scroll

**Fix**: Pause/Resume toggle button that freezes the graph at the current frame while stat cards remain live.

---

### 11. Dashboard WiFi Mode - Serial and WiFi Parsing Could Drift

**Issue**: When WiFi TCP streaming was added alongside serial, two separate reader functions risked diverging in their DATA/FALL parsing logic if either was updated independently.

**Fix**: Refactored shared parsing into a single `process_line()` function called by both `serial_reader()` and `wifi_reader()`. Both modes always use identical parsing, InfluxDB write, and buffer update logic.

---

## ML Pipeline Issues

### 12. Enhanced SisFall Dataset - Joblib Compatibility Failure

**Issue**: Files `x_train_3`, `y_train_3` etc. could not be loaded with joblib on Python 3.10 or 3.14 (`IndexError: pop from empty list`).

**Resolution**: Switched to `np.frombuffer` loading, bypassing joblib/pickle entirely:

```python
X_train = np.frombuffer(open('x_train_3', 'rb').read(), dtype=np.float32)
X_train = X_train.reshape(y_train.shape[0], 512)
```

---

### 13. Dataset Filename Missing Subject ID

**Fix**: Include source filename in output name so SE (elderly) vs SA (adult) subjects can be filtered:
```python
out_name = f"{label}_{source.replace('.txt', '')}_{i:05d}.csv"
```

---

### 14. MMA8451Q vs ADXL345 Sensor Selection

**Fix**: Switched from ADXL345 (columns 1-3) to MMA8451Q (columns 7-9) in SisFall - 14-bit resolution, ±8g range, lower noise, closer to a real wearable accelerometer profile.

---

### 15. Edge Impulse Conv1D Architecture Incompatibility

**Issue**: Edge Impulse Conv1D classifier failed with shape error regardless of which processing block fed it - the platform flattens output before the classifier, but Conv1D needs 3D input.

**Resolution**: Abandoned Edge Impulse for model training. Local TensorFlow CNN training produced 97.77% accuracy vs Edge Impulse's 83.9% ceiling with dense layers.

---

### 16. Full CNN Model Too Large for ESP32-C3 RAM

**Issue**: Full CNN (540k parameters, 97.77% accuracy) needed 3337KB inference RAM. ESP32-C3 has 320KB.

**Fix**: Redesigned lightweight CNN (8/16/32 filter progression, ~15k parameters). Result: 16.9KB flash, 47.1KB RAM, 94.78% accuracy.

---

### 17. Post-Training Quantization Failed to Reduce Model Size

**Issue**: Post-training int8 quantization produced 548KB vs original 545KB float32 - no size reduction, 3.31% accuracy drop.

**Fix**: Quantization-aware training (QAT) from the start using `tensorflow_model_optimization`. Size dropped to 16.9KB.

---

### 18. tfmot.quantize_model() Fails on Conv1D Layers

**Issue**: `RuntimeError: Layer conv1d is not supported. Pass a QuantizeConfig instance.`

**Fix**: Custom `Conv1DQuantizeConfig` class, annotate Conv1D layers individually via `quantize_annotate_layer`, then `quantize_apply()` inside a `quantize_scope`.

---

### 19. Model Trained at 200Hz but Firmware Sampling at 50Hz - Input Distribution Mismatch

**Issue**: SisFall was collected at 200Hz. Running BMI160 at 50Hz and feeding 512 samples meant each sample represented 20ms instead of 5ms. The model's convolutional kernels learned temporal patterns assuming 5ms intervals - a fall spike that should appear as a sharp 200ms event was being presented as a slow 4-second event. Accuracy was degraded and detection latency was 10.24 seconds instead of 2.56 seconds.

**Fix**: Set BMI160 ODR to 200Hz via direct register write after `I2cInit()`, and changed `vTaskDelay` from 20ms to 5ms throughout. Retrained the model at aligned sampling, improving accuracy from 94.78% to 95.21% and reducing detection window from 10.24s to 2.56s.

```cpp
Wire.beginTransmission(0x68);
Wire.write(0x40); Wire.write(0x09);  // ACC_CONF: 200Hz
Wire.endTransmission();
delay(10);
Wire.beginTransmission(0x68);
Wire.write(0x42); Wire.write(0x09);  // GYR_CONF: 200Hz
Wire.endTransmission();
```

Note: DFRobot_BMI160 library exposes ODR constants but the configuration function signature was incompatible. Direct register write is cleaner and guaranteed correct per the BMI160 datasheet (register 0x40/0x42, value 0x09 = 200Hz).

---

### 20. Single ML Inference Result Insufficient - False Positive Risk

**Issue**: A single inference run returning above-threshold probability was enough to confirm a fall. Spurious high-probability outputs from transitional motion (sitting down hard, stumbling without falling) could trigger false alerts.

**Fix**: Consecutive detection filter in `runInference()` - requires 3 consecutive inference runs each returning probability >= 80% before setting `predictedClass = 1`. Counter resets to 0 if any run falls below threshold, and resets again after a confirmed detection.

```cpp
constexpr int   kRequiredConsecutiveDetections = 3;
constexpr float kFallProbabilityThreshold      = 0.80f;
static int      consecutiveFallCount           = 0;

if (output.fallProbability >= kFallProbabilityThreshold) {
    consecutiveFallCount++;
} else {
    consecutiveFallCount = 0;
}

if (consecutiveFallCount >= kRequiredConsecutiveDetections) {
    output.predictedClass = 1;
    consecutiveFallCount  = 0;
}
```

---

## Hardware Migration Issues (BMI160 / ESP32-C3)

### 21. BMI160 CS/SAO Pin Confusion

**Issue**: I2C scanner found nothing despite correct SDA/SCL wiring.

**Cause**: BMI160 breakout exposes `CS` (CSB), `SAO` (SDO), `OCS`. CS must be tied to 3.3V to force I2C mode. SAO sets address: GND = 0x68, 3.3V = 0x69. Also found a loose jumper wire during debugging - always re-check physical connections before assuming a software cause.

**Fix**: CS -> 3.3V, SAO -> GND for address 0x68.

---

### 22. ESP32-C3 Serial Monitor Shows Nothing

**Issue**: Blink sketch worked but no serial output appeared.

**Cause**: ESP32-C3 uses USB-Serial/JTAG, not a separate UART-to-USB bridge. `Serial` requires USB CDC explicitly enabled.

**Fix**: Arduino IDE -> Tools -> USB CDC On Boot -> Enabled.

---

### 23. tanakamasayuki/TensorFlowLite_ESP32 Does Not Support ESP32-C3

**Issue**: PlatformIO build failed with `'SPI3_HOST' undeclared` and implicit-declaration errors from a bundled ILI9341 display driver - peripheral enums that don't exist on C3.

**Resolution**: Migrated firmware to native ESP-IDF with `espressif/esp-tflite-micro` as a git submodule and `arduino-esp32` as an ESP-IDF managed component. See entries 24-31 for migration issues.

---

### 24. EXTRA_COMPONENT_DIRS Pointing to Removed Component

**Fix**: Removed the `EXTRA_COMPONENT_DIRS` entry from root `CMakeLists.txt` entirely - that component was moved to the IDF component manager in v5.x.

---

### 25. DFRobot_BMI160.h Not Found in ESP-IDF Build

**Fix**: Cloned `DFRobot_BMI160` as a git submodule into `firmware_idf/components/`. Created a `DFRobot_BMI160_wrapper/` component alongside it with a local `CMakeLists.txt` that references the submodule sources - this wrapper is tracked in the main repo and survives every clone and submodule reinit, unlike a file placed inside the submodule itself which gets wiped on every reinit.

```cmake
# firmware_idf/components/DFRobot_BMI160_wrapper/CMakeLists.txt
idf_component_register(
    SRCS "../DFRobot_BMI160/DFRobot_BMI160.cpp"
    INCLUDE_DIRS "../DFRobot_BMI160"
    REQUIRES arduino-esp32
)
```

---

### 26. esp-tflite-micro Different Resolver and Error Reporter API

**Issue 1**: `all_ops_resolver.h` does not exist in this component - only `MicroMutableOpResolver`.

**Issue 2**: `MicroInterpreter` constructor takes 4 arguments (model, resolver, arena, size) - no error reporter parameter.

**Fix**: Switched to `MicroMutableOpResolver<N>`, registered ops explicitly, removed error reporter include and argument entirely.

---

### 27. MicroMutableOpResolver Missing Ops

**Issue**: Sequential crashes `Didn't find op for builtin opcode 'X'` after each flash, discovering one missing op at a time.

**Better fix**: `tests/three_classes/list_ops.py` reads the model FlatBuffer schema directly and prints every unique op used. Run once after any retrain to get the complete list before touching the resolver registration.

Final required set: `ADD, CONV_2D, EXPAND_DIMS, FULLY_CONNECTED, LOGISTIC, MAX_POOL_2D, MUL, RESHAPE`

---

### 28. format '%d' for uint32_t Treated as Hard Error

**Fix**: Used `%lu` with explicit `(unsigned long)` casts for all `uint32_t` values - ESP-IDF's `-Werror=format` fails the build on what PlatformIO only warned about.

---

### 29. Git Submodule Path Mismatch After History Purge

**Fix**:
```bash
git rm --cached -f firmware/lib/tflite-micro
git submodule update --init --recursive
```

---

### 30. Submodule Pinned to Commit Not on Remote

**Fix**: Checked out correct existing branch (`master` not `main` - confirmed via `git branch -r`) and re-added/committed the submodule pointer.

---

### 31. esp-tflite-micro Submodule Cloned Empty

**Fix**:
```bash
git submodule update --init --force --recursive firmware_idf/components/esp-tflite-micro
```

---

### 32. App Partition Too Small After Adding WiFi Stack

**Issue**: `Error: app partition is too small for binary fall_sensor.bin size 0x1190d0` - WiFi + lwip added ~100KB over the default 1MB factory partition limit.

**Fix**: Custom `partitions.csv` with 3MB factory partition, plus `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` in `sdkconfig.defaults` to match the actual C3 hardware. Delete `sdkconfig` and run `idf.py set-target esp32c3` to regenerate cleanly after changing partition config.

```csv
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x300000,
```

---

### 33. BMI160 Init Order: softReset() Must Come Before I2cInit()

**Issue**: All accel/gyro readings returned exactly 0.00. State machine triggered constant false freefall detections since 0.00 < 3.0 m/s² threshold.

**Cause**: `I2cInit()` called before `softReset()`. Chip acknowledged I2C transactions (so init reported OK) but never actually streamed valid sensor data.

**Fix**:
```cpp
if (bmi160.softReset() != BMI160_OK) { /* halt */ }
delay(100);
if (bmi160.I2cInit(BMI160_I2C_ADDR) != BMI160_OK) { /* halt */ }
```

---

### 34. Serial.print Silently Delayed on ESP32-C3 Under ESP-IDF + Arduino Component

**Issue**: Large stretches of `Serial.println`/`Serial.printf` output appeared missing from the monitor during early boot, making it look like code wasn't executing.

**Cause**: Arduino `Serial` (USB-CDC) takes longer to attach/flush than the native ESP-IDF UART driver that backs `printf()` on ESP32-C3 USB-Serial/JTAG.

**Fix**: Use `printf()` for any debug output needed from the first instruction of `app_main()`/`setup()`. Reserve `Serial.print*` for steady-state runtime output once initialization is complete. Confirm a missing `Serial` line with a `printf` before chasing a phantom logic bug.

---

### 35. Fall Alert Cleared Immediately While Person Still on Floor

**Issue**: `STATE_FALL_CONFIRMED` was a single-tick pass-through state that immediately reset to `STATE_NORMAL`. The buzzer fired once and stopped. No persistent awareness that the person was still down.

**Fix**: Added `STATE_FALL_ALERTED` as a persistent state entered from `STATE_FALL_CONFIRMED`:
- Captures fall-moment orientation via `captureFallOrientation()`
- Re-triggers buzzer every 10 seconds via `ALERT_BUZZ_INTERVAL`
- Exits only when orientation delta from fall-moment posture exceeds `RECOVERY_ORIENTATION_THRESHOLD = 2.0` (person got up), or button press ("I'm okay")

Recovery uses `getOrientationDelta()` against the fall-moment orientation, not the pre-fall gravity baseline, since the person is expected to still be in the fallen posture until they actually move.

---

### 36. Buzzer Appearing to Cut Short

**Issue**: 5-second buzzer duration in `activityTriggerTask` appeared to stop early. Closer inspection showed the state machine racing back to `STATE_NORMAL` in the same loop iteration that fired the event, and `updateGravityVector()` overwriting the fall-moment gravity reference while buzzer was still active.

**Fix**: Entry 35 (STATE_FALL_ALERTED) resolves the state machine racing issue. Buzzer duration in `activityTriggerTask` reduced to 2 seconds per pulse - persistence now comes from periodic re-triggering in `STATE_FALL_ALERTED` every 10 seconds, not a single long continuous tone.

---

## Summary Table

| # | Area | Issue | Fix |
|---|------|-------|-----|
| 1 | Firmware | volatile bool race condition | FreeRTOS EventGroup |
| 2 | Firmware | Freefall timeout 5000ms | Reduced to 400ms |
| 3 | Firmware | Stale gravity baseline | Continuous low-pass filter |
| 4 | Firmware | Switch case scope violation | Added {} block |
| 5 | Firmware | testConnection() false negative | Boot delay + non-blocking |
| 6 | Firmware | I2C Error 263 | Gated serial prints |
| 7 | Visualization | Dual serial format | Standardized DATA/FALL prefix |
| 8 | Visualization | Bridge parser wrong field count | Fixed len(parts) == 5 |
| 9 | Visualization | Fall marker compresses plot | Remove off-screen lines, lock x range |
| 10 | Visualization | No waveform inspection during scroll | Pause/Resume button |
| 11 | Visualization | Serial/WiFi parsing could diverge | Shared process_line() function |
| 12 | ML | Enhanced dataset joblib incompatible | np.frombuffer loading |
| 13 | ML | Subject ID missing from filenames | Include source name in output |
| 14 | ML | Wrong sensor columns used | Switched to MMA8451Q columns 7-9 |
| 15 | ML | Edge Impulse Conv1D shape error | Moved to local TensorFlow training |
| 16 | ML | Full CNN too large for ESP32-C3 RAM | Lightweight CNN (15k params) |
| 17 | ML | Post-training quantization no size reduction | QAT from training start |
| 18 | ML | tfmot fails on Conv1D layers | Custom Conv1DQuantizeConfig |
| 19 | ML | Sampling rate mismatch with training data | BMI160 ODR set to 200Hz via register |
| 20 | ML | Single inference result triggers false positives | 3 consecutive detections at 80% threshold |
| 21 | Hardware | BMI160 CS/SAO pin confusion | CS->3.3V, SAO->GND |
| 22 | Hardware | ESP32-C3 serial monitor blank | Enable USB CDC On Boot |
| 23 | Build | TensorFlowLite_ESP32 unsupported on C3 | Migrated to ESP-IDF + esp-tflite-micro |
| 24 | Build | EXTRA_COMPONENT_DIRS points to removed component | Removed from CMakeLists.txt |
| 25 | Build | DFRobot_BMI160.h not found in ESP-IDF | Submodule + tracked wrapper component |
| 26 | Build | esp-tflite-micro different resolver/reporter API | MicroMutableOpResolver, no error reporter |
| 27 | Build | Ops missing - discovered one at a time | list_ops.py reads FlatBuffer schema |
| 28 | Build | %d/uint32_t format error treated as hard error | %lu with explicit casts |
| 29 | Git | Orphaned submodule path after history purge | git rm --cached -f, reinit |
| 30 | Git | Submodule pinned to nonexistent remote commit | Checkout correct branch, recommit |
| 31 | Git | Submodule cloned empty despite valid status | Forced --init --force --recursive |
| 32 | Build | App partition too small after WiFi stack added | Custom partitions.csv, 3MB factory |
| 33 | Hardware | BMI160 init order wrong, all-zero readings | softReset() before I2cInit() |
| 34 | Debug | Serial.print delayed on C3 early boot | Use printf() for early-boot debug |
| 35 | Firmware | Fall alert cleared while person still on floor | STATE_FALL_ALERTED persistent state |
| 36 | Firmware | Buzzer appearing to cut short | 2s pulse + periodic re-trigger in alerted state |

---

## References

- FreeRTOS EventGroups: https://www.freertos.org/event-groups-API.html
- C++ Switch Scope: https://en.cppreference.com/w/cpp/language/switch
- TFLite Micro: https://www.tensorflow.org/lite/microcontrollers
- TF Model Optimization: https://www.tensorflow.org/model_optimization
- SisFall Dataset: https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5298771/
- PyQtGraph InfiniteLine: https://pyqtgraph.readthedocs.io/en/latest/api_reference/graphicsItems/infiniteline.html
- esp-tflite-micro: https://github.com/espressif/esp-tflite-micro
- BMI160 Datasheet: https://www.bosch-sensortec.com/products/motion-sensors/imus/bmi160/
- DFRobot_BMI160: https://github.com/DFRobot/DFRobot_BMI160