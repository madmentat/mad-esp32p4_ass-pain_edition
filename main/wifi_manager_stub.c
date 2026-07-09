#include "sdkconfig.h"
#if CONFIG_MAD_OTA_ENABLE
#error "MAD OTA requires a real Wi-Fi backend. CONFIG_DASHBOARD_WIFI_BACKEND_DISABLED selected wifi_manager_stub.c; run prepare_p4_hosted_ota_build.ps1 and rebuild."
#endif

#include "wifi_manager.h"
#include "esp_log.h"

static const char *TAG = "WIFI_STUB";

volatile wifi_manager_state_t g_wifi_state = {
    .status = WIFI_MANAGER_STATUS_FAILED,
    .connected = false,
    .rssi = 0,
};

esp_err_t wifi_manager_start(void)
{
    ESP_LOGW(TAG, "WiFi disabled: display-only safe mode");
    g_wifi_state.status = WIFI_MANAGER_STATUS_FAILED;
    g_wifi_state.connected = false;
    g_wifi_state.rssi = 0;
    return ESP_OK;
}

esp_err_t wifi_manager_start_async(void)
{
    ESP_LOGW(TAG, "WiFi disabled: display-only safe mode");
    g_wifi_state.status = WIFI_MANAGER_STATUS_FAILED;
    g_wifi_state.connected = false;
    g_wifi_state.rssi = 0;
    return ESP_OK;
}

wifi_manager_connection_status_t wifi_manager_get_status(void)
{
    return g_wifi_state.status;
}

int8_t wifi_manager_get_rssi(void)
{
    return g_wifi_state.rssi;
}

const char *wifi_manager_get_connected_ssid(void)
{
    return "";
}

const char *wifi_manager_get_backend_name(void)
{
    return "disabled/display-only";
}
