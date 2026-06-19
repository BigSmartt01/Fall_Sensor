#include "Arduino.h"

// Forward declaration from main.cpp
void setup();
void loop();

extern "C" void app_main() {
    printf("ENTERED APP MAIN\n");

    initArduino();

    printf("ARDUINO INIT DONE\n");

    setup();

    printf("SETUP RETURNED\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}