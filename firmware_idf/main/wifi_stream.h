#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi station mode and start a TCP server.
 * Blocks until WiFi is connected or times out (30s).
 * Call once from setup() after BMI160 init.
 *
 * @param ssid     Your WiFi network name
 * @param password Your WiFi password
 * @param port     TCP port to listen on (e.g. 3333)
 */
void wifiStreamInit(const char* ssid, const char* password, uint16_t port);

/**
 * Send a null-terminated string line to the connected TCP client.
 * Appends '\n' automatically if not already present.
 * Falls back silently if no client is connected - USB Serial continues
 * to receive all output regardless.
 *
 * @param line   Null-terminated string to send
 */
void wifiStreamSend(const char* line);

/**
 * Send a formatted line to the connected TCP client.
 * Wrapper around wifiStreamSend with printf-style formatting.
 * Safe to call from any FreeRTOS task.
 *
 * @param fmt   printf-style format string
 * @param ...   variadic arguments
 */
void wifiStreamPrintf(const char* fmt, ...);

/**
 * Returns true if a TCP client is currently connected.
 */
bool wifiStreamConnected(void);

/**
 * Returns the ESP32-C3 IP address as a string.
 * Valid after wifiStreamInit() completes successfully.
 * Returns "0.0.0.0" if not connected.
 */
const char* wifiStreamIP(void);

#ifdef __cplusplus
}
#endif