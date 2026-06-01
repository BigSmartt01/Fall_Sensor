#include "inference.h"
#include "fall_model.h"

#include <tensorflow/lite/micro/all_ops_resolver.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kTensorArenaSize = 60 * 1024;
alignas(16) uint8_t tensor_arena[kTensorArenaSize];

const tflite::Model*      model       = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;

TfLiteTensor* input_tensor  = nullptr;
TfLiteTensor* output_tensor = nullptr;

float   input_scale       = 1.0f;
int32_t input_zero_point  = 0;
float   output_scale      = 1.0f;
int32_t output_zero_point = 0;

float sample_window[kInferenceWindowSize];
int   window_count       = 0;
int   window_write_index = 0;

int quantizeFloatToInt8(float value, float scale, int32_t zero_point) {
  if (scale == 0.0f) return 0;
  int32_t q = static_cast<int32_t>(lroundf(value / scale) + zero_point);
  return std::max(-128, std::min(127, q));
}

float dequantizeInt8ToFloat(int8_t value, float scale, int32_t zero_point) {
  return (static_cast<float>(value) - static_cast<float>(zero_point)) * scale;
}

}  // namespace

bool initializeInference() {
  Serial.println("Initializing TFLite Micro inference...");

  model = tflite::GetModel(fall_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("Model schema mismatch: got %d, expected %d\n",
                  model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  // AllOpsResolver includes all ops - fine for dev
  // Switch to MicroMutableOpResolver later to save flash
  static tflite::AllOpsResolver resolver;

// Create an ErrorReporter instance
static tflite::MicroErrorReporter micro_error_reporter;
tflite::ErrorReporter* error_reporter = &micro_error_reporter;

// Construct interpreter with 5 arguments
static tflite::MicroInterpreter static_interpreter(
    model, resolver, tensor_arena, kTensorArenaSize, error_reporter);

interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors() failed!");
    return false;
  }

  input_tensor  = interpreter->input(0);
  output_tensor = interpreter->output(0);

  if (input_tensor == nullptr || output_tensor == nullptr) {
    Serial.println("Failed to get input/output tensors!");
    return false;
  }

  input_scale       = input_tensor->params.scale;
  input_zero_point  = input_tensor->params.zero_point;
  output_scale      = output_tensor->params.scale;
  output_zero_point = output_tensor->params.zero_point;

  // Print input shape
  Serial.print("Input shape: ");
  int input_elements = 1;
  for (int i = 0; i < input_tensor->dims->size; i++) {
    Serial.printf("%d ", input_tensor->dims->data[i]);
    input_elements *= input_tensor->dims->data[i];
  }
  Serial.printf("(%d elements, type %d)\n", input_elements, input_tensor->type);
  Serial.printf("Input quant:  scale=%.8f  zero_point=%ld\n",
                input_scale, static_cast<long>(input_zero_point));

  // Print output shape
  Serial.print("Output shape: ");
  int output_elements = 1;
  for (int i = 0; i < output_tensor->dims->size; i++) {
    Serial.printf("%d ", output_tensor->dims->data[i]);
    output_elements *= output_tensor->dims->data[i];
  }
  Serial.printf("(%d elements, type %d)\n", output_elements, output_tensor->type);
  Serial.printf("Output quant: scale=%.8f  zero_point=%ld\n",
                output_scale, static_cast<long>(output_zero_point));

  // Sanity checks
  if (input_elements != kInferenceWindowSize) {
    Serial.printf("WARNING: model expects %d inputs, firmware window is %d\n",
                  input_elements, kInferenceWindowSize);
  }
  if (output_elements != 1) {
    Serial.printf("WARNING: expected 1 output (sigmoid), got %d\n", output_elements);
  }

  window_count       = 0;
  window_write_index = 0;

  getInferenceMemoryInfo();
  Serial.println("TFLite Micro initialized successfully.");
  return true;
}

void pushInferenceSample(float feature) {
  sample_window[window_write_index] = feature;
  window_write_index = (window_write_index + 1) % kInferenceWindowSize;
  if (window_count < kInferenceWindowSize) {
    window_count++;
  }
}

bool inferenceWindowReady() {
  return window_count >= kInferenceWindowSize;
}

bool runInference(InferenceOutput& output) {
  if (!inferenceWindowReady() || interpreter == nullptr ||
      input_tensor == nullptr || output_tensor == nullptr) {
    return false;
  }

  if (input_tensor->type != kTfLiteInt8) {
    Serial.println("runInference: expected int8 input tensor");
    return false;
  }

  int8_t*   input_data = input_tensor->data.int8;
  const int start      = window_write_index;  // oldest sample in ring buffer

  for (int i = 0; i < kInferenceWindowSize; i++) {
    const float value = sample_window[(start + i) % kInferenceWindowSize];
    input_data[i] = static_cast<int8_t>(
        quantizeFloatToInt8(value, input_scale, input_zero_point));
  }

  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Invoke() failed!");
    return false;
  }

  if (output_tensor->type != kTfLiteInt8) {
    Serial.println("runInference: expected int8 output tensor");
    return false;
  }

  const int8_t raw_out = output_tensor->data.int8[0];
  const float  dequant = dequantizeInt8ToFloat(raw_out, output_scale, output_zero_point);

  // Matches train_qat.py: predicted = 1 if output[0][0] > 0 else 0
  output.predictedClass    = (raw_out > 0) ? 1 : 0;
  output.fallProbability   = std::max(0.0f, std::min(1.0f, dequant));
  output.normalProbability = 1.0f - output.fallProbability;

  return true;
}

void getInferenceMemoryInfo() {
  Serial.println("=== Inference Memory ===");
  Serial.printf("Tensor arena size:  %d bytes\n", kTensorArenaSize);
  if (interpreter != nullptr) {
    const size_t used = interpreter->arena_used_bytes();
    Serial.printf("Arena used:         %u bytes (%.1f%%)\n",
                  static_cast<unsigned>(used),
                  100.0f * used / kTensorArenaSize);
  }
  Serial.printf("Window:             %d samples (%.2f s at 50 Hz)\n",
                kInferenceWindowSize,
                kInferenceWindowSize / 50.0f);
  Serial.println("========================");
}

void deinitializeInference() {
  interpreter  = nullptr;
  model        = nullptr;
  input_tensor = nullptr;
  output_tensor = nullptr;
  window_count       = 0;
  window_write_index = 0;
}