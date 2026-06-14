#include "Arduino.h"

// Forward declaration from main.cpp
void setup();
void loop();

extern "C" void app_main() {
    initArduino();
    setup();
    // loop() not needed - FreeRTOS tasks handle everything
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}