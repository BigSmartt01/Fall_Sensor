#include "wifi_stream.h"
#include "dashboard_html.h"
#include "portal_html.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>   // WebSocket server
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ─── CONFIG ───────────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS  10000
#define TCP_MAX_LINE_LEN         256
#define TCP_TASK_STACK           4096
#define TCP_TASK_PRIORITY        1
#define DNS_PORT                 53
#define WEB_SERVER_PORT          81

// SoftAP default IP used by WiFi.softAP() and advertised via captive DNS.
static const IPAddress kApIP(192, 168, 4, 1);
static const char*     kMdnsHostname = "fallsensor";

// Existing credential form HTML — keep byte-for-byte identical to prior behavior.
static const char kPortalFormHtml[] =
    "<form action='/save' method='POST'>"
    "SSID:<input name='ssid'><br>"
    "Password:<input name='pass'><br>"
    "<input type='submit'></form>";

// ─── STATE ────────────────────────────────────────────────────────────────────
static WiFiServer*   tcpServer   = nullptr;
static WiFiClient    tcpClient;
static uint16_t      tcpPort     = 3333;
static SemaphoreHandle_t clientMutex = NULL;
static char          ipStr[20]   = "0.0.0.0";

// WebSocket server on port 81 (dashboard streaming)
WebSocketsServer webSocket(WEB_SERVER_PORT);

// Activity flags for the service polling task
static bool dnsActive       = false;
static bool webSocketActive = false;

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────
static void tcpAcceptTask(void* pvParameters);
static void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
static void webServiceTask(void* pvParameters);   // NEW

// ─── CREDENTIALS ─────────────────────────────────────────────────────────────
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

void saveCredentials(const char* ssid, const char* password) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    prefs.end();
}

bool loadCredentials(String &ssid, String &pass) {
    prefs.begin("wifi", true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
    return ssid.length() > 0;
}

// Serve the WiFi credential form (same HTML as before).
static void sendPortalForm() {
    server.send(200, "text/html", portal_html);
}

// HTTP 302 to the portal root — used for OS captive-portal probes and unknown URLs.
static void redirectToPortal() {
    String location = String("http://") + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Location", location, true);
    server.send(302, "text/plain", "");
}

void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("FallSensor_AP", "12345678");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    if (WiFi.AP.enableDhcpCaptivePortal()) {
        printf("[WiFi] DHCP captive portal URI enabled\n");
    } else {
        printf("[WiFi] DHCP captive portal URI not enabled (DNS/HTTP still active)\n");
    }
#endif

    if (dnsServer.start(DNS_PORT, "*", kApIP)) {
        printf("[WiFi] DNS captive portal started (all queries -> %s)\n",
               kApIP.toString().c_str());
        dnsActive = true;      // signal task to poll DNS
    } else {
        printf("[WiFi] ERROR: DNS server failed to start\n");
    }

    printf("[WiFi] AP started. Connect to SSID 'FallSensor_AP' — captive portal opens automatically\n");
    printf("[WiFi] Manual fallback: open http://%s/\n", WiFi.softAPIP().toString().c_str());

    server.on("/", []() {
        sendPortalForm();
    });

    server.on("/save", []() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        saveCredentials(ssid.c_str(), pass.c_str());
        server.send(200, "text/html", "Saved! Rebooting...");
        delay(1000);
        ESP.restart();
    });

    // ── Captive portal detection endpoints ────────────────────────────────────
    server.on("/generate_204", HTTP_ANY, redirectToPortal);
    server.on("/gen_204", HTTP_ANY, redirectToPortal);
    server.on("/hotspot-detect.html", HTTP_ANY, sendPortalForm);
    server.on("/library/test/success.html", HTTP_ANY, sendPortalForm);
    server.on("/connecttest.txt", HTTP_ANY, redirectToPortal);
    server.on("/ncsi.txt", HTTP_ANY, redirectToPortal);
    server.on("/redirect", HTTP_ANY, redirectToPortal);
    server.on("/fwlink", HTTP_ANY, redirectToPortal);
    server.on("/success.txt", HTTP_ANY, redirectToPortal);
    server.on("/canonical.html", HTTP_ANY, sendPortalForm);
    server.onNotFound(redirectToPortal);

    server.begin();

    // ── Start the service polling task (only once globally) ─────────────────
    static bool webServiceTaskCreated = false;
    if (!webServiceTaskCreated) {
        xTaskCreate(webServiceTask, "WebSvc", 4096, NULL, 1, NULL);
        webServiceTaskCreated = true;
    }
}

bool tryConnect(const char* ssid, const char* password) {
    for (int attempt = 0; attempt < 5; attempt++) {
        WiFi.begin(ssid, password);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (WiFi.status() == WL_CONNECTED) {
            printf("[WiFi] Connected to %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());
            return true;
        }
        printf("[WiFi] Attempt %d failed\n", attempt+1);
    }
    return false;
}

// ─── PUBLIC API ───────────────────────────────────────────────────────────────
void wifiStreamInit(const char* ssid, const char* password, uint16_t port) {
    (void)ssid; (void)password;
    tcpPort = port;
    clientMutex = xSemaphoreCreateMutex();
    if (clientMutex == NULL) {
        printf("[WiFi] Failed to create mutex\n");
        return;
    }

    WiFi.mode(WIFI_STA);

    String storedSsid, storedPass;
    if (loadCredentials(storedSsid, storedPass) && tryConnect(storedSsid.c_str(), storedPass.c_str())) {
        // Connected → start TCP server
        strncpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr) - 1);

        // === mDNS ===
        if (!MDNS.begin(kMdnsHostname)) {
            printf("[WiFi] mDNS failed to start\n");
        } else {
            printf("[WiFi] mDNS started - reachable at %s.local\n", kMdnsHostname);
        }

        tcpServer = new WiFiServer(tcpPort);
        tcpServer->begin();
        printf("[WiFi] TCP server on port %d | Web Dashboard: http://%s.local\n", tcpPort, kMdnsHostname);
        xTaskCreate(tcpAcceptTask, "TCP Accept", TCP_TASK_STACK, NULL, TCP_TASK_PRIORITY, NULL);

        // Start WebSocket server + HTTP server
        webSocket.begin();
        webSocket.onEvent(webSocketEvent);
        webSocketActive = true;        // signal task to poll WebSocket

        server.on("/", [](){ server.send(200, "text/html", dashboard_html); });
        server.begin();

        // ── Start the service polling task (only once globally) ─────────────
        static bool webServiceTaskCreated = false;
        if (!webServiceTaskCreated) {
            xTaskCreate(webServiceTask, "WebSvc", 4096, NULL, 1, NULL);
            webServiceTaskCreated = true;
        }

        return;
    }

    // If no credentials or failed after retries → AP + captive portal
    startAP();
}

void wifiStreamSend(const char* line) {
    if (clientMutex && xSemaphoreTake(clientMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (tcpClient && tcpClient.connected()) tcpClient.print(line);
        xSemaphoreGive(clientMutex);
    }
    webSocket.broadcastTXT(line);
}

void wifiStreamPrintf(const char* fmt, ...) {
    char buf[TCP_MAX_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    wifiStreamSend(buf);
}

// WebSocket event handler
static void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        printf("[WebSocket] Client %u connected\n", num);
    }
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
static void tcpAcceptTask(void* pvParameters) {
    while (1) {
        if (tcpServer) {
            WiFiClient newClient = tcpServer->accept();
            if (newClient) {
                if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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

            if (xSemaphoreTake(clientMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (tcpClient && !tcpClient.connected()) {
                    tcpClient.stop();
                    printf("[WiFi] Client disconnected\n");
                }
                xSemaphoreGive(clientMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─── SERVICE POLLING TASK ────────────────────────────────────────────────────
static void webServiceTask(void* pvParameters) {
    while (1) {
        if (dnsActive) {
            dnsServer.processNextRequest();
        }
        server.handleClient();              // HTTP always needed
        if (webSocketActive) {
            webSocket.loop();
        }
        vTaskDelay(pdMS_TO_TICKS(10));      // 100 Hz polling
    }
}