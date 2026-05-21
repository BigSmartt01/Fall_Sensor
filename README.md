# Fall Sensor - ESP32 Fall Detection System

## Overview

Fall Sensor is an intelligent fall detection system built on the ESP32 microcontroller using the MPU6050 6-axis motion sensor (accelerometer + gyroscope). The system uses a state machine algorithm to reliably detect human falls through multi-stage analysis, distinguishing true fall events from everyday motion and false alarms.

## Features

- **Real-time Fall Detection**: Processes accelerometer and gyroscope data at 50Hz
- **Multi-stage Verification**: Uses a 4-stage state machine to confirm falls
- **FreeRTOS Task-Based Architecture**: Two concurrent tasks for sensor processing and alerting
- **Thread-Safe State Sharing**: FreeRTOS EventGroups for safe inter-task communication
- **Adaptive Baseline**: Continuous low-pass filtering of gravity vector for orientation reference
- **Minimal False Positives**: Combines freefall detection, impact detection, stillness verification, and orientation change analysis

## Hardware Requirements

- **Microcontroller**: ESP32 (or ESP32-S3 variant)
- **IMU Sensor**: MPU6050 (6-axis motion sensor)
- **Communication**: I2C interface
- **GPIO Pins**: 
  - SDA: Pin 21 (configurable)
  - SCL: Pin 22 (configurable)
- **Optional**: Buzzer/Alert output on configurable pin

## Project Structure

```
Fall_Sensor/
│
├── firmware/        # ESP32/MPU6050 code
├── docs/            # Documentation, notes, diagrams
├── hardware/        # Schematics, PCB, BOM
├── visualization/   # All data visualization + monitoring
│   ├── influxdb/    # Configs, scripts, docker-compose
│   ├── grafana/     # Dashboards, JSON exports
│   └── dashboards/  # Custom dashboard definitions
├── README.md        # This document
└── tests/           # Experimental scripts, prototypes
```

## Building & Uploading

### Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) or PlatformIO IDE
- Python 3.6+ (for PlatformIO)
- USB cable (for ESP32 programming)

### Build

```bash
platformio run --environment esp32dev
```

### Upload to ESP32

```bash
platformio run --target upload --environment esp32dev
```

### Monitor Serial Output

```bash
platformio device monitor --baud 115200
```

## How It Works

### Fall Detection Algorithm

The system uses a 4-stage state machine:

#### **Stage 1: Freefall Detection (STATE_FREEFALL)**
- Monitors acceleration magnitude
- Freefall threshold: **< 3.0 m/s²** (gravity removed)
- Timeout: **400ms** (real freefall lasts 150-400ms)
- Triggers when device rapidly loses gravitational reference

#### **Stage 2: Impact Detection (STATE_IMPACT)**
- Detects sudden acceleration change
- Impact threshold: **> 20.0 m/s²**
- Confirms collision with ground/surface
- Window: 500ms

#### **Stage 3: Post-Impact Stillness (STATE_POST_IMPACT_STILLNESS)**
- Verifies person remains still after impact
- Checks gyroscope magnitude: **< 25 °/s**
- Acceleration variance: **< 1.0**
- Duration: **2.5 seconds** minimum
- Distinguishes true falls from stumbles or recoveries

#### **Stage 4: Orientation Verification (STATE_FALL_CONFIRMED)**
- Compares current body orientation to pre-fall baseline
- Orientation change threshold: **> 0.5 m/s²**
- Confirms person has changed position (not simply dropped and caught something)

### Gravity Vector Management

The system maintains a continuous baseline of the gravitational vector (true resting orientation) using a **low-pass filter** applied during normal state:

```
gravity = gravity × (1 - α) + current_accel × α
where α = 0.05 (filter coefficient)
```

This provides an accurate baseline for detecting significant orientation changes that occur during an actual fall.

## Configuration

### Sensor Calibration

Edit the threshold values in `src/main.cpp` to tune sensitivity:

```cpp
const float FREEFALL_THRESHOLD = 3.0;           // Lower = more sensitive
const float IMPACT_THRESHOLD = 20.0;            // Lower = more sensitive
const float GYRO_STILLNESS_THRESHOLD = 25.0;    // Lower = more strict
const float ORIENTATION_CHANGE_THRESHOLD = 0.5; // Higher = more strict
```

### I2C Pin Configuration

Modify pin definitions (default: 21 and 22):

```cpp
#define SDA_PIN 21
#define SCL_PIN 22
```

Alternative pins (commented out):
```cpp
//#define SDA_PIN 16
//#define SCL_PIN 17
```

## RTOS Task Structure

### Task 1: Fall Detection (Priority 2, 4096 bytes stack)
- Reads sensor data from MPU6050
- Executes state machine logic
- Sampling rate: 50Hz (20ms interval)
- Sets EventGroup bits when fall is confirmed

### Task 2: Activity Trigger (Priority 1, 2048 bytes stack)
- Waits for fall detection signal via EventGroup
- Triggers alert/buzzer logic
- Ready for integration with alert systems

## Serial Debug Output

The system outputs debug information every 200ms:

```
>STAGE 1: FREEFALL DETECTED
>STAGE 2: IMPACT DETECTED
>STAGE 3: POST-IMPACT STILLNESS - checking for 2-3sec...
>*** STAGE 4: FALL CONFIRMED ***
>Orientation change: 1.234
>Accel variance: 0.456
>>>> ALERTING - FALL DETECTED! <<<<
```

## Future Enhancements

- [ ] Add MQTT/BLE connectivity for remote alerting
- [ ] Machine learning-based activity classification
- [ ] Low-power sleep modes with motion wake
- [ ] Cloud data logging for statistical analysis
- [ ] Integration with medical alert systems
- [ ] Battery monitoring and alerts

## Troubleshooting

See [docs/ERRORS_AND_FIXES.md](docs/ERRORS_AND_FIXES.md) for common issues and solutions.

## Dependencies

- **espressif32**: ESP32 platform for Arduino
- **MPU6050**: 6-axis motion sensor library (v1.4.4+)
- **FreeRTOS**: Real-time kernel (included with Arduino ESP32)
- **Wire (I2C)**: Built-in Arduino I2C library

## License

[Specify your license here - e.g., MIT, Apache 2.0, etc.]

## Author & Contact

Smart Ayodele
ayosmart129@gmail.com

## References

- [MPU6050 Datasheet](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet1.pdf)
- [ESP32 Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)
- [FreeRTOS EventGroups](https://www.freertos.org/event-groups-API.html)
- [PlatformIO Docs](https://docs.platformio.org/)
