# Fall Sensor - ESP32 Fall Detection System

## Overview

Fall Sensor is an intelligent fall detection system built on the ESP32 NodeMCU (final target: ESP32-C3 Super Mini) using the MPU6050 6-axis motion sensor. The system combines a rule-based state machine with a trained CNN neural network for high-confidence fall detection, targeting elderly users with waist/lower-back placement.

The dual-layer approach uses the state machine as a fast pre-filter and the ML model as a confirmation layer, minimizing both false positives and false negatives.

## Features

- Real-time fall detection at 50Hz via 4-stage state machine
- CNN-based ML confirmation layer (94.78% accuracy, 97.63% fall recall)
- Dual confirmation: fall only confirmed when both state machine and ML agree
- FreeRTOS task-based architecture with three concurrent tasks
- Thread-safe state sharing via FreeRTOS EventGroups
- Adaptive gravity baseline using continuous low-pass filtering
- Live PyQtGraph dashboard with real-time waveform monitoring
- InfluxDB + Grafana logging via Docker for post-event analysis
- Quantization-aware trained int8 TFLite model (16.9KB flash, 47.1KB RAM)

## Hardware

- Current dev board: ESP32 NodeMCU
- Final target: ESP32-C3 Super Mini (low power, portable)
- IMU: MPU6050 (current) - switching to BMI160 on AliExpress delivery
- Interface: I2C - SDA: Pin 16, SCL: Pin 17
- I2C address: 0x68
- AliExpress batch order placed May 2026

## Project Structure

```
Fall_Sensor/
├── firmware/               # ESP32 PlatformIO project
│   ├── src/
│   │   └── main.cpp        # State machine + ML inference firmware
│   └── include/
│       └── fall_model.h    # Auto-generated TFLite model header
├── docs/
│   └── ERRORS_AND_FIXES.md
├── hardware/               # Schematics, BOM
├── visualization/
│   ├── bridge.py           # Serial to InfluxDB bridge
│   ├── dashboard.py        # PyQtGraph real-time dashboard
│   └── docker-compose.yml  # InfluxDB + Grafana stack
├── tests/
│   ├── three_classes/      # ML training pipeline
│   │   ├── inspect_dataset.py
│   │   ├── train_cnn.py
│   │   ├── train_qat.py
│   │   ├── quantize_model.py
│   │   ├── check_model.py
│   │   ├── convert_to_header.py
│   │   ├── fall_cnn_model.keras
│   │   ├── fall_cnn_model.tflite
│   │   └── fall_cnn_qat_int8.tflite
│   ├── edge_impulse_data/      # Full windowed dataset (2.41GB)
│   ├── edge_impulse_data_v2/   # Renamed files with subject ID (2.41GB)
│   └── edge_impulse_sample_v3/ # Balanced SE-weighted sample (550MB)
├── requirements.txt
├── .gitignore
└── README.md
```

## ML Pipeline

### Dataset

SisFall dataset - 4,505 files covering 19 ADL types and 15 fall types from 38 subjects (23 adults SA, 15 elderly SE aged 60-75). Sensor: MMA8451Q accelerometer (columns 7-9) + ITG3200 gyroscope (columns 4-6) at 200Hz.

Download: https://www.kaggle.com/datasets/miguelcleon/sisfall

### Data Preparation

```bash
python tests/prepare_dataset.py    # window and convert to CSV
python tests/sample_dataset.py     # balance SE/SA ratio
```

- Window size: 200 samples (1 second at 200Hz)
- Step size: 100 samples (50% overlap)
- Output: 153,705 windows (52,019 fall, 101,686 ADL)
- Final balanced sample: 17,124 fall + 17,124 ADL (SE-weighted, 30% SA)

### Model Architecture

Lightweight 1D CNN trained with quantization-aware training from the start:

```
Input (512, 1)
Conv1D(8, kernel=5) + BatchNorm + MaxPool(4)
Conv1D(16, kernel=3) + BatchNorm + MaxPool(4)
Conv1D(32, kernel=3) + BatchNorm + MaxPool(4)
Flatten
Dense(16) + Dropout(0.3)
Output sigmoid
```

Total parameters: ~15,000 (vs 540,000 in the full model)

### Training

```bash
cd tests/three_classes
python train_qat.py
```

- Step 1: Train base model (20 epochs, early stopping)
- Step 2: Apply QAT fine-tuning (10 epochs, lower LR)
- Step 3: Convert to int8 TFLite
- Step 4: Verify accuracy and RAM usage

### Model Performance

| Metric | Value |
|---|---|
| Overall accuracy | 94.78% |
| Fall recall | 97.63% |
| ADL recall | 94.72% |
| Flash size | 16.9 KB |
| Inference RAM | 47.1 KB |
| ESP32-C3 flash limit | 4096 KB |
| ESP32-C3 RAM limit | 320 KB |

### Generating the C Header

```bash
python tests/three_classes/convert_to_header.py
```

Outputs `firmware/include/fall_model.h` - the model embedded as a uint8_t array.

## How It Works

### Dual-Layer Detection

```
Sensor (50Hz)
     |
     v
State Machine (fast, rule-based)
     |
     |-- No freefall --> continue
     |
     v
State: POST_IMPACT_STILLNESS confirmed
     |
     v
ML Inference (CNN, runs every 2 seconds)
     |
     |-- ML says NO --> discard (false alarm)
     |-- ML says YES --> FALL CONFIRMED
     |
     v
Alert triggered
```

### State Machine Stages

**Stage 1: Freefall** - accel magnitude drops below 3.0 m/s², timeout 400ms

**Stage 2: Impact** - accel magnitude exceeds 20.0 m/s² within 500ms of freefall

**Stage 3: Post-impact stillness** - gyro below 25 deg/s, accel variance below 1.0, sustained 2.5 seconds

**Stage 4: Orientation verification** - orientation change vs pre-fall baseline exceeds 0.5 m/s²

### Gravity Vector

Continuously updated during STATE_NORMAL using low-pass filter (alpha=0.05):
```
gravity = gravity x (1 - alpha) + current_accel x alpha
```

### ML Inference

Runs as a separate FreeRTOS task (priority 1) every 2 seconds on a 512-sample sliding window of accel magnitude. Input quantized float32 to int8 using model scale/zero_point. Output probability threshold: 0.5.

## Serial Output Format

```
DATA,<timestamp_ms>,<accelMag>,<gyroMag>,<state>
FALL,<timestamp_ms>
ML: prob=<value> fall=<YES|NO>
```

State values: 0=NORMAL, 1=FREEFALL, 2=IMPACT, 3=POST-IMPACT STILLNESS, 4=FALL CONFIRMED

## Visualization

### Real-Time Dashboard

```bash
cd visualization
python dashboard.py
```

Close VS Code serial monitor first. Updates every 20ms. Pause/Resume button for fall event inspection.

### InfluxDB + Grafana

```bash
cd visualization
docker compose up -d
```

- InfluxDB: http://localhost:8086
- Grafana: http://localhost:3000
- Bucket: fallsensor, Org: smartlab

### Python Bridge (serial to InfluxDB)

```bash
python visualization/bridge.py
```

## Building Firmware

```bash
platformio run --environment esp32-s3-devkitc-1
platformio run --target upload --environment esp32-s3-devkitc-1
platformio device monitor --baud 115200
```

Note: close serial monitor before running dashboard.py or bridge.py.

## Python Dependencies

```bash
pip install -r requirements.txt
```

```
pyserial
influxdb-client
pyqtgraph
PyQt6
numpy
tensorflow
tensorflow-model-optimization
scikit-learn
```

## Future Work

- BMI160 sensor swap on hardware delivery
- Retrain CNN on BMI160-equivalent sensor axes from SisFall
- BLE WiFi provisioning for wireless Grafana streaming
- HTTP POST to InfluxDB over WiFi
- Buzzer and alert hardware integration
- Enclosure: waist/belt-clip form factor, 3D printed
- Collect real elderly fall simulation data for fine-tuning

## Troubleshooting

See docs/ERRORS_AND_FIXES.md

## Dependencies

- espressif32: ESP32 platform
- electroniccats/MPU6050: IMU library
- TFLite Micro: tensorflow/lite/micro (ESP-IDF component)
- FreeRTOS: included with Arduino ESP32 core

## Author

Smart Ayodele
ayosmart129@gmail.com
