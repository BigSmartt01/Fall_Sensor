# Quantization Parameter Calibration Guide

## Overview

This guide helps you determine the correct quantization parameters for your TFLite Micro model. These parameters are critical for accurate inference.

## What are Quantization Parameters?

When a model is quantized to int8, float values are converted using this formula:

```
int8_value = round(float_value / scale) + zero_point
```

And dequantized back using:

```
float_value = (int8_value - zero_point) × scale
```

The **scale** and **zero_point** parameters depend on how your model was trained and quantized.

## Finding Quantization Parameters

### Method 1: Check TFLite Model Metadata

The most accurate way is to read the quantization parameters directly from your TFLite model file:

```python
import tensorflow as tf
import json

# Load the model
interpreter = tf.lite.Interpreter(model_path='fall_cnn_qat_int8.tflite')
interpreter.allocate_tensors()

# Get input details
input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

print("=== Input Quantization ===")
for i, inp in enumerate(input_details):
    print(f"Input {i}:")
    print(f"  Scale: {inp['quantization'][0]}")
    print(f"  Zero Point: {inp['quantization'][1]}")
    print(f"  Name: {inp['name']}")
    print(f"  Shape: {inp['shape']}")

print("\n=== Output Quantization ===")
for i, out in enumerate(output_details):
    print(f"Output {i}:")
    print(f"  Scale: {out['quantization'][0]}")
    print(f"  Zero Point: {out['quantization'][1]}")
    print(f"  Name: {out['name']}")
    print(f"  Shape: {out['shape']}")
```

### Method 2: Check TensorFlow Conversion Logs

When converting your model, TensorFlow prints quantization info:

```python
import tensorflow as tf

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.target_spec.supported_ops = [
    tf.lite.OpsSet.TFLITE_BUILTINS_INT8
]

# Print detailed info
converter.dump_graphviz_dir = './graphviz'  # Generates visualization

tflite_model = converter.convert()

# Check conversion report
print(converter.metrics  # May contain quantization statistics
```

## Common Quantization Patterns

### Pattern 1: Input in Range [-1, 1] (Normalized)

Typical for models trained with normalized inputs:

```cpp
// Example for accel/gyro data normalized to [-1, 1]
float input_scale = 0.0078125f;      // 2 / 256
int32_t input_zero_point = 0;        // Center at 0
```

### Pattern 2: Input in Physical Units (Not Normalized)

For models expecting raw sensor values:

```cpp
// Example for accel in [-20, 20] m/s²
float input_scale = 0.15625f;        // 40 / 256
int32_t input_zero_point = 0;

// Example for gyro in [-250, 250] °/s
float input_scale = 1.953125f;       // 500 / 256
int32_t input_zero_point = 0;
```

### Pattern 3: Asymmetric Quantization

For outputs or inputs with asymmetric ranges:

```cpp
// Example for probabilities [0, 1]
float output_scale = 0.00390625f;    // 1 / 256
int32_t output_zero_point = 0;

// Example for int8 full range [-128, 127]
float output_scale = 0.00390625f;    // 256 / 256 / 256
int32_t output_zero_point = -128;
```

## Validation Process

### Step 1: Run Test Inference

Modify [inference.cpp](../src/inference.cpp) temporarily to validate parameters:

```cpp
// Add this to runInference() for debugging
if (input_tensor->type == kTfLiteInt8) {
  int8_t* input_data = input_tensor->data.int8;
  
  // Log original and quantized values
  Serial.printf("Input (float): [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f]\n",
                input.accelX, input.accelY, input.accelZ,
                input.gyroX, input.gyroY, input.gyroZ);
  
  Serial.printf("Input (int8):  [%d, %d, %d, %d, %d, %d]\n",
                input_data[0], input_data[1], input_data[2],
                input_data[3], input_data[4], input_data[5]);
  
  // Check for clipping (bad sign = wrong parameters)
  for (int i = 0; i < 6; i++) {
    if (input_data[i] == 127 || input_data[i] == -128) {
      Serial.printf("WARNING: Input %d clipped to int8 range!\n", i);
    }
  }
}
```

### Step 2: Monitor Output Range

Log the quantized output to ensure it's in valid range:

```cpp
if (output_tensor->type == kTfLiteInt8) {
  int8_t* output_data = output_tensor->data.int8;
  
  Serial.printf("Output (int8): [%d, %d]\n",
                output_data[0], output_data[1]);
  Serial.printf("Output (float): [%.3f, %.3f]\n",
                output.normalProbability, output.fallProbability);
}
```

### Step 3: Compare with Python

Compare results from TensorFlow Lite on desktop:

```python
# Desktop validation
import tensorflow as tf
import numpy as np

interpreter = tf.lite.Interpreter(model_path='fall_cnn_qat_int8.tflite')
interpreter.allocate_tensors()

input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

# Create test input
test_input = np.array([[1.0, -0.5, 0.0, 10.0, -5.0, 2.0]], 
                       dtype=np.float32)

# Run inference
interpreter.set_tensor(input_details[0]['index'], test_input)
interpreter.invoke()

# Get output
output = interpreter.get_tensor(output_details[0]['index'])
print(f"Python result: {output}")

# Compare with ESP32 output for same input
```

## Calibration Adjustment Process

If values are getting clipped or inference accuracy is poor:

### If Input Values Are Clipping

**Symptom**: Input quantized values always -128 or 127

**Fix**: Increase input scale
```cpp
// Old (clipping)
float input_scale = 0.001f;

// New (larger range)
float input_scale = 0.01f;   // 10x larger range
```

### If Output Probabilities Are Inverted

**Symptom**: High fall probability for normal activity

**Fix**: Check zero_point sign
```cpp
// Old (wrong polarity)
int32_t output_zero_point = 127;

// New (correct)
int32_t output_zero_point = -128;
```

### If All Inputs Look Constant

**Symptom**: Quantized inputs don't change with sensor movement

**Fix**: Decrease input scale
```cpp
// Old (too coarse)
float input_scale = 1.0f;

// New (finer resolution)
float input_scale = 0.1f;
```

## Per-Channel vs Single Scale

Your model might use:

1. **Single scale** (simpler): One scale/zero_point for all channels
   ```cpp
   float input_scale = 0.01f;
   int32_t input_zero_point = 0;
   ```

2. **Per-channel scale** (more complex): Different scale per tensor
   ```cpp
   // More efficient but requires array handling
   float input_scales[6] = {0.01f, 0.01f, 0.01f, 0.1f, 0.1f, 0.1f};
   ```

Check your model's quantization spec to determine which is used.

## Reference Quantization Values

### For Models with Accel [m/s²] + Gyro [°/s] Input

If your training normalized to [-1, 1]:

```cpp
// Input
float input_scale = 0.0078125f;      // 2/256
int32_t input_zero_point = 0;

// Output (binary classification)
float output_scale = 0.00390625f;    // 1/256
int32_t output_zero_point = -128;
```

If your training used physical units directly:

```cpp
// Input (assuming ±20g accel, ±250°/s gyro)
float input_scale = 0.078125f;       // 20/256
int32_t input_zero_point = 0;

// Output (binary classification)
float output_scale = 0.00390625f;
int32_t output_zero_point = -128;
```

## Updating Parameters in Code

Once calibrated, update [inference.cpp](../src/inference.cpp):

```cpp
namespace {
  // ... other code ...
  
  // YOUR CALIBRATED VALUES HERE
  float input_scale = 0.0078125f;     // ← Update this
  int32_t input_zero_point = 0;       // ← Update this
  
  float output_scale = 0.00390625f;   // ← Update this
  int32_t output_zero_point = -128;   // ← Update this
}
```

Then rebuild and test:

```bash
platformio run -e esp32-c3 --target upload
platformio device monitor --baud 115200
```

## Troubleshooting Calibration

| Issue | Cause | Solution |
|-------|-------|----------|
| Always outputs class 0 | Output scale wrong | Increase output_scale |
| Noisy predictions | Input scale too fine | Increase input_scale |
| All inputs map to ±127 | Input scale too coarse | Decrease input_scale |
| Output probabilities inverted | Wrong zero_point sign | Flip sign of zero_point |
| 0% accuracy on training data | Quantization method wrong | Verify conversion parameters |

## Advanced: Custom Quantization Statistics

For very accurate calibration, compute statistics from your training data:

```python
import numpy as np

# Your training dataset
accel_data = np.random.randn(10000, 3) * 9.81  # ±3g
gyro_data = np.random.randn(10000, 3) * 100   # ±100°/s

# Combined statistics
all_data = np.concatenate([accel_data, gyro_data], axis=1)

min_val = np.min(all_data)
max_val = np.max(all_data)

# Calculate scale for int8 range [-128, 127]
scale = (max_val - min_val) / 255
zero_point = round(-128 - min_val / scale)

print(f"Recommended input_scale: {scale}")
print(f"Recommended input_zero_point: {zero_point}")
```

## When Quantization Fails

If after proper calibration accuracy is still poor:

1. **Retrain with QAT**: Use Quantization Aware Training in TensorFlow
   ```python
   import tensorflow_model_optimization as tfmot
   
   quantize_model = tfmot.quantization.keras.quantize_model(model)
   quantize_model.compile(...)
   quantize_model.fit(train_data, ...)
   ```

2. **Use larger model**: Increase model capacity before quantization

3. **Use float32**: Fall back to full precision (uses more memory/power)
   ```cpp
   // In inference.cpp: let float32 models run without quantization
   ```

## Support

For quantization issues specific to your model, check:
- Your model's original TensorFlow Lite conversion logs
- [TensorFlow Lite Quantization Guide](https://www.tensorflow.org/lite/performance/quantization_spec)
- Model metadata using `tf.lite.experimental.Analyzer.analyze(model_path='...')`
