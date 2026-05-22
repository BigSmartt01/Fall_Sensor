# Fall Sensor - Errors and Fixes Log

## Overview

This document details issues encountered during development of the fall detection firmware and visualization stack, along with the fixes applied. Entries are ordered chronologically.

---

## Firmware Issues

### 1. Shared RTOS Variables - Race Condition

**Issue**: Unsafe access to shared state between FreeRTOS tasks using only `volatile bool`.

**Original code**:
```cpp
volatile bool fallDetected = false;
volatile bool activityTriggered = false;
```

**Why it fails**: `volatile` prevents compiler optimization but does not provide atomic operations or mutual exclusion. Concurrent reads and writes across tasks cause race conditions and undefined behavior.

**Fix**: Replace with FreeRTOS EventGroup for thread-safe inter-task signaling.

```cpp
EventGroupHandle_t eventGroup = NULL;
#define FALL_DETECTED_BIT     (1 << 0)
#define ACTIVITY_TRIGGERED_BIT (1 << 1)

// In setup()
eventGroup = xEventGroupCreate();

// Task 1: signal safely
xEventGroupSetBits(eventGroup, FALL_DETECTED_BIT);

// Task 2: wait with timeout
EventBits_t uxBits = xEventGroupWaitBits(
    eventGroup,
    ACTIVITY_TRIGGERED_BIT,
    pdTRUE,
    pdFALSE,
    100 / portTICK_PERIOD_MS
);
```

---

### 2. Freefall Timeout - Incorrect Duration

**Issue**: Timeout set to 5000ms, far beyond the physical reality of human freefall.

**Original**:
```cpp
const unsigned long FREEFALL_TIMEOUT = 5000;
```

**Why it's wrong**: Real human freefall from standing height lasts roughly 150-400ms. A 5-second window allows the state machine to chase false positives indefinitely and delays reset to normal state.

**Fix**:
```cpp
const unsigned long FREEFALL_TIMEOUT = 400;
```

Physics reference: from h = 1.5m, freefall time = sqrt(2h/g) = approximately 0.55 seconds.

---

### 3. Gravity Vector - Stale Baseline

**Issue**: Gravity reference vector only stored at the moment freefall was detected, not continuously updated.

**Original code**:
```cpp
case STATE_NORMAL:
    if (sensorData.accelMag < FREEFALL_THRESHOLD) {
        fallState = STATE_FREEFALL;
        storeGravityVector(...);   // only updated here
    }
    break;
```

**Why it fails**: If the person moves or changes posture between fall events, the stored vector becomes stale. Orientation change detection then compares against a corrupted baseline, producing false positives or missed detections.

**Fix**: Continuously update the gravity vector during STATE_NORMAL using a low-pass filter.

```cpp
const float GRAVITY_FILTER_ALPHA = 0.05;

void updateGravityVector(float x, float y, float z) {
    gravityX = gravityX * (1.0 - GRAVITY_FILTER_ALPHA) + x * GRAVITY_FILTER_ALPHA;
    gravityY = gravityY * (1.0 - GRAVITY_FILTER_ALPHA) + y * GRAVITY_FILTER_ALPHA;
    gravityZ = gravityZ * (1.0 - GRAVITY_FILTER_ALPHA) + z * GRAVITY_FILTER_ALPHA;
}

case STATE_NORMAL:
    updateGravityVector(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
    if (sensorData.accelMag < FREEFALL_THRESHOLD) {
        fallState = STATE_FREEFALL;
        freefallStartTime = millis();
    }
    break;
```

---

### 4. Switch Case Variable Declarations - Scope Violation

**Issue**: Variables declared inside a switch case without a scope block, violating C++ standard.

**Original**:
```cpp
case STATE_POST_IMPACT_STILLNESS:
    float accelVariance = getAccelVariance(sensorData.accelMag);
    float orientationChange = getOrientationChange(...);
    break;
```

**Why it fails**: C++ switch statements share a single scope across all cases. Declaring variables without a block means they are technically in scope for other cases, causing jump-to-case-label errors and potential uninitialized memory access.

**Fix**: Wrap the case body in curly braces to create an isolated scope.

```cpp
case STATE_POST_IMPACT_STILLNESS: {
    float accelVariance = getAccelVariance(sensorData.accelMag);
    float orientationChange = getOrientationChange(...);
    // ... logic ...
    break;
}
```

---

### 5. MPU6050 testConnection() Returns False on ESP32-S3

**Issue**: `mpu.testConnection()` consistently returns false on ESP32-S3 with the electroniccats library, even when the sensor is correctly wired and responding.

**Observation**: Serial monitor shows "MPU6050 connection failed!" at boot, but `getMotion6()` reads valid data immediately after.

**Cause**: The `testConnection()` implementation in the electroniccats library has known reliability issues on ESP32-S3 due to timing differences in the I2C peripheral. The WHO_AM_I register read occasionally returns an unexpected value during the fast boot sequence.

**Fix**: Add a 500ms boot delay before `Wire.begin()` to allow the sensor to settle, and treat `testConnection()` as a non-blocking warning rather than a hard failure. Confirm sensor operation from actual data output instead.

```cpp
delay(500);
Wire.begin(SDA_PIN, SCL_PIN);
mpu.initialize();

if (!mpu.testConnection()) {
    Serial.println("testConnection() failed - continuing anyway, verify from data");
}
```

---

### 6. I2C Error 263 During Fast Serial Printing

**Issue**: Error message in serial monitor: `[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error 263`

**Cause**: ESP_ERR_TIMEOUT on the I2C bus. Serial.print() calls inside the FreeRTOS task were blocking long enough to starve the I2C transaction mid-read, causing the bus to time out.

**Fix**: Reduce serial print frequency and move prints behind a timestamp gate. Confirmed stable at 200ms print interval with 20ms sensor sampling.

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

**Issue**: Two different serial print formats in the same code block - one with `>` prefix for serial plotter, one without for human reading. Python bridge could not reliably parse either.

**Fix**: Standardized to a single CSV-style format with a `DATA` prefix, parseable by both the bridge and dashboard, ignoring all other lines by prefix.

```
DATA,<timestamp_ms>,<accelMag>,<gyroMag>,<state>
FALL,<timestamp_ms>
```

---

### 8. Python Bridge - DATA Line Parser Off by One

**Issue**: Parser condition `len(parts) == 4` was checking for 4 fields, but a `DATA` line splits into 5 parts (`DATA`, timestamp, accel, gyro, state).

**Fix**:
```python
if len(parts) == 5:   # was 4
    ts  = int(parts[1])
    acc = float(parts[2])
    gyr = float(parts[3])
    st  = int(parts[4])
```

---

### 9. PyQtGraph Dashboard - Fall Marker Line Compresses Plot View

**Issue**: Red `InfiniteLine` markers for fall events were drawn at a fixed x position. As new data scrolled in, the line stayed at its original pixel position and PyQtGraph expanded the x-axis range to accommodate it, compressing the waveform into a smaller portion of the screen.

**Root cause**: `InfiniteLine` extends infinitely on the plot axis. When its value goes outside the visible data range, PyQtGraph auto-scales to include it.

**Fix applied**:

1. Track each fall line by its absolute sample index at the moment of creation.
2. Each UI refresh, recompute the line's position relative to the current window start (`rel_pos = abs_idx - window_start`).
3. If `rel_pos` falls outside `[0, WINDOW_SIZE]`, remove the line from both plots entirely using `plot.removeItem()`.
4. Lock both plots to a fixed x range with hard limits so no item can ever push or compress the view.

```python
self.accel_plot.setXRange(0, WINDOW_SIZE, padding=0)
self.accel_plot.setLimits(xMin=0, xMax=WINDOW_SIZE)
```

---

### 10. PyQtGraph Dashboard - No Way to Inspect Fall Events

**Issue**: Graph scrolled continuously during live monitoring. When a fall was confirmed, the relevant waveform section scrolled away before it could be examined.

**Fix**: Added a Pause/Resume toggle button that freezes the graph at the current frame. The stat cards and status bar remain live while paused. Resuming catches up to the current buffer instantly.

---

## Summary Table

| # | Area | Issue | Fix |
|---|------|-------|-----|
| 1 | Firmware | `volatile bool` race condition between RTOS tasks | FreeRTOS EventGroup |
| 2 | Firmware | Freefall timeout 5000ms, physically inaccurate | Reduced to 400ms |
| 3 | Firmware | Gravity baseline stored only at fall onset | Continuous low-pass filter during STATE_NORMAL |
| 4 | Firmware | Variable declarations in switch case without scope | Added `{}` block around case body |
| 5 | Firmware | `testConnection()` false negative on ESP32-S3 | Boot delay + non-blocking warning |
| 6 | Firmware | I2C Error 263 from serial print blocking I2C | Gated prints behind 200ms interval |
| 7 | Visualization | Dual serial format broke parsing | Standardized to DATA/FALL prefix format |
| 8 | Visualization | Bridge parser checking wrong field count | Fixed `len(parts) == 5` |
| 9 | Visualization | Fall marker line compressed plot x-axis | Remove line when off-screen, lock x range |
| 10 | Visualization | No way to inspect waveform during live scroll | Added Pause/Resume button |

---

## References

- FreeRTOS EventGroups: https://www.freertos.org/event-groups-API.html
- C++ Switch Statement Scope: https://en.cppreference.com/w/cpp/language/switch
- PyQtGraph InfiniteLine: https://pyqtgraph.readthedocs.io/en/latest/api_reference/graphicsItems/infiniteline.html
- ESP32 I2C Timeout: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
