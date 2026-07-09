#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_STATUS_IDLE = 0,
    WIFI_MANAGER_STATUS_CONNECTING,
    WIFI_MANAGER_STATUS_CONNECTED,
    WIFI_MANAGER_STATUS_DISCONNECTED,
    WIFI_MANAGER_STATUS_FAILED,
} wifi_manager_connection_status_t;

/* Connection state — written by wifi_manager, read by the LVGL timer. */
typedef struct {
    wifi_manager_connection_status_t status;
    bool    connected;
    int8_t  rssi;
} wifi_manager_state_t;

extern volatile wifi_manager_state_t g_wifi_state;

/*
 * Blocking start: initialise NVS/TCPIP/WiFi and wait until connected,
 * failed, or CONFIG_DASHBOARD_WIFI_CONNECT_TIMEOUT_MS expires.
 */
esp_err_t wifi_manager_start(void);

/*
 * Non-blocking start: creates a small FreeRTOS task which calls
 * wifi_manager_start().  This lets the LVGL UI appear immediately while the
 * status LED shows CONNECTING/CONNECTED/FAILED.
 */
esp_err_t wifi_manager_start_async(void);

wifi_manager_connection_status_t wifi_manager_get_status(void);

/* Returns current RSSI in dBm, or 0 if not connected / unavailable. */
int8_t wifi_manager_get_rssi(void);

/* Current connected SSID, or an empty string if not connected / unavailable. */
const char *wifi_manager_get_connected_ssid(void);

/* Human-readable backend name for logs/diagnostics. */
const char *wifi_manager_get_backend_name(void);

#ifdef __cplusplus
}
#endif
