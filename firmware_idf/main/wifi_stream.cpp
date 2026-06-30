#include "wifi_stream.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ─── CONFIG ───────────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS  30000
#define TCP_MAX_LINE_LEN         256
#define TCP_TASK_STACK           4096
#define TCP_TASK_PRIORITY        1

// ─── STATE ────────────────────────────────────────────────────────────────────
static WiFiServer*   tcpServer   = nullptr;
static WiFiClient    tcpClient;
static uint16_t      tcpPort     = 3333;
static SemaphoreHandle_t clientMutex = NULL;
static char          ipStr[20]   = "0.0.0.0";

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────
static void tcpAcceptTask(void* pvParameters);

// ─── PUBLIC API ───────────────────────────────────────────────────────────────
void wifiStreamInit(const char* ssid, const char* password, uint16_t port) {
    tcpPort = port;

    clientMutex = xSemaphoreCreateMutex();
    if (clientMutex == NULL) {
        printf("[WiFi] Failed to create mutex\n");
        return;
    }

    WiFi.mode(WIFI_STA);

    while (true) {
        printf("[WiFi] Connecting to %s", ssid);
        WiFi.disconnect(true);
        WiFi.begin(ssid, password);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            printf(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
            strncpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr) - 1);

            // Start TCP server
            tcpServer = new WiFiServer(tcpPort);
            tcpServer->begin();
            printf("[WiFi] TCP server listening on port %d\n", tcpPort);
            printf("[WiFi] Connect dashboard with: TCP %s:%d\n", ipStr, tcpPort);

            // Spawn accept task
            xTaskCreate(tcpAcceptTask, "TCP Accept", TCP_TASK_STACK, NULL, TCP_TASK_PRIORITY, NULL);
            break;  // exit retry loop once connected
        } else {
            printf("\n[WiFi] Connection failed, retrying in 10s...\n");
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
}


void wifiStreamSend(const char* line) {
    if (clientMutex == NULL) return;
    if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (tcpClient && tcpClient.connected()) {
        tcpClient.print(line);
        // Append newline if not present
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {
            tcpClient.print('\n');
        }
    }

    xSemaphoreGive(clientMutex);
}

void wifiStreamPrintf(const char* fmt, ...) {
    char buf[TCP_MAX_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    wifiStreamSend(buf);
}

bool wifiStreamConnected(void) {
    if (clientMutex == NULL) return false;
    if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    bool connected = tcpClient && tcpClient.connected();
    xSemaphoreGive(clientMutex);
    return connected;
}

const char* wifiStreamIP(void) {
    return ipStr;
}

// ─── TCP ACCEPT TASK ─────────────────────────────────────────────────────────
// Runs forever, polls for new client connections and cleans up dropped ones.
// Low priority (1) - never blocks the 50Hz fall detection task (priority 2).
static void tcpAcceptTask(void* pvParameters) {
    while (1) {
        if (tcpServer) {
            WiFiClient newClient = tcpServer->accept();
            if (newClient) {
                if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    // Disconnect any existing client before accepting new one
                    if (tcpClient && tcpClient.connected()) {
                        tcpClient.stop();
                        printf("[WiFi] Previous client disconnected, new client accepted\n");
                    } else {
                        printf("[WiFi] Client connected from %s\n",
                               newClient.remoteIP().toString().c_str());
                    }
                    tcpClient = newClient;
                    xSemaphoreGive(clientMutex);
                }
            }

            // Check if current client dropped
            if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (tcpClient && !tcpClient.connected()) {
                    tcpClient.stop();
                    printf("[WiFi] Client disconnected\n");
                }
                xSemaphoreGive(clientMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));  // poll every 100ms
    }
}