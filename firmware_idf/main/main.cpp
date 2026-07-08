#include <Arduino.h>
#include <Wire.h>

#undef LITTLE_ENDIAN
#include <DFRobot_BMI160.h>

#include "inference.h"
#include "wifi_stream.h"
#include "nvs_flash.h"

// ESP32-C3 Super Mini (current)
#define SDA_PIN     8
#define SCL_PIN     9
#define INT1_PIN    5
#define INT2_PIN    6
#define BUZZER_PIN  2

// Acts as dual-purpose button:
//   - During STATE_FALL_ALERTED: "I'm okay" -> dismiss alert, return to normal
//   - During STATE_NORMAL/other: "I need help" -> manual alert, skips detection logic
// Active LOW with internal pull-up.
#define BUTTON_PIN  3

//const int8_t BMI160_I2C_ADDR = 0x68;  // SDIO pin connected to GND
struct bmi160Dev dev;

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
  STATE_FALL_CONFIRMED,
  STATE_FALL_ALERTED   // persistent "person is down" state - does not auto-clear
};

static FallState     fallState           = STATE_NORMAL;
static unsigned long freefallStartTime   = 0;
static unsigned long impactStartTime     = 0;
static unsigned long postImpactStartTime = 0;
static unsigned long lastAlertBuzzTime   = 0;

static float gravityX = 0, gravityY = 0, gravityZ = 0;
const float  GRAVITY_FILTER_ALPHA = 0.05;

// Orientation captured at the moment a fall is confirmed.
// Recovery from STATE_FALL_ALERTED is judged against THIS, not the
// pre-fall gravity baseline - the person is expected to still be down
// until their posture deliberately changes again.
static float fallOrientX = 0, fallOrientY   = 0, fallOrientZ = 0;
const float  RECOVERY_ORIENTATION_THRESHOLD = 5.0;      // larger than fall-detection threshold - needs deliberate posture change
const unsigned long ALERT_BUZZ_INTERVAL     = 10000;    // re-buzz every 10s while alerted

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

TaskHandle_t fallDetectionTaskHandle    = NULL;
TaskHandle_t activityTriggerTaskHandle  = NULL;
TaskHandle_t buttonTaskHandle           = NULL;

struct SensorData {
  float accelX, accelY, accelZ;
  float gyroX,  gyroY,  gyroZ;
  float accelMag, gyroMag;
  unsigned long timestamp;
};

void fallDetectionTask(void *pvParameters);
void activityTriggerTask(void *pvParameters);
void buttonTask(void *pvParameters);

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

// Generic orientation delta vs an arbitrary reference vector.
float getOrientationDelta(float cx, float cy, float cz, float refX, float refY, float refZ) {
  return magnitude(cx - refX, cy - refY, cz - refZ);
}

float getOrientationChange(float cx, float cy, float cz) {
  return getOrientationDelta(cx, cy, cz, gravityX, gravityY, gravityZ);
}

void updateGravityVector(float x, float y, float z) {
  gravityX = gravityX * (1.0 - GRAVITY_FILTER_ALPHA) + x * GRAVITY_FILTER_ALPHA;
  gravityY = gravityY * (1.0 - GRAVITY_FILTER_ALPHA) + y * GRAVITY_FILTER_ALPHA;
  gravityZ = gravityZ * (1.0 - GRAVITY_FILTER_ALPHA) + z * GRAVITY_FILTER_ALPHA;
}

void captureFallOrientation(float x, float y, float z) {
  fallOrientX = x;
  fallOrientY = y;
  fallOrientZ = z;
}

// Feature fed into the CNN sliding window.
// x_train_3 was vertical accel in g (not m/s²) - match that scale here.
static float featureForInference(const SensorData& s) {
  return s.accelY / 9.81f;
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);  // 100 kHz I2C
  delay(500);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // placeholder - active LOW when wired

  eventGroup = xEventGroupCreate();
  if (eventGroup == NULL) {
    printf("Failed to create event group!\n");
    return;
  }

  printf("Initializing BMI160\n");

  if (bmi160.softReset() != BMI160_OK) {
    printf("BMI160 reset failed!\n");
    while (1);
  }

  delay(100);

  if (bmi160.I2cInit(BMI160_I2C_ADDR) != BMI160_OK) {
    printf("BMI160 init failed!\n");
    while (1) {
    }
  }
  // Set BMI160 accel and gyro ODR to 200Hz via direct register write
  // Register 0x40 (ACC_CONF): bits[3:0] = ODR, 0x09 = 200Hz
  // Register 0x42 (GYR_CONF): bits[3:0] = ODR, 0x09 = 200Hz
  uint8_t regVal = 0;

  Wire.beginTransmission(0x68);
  Wire.write(0x40);
  Wire.write(0x09);
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  if (Wire.available()) {
    regVal = Wire.read();
    printf("ACC_CONF register value: 0x%02X\n", regVal);
  }
  delay(10);

  Wire.beginTransmission(0x68);
  Wire.write(0x42);
  Wire.write(0x09);
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 1);
  if (Wire.available()) {
    regVal = Wire.read();
    printf("GYR_CONF register value: 0x%02X\n", regVal);
  }
  delay(10);

  printf("BMI160 ODR set to 200Hz\n");

  printf("BMI160 Ready\n");

  // Initialize NVS - required by WiFi
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  // Connect WiFi and start TCP server
  wifiStreamInit("SkillGInnovation", "INNOV8HUB", 3333);
  //wifiStreamInit("BS-Andriod", "Dam_rat129!are", 3333);

  // DFRobot library uses begin() for range configuration
  // Default ranges after I2cInit: accel ±2g, gyro ±2000 deg/s

  printf("\n=== Initializing Neural Network ===\n");
  if (!initializeInference()) {
    printf("ERROR: Failed to initialize inference engine!\n");
    printf("Falling back to rule-based detection only.\n");
  } else {
    printf("Neural network initialized successfully!\n");
  }
  printf("====================================\n");

  xTaskCreate(fallDetectionTask,   "Fall Detection",  4096, NULL, 2, &fallDetectionTaskHandle);
  xTaskCreate(activityTriggerTask, "Activity Trigger", 2048, NULL, 1, &activityTriggerTaskHandle);
  xTaskCreate(buttonTask,          "Button Watch",     2048, NULL, 1, &buttonTaskHandle);

  printf("RTOS tasks created.\n");
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
          printf("STAGE 1: FREEFALL DETECTED\n");
        }
        break;

      case STATE_FREEFALL:
        if (sensorData.accelMag > IMPACT_THRESHOLD) {
          fallState = STATE_IMPACT;
          impactStartTime = millis();
          printf("STAGE 2: IMPACT DETECTED\n");
        } else if (millis() - freefallStartTime > FREEFALL_TIMEOUT) {
          fallState = STATE_NORMAL;
          printf("Freefall timeout - false alarm\n");
        }
        break;

      case STATE_IMPACT:
        if (sensorData.gyroMag < GYRO_STILLNESS_THRESHOLD) {
          fallState = STATE_POST_IMPACT_STILLNESS;
          postImpactStartTime = millis();
          printf("STAGE 3: POST-IMPACT STILLNESS - checking...\n");
        } else if (millis() - impactStartTime > IMPACT_WINDOW) {
          fallState = STATE_NORMAL;
          printf("Impact timeout - false alarm\n");
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
          printf("*** STAGE 4: FALL CONFIRMED ***\n");
          const char* nnStatus = !inferenceValid ? "NOT_READY" :
                                 (nnConfirmedFall ? "YES" : "NO");
          if (inferenceValid) {
            printf("Rule-based: YES | NN: %s | Prob: %.3f | Orient: %.3f\n",
              nnStatus,
              inferenceOutput.fallProbability,
              orientationChange
            );
          } else {
            printf("Rule-based: YES | NN: %s | Prob: N/A | Orient: %.3f\n",
              nnStatus,
              orientationChange
            );
          }
        } else if (timeSinceImpact > 5000) {
          fallState = STATE_NORMAL;
          printf("Post-impact stillness timeout - false alarm\n");
        }
        break;
      }

      case STATE_FALL_CONFIRMED: {
        // One-shot transition into the persistent alerted state.
        // Capture the orientation at this exact moment - recovery is
        // judged against THIS posture, not the pre-fall gravity baseline.
        captureFallOrientation(sensorData.accelX, sensorData.accelY, sensorData.accelZ);

        printf("FALL,%lu\n", millis());
        wifiStreamPrintf("FALL,%lu\n", millis());

        xEventGroupSetBits(eventGroup, ACTIVITY_TRIGGERED_BIT);
        lastAlertBuzzTime = millis();

        fallState = STATE_FALL_ALERTED;
        printf("Entering STATE_FALL_ALERTED - holding until orientation changes or button pressed\n");
        break;
      }

      case STATE_FALL_ALERTED: {
        // Person is presumed still down. Stay here until either:
        //   1. Orientation changes significantly from the fall-moment posture (handled here), or
        //   2. The "I'm okay" button is pressed (handled directly in buttonTask)
        float recoveryDelta = getOrientationDelta(
          sensorData.accelX, sensorData.accelY, sensorData.accelZ,
          fallOrientX, fallOrientY, fallOrientZ
        );

        // Re-trigger buzzer periodically while still alerted
        if (millis() - lastAlertBuzzTime >= ALERT_BUZZ_INTERVAL) {
          lastAlertBuzzTime = millis();
          xEventGroupSetBits(eventGroup, ACTIVITY_TRIGGERED_BIT);
        }
  
        static unsigned long recoveryStart = 0;
        if (recoveryDelta > RECOVERY_ORIENTATION_THRESHOLD) {
          if (recoveryStart == 0) recoveryStart = millis();
          if (millis() - recoveryStart > 3000) {
            fallState = STATE_NORMAL;
            recoveryStart = 0;
            printf("Posture changed (delta=%.3f) - returning to STATE_NORMAL\n", recoveryDelta);
          }
        } else {
            recoveryStart = 0; // reset if delta drops again
        }
        break;
      }
    }

    // Serial & Wifi output for Python dashboard
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 20) {
      lastPrint = millis();
      printf("DATA,%lu,%.2f,%.2f,%d\n",
        sensorData.timestamp, sensorData.accelMag, sensorData.gyroMag, (int)fallState);
      wifiStreamPrintf("DATA,%lu,%.2f,%.2f,%d\n",
        sensorData.timestamp, sensorData.accelMag, sensorData.gyroMag, (int)fallState);
    }

    vTaskDelay(5 / portTICK_PERIOD_MS);  // 50Hz sampling
  }
}

// ─── TASK 2: ACTIVITY TRIGGER ─────────────────────────────────────────────────
// Each ACTIVITY_TRIGGERED_BIT firing produces one ~2s buzz pulse.
// Persistence comes from STATE_FALL_ALERTED re-setting this bit every
// ALERT_BUZZ_INTERVAL, not from a single long tone here.
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
      printf(">>>> ALERTING - FALL DETECTED! <<<<\n");
      digitalWrite(BUZZER_PIN, HIGH);
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      digitalWrite(BUZZER_PIN, LOW);
    }
  }
}

// ─── TASK 3: BUTTON WATCH ─────────────────────────────────────────────────────
// Placeholder until physically wired - polls BUTTON_PIN with simple debounce.
// Dual purpose depending on current fallState:
//   - STATE_FALL_ALERTED -> "I'm okay": dismiss alert, return to STATE_NORMAL
//   - anything else      -> "I need help": manual alert, same buzzer path
void buttonTask(void *pvParameters) {
  bool lastReading = HIGH;  // assumes INPUT_PULLUP, idle HIGH
  unsigned long lastChangeTime = 0;
  const unsigned long DEBOUNCE_MS = 50;

  while (1) {
    bool reading = digitalRead(BUTTON_PIN);

    if (reading != lastReading) {
      lastChangeTime = millis();
    }

    if ((millis() - lastChangeTime) > DEBOUNCE_MS && reading == LOW) {
      // Button confirmed pressed
      if (fallState == STATE_FALL_ALERTED) {
        printf("BUTTON: \"I'm okay\" pressed - dismissing alert\n");
        fallState = STATE_NORMAL;
      } else {
        printf("BUTTON: \"I need help\" pressed - manual alert\n");
        printf("FALL,%lu\n", millis());
        wifiStreamPrintf("FALL,%lu\n", millis());
        xEventGroupSetBits(eventGroup, ACTIVITY_TRIGGERED_BIT);
        // Manual help does not enter STATE_FALL_ALERTED automatically -
        // person is conscious enough to press a button, so no persistent
        // "down" assumption is made here. Revisit if this should also
        // hold until a separate dismissal.
      }

      // Wait for release before allowing another trigger
      while (digitalRead(BUTTON_PIN) == LOW) {
        vTaskDelay(5 / portTICK_PERIOD_MS);
      }
    }

    lastReading = reading;
    vTaskDelay(5 / portTICK_PERIOD_MS); // 200Hz sampling
  }
}

void loop() {
  delay(10);
}