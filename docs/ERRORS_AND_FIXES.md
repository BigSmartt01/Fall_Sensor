# Fall Sensor - Errors and Fixes Log

## Overview

This document details issues encountered during development of the fall detection firmware, visualization stack, and ML pipeline, along with the fixes applied. Entries are ordered chronologically.

---

## Firmware Issues

### 1. Shared RTOS Variables - Race Condition

**Issue**: Unsafe access to shared state between FreeRTOS tasks using only `volatile bool`.

**Original**:
```cpp
volatile bool fallDetected = false;
volatile bool activityTriggered = false;
```

**Why it fails**: `volatile` prevents compiler optimization but does not provide atomic operations or mutual exclusion. Concurrent reads and writes across tasks cause race conditions.

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

**Issue**: Timeout set to 5000ms, far beyond physical reality of human freefall.

**Fix**: Reduced to 400ms. Real freefall from standing height lasts 150-550ms.

```cpp
const unsigned long FREEFALL_TIMEOUT = 400;
```

---

### 3. Gravity Vector - Stale Baseline

**Issue**: Gravity reference only stored at freefall onset. Person movement between falls made baseline stale.

**Fix**: Continuous low-pass filter update during STATE_NORMAL.

```cpp
const float GRAVITY_FILTER_ALPHA = 0.05;

void updateGravityVector(float x, float y, float z) {
    gravityX = gravityX * (1.0 - GRAVITY_FILTER_ALPHA) + x * GRAVITY_FILTER_ALPHA;
    gravityY = gravityY * (1.0 - GRAVITY_FILTER_ALPHA) + y * GRAVITY_FILTER_ALPHA;
    gravityZ = gravityZ * (1.0 - GRAVITY_FILTER_ALPHA) + z * GRAVITY_FILTER_ALPHA;
}
```

---

### 4. Switch Case Variable Declarations - Scope Violation

**Issue**: Variables declared inside switch case without braces cause undefined behavior in C++.

**Fix**: Wrap case body in `{}`.

```cpp
case STATE_POST_IMPACT_STILLNESS: {
    float accelVariance = getAccelVariance(sensorData.accelMag);
    float orientationChange = getOrientationChange(...);
    break;
}
```

---

### 5. MPU6050 testConnection() False Negative on ESP32-S3

**Issue**: `testConnection()` returns false consistently even when sensor is wired correctly and data is valid.

**Cause**: Timing difference in ESP32-S3 I2C peripheral causes WHO_AM_I register read to occasionally fail during fast boot.

**Fix**: 500ms boot delay before Wire.begin(), treat testConnection() as non-blocking warning.

```cpp
delay(500);
Wire.begin(SDA_PIN, SCL_PIN);
mpu.initialize();
if (!mpu.testConnection()) {
    Serial.println("testConnection() failed - continuing, verify from data");
}
```

---

### 6. I2C Error 263 During Fast Serial Printing

**Issue**: `[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error 263`

**Cause**: Serial.print() blocking starved the I2C read mid-transaction.

**Fix**: Gate all serial prints behind a 200ms interval check.

```cpp
static unsigned long lastPrint = 0;
if (millis() - lastPrint >= 200) {
    lastPrint = millis();
    Serial.printf("DATA,%lu,%.2f,%.2f,%d\n", ...);
}
```

---

## Visualization Issues

### 7. Serial Output Format - Inconsistent Parsing

**Issue**: Two different print formats in the same code block broke Python bridge parsing.

**Fix**: Single CSV-style format with DATA and FALL prefixes.

```
DATA,<timestamp_ms>,<accelMag>,<gyroMag>,<state>
FALL,<timestamp_ms>
```

---

### 8. Python Bridge - DATA Line Parser Off by One

**Issue**: `len(parts) == 4` should be `len(parts) == 5`. DATA line splits into 5 fields.

**Fix**:
```python
if len(parts) == 5:
    ts  = int(parts[1])
    acc = float(parts[2])
    gyr = float(parts[3])
    st  = int(parts[4])
```

---

### 9. PyQtGraph - Fall Marker Compresses Plot View

**Issue**: InfiniteLine drawn at fixed x position. As data scrolled, PyQtGraph expanded x-axis to include the line, compressing waveforms.

**Fix**: Track each line by absolute sample index. Recompute relative position each refresh. Remove line from plot when it scrolls off screen. Lock x-axis range with hard limits.

```python
self.accel_plot.setXRange(0, WINDOW_SIZE, padding=0)
self.accel_plot.setLimits(xMin=0, xMax=WINDOW_SIZE)

# Remove lines that scrolled off screen
if 0 <= rel_pos <= WINDOW_SIZE:
    a_line.setValue(rel_pos)
else:
    self.accel_plot.removeItem(a_line)
```

---

### 10. PyQtGraph - No Way to Inspect Fall Events During Live Scroll

**Issue**: Graph scrolled continuously. Fall waveform scrolled away before it could be examined.

**Fix**: Pause/Resume toggle button. Freezes graph at current frame while stat cards remain live.

---

## ML Pipeline Issues

### 11. Enhanced SisFall Dataset - Joblib Compatibility Failure

**Issue**: Files `x_train_3`, `y_train_3` etc. could not be loaded with joblib on Python 3.10 or 3.14.

**Error**: `IndexError: pop from empty list` across all joblib versions.

**Cause**: Files were saved with a specific older joblib/sklearn environment. The pickle protocol used is incompatible with current versions.

**Attempted fixes**: joblib==1.1.0, pickle with latin1 encoding, Python 3.10 venv - all failed.

**Resolution**: Switched to original SisFall dataset in raw .txt format. Used `np.frombuffer` to load the enhanced dataset when needed:

```python
X_train = np.frombuffer(open('x_train_3', 'rb').read(), dtype=np.float32)
X_train = X_train.reshape(y_train.shape[0], 512)
```

---

### 12. Dataset Filename Missing Subject ID

**Issue**: `prepare_dataset.py` exported files as `adl_00000.csv` without subject ID. Could not filter SE (elderly) vs SA (adult) subjects for resampling.

**Fix**: Include source filename in output name.

```python
out_name = f"{label}_{source.replace('.txt', '')}_{i:05d}.csv"
# Produces: adl_D01_SA01_R01_00000.csv
```

---

### 13. MMA8451Q vs ADXL345 Sensor Selection

**Issue**: Initially used ADXL345 (columns 1-3) from SisFall. MMA8451Q (columns 7-9) is more appropriate - 14-bit resolution, +-8g range, lower noise, wearable-oriented.

**Fix**: Updated `convert_line()` to use columns 7-9 and MMA8451Q conversion constants.

```python
MMA8451Q_SCALE = (2 * 8) / (2 ** 14)   # was ADXL345
ax = float(values[6]) * MMA8451Q_SCALE  # was values[0]
```

---

### 14. Edge Impulse Conv1D Architecture Incompatibility

**Issue**: Edge Impulse Conv1D classifier failed with shape error on both Spectral Analysis and Raw Data processing blocks.

```
ValueError: Input 0 of layer "conv1d" is incompatible with the layer:
expected min_ndim=3, found ndim=2. Full shape received: (32, 1452)
```

**Cause**: Edge Impulse flattens the processing block output before passing to the classifier. Conv1D requires 3D input (batch, timesteps, channels). Their UI does not correctly reshape the input for Conv1D when using their standard processing blocks.

**Resolution**: Abandoned Edge Impulse for model training. Moved to local TensorFlow CNN training which produced 97.77% accuracy vs Edge Impulse's 83.9% ceiling with dense layers.

---

### 15. Full CNN Model Too Large for ESP32-C3 RAM

**Issue**: Full CNN (540k parameters) produced 97.77% accuracy but required 3337KB inference RAM. ESP32-C3 has 320KB.

**Model sizes**:
| Model | Flash | RAM |
|---|---|---|
| Full CNN float32 | 545KB | 3337KB |
| ESP32-C3 limit | 4096KB | 320KB |

**Fix**: Redesigned lightweight CNN with 8/16/32 filter progression (vs 32/64/128). Reduced parameters from 540k to ~15k.

```python
Conv1D(8,  5, padding='same')  # was 32
Conv1D(16, 3, padding='same')  # was 64
Conv1D(32, 3, padding='same')  # was 128
Dense(16)                       # was 64
```

Result: 16.9KB flash, 47.1KB RAM, 94.78% accuracy.

---

### 16. Post-Training Quantization Failed to Reduce Model Size

**Issue**: Post-training int8 quantization produced 548KB vs original 545KB. No size reduction, 3.31% accuracy drop.

**Cause**: Some layers did not quantize cleanly when converting float32 model after training. Partial fallback to float operations kept size the same.

**Fix**: Quantization-aware training (QAT) from the start using `tensorflow_model_optimization`. Model trains with simulated quantization, then exports cleanly to int8.

```python
import tensorflow_model_optimization as tfmot
qat_model = tfmot.quantization.keras.quantize_model(base_model)
# Fine-tune for 10 epochs at lower LR
# Then convert - size drops to 16.9KB
```

---

### 17. tfmot.quantize_model() Fails on Conv1D Layers

**Issue**: `quantize_model()` raised RuntimeError on Conv1D layers.

```
RuntimeError: Layer conv1d is not supported. Pass a QuantizeConfig instance.
```

**Fix**: Annotate Conv1D layers individually with a custom `QuantizeConfig`, then apply `quantize_apply()`.

```python
class Conv1DQuantizeConfig(tfmot.quantization.keras.QuantizeConfig):
    def get_weights_and_quantizers(self, layer):
        return [(layer.kernel, tfmot.quantization.keras.quantizers.LastValueQuantizer(
            num_bits=8, symmetric=True, narrow_range=False, per_axis=False))]
    def get_activations_and_quantizers(self, layer):
        return [(layer.activation, tfmot.quantization.keras.quantizers.MovingAverageQuantizer(
            num_bits=8, symmetric=False, narrow_range=False, per_axis=False))]
    def set_quantize_weights(self, layer, quantize_weights):
        layer.kernel = quantize_weights[0]
    def set_quantize_activations(self, layer, quantize_activations):
        layer.activation = quantize_activations[0]
    def get_output_quantizers(self, layer):
        return []
    def get_config(self):
        return {}

annotated_model = tf.keras.models.clone_model(
    base_model,
    clone_function=lambda layer: (
        tfmot.quantization.keras.quantize_annotate_layer(layer, Conv1DQuantizeConfig())
        if isinstance(layer, tf.keras.layers.Conv1D) else layer
    )
)
with tfmot.quantization.keras.quantize_scope({'Conv1DQuantizeConfig': Conv1DQuantizeConfig}):
    qat_model = tfmot.quantization.keras.quantize_apply(annotated_model)
```

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
| 11 | ML | Enhanced dataset joblib incompatible | np.frombuffer loading |
| 12 | ML | Subject ID missing from filenames | Include source name in output |
| 13 | ML | Wrong sensor columns used | Switched to MMA8451Q columns 7-9 |
| 14 | ML | Edge Impulse Conv1D shape error | Moved to local TensorFlow training |
| 15 | ML | Full CNN too large for ESP32-C3 RAM | Lightweight CNN (15k params) |
| 16 | ML | Post-training quantization no size reduction | QAT from training start |
| 17 | ML | tfmot fails on Conv1D layers | Custom Conv1DQuantizeConfig |

---

## References

- FreeRTOS EventGroups: https://www.freertos.org/event-groups-API.html
- C++ Switch Scope: https://en.cppreference.com/w/cpp/language/switch
- TFLite Micro ESP32: https://www.tensorflow.org/lite/microcontrollers
- TF Model Optimization: https://www.tensorflow.org/model_optimization
- SisFall Dataset: https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5298771/
- PyQtGraph InfiniteLine: https://pyqtgraph.readthedocs.io/en/latest/api_reference/graphicsItems/infiniteline.html
