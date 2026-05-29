#ifndef FALL_INFERENCE_H
#define FALL_INFERENCE_H

#include <stdint.h>

// Must match train_qat.py / x_train_3: (batch, 512, 1) float32 windows.
constexpr int kInferenceWindowSize = 512;

struct InferenceOutput {
  float fallProbability;    // P(fall) after dequantization, ~0.0–1.0
  float normalProbability;  // 1.0 - fallProbability
  uint8_t predictedClass;   // 0 = normal, 1 = fall (matches int8 output > 0)
};

bool initializeInference();

// Append one float feature to the sliding window (call at sensor rate, e.g. 50 Hz).
void pushInferenceSample(float feature);

// True after kInferenceWindowSize samples have been collected.
bool inferenceWindowReady();

// Run the model on the current window. Returns false if not ready or invoke failed.
bool runInference(InferenceOutput& output);

void getInferenceMemoryInfo();
void deinitializeInference();

#endif  // FALL_INFERENCE_H
