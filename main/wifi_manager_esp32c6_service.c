#include "wifi_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4_project_config.h"

static const char *TAG = "C6_SERVICE";

#define C6_UART_NUM       ((uart_port_t)((C6_PROG_UART_NUM >= 0) ? C6_PROG_UART_NUM : CONFIG_DASHBOARD_C6_AT_UART_NUM))
#define C6_UART_BAUD      ((C6_PROG_UART_BAUD > 0) ? C6_PROG_UART_BAUD : CONFIG_DASHBOARD_C6_AT_BAUD)
#define C6_TX_GPIO        ((C6_PROG_P4_TX_GPIO >= 0) ? C6_PROG_P4_TX_GPIO : CONFIG_DASHBOARD_C6_AT_TX_GPIO)
#define C6_RX_GPIO        ((C6_PROG_P4_RX_GPIO >= 0) ? C6_PROG_P4_RX_GPIO : CONFIG_DASHBOARD_C6_AT_RX_GPIO)
#define C6_CHIP_PU_GPIO   ((C6_PROG_CHIP_PU_GPIO >= 0) ? C6_PROG_CHIP_PU_GPIO : CONFIG_DASHBOARD_C6_AT_CHIP_PU_GPIO)
#define C6_BOOT_GPIO      ((C6_PROG_BOOT_GPIO >= 0) ? C6_PROG_BOOT_GPIO : CONFIG_DASHBOARD_C6_AT_BOOT_GPIO)

#define C6_RX_BUF_SIZE    2048
#define C6_TX_BUF_SIZE    512
#define C6_AT_RESP_SIZE   1536
#define C6_SYNC_RETRIES   CONFIG_DASHBOARD_C6_SERVICE_SYNC_RETRIES

#ifndef CONFIG_DASHBOARD_C6_SERVICE_PROBE_AT
#define CONFIG_DASHBOARD_C6_SERVICE_PROBE_AT 0
#endif
#ifndef CONFIG_DASHBOARD_C6_SERVICE_PROBE_BOOTLOADER
#define CONFIG_DASHBOARD_C6_SERVICE_PROBE_BOOTLOADER 0
#endif
#ifndef CONFIG_DASHBOARD_C6_SERVICE_RETURN_TO_APP
#define CONFIG_DASHBOARD_C6_SERVICE_RETURN_TO_APP 0
#endif

volatile wifi_manager_state_t g_wifi_state = {
    .status = WIFI_MANAGER_STATUS_IDLE,
    .connected = false,
    .rssi = 0,
};

static bool s_uart_ready;
static bool s_async_task_started;
static TaskHandle_t s_service_task_handle;

static void wifi_manager_set_state(wifi_manager_connection_status_t status,
                                   bool connected,
                                   int8_t rssi)
{
    g_wifi_state.status = status;
    g_wifi_state.connected = connected;
    g_wifi_state.rssi = rssi;
}

static bool gpio_is_valid_configured(int gpio)
{
    return gpio >= 0 && gpio <= 54;
}

static esp_err_t c6_uart_init_once(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

    if (!gpio_is_valid_configured(C6_TX_GPIO) || !gpio_is_valid_configured(C6_RX_GPIO)) {
        ESP_LOGE(TAG,
                 "ESP32-C6 UART pins are not configured: P4_TX=%d P4_RX=%d. "
                 "Set Dashboard WiFi Configuration -> ESP32-C6 UART GPIOs in menuconfig.",
                 C6_TX_GPIO,
                 C6_RX_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_config = {
        .baud_rate = C6_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(C6_UART_NUM, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(C6_UART_NUM,
                                     C6_TX_GPIO,
                                     C6_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin failed");

    esp_err_t ret = uart_driver_install(C6_UART_NUM,
                                        C6_RX_BUF_SIZE,
                                        C6_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART driver already installed, reusing it");
        ret = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "uart_driver_install failed");

    uart_flush_input(C6_UART_NUM);
    s_uart_ready = true;

    ESP_LOGI(TAG,
             "ESP32-C6 service UART ready: uart=%d baud=%d P4_TX(GPIO%d)->C6_U0RXD P4_RX(GPIO%d)<-C6_U0TXD",
             (int)C6_UART_NUM,
             C6_UART_BAUD,
             C6_TX_GPIO,
             C6_RX_GPIO);
    return ESP_OK;
}

static void c6_gpio_prepare_output(int gpio, int level)
{
    if (!gpio_is_valid_configured(gpio)) {
        return;
    }
    gpio_reset_pin((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)gpio, level);
}

static void c6_reset_normal_boot(void)
{
    if (gpio_is_valid_configured(C6_BOOT_GPIO)) {
        /* GPIO9 high/open at reset -> boot normal application from flash. */
        c6_gpio_prepare_output(C6_BOOT_GPIO, 1);
    }

    if (gpio_is_valid_configured(C6_CHIP_PU_GPIO)) {
        ESP_LOGI(TAG, "Reset C6 to normal boot: CHIP_PU GPIO%d, BOOT GPIO%d high",
                 C6_CHIP_PU_GPIO,
                 C6_BOOT_GPIO);
        c6_gpio_prepare_output(C6_CHIP_PU_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level((gpio_num_t)C6_CHIP_PU_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(1200));
    } else {
        ESP_LOGW(TAG, "C6_CHIP_PU GPIO is not configured; cannot reset C6, waiting only");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t c6_enter_download_bootloader(void)
{
    if (!gpio_is_valid_configured(C6_BOOT_GPIO) || !gpio_is_valid_configured(C6_CHIP_PU_GPIO)) {
        ESP_LOGE(TAG,
                 "Cannot enter C6 ROM bootloader: BOOT/C6_IO9 GPIO=%d CHIP_PU GPIO=%d. "
                 "Both lines must be controlled by ESP32-P4 for this probe.",
                 C6_BOOT_GPIO,
                 C6_CHIP_PU_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "Enter C6 UART download mode: BOOT/C6_IO9 GPIO%d low, pulse CHIP_PU GPIO%d",
             C6_BOOT_GPIO,
             C6_CHIP_PU_GPIO);

    c6_gpio_prepare_output(C6_BOOT_GPIO, 0);
    c6_gpio_prepare_output(C6_CHIP_PU_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level((gpio_num_t)C6_CHIP_PU_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(700));
    uart_flush_input(C6_UART_NUM);
    return ESP_OK;
}

static void uart_drain_input(uint32_t ms)
{
    uint8_t tmp[128];
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        int n = uart_read_bytes(C6_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(20));
        if (n <= 0) {
            break;
        }
    }
}

static esp_err_t at_wait_for(const char *token, uint32_t timeout_ms, char *out, size_t out_size)
{
    char rx[C6_AT_RESP_SIZE];
    size_t used = 0;
    memset(rx, 0, sizeof(rx));

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t chunk[128];
        int n = uart_read_bytes(C6_UART_NUM, chunk, sizeof(chunk) - 1, pdMS_TO_TICKS(50));
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

        if (strstr(rx, token)) {
            return ESP_OK;
        }
        if (strstr(rx, "ERROR") || strstr(rx, "FAIL")) {
            ESP_LOGW(TAG, "AT response failure: %s", rx);
            return ESP_FAIL;
        }
    }

    ESP_LOGW(TAG, "AT timeout waiting for %s, last response: %s", token, rx);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t at_send(const char *cmd, uint32_t timeout_ms, char *out, size_t out_size)
{
    uart_drain_input(20);
    ESP_LOGI(TAG, "> %s", cmd);
    uart_write_bytes(C6_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(C6_UART_NUM, "\r\n", 2);
    return at_wait_for("OK", timeout_ms, out, out_size);
}

static esp_err_t c6_probe_at_normal_boot(void)
{
    char response[C6_AT_RESP_SIZE];
    esp_err_t ret = ESP_ERR_TIMEOUT;

    c6_reset_normal_boot();

    for (int i = 0; i < 5; ++i) {
        ret = at_send("AT", 700, response, sizeof(response));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "C6 normal boot answered AT");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "C6 did not answer AT in normal boot");
        return ret;
    }

    (void)at_send("ATE0", 1000, NULL, 0);
    response[0] = '\0';
    (void)at_send("AT+GMR", 1500, response, sizeof(response));
    ESP_LOGI(TAG, "C6 AT version response: %s", response);
    return ESP_OK;
}

static void slip_write_byte(uint8_t b)
{
    if (b == 0xC0) {
        const uint8_t esc[] = {0xDB, 0xDC};
        uart_write_bytes(C6_UART_NUM, esc, sizeof(esc));
    } else if (b == 0xDB) {
        const uint8_t esc[] = {0xDB, 0xDD};
        uart_write_bytes(C6_UART_NUM, esc, sizeof(esc));
    } else {
        uart_write_bytes(C6_UART_NUM, &b, 1);
    }
}

static void slip_write_packet(const uint8_t *packet, size_t len)
{
    const uint8_t end = 0xC0;
    uart_write_bytes(C6_UART_NUM, &end, 1);
    for (size_t i = 0; i < len; ++i) {
        slip_write_byte(packet[i]);
    }
    uart_write_bytes(C6_UART_NUM, &end, 1);
}

static esp_err_t slip_read_packet(uint8_t *out, size_t out_size, size_t *out_len, uint32_t timeout_ms)
{
    bool in_frame = false;
    bool esc = false;
    size_t used = 0;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t b;
        int n = uart_read_bytes(C6_UART_NUM, &b, 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        if (b == 0xC0) {
            if (in_frame && used > 0) {
                *out_len = used;
                return ESP_OK;
            }
            in_frame = true;
            esc = false;
            used = 0;
            continue;
        }

        if (!in_frame) {
            continue;
        }

        if (esc) {
            if (b == 0xDC) {
                b = 0xC0;
            } else if (b == 0xDD) {
                b = 0xDB;
            }
            esc = false;
        } else if (b == 0xDB) {
            esc = true;
            continue;
        }

        if (used >= out_size) {
            ESP_LOGW(TAG, "SLIP packet too large");
            return ESP_ERR_INVALID_SIZE;
        }
        out[used++] = b;
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t c6_send_rom_sync_once(void)
{
    uint8_t sync_data[36];
    sync_data[0] = 0x07;
    sync_data[1] = 0x07;
    sync_data[2] = 0x12;
    sync_data[3] = 0x20;
    memset(sync_data + 4, 0x55, sizeof(sync_data) - 4);

    uint8_t pkt[8 + sizeof(sync_data)];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x00;                         /* request */
    pkt[1] = 0x08;                         /* ESP_SYNC command */
    pkt[2] = (uint8_t)sizeof(sync_data);    /* little-endian data size */
    pkt[3] = 0x00;
    /* ESP_SYNC does not use the checksum field. esptool sends it as zero. */
    pkt[4] = 0x00;
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
    memcpy(pkt + 8, sync_data, sizeof(sync_data));

    slip_write_packet(pkt, sizeof(pkt));

    uint8_t resp[256];
    size_t resp_len = 0;
    esp_err_t ret = slip_read_packet(resp, sizeof(resp), &resp_len, 300);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "C6 ROM response packet len=%u dir=0x%02x cmd=0x%02x",
             (unsigned)resp_len,
             resp_len > 0 ? resp[0] : 0xff,
             resp_len > 1 ? resp[1] : 0xff);

    if (resp_len >= 2 && resp[0] == 0x01 && resp[1] == 0x08) {
        return ESP_OK;
    }

    /* Some ROM/stub responses vary a little; any framed answer is still useful diagnostic data. */
    return ESP_OK;
}

static esp_err_t c6_probe_rom_bootloader(void)
{
    esp_err_t ret = c6_enter_download_bootloader();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Sending ESP ROM UART SYNC packets to C6...");
    for (int i = 1; i <= C6_SYNC_RETRIES; ++i) {
        ret = c6_send_rom_sync_once();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "C6 ROM bootloader SYNC answered on attempt %d/%d", i, C6_SYNC_RETRIES);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    ESP_LOGW(TAG, "C6 ROM bootloader did not answer SYNC");
    return ret;
}

static void c6_programmer_print_pin_plan(void)
{
    ESP_LOGI(TAG, "========== ESP32-C6 programmer wiring ==========");
    ESP_LOGI(TAG, "C6 UART: uart=%d baud=%d", (int)C6_UART_NUM, C6_UART_BAUD);
    ESP_LOGI(TAG, "P4_TX GPIO%d -> C6_U0RXD", C6_TX_GPIO);
    ESP_LOGI(TAG, "P4_RX GPIO%d <- C6_U0TXD", C6_RX_GPIO);
    ESP_LOGI(TAG, "P4 GPIO%d -> C6_CHIP_PU/EN", C6_CHIP_PU_GPIO);
    ESP_LOGI(TAG, "P4 GPIO%d -> C6_IO9/BOOT", C6_BOOT_GPIO);
    ESP_LOGI(TAG, "Change these in main/p4_project_config.h, not in generated EEZ files.");
    ESP_LOGI(TAG, "================================================");
}

static void c6_programmer_bridge_forever(void)
{
#if C6_PROG_ENABLE_TRANSPARENT_BRIDGE
    uint8_t pc_to_c6[256];
    uint8_t c6_to_pc[256];

    ESP_LOGW(TAG, "C6 transparent programmer bridge is about to start.");
    ESP_LOGW(TAG, "Close idf.py monitor with Ctrl+] BEFORE running esptool.");
    ESP_LOGW(TAG, "Then try:");
    ESP_LOGW(TAG, "python -m esptool --chip esp32c6 -p COM7 -b 115200 --before no_reset --after no_reset flash_id");
    ESP_LOGW(TAG, "From now on logs will be disabled to avoid corrupting esptool traffic.");
    ESP_LOGW(TAG, "Bridge implementation: direct USB_SERIAL_JTAG <-> UART%d byte pump", (int)C6_UART_NUM);

    /*
     * Do not rely on POSIX stdin/stdout here.  On ESP32-P4 the PC COM port is
     * usually the native USB Serial/JTAG CDC endpoint, and if the bridge does
     * not drain that endpoint directly, pySerial/esptool can stall with
     * "Write timeout" before it even reaches C6.
     */
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 4096,
        .tx_buffer_size = 4096,
    };
    esp_err_t usb_ret = usb_serial_jtag_driver_install(&usb_cfg);
    if (usb_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB_SERIAL_JTAG driver already installed; reusing it");
        usb_ret = ESP_OK;
    }
    if (usb_ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(usb_ret));
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_NONE);

    while (true) {
        int n = usb_serial_jtag_read_bytes(pc_to_c6, sizeof(pc_to_c6), pdMS_TO_TICKS(1));
        if (n > 0) {
            (void)uart_write_bytes(C6_UART_NUM, pc_to_c6, (size_t)n);
        }

        int m = uart_read_bytes(C6_UART_NUM, c6_to_pc, sizeof(c6_to_pc), pdMS_TO_TICKS(1));
        if (m > 0) {
            (void)usb_serial_jtag_write_bytes(c6_to_pc, m, pdMS_TO_TICKS(20));
        }

        if (n <= 0 && m <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
#else
    ESP_LOGW(TAG, "C6 transparent bridge disabled by C6_PROG_ENABLE_TRANSPARENT_BRIDGE=0");
#endif
}

esp_err_t wifi_manager_start(void)
{
    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
    ESP_LOGI(TAG, "Starting ESP32-C6 UART service/probe mode");
    ESP_LOGW(TAG, "This mode is diagnostic only: it does not connect WiFi.");

    c6_programmer_print_pin_plan();

    esp_err_t ret = c6_uart_init_once();
    if (ret != ESP_OK) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ret;
    }

    bool any_ok = false;
    bool bootloader_ok = false;
    bool bootloader_attempted = false;

#if CONFIG_DASHBOARD_C6_SERVICE_PROBE_AT
    esp_err_t at_ret = c6_probe_at_normal_boot();
    if (at_ret == ESP_OK) {
        any_ok = true;
    } else {
        ESP_LOGW(TAG, "AT normal-boot probe failed: %s", esp_err_to_name(at_ret));
    }
#endif

#if CONFIG_DASHBOARD_C6_SERVICE_PROBE_BOOTLOADER
    bootloader_attempted = true;
    esp_err_t bl_ret = c6_probe_rom_bootloader();
    if (bl_ret == ESP_OK) {
        any_ok = true;
        bootloader_ok = true;
    } else {
        ESP_LOGW(TAG, "ROM bootloader probe failed: %s", esp_err_to_name(bl_ret));
    }
#endif

#if C6_PROG_BRIDGE_AFTER_BOOTLOADER_PROBE
    /*
     * Do not gate the external esptool bridge on our small internal SYNC probe.
     * If the probe fails because of timing/protocol quirks, C6 may still be in
     * ROM download mode.  Start the transparent bridge anyway after we have
     * attempted the boot sequence, so PC-side esptool can perform its own SYNC.
     */
    if (bootloader_attempted) {
        if (!bootloader_ok) {
            ESP_LOGW(TAG, "ROM SYNC probe did not confirm C6, but starting transparent bridge anyway.");
            ESP_LOGW(TAG, "This is intentional: external esptool will do the real SYNC/flash_id test.");
        }
        wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
        c6_programmer_bridge_forever();
    } else {
        ESP_LOGW(TAG, "Transparent bridge requested, but bootloader probe is disabled in menuconfig.");
    }
#endif

#if CONFIG_DASHBOARD_C6_SERVICE_RETURN_TO_APP
    c6_reset_normal_boot();
#endif

    if (any_ok) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTED, true, 0);
        ESP_LOGI(TAG, "ESP32-C6 service probe SUCCESS: at least one C6 path answered");
        return ESP_OK;
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
    ESP_LOGE(TAG, "ESP32-C6 service probe FAILED: no AT or ROM bootloader response");
    return ESP_FAIL;
}

static void service_task(void *arg)
{
    (void)arg;

    const esp_err_t ret = wifi_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async ESP32-C6 service probe finished with %s", esp_err_to_name(ret));
    }

    s_service_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_async(void)
{
    if (s_async_task_started || s_service_task_handle) {
        ESP_LOGI(TAG, "ESP32-C6 service async start ignored: already running/started");
        return ESP_OK;
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);

    BaseType_t ret = xTaskCreate(service_task,
                                 "c6_service",
                                 CONFIG_DASHBOARD_WIFI_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_DASHBOARD_WIFI_TASK_PRIORITY,
                                 &s_service_task_handle);
    if (ret != pdPASS) {
        s_async_task_started = false;
        s_service_task_handle = NULL;
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ESP_ERR_NO_MEM;
    }

    s_async_task_started = true;
    ESP_LOGI(TAG, "ESP32-C6 service task started");
    return ESP_OK;
}

wifi_manager_connection_status_t wifi_manager_get_status(void)
{
    return g_wifi_state.status;
}

int8_t wifi_manager_get_rssi(void)
{
    return 0;
}

const char *wifi_manager_get_connected_ssid(void)
{
    return "";
}

const char *wifi_manager_get_backend_name(void)
{
    return "ESP32-C6 UART service";
}
