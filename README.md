# Fall Sensor - ESP32 Fall Detection System

## Overview

Fall Sensor is an intelligent fall detection system built on the ESP32-S3 microcontroller using the MPU6050 6-axis motion sensor (accelerometer + gyroscope). The system uses a state machine algorithm to reliably detect human falls through multi-stage analysis, distinguishing true fall events from everyday motion and false alarms.

Intended for elderly users, the device is designed for waist or lower-back placement to capture the body's center of mass during a fall - the most reliable position for distinguishing genuine falls from normal activity.

## Features

- Real-time fall detection processing accelerometer and gyroscope data at 50Hz
- Multi-stage 4-state verification combining freefall, impact, stillness, and orientation analysis
- FreeRTOS task-based architecture with two concurrent tasks for sensor processing and alerting
- Thread-safe state sharing via FreeRTOS EventGroups
- Adaptive gravity baseline using continuous low-pass filtering for accurate orientation reference
- Live data dashboard with PyQtGraph for real-time waveform monitoring during testing
- Simultaneous InfluxDB logging with Grafana for post-event analysis and threshold tuning
- Docker-based visualization stack for portable, reproducible setup

## Hardware

- Microcontroller: ESP32-S3
- IMU Sensor: MPU6050 (6-axis, I2C address 0x68)
- Interface: I2C
- SDA: Pin 16, SCL: Pin 17
- Planned hardware upgrade: BMI160 (lower noise, lower power, better suited for wearable use)
- Enclosure: TBD (perfboard build in progress, components on order)

## Project Structure

```
Fall_Sensor/
├── firmware/           # ESP32-S3 / MPU6050 PlatformIO project
├── docs/               # Documentation, notes, diagrams
│   └── ERRORS_AND_FIXES.md
├── hardware/           # Schematics, PCB, BOM
├── visualization/      # All data visualization and monitoring tools
│   ├── bridge.py       # Serial to InfluxDB bridge script
│   ├── dashboard.py    # PyQtGraph real-time dashboard
│   ├── docker-compose.yml
│   ├── influxdb/       # InfluxDB configs
│   ├── grafana/        # Grafana dashboard exports
│   └── dashboards/     # Custom dashboard definitions
├── tests/              # Experimental scripts, prototypes
├── requirements.txt    # Python dependencies
├── .gitignore
└── README.md
```

## Python Dependencies

Install all visualization dependencies:

```bash
pip install -r requirements.txt
```

Contents of `requirements.txt`:
```
pyserial
influxdb-client
pyqtgraph
PyQt6
numpy
```

## Visualization Stack

### Real-Time Dashboard (PyQtGraph)

The primary tool for active fall simulation testing. Runs over USB serial and provides serial-plotter-speed waveform updates with fall event markers.

```bash
cd visualization
python dashboard.py
```

Features:
- Scrolling accel and gyro magnitude waveforms updated every 20ms
- Dashed threshold lines for freefall (3.0), impact (20.0), and stillness (25.0)
- State machine card with color coding per stage
- Red vertical marker injected at the exact sample where a fall was confirmed
- Marker scrolls with the waveform and is removed cleanly when it leaves the screen
- Pause/Resume button to freeze the graph and inspect fall events without losing live data
- Simultaneous InfluxDB write in the background

### InfluxDB + Grafana (Docker)

Used for post-event analysis, long-term logging, and threshold tuning from stored data.

```bash
cd visualization
docker compose up -d
```

- InfluxDB UI: http://localhost:8086
- Grafana: http://localhost:3000
- Bucket: `fallsensor`
- Org: `smartlab`

### Serial Output Format

The firmware outputs a consistent CSV-style format parseable by both the bridge and dashboard:

```
DATA,<timestamp_ms>,<accelMag>,<gyroMag>,<state>
FALL,<timestamp_ms>
```

Example:
```
DATA,27496,9.95,26.69,0
DATA,28816,4.31,505.80,1
FALL,31522
```

State values: 0=NORMAL, 1=FREEFALL, 2=IMPACT, 3=POST-IMPACT STILLNESS, 4=FALL CONFIRMED

## Building and Uploading Firmware

### Prerequisites

- PlatformIO (VS Code extension or CLI)
- USB cable

### Build

```bash
platformio run --environment esp32-s3-devkitc-1
```

### Upload

```bash
platformio run --target upload --environment esp32-s3-devkitc-1
```

### Monitor Serial

```bash
platformio device monitor --baud 115200
```

Note: close serial monitor before running dashboard.py or bridge.py - only one process can hold the COM port at a time.

## Fall Detection Algorithm

The system uses a 4-stage state machine:

**Stage 1: Freefall Detection (STATE_FREEFALL)**
- Accel magnitude drops below 3.0 m/s²
- Timeout: 400ms (real human freefall lasts 150-400ms)
- Gravity vector continuously maintained via low-pass filter during STATE_NORMAL

**Stage 2: Impact Detection (STATE_IMPACT)**
- Accel magnitude exceeds 20.0 m/s²
- Window: 500ms after freefall

**Stage 3: Post-Impact Stillness (STATE_POST_IMPACT_STILLNESS)**
- Gyro magnitude below 25.0 deg/s
- Accel variance below 1.0
- Sustained for 2.5 seconds minimum

**Stage 4: Orientation Verification (STATE_FALL_CONFIRMED)**
- Orientation change vs pre-fall gravity baseline exceeds 0.5 m/s²
- Confirms body position has changed, not a stumble or recovery

### Gravity Vector Management

A low-pass filter continuously tracks the resting orientation during normal state:

```
gravity = gravity x (1 - alpha) + current_accel x alpha
alpha = 0.05
```

This ensures the orientation baseline is always current before any fall begins.

## Threshold Tuning

Edit in `firmware/src/main.cpp`:

```cpp
const float FREEFALL_THRESHOLD = 3.0;
const float IMPACT_THRESHOLD = 20.0;
const float GYRO_STILLNESS_THRESHOLD = 25.0;
const float ORIENTATION_CHANGE_THRESHOLD = 0.5;
const unsigned long FREEFALL_TIMEOUT = 400;
const unsigned long IMPACT_WINDOW = 500;
const unsigned long POST_IMPACT_STILLNESS_DURATION = 2500;
```

Use the PyQtGraph dashboard during physical testing to observe threshold crossings in real time, then use Grafana to review stored data from each session.

## RTOS Task Structure

**Task 1: Fall Detection (Priority 2, 4096 bytes stack)**
- Reads MPU6050 via I2C at 50Hz
- Runs state machine logic
- Signals fall confirmation via FreeRTOS EventGroup

**Task 2: Activity Trigger (Priority 1, 2048 bytes stack)**
- Waits on EventGroup bit
- Triggers alert output (buzzer, BLE notification, etc.)

## Future Work

- BMI160 sensor swap (lower noise, lower power consumption)
- BLE WiFi provisioning for wireless data streaming to Grafana
- MQTT or HTTP POST for wireless InfluxDB writes
- Low-power sleep with motion wake interrupt
- Buzzer and alert hardware integration
- Enclosure design for waist/belt clip wearable form factor
- Machine learning activity classifier to reduce false positives

## Troubleshooting

See [docs/ERRORS_AND_FIXES.md](docs/ERRORS_AND_FIXES.md) for documented issues and fixes.

## Dependencies

- espressif32: ESP32 platform for PlatformIO
- electroniccats/MPU6050: IMU sensor library
- FreeRTOS: Included with Arduino ESP32 core
- Wire: Built-in Arduino I2C library

## Author

Smart Ayodele
ayosmart129@gmail.com
