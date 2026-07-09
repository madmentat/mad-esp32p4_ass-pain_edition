#include "wifi_manager.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_C6_AT";

#define C6_AT_UART_NUM      ((uart_port_t)CONFIG_DASHBOARD_C6_AT_UART_NUM)
#define C6_AT_BAUD          CONFIG_DASHBOARD_C6_AT_BAUD
#define C6_AT_TX_GPIO       CONFIG_DASHBOARD_C6_AT_TX_GPIO
#define C6_AT_RX_GPIO       CONFIG_DASHBOARD_C6_AT_RX_GPIO
#define C6_AT_CHIP_PU_GPIO  CONFIG_DASHBOARD_C6_AT_CHIP_PU_GPIO
#define C6_AT_BOOT_GPIO     CONFIG_DASHBOARD_C6_AT_BOOT_GPIO

#define WIFI_SSID           CONFIG_ESP_WIFI_SSID
#define WIFI_PASS           CONFIG_ESP_WIFI_PASSWORD
#define WIFI_WAIT_MS        CONFIG_DASHBOARD_WIFI_CONNECT_TIMEOUT_MS
#define WIFI_MAX_RETRY      CONFIG_ESP_MAXIMUM_RETRY

#define C6_AT_RX_BUF_SIZE   2048
#define C6_AT_TX_BUF_SIZE   512
#define C6_AT_LINE_MAX      512

static bool s_uart_ready;
static bool s_async_task_started;
static TaskHandle_t s_wifi_task_handle;
static char s_last_ip[64];
static char s_connected_ssid[33];

volatile wifi_manager_state_t g_wifi_state = {
    .status = WIFI_MANAGER_STATUS_IDLE,
    .connected = false,
    .rssi = 0,
};

static void wifi_manager_set_state(wifi_manager_connection_status_t status,
                                   bool connected,
                                   int8_t rssi)
{
    g_wifi_state.status = status;
    g_wifi_state.connected = connected;
    g_wifi_state.rssi = rssi;
    if (!connected) {
        s_connected_ssid[0] = '\0';
    }
}

static bool gpio_is_valid_configured(int gpio)
{
    return gpio >= 0 && gpio <= 54;
}

static void c6_optional_reset(void)
{
    if (gpio_is_valid_configured(C6_AT_BOOT_GPIO)) {
        gpio_reset_pin((gpio_num_t)C6_AT_BOOT_GPIO);
        gpio_set_direction((gpio_num_t)C6_AT_BOOT_GPIO, GPIO_MODE_OUTPUT);
        /* Normal application boot, not download mode. */
        gpio_set_level((gpio_num_t)C6_AT_BOOT_GPIO, 1);
    }

    if (gpio_is_valid_configured(C6_AT_CHIP_PU_GPIO)) {
        ESP_LOGI(TAG, "Resetting ESP32-C6 through CHIP_PU GPIO%d", C6_AT_CHIP_PU_GPIO);
        gpio_reset_pin((gpio_num_t)C6_AT_CHIP_PU_GPIO);
        gpio_set_direction((gpio_num_t)C6_AT_CHIP_PU_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)C6_AT_CHIP_PU_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level((gpio_num_t)C6_AT_CHIP_PU_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(1200));
    } else {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t c6_uart_init_once(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

    if (!gpio_is_valid_configured(C6_AT_TX_GPIO) || !gpio_is_valid_configured(C6_AT_RX_GPIO)) {
        ESP_LOGE(TAG,
                 "ESP32-C6 AT UART pins are not configured: TX=%d RX=%d. "
                 "Set Dashboard WiFi Configuration -> ESP32-C6 AT UART GPIOs in menuconfig.",
                 C6_AT_TX_GPIO,
                 C6_AT_RX_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_config = {
        .baud_rate = C6_AT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(C6_AT_UART_NUM, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(C6_AT_UART_NUM,
                                     C6_AT_TX_GPIO,
                                     C6_AT_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin failed");

    esp_err_t ret = uart_driver_install(C6_AT_UART_NUM,
                                        C6_AT_RX_BUF_SIZE,
                                        C6_AT_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART driver already installed, reusing it");
        ret = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_install failed");

    uart_flush_input(C6_AT_UART_NUM);
    s_uart_ready = true;

    ESP_LOGI(TAG,
             "ESP32-C6 AT UART ready: uart=%d baud=%d TX(GPIO%d)->C6_U0RXD RX(GPIO%d)<-C6_U0TXD",
             (int)C6_AT_UART_NUM,
             C6_AT_BAUD,
             C6_AT_TX_GPIO,
             C6_AT_RX_GPIO);
    return ESP_OK;
}

static void at_flush_input_quiet(uint32_t ms)
{
    uint8_t tmp[128];
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        int n = uart_read_bytes(C6_AT_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(20));
        if (n <= 0) {
            break;
        }
    }
}

static void at_log_command(const char *cmd)
{
    if (cmd && strstr(cmd, "AT+CWJAP=") == cmd) {
        ESP_LOGI(TAG, "> AT+CWJAP=<ssid>,<hidden>");
    } else {
        ESP_LOGI(TAG, "> %s", cmd ? cmd : "");
    }
}

static esp_err_t at_wait_for_tokens(const char *ok_token,
                                    const char *alt_ok_token,
                                    uint32_t timeout_ms,
                                    char *out,
                                    size_t out_size)
{
    char rx[1536];
    size_t used = 0;
    memset(rx, 0, sizeof(rx));

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t chunk[128];
        int n = uart_read_bytes(C6_AT_UART_NUM, chunk, sizeof(chunk) - 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        if (used + (size_t)n >= sizeof(rx) - 1) {
            const size_t keep = sizeof(rx) / 2;
            memmove(rx, rx + used - keep, keep);
            used = keep;
            rx[used] = '\0';
        }

        memcpy(rx + used, chunk, (size_t)n);
        used += (size_t)n;
        rx[used] = '\0';

        if (out && out_size > 0) {
            size_t copy = used < out_size - 1 ? used : out_size - 1;
            memcpy(out, rx + used - copy, copy);
            out[copy] = '\0';
        }

        if (strstr(rx, "ERROR") || strstr(rx, "FAIL")) {
            ESP_LOGW(TAG, "AT response failure: %s", rx);
            return ESP_FAIL;
        }
        if ((ok_token && strstr(rx, ok_token)) ||
            (alt_ok_token && strstr(rx, alt_ok_token))) {
            ESP_LOGD(TAG, "AT response OK: %s", rx);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "AT wait timeout, last response: %s", rx);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t at_send_raw(const char *cmd,
                             const char *ok_token,
                             const char *alt_ok_token,
                             uint32_t timeout_ms,
                             char *out,
                             size_t out_size)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    at_flush_input_quiet(20);
    at_log_command(cmd);
    uart_write_bytes(C6_AT_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(C6_AT_UART_NUM, "\r\n", 2);
    return at_wait_for_tokens(ok_token ? ok_token : "OK", alt_ok_token, timeout_ms, out, out_size);
}

static size_t at_append_quoted(char *dst, size_t dst_size, const char *src)
{
    size_t pos = strlen(dst);
    if (pos + 1 >= dst_size) {
        return pos;
    }
    dst[pos++] = '"';
    dst[pos] = '\0';

    for (const char *p = src ? src : ""; *p && pos + 2 < dst_size; ++p) {
        if (*p == '"' || *p == '\\') {
            dst[pos++] = '\\';
        }
        dst[pos++] = *p;
        dst[pos] = '\0';
    }

    if (pos + 1 < dst_size) {
        dst[pos++] = '"';
        dst[pos] = '\0';
    }
    return pos;
}

static esp_err_t at_probe(void)
{
    for (int i = 0; i < 5; ++i) {
        esp_err_t ret = at_send_raw("AT", "OK", NULL, 700, NULL, 0);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "ESP32-C6 AT firmware responded");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGE(TAG, "ESP32-C6 did not respond to AT");
    return ESP_ERR_TIMEOUT;
}

static int8_t parse_rssi_from_cwjap(const char *resp)
{
    if (!resp) {
        return 0;
    }

    const char *p = strstr(resp, "+CWJAP:");
    if (!p) {
        return 0;
    }

    /* Typical ESP-AT response contains RSSI as the last comma-separated number. */
    const char *last_comma = strrchr(p, ',');
    if (!last_comma) {
        return 0;
    }

    int rssi = atoi(last_comma + 1);
    if (rssi < -127 || rssi > 0) {
        return 0;
    }
    return (int8_t)rssi;
}

static void parse_ip_from_cipsta(const char *resp)
{
    s_last_ip[0] = '\0';
    if (!resp) {
        return;
    }

    const char *p = strstr(resp, "+CIPSTA:ip:");
    if (!p) {
        return;
    }

    const char *q1 = strchr(p, '"');
    if (!q1) {
        return;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2 || q2 <= q1 + 1) {
        return;
    }

    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= sizeof(s_last_ip)) {
        len = sizeof(s_last_ip) - 1;
    }
    memcpy(s_last_ip, q1 + 1, len);
    s_last_ip[len] = '\0';
}

static esp_err_t c6_at_connect_wifi(void)
{
    char response[1536];

    ESP_RETURN_ON_ERROR(at_probe(), TAG, "AT probe failed");

    (void)at_send_raw("ATE0", "OK", NULL, 1000, NULL, 0);
    (void)at_send_raw("AT+GMR", "OK", NULL, 1500, response, sizeof(response));
    ESP_LOGI(TAG, "ESP32-C6 version response: %s", response);

    ESP_RETURN_ON_ERROR(at_send_raw("AT+CWMODE=1", "OK", NULL, 2000, NULL, 0), TAG, "CWMODE STA failed");
    (void)at_send_raw("AT+CWAUTOCONN=0", "OK", NULL, 1500, NULL, 0);

    char cmd[C6_AT_LINE_MAX];
    strlcpy(cmd, "AT+CWJAP=", sizeof(cmd));
    at_append_quoted(cmd, sizeof(cmd), WIFI_SSID);
    strlcat(cmd, ",", sizeof(cmd));
    at_append_quoted(cmd, sizeof(cmd), WIFI_PASS);

    ESP_LOGI(TAG, "Connecting ESP32-C6 AT station to SSID \"%s\"", WIFI_SSID);

    esp_err_t ret = ESP_FAIL;
    int max_retry = WIFI_MAX_RETRY > 0 ? WIFI_MAX_RETRY : 1;
    for (int attempt = 1; attempt <= max_retry; ++attempt) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
        ESP_LOGI(TAG, "AT WiFi connect attempt %d/%d", attempt, max_retry);

        ret = at_send_raw(cmd, "WIFI GOT IP", "OK", WIFI_WAIT_MS, response, sizeof(response));
        if (ret == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "AT+CWJAP failed: %s", esp_err_to_name(ret));
        (void)at_send_raw("AT+CWQAP", "OK", NULL, 3000, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (ret != ESP_OK) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ret;
    }

    response[0] = '\0';
    (void)at_send_raw("AT+CWJAP?", "OK", NULL, 2000, response, sizeof(response));
    int8_t rssi = parse_rssi_from_cwjap(response);

    response[0] = '\0';
    (void)at_send_raw("AT+CIPSTA?", "OK", NULL, 2000, response, sizeof(response));
    parse_ip_from_cipsta(response);

    if (s_last_ip[0]) {
        ESP_LOGI(TAG, "ESP32-C6 got IP: %s", s_last_ip);
    } else {
        ESP_LOGI(TAG, "ESP32-C6 connected; IP query response: %s", response);
    }

    strlcpy(s_connected_ssid, WIFI_SSID, sizeof(s_connected_ssid));
    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTED, true, rssi);
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);

    ESP_LOGI(TAG, "Starting ESP32-C6 UART AT WiFi backend");
    ESP_LOGW(TAG,
             "This backend connects WiFi on the C6 AT modem. It does not create a native lwIP netif on ESP32-P4.");

    esp_err_t ret = c6_uart_init_once();
    if (ret != ESP_OK) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ret;
    }

    c6_optional_reset();

    ret = c6_at_connect_wifi();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP32-C6 AT WiFi start failed: %s", esp_err_to_name(ret));
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ret;
    }

    ESP_LOGI(TAG, "ESP32-C6 AT WiFi connected, RSSI=%d dBm", g_wifi_state.rssi);
    return ESP_OK;
}

static void wifi_manager_task(void *arg)
{
    (void)arg;

    const esp_err_t ret = wifi_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async ESP32-C6 AT WiFi start finished with %s", esp_err_to_name(ret));
    }

    s_wifi_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_async(void)
{
    if (s_async_task_started || s_wifi_task_handle) {
        ESP_LOGI(TAG, "ESP32-C6 AT WiFi async start ignored: already running/started");
        return ESP_OK;
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);

    BaseType_t ret = xTaskCreate(wifi_manager_task,
                                 "wifi_c6_at",
                                 CONFIG_DASHBOARD_WIFI_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_DASHBOARD_WIFI_TASK_PRIORITY,
                                 &s_wifi_task_handle);
    if (ret != pdPASS) {
        s_async_task_started = false;
        s_wifi_task_handle = NULL;
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ESP_ERR_NO_MEM;
    }

    s_async_task_started = true;
    ESP_LOGI(TAG, "ESP32-C6 AT WiFi manager task started");
    return ESP_OK;
}

wifi_manager_connection_status_t wifi_manager_get_status(void)
{
    return g_wifi_state.status;
}

int8_t wifi_manager_get_rssi(void)
{
    return g_wifi_state.connected ? g_wifi_state.rssi : 0;
}

const char *wifi_manager_get_connected_ssid(void)
{
    if (!g_wifi_state.connected || !s_connected_ssid[0]) {
        return "";
    }
    return s_connected_ssid;
}

const char *wifi_manager_get_backend_name(void)
{
    return "ESP32-C6 UART AT";
}
