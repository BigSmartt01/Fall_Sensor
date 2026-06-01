# Firmware Quick Start

[← Documentation index](README.md) · [TFLite inference](TFLITE_INFERENCE.md) · [Project README](../README.md)

## Prerequisites

PlatformIO, ESP32-C3 or ESP32 dev board, MPU6050, USB data cable.

## Wiring (`firmware/src/main.cpp`)

| MPU6050 | ESP32 |
|---------|--------|
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO **21** |
| SCL | GPIO **22** |

Uncomment GPIO 16/17 in `main.cpp` if your board uses those instead.

## Build and upload

```bash
cd firmware
platformio run -e esp32-c3 --target upload
platformio device monitor -e esp32-c3
```

Use `-e esp32dev` for a classic ESP32 Dev Module. Library: `tanakamasayuki/TensorFlowLite_ESP32`.

## Boot check

Expect neural-network init lines with `Input shape: 1 512 1` and `type 9` (INT8). See [TFLITE_INFERENCE.md](TFLITE_INFERENCE.md) if init fails.

## Test a fall

1. Steady hold → 2. Low accel (freefall) → 3. Sharp spike (impact) → 4. Still ~3 s.

```
STAGE 1: FREEFALL DETECTED
STAGE 2: IMPACT DETECTED
STAGE 3: POST-IMPACT STILLNESS ...
*** STAGE 4: FALL CONFIRMED ***
FALL,<timestamp>
```

NN confirmation needs ~10 s of samples after boot ([details](TFLITE_INFERENCE.md)).

## Tune (`firmware/src/main.cpp`)

```cpp
const float FREEFALL_THRESHOLD = 3.0;       // m/s²
const float IMPACT_THRESHOLD = 20.0;
const float GYRO_STILLNESS_THRESHOLD = 25.0;
const unsigned long POST_IMPACT_STILLNESS_DURATION = 2500;
```

ML feature default: `accelY / 9.81f` in `featureForInference()` — must match `x_train_3`.

## Troubleshooting

| Problem | Action |
|---------|--------|
| Build / lib errors | `platformio run -e esp32-c3 -v` |
| Upload fails | `pio device list`; set `--upload-port` |
| Garbled serial | 115200 baud |
| `AllocateTensors()` failed | Increase `kTensorArenaSize` in `inference.cpp` |
| False positives / NN mismatch | Thresholds; [quantization](QUANTIZATION_CALIBRATION.md); feature alignment |
| MPU6050 not found | Wiring, 3.3 V, I2C pins — [ERRORS_AND_FIXES.md](ERRORS_AND_FIXES.md) |
