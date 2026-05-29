#include <Arduino.h>
#include <Wire.h>
#include <MPU6050.h>
#include "inference.h"

#define SDA_PIN 21
#define SCL_PIN 22

//#define SDA_PIN 16
//#define SCL_PIN 17

MPU6050 mpu;

// FreeRTOS EventGroup for thread-safe state sharing
EventGroupHandle_t eventGroup = NULL;
#define FALL_DETECTED_BIT (1 << 0)
#define ACTIVITY_TRIGGERED_BIT (1 << 1)

// Fall detection state machine
enum FallState {
  STATE_NORMAL,
  STATE_FREEFALL,
  STATE_IMPACT,
  STATE_POST_IMPACT_STILLNESS,
  STATE_FALL_CONFIRMED
};

// Global state variables
static FallState fallState = STATE_NORMAL;
static unsigned long freefallStartTime = 0;
static unsigned long impactStartTime = 0;
static unsigned long postImpactStartTime = 0;

// Gravity vector storage and low-pass filter for continuous update
static float gravityX = 0, gravityY = 0, gravityZ = 0;
const float GRAVITY_FILTER_ALPHA = 0.05;  // Low-pass filter coefficient for gravity update

// Acceleration variance tracking
static float accelHistory[10] = {0};
static int historyIndex = 0;
const int HISTORY_SIZE = 10;

// Thresholds
const float FREEFALL_THRESHOLD = 3.0;
const float IMPACT_THRESHOLD = 20.0;
const float GYRO_STILLNESS_THRESHOLD = 25.0;
const float ORIENTATION_CHANGE_THRESHOLD = 0.5;  // Large value = orientation changed
const unsigned long FREEFALL_TIMEOUT = 400;  // 400ms max freefall
const unsigned long IMPACT_WINDOW = 500;      // 500ms to confirm impact
const unsigned long POST_IMPACT_STILLNESS_DURATION = 2500;  // 2.5 sec post-impact check

// Task handles
TaskHandle_t fallDetectionTaskHandle = NULL;
TaskHandle_t activityTriggerTaskHandle = NULL;

// Shared sensor data queue
struct SensorData {
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  float accelMag, gyroMag;
  unsigned long timestamp;
};

// Forward declarations
void fallDetectionTask(void *pvParameters);
void activityTriggerTask(void *pvParameters);

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Create FreeRTOS EventGroup for thread-safe state sharing
  eventGroup = xEventGroupCreate();
  if (eventGroup == NULL) {
    Serial.println("Failed to create event group!");
    return;
  }

  Serial.println("Initializing MPU6050...");
  mpu.initialize();

  if (mpu.testConnection()) {
    Serial.println("MPU6050 connection successful!");
  } else {
    Serial.println("MPU6050 connection failed!");
  }

  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2);
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_500);

  // Initialize TFLite Micro inference engine
  Serial.println("\n=== Initializing Neural Network ===");
  if (!initializeInference()) {
    Serial.println("ERROR: Failed to initialize inference engine!");
    // Continue anyway - fall back to rule-based detection
  } else {
    Serial.println("Neural network initialized successfully!");
  }
  Serial.println("====================================\n");

  // Create RTOS tasks
  xTaskCreate(
    fallDetectionTask,      /* Function to implement the task */
    "Fall Detection",       /* Name of the task */
    4096,                   /* Stack size in words */
    NULL,                   /* Task input parameter */
    2,                      /* Priority */
    &fallDetectionTaskHandle /* Task handle */
  );

  xTaskCreate(
    activityTriggerTask,    /* Function to implement the task */
    "Activity Trigger",     /* Name of the task */
    2048,                   /* Stack size in words */
    NULL,                   /* Task input parameter */
    1,                      /* Priority */
    &activityTriggerTaskHandle /* Task handle */
  );

  Serial.println("RTOS tasks created.");
}

// Conversion helpers for MPU6050 raw values

// Accelerometer: raw → g
float accelRawToG(int16_t raw, int range) {
  // range can be 2, 4, 8, or 16 (g)
  float scale;
  switch (range) {
    case 2:  scale = 16384.0; break;
    case 4:  scale = 8192.0;  break;
    case 8:  scale = 4096.0;  break;
    case 16: scale = 2048.0;  break;
    default: scale = 16384.0; break;
  }
  return (float)raw / scale; // result in g
}

// Accelerometer: raw → m/s²
float accelRawToMS2(int16_t raw, int range) {
  return accelRawToG(raw, range) * 9.81;
}

// Gyroscope: raw → °/s
float gyroRawToDPS(int16_t raw, int range) {
  float scale;
  switch (range) {
    case 250:  scale = 131.0;  break;
    case 500:  scale = 65.5;   break;
    case 1000: scale = 32.8;   break;
    case 2000: scale = 16.4;   break;
    default:   scale = 131.0;  break;
  }
  return (float)raw / scale;
}

float magnitude(float x, float y, float z) {
  return sqrt(x*x + y*y + z*z);
}

// Calculate acceleration variance from history
float getAccelVariance(float newAccel) {
  accelHistory[historyIndex] = newAccel;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  
  float mean = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    mean += accelHistory[i];
  }
  mean /= HISTORY_SIZE;
  
  float variance = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    variance += (accelHistory[i] - mean) * (accelHistory[i] - mean);
  }
  return variance / HISTORY_SIZE;
}

// Compare gravity vectors to detect orientation change
float getOrientationChange(float currentX, float currentY, float currentZ) {
  float dx = currentX - gravityX;
  float dy = currentY - gravityY;
  float dz = currentZ - gravityZ;
  return magnitude(dx, dy, dz);
}

// Store current acceleration as gravity reference
void storeGravityVector(float x, float y, float z) {
  gravityX = x;
  gravityY = y;
  gravityZ = z;
}

// Update gravity vector using low-pass filter (continuous during normal state)
void updateGravityVector(float x, float y, float z) {
  gravityX = gravityX * (1.0 - GRAVITY_FILTER_ALPHA) + x * GRAVITY_FILTER_ALPHA;
  gravityY = gravityY * (1.0 - GRAVITY_FILTER_ALPHA) + y * GRAVITY_FILTER_ALPHA;
  gravityZ = gravityZ * (1.0 - GRAVITY_FILTER_ALPHA) + z * GRAVITY_FILTER_ALPHA;
}

// Single feature for the 1x512x1 CNN input. x_train_3 values are ~[-2.7, 1.9] (float32).
// Must match offline preprocessing; default is vertical accel in g (not m/s²).
static float featureForInference(const SensorData& s) {
  return s.accelY / 9.81f;
}

// Task 1: Fall Detection Logic
void fallDetectionTask(void *pvParameters) {
  SensorData sensorData;
  InferenceOutput inferenceOutput;
  
  while (1) {
    // Read sensor data
    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    
    // Convert to physical units
    sensorData.accelX = accelRawToMS2(ax, 2);
    sensorData.accelY = accelRawToMS2(ay, 2);
    sensorData.accelZ = accelRawToMS2(az, 2);
    sensorData.gyroX = gyroRawToDPS(gx, 500);
    sensorData.gyroY = gyroRawToDPS(gy, 500);
    sensorData.gyroZ = gyroRawToDPS(gz, 500);
    
    sensorData.accelMag = magnitude(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
    sensorData.gyroMag = magnitude(sensorData.gyroX, sensorData.gyroY, sensorData.gyroZ);
    sensorData.timestamp = millis();
    
    pushInferenceSample(featureForInference(sensorData));

    bool inferenceValid = false;
    if (inferenceWindowReady() && runInference(inferenceOutput)) {
      inferenceValid = true;
    }
    
    // Fall detection state machine
    switch(fallState) {
      case STATE_NORMAL:
        // Stage 1: Continuously update gravity reference during normal state
        updateGravityVector(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
        
        // Monitor for freefall onset (rule-based + optional NN confirmation)
        if (sensorData.accelMag < FREEFALL_THRESHOLD) {
          fallState = STATE_FREEFALL;
          freefallStartTime = millis();
          Serial.println("STAGE 1: FREEFALL DETECTED");
        }
        break;

      case STATE_FREEFALL:
        // Stage 2: In freefall, waiting for impact
        if (sensorData.accelMag > IMPACT_THRESHOLD) {
          fallState = STATE_IMPACT;
          impactStartTime = millis();
          Serial.println("STAGE 2: IMPACT DETECTED");
        } 
        // Timeout on freefall (false alarm)
        else if (millis() - freefallStartTime > FREEFALL_TIMEOUT) {
          fallState = STATE_NORMAL;
          Serial.println("Freefall timeout - false alarm");
        }
        break;

      case STATE_IMPACT:
        // Stage 3: Post-impact stillness check
        if (sensorData.gyroMag < GYRO_STILLNESS_THRESHOLD) {
          fallState = STATE_POST_IMPACT_STILLNESS;
          postImpactStartTime = millis();
          Serial.println("STAGE 3: POST-IMPACT STILLNESS - checking for 2-3sec...");
        }
        // Timeout if impact resolves too quickly (false alarm)
        else if (millis() - impactStartTime > IMPACT_WINDOW) {
          fallState = STATE_NORMAL;
          Serial.println("Impact timeout - false alarm");
        }
        break;

      case STATE_POST_IMPACT_STILLNESS: {
        // Stage 3 continued: Confirm stillness + orientation change + NN confidence
        float accelVariance = getAccelVariance(sensorData.accelMag);
        float orientationChange = getOrientationChange(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
        
        unsigned long timeSinceImpact = millis() - postImpactStartTime;
        
        // Determine if this is a fall based on:
        // 1. Rule-based checks: stillness, variance, orientation
        // 2. Optional NN confidence if inference is valid
        bool ruleBasedFall = (timeSinceImpact >= POST_IMPACT_STILLNESS_DURATION && 
                              sensorData.gyroMag < GYRO_STILLNESS_THRESHOLD && 
                              accelVariance < 1.0 &&
                              orientationChange > ORIENTATION_CHANGE_THRESHOLD);
        
        // Matches train_qat.py threshold: fall if int8 output > 0 (~ prob >= 0.5)
        bool nnConfirmedFall =
            inferenceValid && inferenceOutput.predictedClass == 1;

        if (ruleBasedFall && (!inferenceValid || nnConfirmedFall)) {
          
          fallState = STATE_FALL_CONFIRMED;
          xEventGroupSetBits(eventGroup, FALL_DETECTED_BIT);
          Serial.println("*** STAGE 4: FALL CONFIRMED ***");
          Serial.print("Rule-based: ");
          Serial.print(ruleBasedFall ? "YES" : "NO");
          Serial.print(" | NN Confidence: ");
          Serial.print(inferenceOutput.fallProbability, 3);
          Serial.print(" | Orientation: ");
          Serial.println(orientationChange, 3);
        }
        // Timeout if stillness doesn't hold (false alarm)
        else if (timeSinceImpact > 5000) {
          fallState = STATE_NORMAL;
          Serial.println("Post-impact stillness timeout - false alarm");
        }
        break;
      }

      case STATE_FALL_CONFIRMED:
        Serial.printf("FALL,%lu\n", millis()); // add this line to signal fall event to Python bridge
        // Signal activity trigger task
        xEventGroupSetBits(eventGroup, ACTIVITY_TRIGGERED_BIT);
        fallState = STATE_NORMAL;  // Reset to normal state
        break;
    }

    /*
    // Debug output for serial plotter
    Serial.print(">accelMag: ");
    Serial.print(sensorData.accelMag, 2);
    Serial.print(",gyroMag: ");
    Serial.println(sensorData.gyroMag, 2);

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 200) {
      lastPrint = millis();
      Serial.print("accelMag: ");
      Serial.print(sensorData.accelMag, 2);
      Serial.print(" | gyroMag: ");
      Serial.print(sensorData.gyroMag, 2);
      Serial.print(" | State: ");
      Serial.println(fallState);
    }
    */

    // Debug output - single consistent format for Python bridge
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 20) {
      lastPrint = millis();
      Serial.printf("DATA,%lu,%.2f,%.2f,%d\n",
          sensorData.timestamp,
          sensorData.accelMag,
          sensorData.gyroMag,
          (int)fallState
      );
  }
    
    vTaskDelay(20 / portTICK_PERIOD_MS);  // 50Hz sampling
  }
}

// Task 2: Activity Trigger (Buzzer/Alert)
void activityTriggerTask(void *pvParameters) {
  EventBits_t uxBits;
  
  while (1) {
    // Wait for ACTIVITY_TRIGGERED_BIT with 100ms timeout
    uxBits = xEventGroupWaitBits(
      eventGroup,
      ACTIVITY_TRIGGERED_BIT,
      pdTRUE,  // Clear the bit on exit
      pdFALSE, // Don't wait for all bits
      100 / portTICK_PERIOD_MS
    );
    
    if ((uxBits & ACTIVITY_TRIGGERED_BIT) != 0) {
      Serial.println(">>>> ALERTING - FALL DETECTED! <<<<");
      // TODO: Implement buzzer logic here
      // digitalWrite(BUZZER_PIN, HIGH);
      // vTaskDelay(500 / portTICK_PERIOD_MS);
      // digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

void loop() {
  // Main loop is now handled by RTOS tasks
  delay(10);  // Keep main loop alive
}
