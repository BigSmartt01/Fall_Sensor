#include "inference.h"
#include "fall_model.h"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>

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

// ─── CONSECUTIVE DETECTION FILTER ────────────────────────────────────────────
// Fall is only confirmed when 3 consecutive inference runs each return
// probability >= 0.80. A single high reading is not enough.
// Resets to 0 any time a run returns below the threshold.
constexpr int   kRequiredConsecutiveDetections = 3;
constexpr float kFallProbabilityThreshold      = 0.80f;
static int      consecutiveFallCount           = 0;

int quantizeFloatToInt8(float value, float scale, int32_t zero_point) {
  if (scale == 0.0f) return 0;
  int32_t q = static_cast<int32_t>(lroundf(value / scale) + zero_point);
  return std::max<int32_t>(-128, std::min<int32_t>(127, q));
}

float dequantizeInt8ToFloat(int8_t value, float scale, int32_t zero_point) {
  return (static_cast<float>(value) - static_cast<float>(zero_point)) * scale;
}

}  // namespace

bool initializeInference() {
    printf("Initializing TFLite Micro inference...\n");

    model = tflite::GetModel(fall_model_data);

    if (model->version() != TFLITE_SCHEMA_VERSION) {
      printf("Model schema mismatch: got %lu, expected %lu\n",
                    (unsigned long)model->version(), (unsigned long)TFLITE_SCHEMA_VERSION);
      return false;
    }

    static tflite::MicroMutableOpResolver<8> resolver;
    resolver.AddAdd();
    resolver.AddConv2D();
    resolver.AddExpandDims();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddMaxPool2D();
    resolver.AddMul();
    resolver.AddReshape();

    static tflite::MicroInterpreter static_interpreter(
          model, resolver, tensor_arena, kTensorArenaSize);

    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        printf("AllocateTensors() failed!\n");
        return false;
    }

    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);

    if (input_tensor == nullptr || output_tensor == nullptr) {
      printf("Failed to get input/output tensors!\n");
      return false;
    }

    input_scale       = input_tensor->params.scale;
    input_zero_point  = input_tensor->params.zero_point;
    output_scale      = output_tensor->params.scale;
    output_zero_point = output_tensor->params.zero_point;

    printf("Input shape: ");
    int input_elements = 1;
    for (int i = 0; i < input_tensor->dims->size; i++) {
        printf("%d ", input_tensor->dims->data[i]);
        input_elements *= input_tensor->dims->data[i];
    }
    printf("(%d elements, type %d)\n", input_elements, input_tensor->type);
    printf("Input quant:  scale=%.8f  zero_point=%ld\n",
                  input_scale, static_cast<long>(input_zero_point));

    printf("Output shape: ");
    int output_elements = 1;
    for (int i = 0; i < output_tensor->dims->size; i++) {
        printf("%d ", output_tensor->dims->data[i]);
        output_elements *= output_tensor->dims->data[i];
    }
    printf("(%d elements, type %d)\n", output_elements, output_tensor->type);
    printf("Output quant: scale=%.8f  zero_point=%ld\n",
                  output_scale, static_cast<long>(output_zero_point));

    if (input_elements != kInferenceWindowSize) {
        printf("WARNING: model expects %d inputs, firmware window is %d\n",
                      input_elements, kInferenceWindowSize);
    }
    if (output_elements != 1) {
        printf("WARNING: expected 1 output (sigmoid), got %d\n", output_elements);
    }

    window_count            = 0;
    window_write_index      = 0;
    consecutiveFallCount    = 0;

    getInferenceMemoryInfo();
    printf("TFLite Micro initialized successfully.\n");
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
    printf("runInference: expected int8 input tensor\n");
    return false;
  }

  int8_t*   input_data = input_tensor->data.int8;
  const int start      = window_write_index;

  for (int i = 0; i < kInferenceWindowSize; i++) {
    const float value = sample_window[(start + i) % kInferenceWindowSize];
    input_data[i] = static_cast<int8_t>(
        quantizeFloatToInt8(value, input_scale, input_zero_point));
  }

  if (interpreter->Invoke() != kTfLiteOk) {
    printf("Invoke() failed!\n");
    return false;
  }

  if (output_tensor->type != kTfLiteInt8) {
    printf("runInference: expected int8 output tensor\n");
    return false;
  }

  const int8_t raw_out = output_tensor->data.int8[0];
  const float  dequant = dequantizeInt8ToFloat(raw_out, output_scale, output_zero_point);

  output.fallProbability   = std::max(0.0f, std::min(1.0f, dequant));
  output.normalProbability = 1.0f - output.fallProbability;

  // ─── CONSECUTIVE DETECTION FILTER ──────────────────────────────────────────
  // Increment counter if this run exceeds threshold, reset otherwise.
  // predictedClass is only 1 (fall) once kRequiredConsecutiveDetections
  // consecutive runs have all exceeded kFallProbabilityThreshold.
  // This prevents a single spurious high-probability output from triggering
  // a false alarm.
  if (output.fallProbability >= kFallProbabilityThreshold) {
    consecutiveFallCount++;
  } else {
    consecutiveFallCount = 0;
  }

  if (consecutiveFallCount >= kRequiredConsecutiveDetections) {
    output.predictedClass = 1;
    consecutiveFallCount  = 0;  // reset after confirmed - next event needs 3 fresh runs
    printf("ML: FALL CONFIRMED after %d consecutive detections (prob=%.3f)\n",
           kRequiredConsecutiveDetections, output.fallProbability);
  } else {
    output.predictedClass = 0;
    printf("ML: prob=%.3f consecutive=%d/%d\n",
           output.fallProbability,
           consecutiveFallCount,
           kRequiredConsecutiveDetections);
  }

  return true;
}

void getInferenceMemoryInfo() {
  printf("=== Inference Memory ===\n");
  printf("Tensor arena size:  %d bytes\n", kTensorArenaSize);
  if (interpreter != nullptr) {
    const size_t used = interpreter->arena_used_bytes();
    printf("Arena used:         %u bytes (%.1f%%)\n",
                  static_cast<unsigned>(used),
                  100.0f * used / kTensorArenaSize);
  }
  printf("Window:             %d samples (%.2f s at 200 Hz)\n",
                kInferenceWindowSize,
                kInferenceWindowSize / 200.0f);
  printf("========================\n");
}

void deinitializeInference() {
  interpreter   = nullptr;
  model         = nullptr;
  input_tensor  = nullptr;
  output_tensor = nullptr;
  window_count            = 0;
  window_write_index      = 0;
  consecutiveFallCount    = 0;
}