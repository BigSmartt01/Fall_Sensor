#include <Arduino.h>
#include <Wire.h>
#include <DFRobot_BMI160.h>
#include "inference.h"

// Pin definitions
// NodeMCU dev module (Previous test module)
//#define SDA_PIN 21
//#define SCL_PIN 22

// ESP32-C3 Super Mini (current)
#define SDA_PIN     8
#define SCL_PIN     9
#define INT1_PIN    5
#define INT2_PIN    6
#define BUZZER_PIN  2

//const int8_t BMI160_I2C_ADDR = 0x68;  // SDIO pin connected to GND

DFRobot_BMI160 bmi160;

// ─── FREERTOS ─────────────────────────────────────────────────────────────────
EventGroupHandle_t eventGroup = NULL;
#define FALL_DETECTED_BIT      (1 << 0)
#define ACTIVITY_TRIGGERED_BIT (1 << 1)

// ─── STATE MACHINE ────────────────────────────────────────────────────────────
enum FallState {
  STATE_NORMAL,
  STATE_FREEFALL,
  STATE_IMPACT,
  STATE_POST_IMPACT_STILLNESS,
  STATE_FALL_CONFIRMED
};

static FallState     fallState           = STATE_NORMAL;
static unsigned long freefallStartTime   = 0;
static unsigned long impactStartTime     = 0;
static unsigned long postImpactStartTime = 0;

static float gravityX = 0, gravityY = 0, gravityZ = 0;
const float  GRAVITY_FILTER_ALPHA = 0.05;

static float accelHistory[10] = {0};
static int   historyIndex     = 0;
const int    HISTORY_SIZE     = 10;

const float         FREEFALL_THRESHOLD             = 3.0;
const float         IMPACT_THRESHOLD               = 20.0;
const float         GYRO_STILLNESS_THRESHOLD       = 25.0;
const float         ORIENTATION_CHANGE_THRESHOLD   = 0.5;
const unsigned long FREEFALL_TIMEOUT               = 400;
const unsigned long IMPACT_WINDOW                  = 500;
const unsigned long POST_IMPACT_STILLNESS_DURATION = 2500;

TaskHandle_t fallDetectionTaskHandle   = NULL;
TaskHandle_t activityTriggerTaskHandle = NULL;

struct SensorData {
  float accelX, accelY, accelZ;
  float gyroX,  gyroY,  gyroZ;
  float accelMag, gyroMag;
  unsigned long timestamp;
};

void fallDetectionTask(void *pvParameters);
void activityTriggerTask(void *pvParameters);

// ─── CONVERSION HELPERS ───────────────────────────────────────────────────────
// BMI160 accel at ±2g: 16384 LSB/g
float accelRawToG(int16_t raw, int range) {
  float scale;
  switch (range) {
    case 2:  scale = 16384.0; break;
    case 4:  scale = 8192.0;  break;
    case 8:  scale = 4096.0;  break;
    case 16: scale = 2048.0;  break;
    default: scale = 16384.0; break;
  }
  return (float)raw / scale;
}

float accelRawToMS2(int16_t raw, int range) {
  return accelRawToG(raw, range) * 9.81;
}

// BMI160 gyro at ±2000 deg/s: 16.4 LSB/deg/s
float gyroRawToDPS(int16_t raw, int range) {
  float scale;
  switch (range) {
    case 250:  scale = 131.0; break;
    case 500:  scale = 65.5;  break;
    case 1000: scale = 32.8;  break;
    case 2000: scale = 16.4;  break;
    default:   scale = 16.4; break;
  }
  return (float)raw / scale;
}

float magnitude(float x, float y, float z) {
  return sqrt(x*x + y*y + z*z);
}

float getAccelVariance(float newAccel) {
  accelHistory[historyIndex] = newAccel;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  float mean = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) mean += accelHistory[i];
  mean /= HISTORY_SIZE;
  float variance = 0;
  for (int i = 0; i < HISTORY_SIZE; i++)
    variance += (accelHistory[i] - mean) * (accelHistory[i] - mean);
  return variance / HISTORY_SIZE;
}

float getOrientationChange(float cx, float cy, float cz) {
  return magnitude(cx - gravityX, cy - gravityY, cz - gravityZ);
}

void updateGravityVector(float x, float y, float z) {
  gravityX = gravityX * (1.0 - GRAVITY_FILTER_ALPHA) + x * GRAVITY_FILTER_ALPHA;
  gravityY = gravityY * (1.0 - GRAVITY_FILTER_ALPHA) + y * GRAVITY_FILTER_ALPHA;
  gravityZ = gravityZ * (1.0 - GRAVITY_FILTER_ALPHA) + z * GRAVITY_FILTER_ALPHA;
}

// Feature fed into the CNN sliding window.
// x_train_3 was vertical accel in g (not m/s²) - match that scale here.
static float featureForInference(const SensorData& s) {
  return s.accelY / 9.81f;
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  //Wire.end(); // forces a clean re‑init
  //Wire.begin(SDA_PIN, SCL_PIN);
  Wire.begin(8, 9);  // For ESP32-C3 Super Mini, SDA=8 SCL=9
  Wire.setClock(100000);  // 100 kHz I2C
  delay(500);  // let bus settle

  pinMode(BUZZER_PIN, OUTPUT);

  eventGroup = xEventGroupCreate();
  if (eventGroup == NULL) {
    Serial.println("Failed to create event group!");
    return;
  }

  Serial.println("Initializing BMI160...");

  if (bmi160.I2cInit(BMI160_I2C_ADDR) != BMI160_OK) {
    Serial.println("BMI160 init failed!");
    while (1);
  }

  delay(100);

  if (bmi160.softReset() != BMI160_OK) {
    Serial.println("BMI160 reset failed!");
    while (1);
  }

  /*
  // BMI Initialization sequence
  if (bmi160.softReset() != BMI160_OK) {
    Serial.println("BMI160 reset failed!");
    while (1);
  }

  if (bmi160.I2cInit(BMI160_I2C_ADDR) != BMI160_OK) {
    Serial.println("BMI160 init failed!");
    while (1);
  }
  */

  Serial.println("BMI160 Ready");

  // DFRobot library uses begin() for range configuration
  // Default ranges after I2cInit: accel ±2g, gyro ±2000 deg/s

  Serial.println("\n=== Initializing Neural Network ===");
  if (!initializeInference()) {
    Serial.println("ERROR: Failed to initialize inference engine!");
    Serial.println("Falling back to rule-based detection only.");
  } else {
    Serial.println("Neural network initialized successfully!");
  }
  Serial.println("====================================\n");

  xTaskCreate(fallDetectionTask,   "Fall Detection",  4096, NULL, 2, &fallDetectionTaskHandle);
  xTaskCreate(activityTriggerTask, "Activity Trigger", 2048, NULL, 1, &activityTriggerTaskHandle);

  Serial.println("RTOS tasks created.");
}

// ─── TASK 1: FALL DETECTION ───────────────────────────────────────────────────
void fallDetectionTask(void *pvParameters) {
  SensorData     sensorData;
  InferenceOutput inferenceOutput;

  while (1) {
    // DFRobot_BMI160: getAccelGyroData returns array of 6 int16_t
    // Index order: [0]=gyroX [1]=gyroY [2]=gyroZ [3]=accelX [4]=accelY [5]=accelZ
    int16_t accelGyro[6] = {0};
    bmi160.getAccelGyroData(accelGyro);
 
    // Gyro: indices 0-2
    sensorData.gyroX  = gyroRawToDPS(accelGyro[0], 2000);
    sensorData.gyroY  = gyroRawToDPS(accelGyro[1], 2000);
    sensorData.gyroZ  = gyroRawToDPS(accelGyro[2], 2000);
 
    // Accel: indices 3-5
    sensorData.accelX = accelRawToMS2(accelGyro[3], 2);
    sensorData.accelY = accelRawToMS2(accelGyro[4], 2);
    sensorData.accelZ = accelRawToMS2(accelGyro[5], 2);
 
    sensorData.accelMag  = magnitude(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
    sensorData.gyroMag   = magnitude(sensorData.gyroX,  sensorData.gyroY,  sensorData.gyroZ);
    sensorData.timestamp = millis();

    // Feed latest sample into inference sliding window
    pushInferenceSample(featureForInference(sensorData));

    // Run inference if window is full
    bool inferenceValid = false;
    if (inferenceWindowReady()) {
      inferenceValid = runInference(inferenceOutput);
    }

    // ── State machine ──
    switch (fallState) {
      case STATE_NORMAL:
        updateGravityVector(sensorData.accelX, sensorData.accelY, sensorData.accelZ);
        if (sensorData.accelMag < FREEFALL_THRESHOLD) {
          fallState = STATE_FREEFALL;
          freefallStartTime = millis();
          Serial.println("STAGE 1: FREEFALL DETECTED");
        }
        break;

      case STATE_FREEFALL:
        if (sensorData.accelMag > IMPACT_THRESHOLD) {
          fallState = STATE_IMPACT;
          impactStartTime = millis();
          Serial.println("STAGE 2: IMPACT DETECTED");
        } else if (millis() - freefallStartTime > FREEFALL_TIMEOUT) {
          fallState = STATE_NORMAL;
          Serial.println("Freefall timeout - false alarm");
        }
        break;

      case STATE_IMPACT:
        if (sensorData.gyroMag < GYRO_STILLNESS_THRESHOLD) {
          fallState = STATE_POST_IMPACT_STILLNESS;
          postImpactStartTime = millis();
          Serial.println("STAGE 3: POST-IMPACT STILLNESS - checking...");
        } else if (millis() - impactStartTime > IMPACT_WINDOW) {
          fallState = STATE_NORMAL;
          Serial.println("Impact timeout - false alarm");
        }
        break;

      case STATE_POST_IMPACT_STILLNESS: {
        float        accelVariance     = getAccelVariance(sensorData.accelMag);
        float        orientationChange = getOrientationChange(
                                           sensorData.accelX,
                                           sensorData.accelY,
                                           sensorData.accelZ);
        unsigned long timeSinceImpact  = millis() - postImpactStartTime;

        bool ruleBasedFall = (timeSinceImpact >= POST_IMPACT_STILLNESS_DURATION &&
                              sensorData.gyroMag < GYRO_STILLNESS_THRESHOLD &&
                              accelVariance < 1.0f &&
                              orientationChange > ORIENTATION_CHANGE_THRESHOLD);

        // NN confirms fall if predictedClass == 1.
        // If inference is not ready, fall back to rule-based alone.
        bool nnConfirmedFall = inferenceValid && (inferenceOutput.predictedClass == 1);

        if (ruleBasedFall && (!inferenceValid || nnConfirmedFall)) {
          fallState = STATE_FALL_CONFIRMED;
          xEventGroupSetBits(eventGroup, FALL_DETECTED_BIT);
          Serial.println("*** STAGE 4: FALL CONFIRMED ***");
          const char* nnStatus = !inferenceValid ? "NOT_READY" :
                                 (nnConfirmedFall ? "YES" : "NO");
          if (inferenceValid) {
            Serial.printf("Rule-based: YES | NN: %s | Prob: %.3f | Orient: %.3f\n",
              nnStatus,
              inferenceOutput.fallProbability,
              orientationChange
            );
          } else {
            Serial.printf("Rule-based: YES | NN: %s | Prob: N/A | Orient: %.3f\n",
              nnStatus,
              orientationChange
            );
          }
        } else if (timeSinceImpact > 5000) {
          fallState = STATE_NORMAL;
          Serial.println("Post-impact stillness timeout - false alarm");
        }
        break;
      }

      case STATE_FALL_CONFIRMED:
        Serial.printf("FALL,%lu\n", millis());
        xEventGroupSetBits(eventGroup, ACTIVITY_TRIGGERED_BIT);
        fallState = STATE_NORMAL;
        break;
    }

    // Serial output for Python dashboard
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

// ─── TASK 2: ACTIVITY TRIGGER ─────────────────────────────────────────────────
void activityTriggerTask(void *pvParameters) {
  EventBits_t uxBits;
  while (1) {
    uxBits = xEventGroupWaitBits(
      eventGroup,
      ACTIVITY_TRIGGERED_BIT,
      pdTRUE,
      pdFALSE,
      100 / portTICK_PERIOD_MS
    );
    if ((uxBits & ACTIVITY_TRIGGERED_BIT) != 0) {
      Serial.println(">>>> ALERTING - FALL DETECTED! <<<<");
      digitalWrite(BUZZER_PIN, HIGH);
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

void loop() {
  delay(10);
}