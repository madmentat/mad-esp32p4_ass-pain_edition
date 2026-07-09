#include "update/update_manager.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "esp_task_wdt.h"
#include "mbedtls/sha256.h"

#include "wifi_manager.h"
#include "stm32_swd_programmer.h"
#include "demo_backlight.h"
#include "ui_background_direct.h"

static const char *TAG = "MAD_OTA";

#define MAD_OTA_RESPONSE_MAX_BYTES (32 * 1024)
#define MAD_OTA_URL_MAX_BYTES      384
#define MAD_OTA_POST_MAX_BYTES     1024
#define MAD_OTA_SHA256_HEX_LEN     64
#define MAD_OTA_LOG_CHUNK_BYTES    220

#ifndef CONFIG_MAD_OTA_LOG_HTTP_PAYLOADS
#define CONFIG_MAD_OTA_LOG_HTTP_PAYLOADS 1
#endif

#ifndef CONFIG_MAD_OTA_P4_MIN_IMAGE_BYTES
#define CONFIG_MAD_OTA_P4_MIN_IMAGE_BYTES (128 * 1024)
#endif

#ifndef CONFIG_MAD_OTA_ENABLE
#define CONFIG_MAD_OTA_ENABLE 0
#endif

#ifndef CONFIG_MAD_OTA_AUTO_CHECK
#define CONFIG_MAD_OTA_AUTO_CHECK 0
#endif

#ifndef CONFIG_MAD_OTA_AUTO_INSTALL
#define CONFIG_MAD_OTA_AUTO_INSTALL 0
#endif

#ifndef CONFIG_MAD_OTA_TRY_STATIC_MANIFEST_FALLBACK
#define CONFIG_MAD_OTA_TRY_STATIC_MANIFEST_FALLBACK 0
#endif

#ifndef CONFIG_MAD_OTA_HTTP_TIMEOUT_MS
#define CONFIG_MAD_OTA_HTTP_TIMEOUT_MS 20000
#endif

#ifndef CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS
#define CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_MAD_OTA_TASK_STACK_SIZE
#define CONFIG_MAD_OTA_TASK_STACK_SIZE 8192
#endif

#ifndef CONFIG_MAD_OTA_TASK_PRIORITY
#define CONFIG_MAD_OTA_TASK_PRIORITY 2
#endif

#ifndef CONFIG_MAD_OTA_TASK_CORE
#define CONFIG_MAD_OTA_TASK_CORE 0
#endif

#ifndef CONFIG_MAD_OTA_DOWNLOAD_CHUNK_BYTES
#define CONFIG_MAD_OTA_DOWNLOAD_CHUNK_BYTES 2048
#endif

#ifndef CONFIG_MAD_OTA_WRITE_THROTTLE_MS
#define CONFIG_MAD_OTA_WRITE_THROTTLE_MS 2
#endif

/* fix49: ESP32-P4 self-OTA transfer limiter.
 *
 * The previous diagnostic/no-touch path still capped P4 self-OTA at
 * 256 bytes + 60 ms between writes.  A 6.9 MB image then looked like it
 * never downloaded: the throttling alone costs ~27 minutes.  In this branch
 * LVGL flush/timer work is already suspended during P4 self-OTA, so let the
 * configured values work again while keeping sane bounds.  STM32/SWD OTA is
 * not throttled by these constants. */
#ifndef MAD_OTA_P4_SELF_CHUNK_MAX_BYTES
#define MAD_OTA_P4_SELF_CHUNK_MAX_BYTES 4096U
#endif

#ifndef MAD_OTA_P4_SELF_CHUNK_MIN_BYTES
#define MAD_OTA_P4_SELF_CHUNK_MIN_BYTES 256U
#endif

#ifndef MAD_OTA_P4_SELF_THROTTLE_MIN_MS
#define MAD_OTA_P4_SELF_THROTTLE_MIN_MS 0
#endif

#ifndef MAD_OTA_P4_SELF_THROTTLE_MAX_MS
#define MAD_OTA_P4_SELF_THROTTLE_MAX_MS 250
#endif

#ifndef MAD_OTA_P4_DISPLAY_HUD_REFRESH_MS
#define MAD_OTA_P4_DISPLAY_HUD_REFRESH_MS 250
#endif

#ifndef MAD_OTA_P4_REBOOT_MESSAGE_MS
#define MAD_OTA_P4_REBOOT_MESSAGE_MS 1200
#endif

#ifndef MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_MS
#define MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_MS 350
#endif

#ifndef MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_PCT
#define MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_PCT 0
#endif

#ifndef CONFIG_MAD_OTA_REPORT_STEP_PERCENT
#define CONFIG_MAD_OTA_REPORT_STEP_PERCENT 25
#endif

/* fix50: UART-side self-OTA observability.  During ESP32-P4 self-OTA the
 * display is intentionally frozen to avoid LCD/PSRAM/flash contention, so the
 * LVGL progress bar cannot be trusted as a visible indicator.  Emit frequent
 * console progress instead, independently from the less frequent HTTPS
 * progress reports sent to the update server. */
#ifndef MAD_OTA_P4_CONSOLE_PROGRESS_PERIOD_MS
#define MAD_OTA_P4_CONSOLE_PROGRESS_PERIOD_MS 2000
#endif

#ifndef MAD_OTA_P4_CONSOLE_PROGRESS_STEP_PERCENT
#define MAD_OTA_P4_CONSOLE_PROGRESS_STEP_PERCENT 1
#endif

#ifndef CONFIG_MAD_OTA_APP_VERSION
#define CONFIG_MAD_OTA_APP_VERSION "0.1.1-test"
#endif

#ifndef CONFIG_MAD_OTA_PRODUCT_ID
#define CONFIG_MAD_OTA_PRODUCT_ID "jc4880p443c_demo"
#endif

#ifndef CONFIG_MAD_OTA_HW_REV
#define CONFIG_MAD_OTA_HW_REV "jc4880p443c-p4-c6-v1"
#endif

#ifndef CONFIG_MAD_OTA_CHANNEL
#define CONFIG_MAD_OTA_CHANNEL "test"
#endif

#ifndef CONFIG_MAD_OTA_TARGET_ID
#define CONFIG_MAD_OTA_TARGET_ID "esp32p4"
#endif

#ifndef CONFIG_MAD_OTA_LOCAL_SSID
#define CONFIG_MAD_OTA_LOCAL_SSID "ARD"
#endif

#ifndef CONFIG_MAD_OTA_USE_LOCAL_SERVER
#define CONFIG_MAD_OTA_USE_LOCAL_SERVER 0
#endif

#ifndef CONFIG_MAD_OTA_LOCAL_BASE_URL
#define CONFIG_MAD_OTA_LOCAL_BASE_URL "http://192.168.88.17:8090"
#endif

#ifndef CONFIG_MAD_OTA_PUBLIC_BASE_URL
#define CONFIG_MAD_OTA_PUBLIC_BASE_URL "https://test.ard-s.ru"
#endif

#ifndef CONFIG_MAD_OTA_LOCAL_FIRMWARE_BASE_URL
#define CONFIG_MAD_OTA_LOCAL_FIRMWARE_BASE_URL "http://192.168.88.17/firmware"
#endif

#ifndef CONFIG_MAD_OTA_PUBLIC_FIRMWARE_BASE_URL
#define CONFIG_MAD_OTA_PUBLIC_FIRMWARE_BASE_URL "https://test.ard-s.ru"
#endif

#ifndef CONFIG_MAD_OTA_START_PATH
#define CONFIG_MAD_OTA_START_PATH "/api/v1/update/start"
#endif

#ifndef CONFIG_MAD_OTA_PROGRESS_PATH
#define CONFIG_MAD_OTA_PROGRESS_PATH "/api/v1/update/progress"
#endif

#ifndef CONFIG_MAD_OTA_REPORT_PATH
#define CONFIG_MAD_OTA_REPORT_PATH "/api/v1/update/report"
#endif

#ifndef CONFIG_MAD_OTA_CHECK_PATH
#define CONFIG_MAD_OTA_CHECK_PATH "/api/v1/update/check"
#endif

#ifndef CONFIG_MAD_OTA_STATIC_MANIFEST_PATH
#define CONFIG_MAD_OTA_STATIC_MANIFEST_PATH "/firmware/test/manifest_p4_ota_test.json"
#endif

#ifndef CONFIG_MAD_OTA_STM32_TARGET_ID
#define CONFIG_MAD_OTA_STM32_TARGET_ID "STM32F030K6T6"
#endif

#ifndef CONFIG_MAD_OTA_STM32_CURRENT_VERSION
#define CONFIG_MAD_OTA_STM32_CURRENT_VERSION "4.6"
#endif

#ifndef CONFIG_MAD_OTA_STM32_FLASH_BASE
#define CONFIG_MAD_OTA_STM32_FLASH_BASE 0x08000000U
#endif

#ifndef CONFIG_MAD_OTA_STM32_MAX_IMAGE_BYTES
#define CONFIG_MAD_OTA_STM32_MAX_IMAGE_BYTES (32 * 1024)
#endif

#ifndef CONFIG_MAD_OTA_STM32_FLASH_ATTEMPTS
#define CONFIG_MAD_OTA_STM32_FLASH_ATTEMPTS 5
#endif

#ifndef CONFIG_MAD_OTA_STM32_FLASH_RETRY_DELAY_MS
#define CONFIG_MAD_OTA_STM32_FLASH_RETRY_DELAY_MS 1000
#endif

volatile mad_ota_state_t g_mad_ota_state = {
    .status = MAD_OTA_STATUS_IDLE,
    .progress_percent = 0,
    .current_version = CONFIG_MAD_OTA_APP_VERSION,
    .available_version = "",
    .last_error = "",
};

static TaskHandle_t s_ota_task_handle;
static TaskHandle_t s_ota_install_task_handle;
static TaskHandle_t s_stm32_ota_task_handle;
static bool s_ota_task_started;
static bool s_update_component_valid;
static volatile bool s_manual_install_attempted;

typedef struct {
    char base_url[MAD_OTA_URL_MAX_BYTES];
    char manifest_url[MAD_OTA_URL_MAX_BYTES];
    char firmware_url[MAD_OTA_URL_MAX_BYTES];
    char version[32];
    char sha256[80];
    char file_name[128];
    char target[32];
    int64_t size_bytes;
    bool reboot_required;
    bool rollback_supported;
    bool mandatory;
    bool allow_downgrade;
    bool rollback;
    bool auto_update;
    bool auto_rollback;
    int release_id;
    int deployment_id;
    int firmware_file_id;
} mad_ota_manifest_component_t;

static mad_ota_manifest_component_t s_update_component;

static void ota_set_status(mad_ota_status_t status)
{
    g_mad_ota_state.status = status;
}

static void ota_set_progress(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    g_mad_ota_state.progress_percent = percent;
}

static void ota_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf((char *)g_mad_ota_state.last_error, sizeof(g_mad_ota_state.last_error), fmt, ap);
    va_end(ap);
    ota_set_status(MAD_OTA_STATUS_FAILED);
    ESP_LOGE(TAG, "%s", g_mad_ota_state.last_error);
}

static void ota_set_check_warning(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf((char *)g_mad_ota_state.last_error, sizeof(g_mad_ota_state.last_error), fmt, ap);
    va_end(ap);
    /* Auto-check failures must not look like a failed firmware installation.
     * Keep the UI on the current FW version; the network status is shown separately. */
    ota_set_status(MAD_OTA_STATUS_IDLE);
    ESP_LOGW(TAG, "%s", g_mad_ota_state.last_error);
}

static bool str_is_empty(const char *s)
{
    return !s || !s[0];
}



typedef struct {
    bool active;
    bool reconfigured;
    uint32_t ota_timeout_ms;
    uint32_t restore_timeout_ms;
} ota_task_wdt_guard_t;

#if CONFIG_ESP_TASK_WDT
#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 5
#endif

#ifndef MAD_OTA_TASK_WDT_TIMEOUT_MS
#define MAD_OTA_TASK_WDT_TIMEOUT_MS (120U * 1000U)
#endif

static uint32_t ota_task_wdt_idle_core_mask(void)
{
    uint32_t mask = 0;

#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= (1U << 0);
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    mask |= (1U << 1);
#endif

    return mask;
}

static bool ota_task_wdt_trigger_panic(void)
{
#if defined(CONFIG_ESP_TASK_WDT_PANIC) && CONFIG_ESP_TASK_WDT_PANIC
    return true;
#else
    return false;
#endif
}

static esp_err_t ota_task_wdt_apply_timeout(uint32_t timeout_ms, const char *reason)
{
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = ota_task_wdt_idle_core_mask(),
        .trigger_panic = ota_task_wdt_trigger_panic(),
    };

    const esp_err_t ret = esp_task_wdt_reconfigure(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "Task WDT reconfigured: timeout=%lu ms idle_mask=0x%lx panic=%d reason=%s",
                 (unsigned long)timeout_ms,
                 (unsigned long)cfg.idle_core_mask,
                 (int)cfg.trigger_panic,
                 reason ? reason : "OTA");
    } else {
        ESP_LOGW(TAG,
                 "Task WDT reconfigure failed: timeout=%lu ms reason=%s err=%s",
                 (unsigned long)timeout_ms,
                 reason ? reason : "OTA",
                 esp_err_to_name(ret));
    }

    return ret;
}
#endif

static void ota_task_wdt_suspend_for_ota(ota_task_wdt_guard_t *guard, const char *reason)
{
    if (!guard) {
        return;
    }
    memset(guard, 0, sizeof(*guard));

#if CONFIG_ESP_TASK_WDT
    /* fix27 tried to unsubscribe IDLE0/IDLE1 from TWDT.  On ESP-IDF the idle
     * tasks can still call esp_task_wdt_reset() from their idle hook, which
     * then floods the log with "task not found" and makes LCD/DSI timing even
     * worse.  Do not delete idle tasks.  Instead keep the TWDT initialized and
     * stretch its timeout for the OTA/SWD critical window; after the operation
     * restore the sdkconfig timeout. */
    guard->restore_timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U;
    guard->ota_timeout_ms = MAD_OTA_TASK_WDT_TIMEOUT_MS;

    if (guard->ota_timeout_ms < guard->restore_timeout_ms) {
        guard->ota_timeout_ms = guard->restore_timeout_ms;
    }

    const esp_err_t ret = ota_task_wdt_apply_timeout(guard->ota_timeout_ms,
                                                     reason ? reason : "OTA start");
    guard->active = (ret == ESP_OK);
    guard->reconfigured = (ret == ESP_OK);
#else
    (void)reason;
    guard->active = false;
#endif
}

static void ota_task_wdt_resume_after_ota(ota_task_wdt_guard_t *guard, const char *reason)
{
    if (!guard || !guard->active) {
        return;
    }

#if CONFIG_ESP_TASK_WDT
    if (guard->reconfigured) {
        (void)ota_task_wdt_apply_timeout(guard->restore_timeout_ms,
                                         reason ? reason : "OTA end");
        ESP_LOGW(TAG, "Task WDT timeout restored after OTA window: %s", reason ? reason : "OTA");
    }
#else
    (void)reason;
#endif

    memset(guard, 0, sizeof(*guard));
}

static volatile bool s_p4_ota_display_hold_active;
static int64_t s_p4_ota_display_hud_next_us;

bool mad_ota_p4_self_ota_display_hold_active(void)
{
    return s_p4_ota_display_hold_active;
}

static void ota_p4_display_hud_refresh(bool force)
{
    if (!s_p4_ota_display_hold_active) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (!force && s_p4_ota_display_hud_next_us > 0 && now_us < s_p4_ota_display_hud_next_us) {
        return;
    }

    s_p4_ota_display_hud_next_us = now_us + ((int64_t)MAD_OTA_P4_DISPLAY_HUD_REFRESH_MS * 1000LL);
    (void)ui_background_redraw_runtime_overlay_regions();
}

static void ota_p4_show_reboot_message_then_blank_backlight(void)
{
    /* fix55: after the new ESP32-P4 image is fully installed and the success
     * report has been sent, make the last user-visible state explicit:
     * show REBOOTING / wait for a while, let the frame reach the panel, then
     * switch the backlight off immediately before esp_restart().  The next
     * boot restores the normal backlight during display initialization. */
    ota_set_status(MAD_OTA_STATUS_REBOOT_PENDING);
    ota_set_progress(100);
    ota_p4_display_hud_refresh(true);

    ESP_LOGW(TAG,
             "P4 OTA reboot screen: showing REBOOTING/wait message for %d ms before backlight off",
             (int)MAD_OTA_P4_REBOOT_MESSAGE_MS);
    vTaskDelay(pdMS_TO_TICKS(MAD_OTA_P4_REBOOT_MESSAGE_MS));

    ESP_LOGW(TAG,
             "P4 OTA reboot screen: setting backlight to %d%% for %d ms before esp_restart",
             (int)MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_PCT,
             (int)MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_MS);
    esp_err_t bl_ret = demo_backlight_set(MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_PCT);
    if (bl_ret != ESP_OK) {
        ESP_LOGW(TAG, "P4 OTA reboot screen: backlight off failed: %s", esp_err_to_name(bl_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(MAD_OTA_P4_REBOOT_BACKLIGHT_OFF_MS));
}

static void ota_display_quiet_mode(bool enable)
{
    /* fix52: UI-active ESP32-P4 self-OTA with clear phases/status.
     *
     * The previous fix34/fix50 mode froze LVGL task+flush and all native
     * overlays while P4 wrote its own flash. It was visually stable, but the
     * user had no on-screen indication that the OTA transfer was running.
     *
     * In this build we keep the display ON *and* keep LVGL/direct
     * overlay redraws enabled. The info strip and native OTA HUD can now show
     * DL/INSTALL progress while UART progress logs remain active. If flicker
     * returns, this mode is the cause and we can compromise later by throttling
     * redraw frequency rather than fully freezing the UI.
     *
     * This function is intentionally NOT used for STM32/SWD OTA. */
    static bool s_display_guard_active;

    if (enable) {
        if (s_display_guard_active) {
            return;
        }
        s_display_guard_active = true;
        s_p4_ota_display_hold_active = true;
        s_p4_ota_display_hud_next_us = 0;

        ui_background_set_lvgl_task_suspended(false);
        ui_background_set_lvgl_flush_suspended(false);
        ui_background_set_runtime_overlay_suspended(false);

        /* Give any just-invalidated widgets a chance to settle before the
         * first HTTP/flash burst.  Unlike the old no-touch guard, do not freeze
         * lv_timer_handler() and do not acknowledge flushes without drawing. */
        vTaskDelay(pdMS_TO_TICKS(20));
        ota_p4_display_hud_refresh(true);

        ESP_LOGW(TAG,
                 "P4 OTA UI-active guard enabled: screen ON, LVGL task+flush ACTIVE, direct overlay/HUD ACTIVE; on-screen progress should move");
        return;
    }

    if (!s_display_guard_active) {
        return;
    }

    s_p4_ota_display_hold_active = false;
    s_p4_ota_display_hud_next_us = 0;
    ui_background_set_lvgl_task_suspended(false);
    ui_background_set_lvgl_flush_suspended(false);
    ui_background_set_runtime_overlay_suspended(false);
    ESP_LOGW(TAG, "P4 OTA UI-active guard disabled: LVGL/direct overlay remain enabled, screen stayed ON");

    s_display_guard_active = false;
}

static size_t ota_effective_chunk_bytes(void)
{
    size_t chunk_bytes = CONFIG_MAD_OTA_DOWNLOAD_CHUNK_BYTES;

    if (chunk_bytes < MAD_OTA_P4_SELF_CHUNK_MIN_BYTES) {
        chunk_bytes = MAD_OTA_P4_SELF_CHUNK_MIN_BYTES;
    } else if (chunk_bytes > MAD_OTA_P4_SELF_CHUNK_MAX_BYTES) {
        chunk_bytes = MAD_OTA_P4_SELF_CHUNK_MAX_BYTES;
    }

    return chunk_bytes;
}

static int ota_effective_write_throttle_ms(void)
{
    int throttle_ms = CONFIG_MAD_OTA_WRITE_THROTTLE_MS;

    if (throttle_ms < MAD_OTA_P4_SELF_THROTTLE_MIN_MS) {
        throttle_ms = MAD_OTA_P4_SELF_THROTTLE_MIN_MS;
    } else if (throttle_ms > MAD_OTA_P4_SELF_THROTTLE_MAX_MS) {
        throttle_ms = MAD_OTA_P4_SELF_THROTTLE_MAX_MS;
    }

    return throttle_ms;
}

static TickType_t ota_effective_write_throttle_ticks(void)
{
    int throttle_ms = ota_effective_write_throttle_ms();
    if (throttle_ms <= 0) {
        return 0;
    }
    return pdMS_TO_TICKS(throttle_ms);
}


static const char *mad_ota_target_id(void)
{
    /* Historical lab builds used "esp32p4_app" while madUpdateServer now
     * publishes the P4 application component as target="esp32p4".  Keep old
     * sdkconfig builds compatible without forcing a full config reset. */
    if (strcmp(CONFIG_MAD_OTA_TARGET_ID, "esp32p4_app") == 0) {
        return "esp32p4";
    }
    return CONFIG_MAD_OTA_TARGET_ID;
}

static bool mad_ota_target_matches(const char *target)
{
    if (!target || !target[0]) {
        return false;
    }
    if (strcmp(target, mad_ota_target_id()) == 0) {
        return true;
    }
    if (strcmp(target, CONFIG_MAD_OTA_TARGET_ID) == 0) {
        return true;
    }
    /* Accept both names for the P4 application component. */
    if ((strcmp(target, "esp32p4") == 0 && strcmp(CONFIG_MAD_OTA_TARGET_ID, "esp32p4_app") == 0) ||
        (strcmp(target, "esp32p4_app") == 0 && strcmp(mad_ota_target_id(), "esp32p4") == 0)) {
        return true;
    }
    return false;
}

static void make_device_uid(char out[32])
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        snprintf(out, 32, "P4-UNKNOWN");
        return;
    }
    snprintf(out,
             32,
             "P4-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool ota_on_local_ssid(void)
{
    const char *ssid = wifi_manager_get_connected_ssid();
    return ssid && strcmp(ssid, CONFIG_MAD_OTA_LOCAL_SSID) == 0;
}

static const char *select_api_base_url(void)
{
#if CONFIG_MAD_OTA_USE_LOCAL_SERVER
    return ota_on_local_ssid() ? CONFIG_MAD_OTA_LOCAL_BASE_URL : CONFIG_MAD_OTA_PUBLIC_BASE_URL;
#else
    return CONFIG_MAD_OTA_PUBLIC_BASE_URL;
#endif
}

static const char *select_firmware_base_url(void)
{
#if CONFIG_MAD_OTA_USE_LOCAL_SERVER
    return ota_on_local_ssid() ? CONFIG_MAD_OTA_LOCAL_FIRMWARE_BASE_URL : CONFIG_MAD_OTA_PUBLIC_FIRMWARE_BASE_URL;
#else
    return CONFIG_MAD_OTA_PUBLIC_FIRMWARE_BASE_URL;
#endif
}

static void append_url_part(char *out, size_t out_size, const char *part, size_t part_len)
{
    if (!out || out_size == 0 || !part) {
        return;
    }

    size_t used = strlen(out);
    if (used >= out_size - 1) {
        return;
    }

    size_t avail = out_size - used - 1;
    if (part_len > avail) {
        part_len = avail;
    }

    if (part_len > 0) {
        memcpy(out + used, part, part_len);
        out[used + part_len] = '\0';
    }
}

static void join_url(char *out, size_t out_size, const char *base, const char *path_or_url)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (!path_or_url) {
        return;
    }

    if (strncmp(path_or_url, "http://", 7) == 0 || strncmp(path_or_url, "https://", 8) == 0) {
        append_url_part(out, out_size, path_or_url, strlen(path_or_url));
        return;
    }

    if (!base) {
        base = "";
    }

    const size_t base_len = strlen(base);
    const bool base_ends_slash = base_len > 0 && base[base_len - 1] == '/';
    const bool path_starts_slash = path_or_url[0] == '/';

    append_url_part(out, out_size, base, base_len);

    if (base_ends_slash && path_starts_slash) {
        append_url_part(out, out_size, path_or_url + 1, strlen(path_or_url + 1));
    } else if (!base_ends_slash && !path_starts_slash) {
        append_url_part(out, out_size, "/", 1);
        append_url_part(out, out_size, path_or_url, strlen(path_or_url));
    } else {
        append_url_part(out, out_size, path_or_url, strlen(path_or_url));
    }
}

static bool valid_sha256_hex(const char *hex)
{
    if (!hex || strlen(hex) != MAD_OTA_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < MAD_OTA_SHA256_HEX_LEN; ++i) {
        if (!isxdigit((unsigned char)hex[i])) {
            return false;
        }
    }
    return true;
}

static void sha256_to_hex(const uint8_t digest[32], char out[65])
{
    static const char lut[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = lut[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = lut[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static void log_payload_chunks(const char *title, const char *payload)
{
#if CONFIG_MAD_OTA_LOG_HTTP_PAYLOADS
    if (!payload) {
        ESP_LOGI(TAG, "%s: <null>", title ? title : "payload");
        return;
    }

    const size_t len = strlen(payload);
    ESP_LOGW(TAG, "%s begin len=%u", title ? title : "payload", (unsigned)len);
    for (size_t off = 0; off < len; off += MAD_OTA_LOG_CHUNK_BYTES) {
        size_t n = len - off;
        if (n > MAD_OTA_LOG_CHUNK_BYTES) {
            n = MAD_OTA_LOG_CHUNK_BYTES;
        }
        char line[MAD_OTA_LOG_CHUNK_BYTES + 1];
        memcpy(line, payload + off, n);
        line[n] = '\0';
        ESP_LOGW(TAG, "%s[%03u]: %s", title ? title : "payload", (unsigned)(off / MAD_OTA_LOG_CHUNK_BYTES), line);
    }
    ESP_LOGW(TAG, "%s end", title ? title : "payload");
#else
    (void)title;
    (void)payload;
#endif
}

static void get_sta_ip_string(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }

    snprintf(out, out_size, IPSTR, IP2STR(&ip_info.ip));
}

static bool str_ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) {
        return false;
    }
    const size_t sl = strlen(s);
    const size_t tl = strlen(suffix);
    if (tl > sl) {
        return false;
    }
    return strcasecmp(s + sl - tl, suffix) == 0;
}

static bool p4_firmware_url_extension_looks_valid(const char *url)
{
    if (str_is_empty(url)) {
        return false;
    }

    const char *q = strchr(url, '?');
    char tmp[MAD_OTA_URL_MAX_BYTES];
    if (q) {
        size_t n = (size_t)(q - url);
        if (n >= sizeof(tmp)) {
            n = sizeof(tmp) - 1;
        }
        memcpy(tmp, url, n);
        tmp[n] = '\0';
        url = tmp;
    }

    return str_ends_with_ci(url, ".bin") ||
           str_ends_with_ci(url, ".app") ||
           str_ends_with_ci(url, ".ota");
}

static bool p4_manifest_component_looks_installable(const mad_ota_manifest_component_t *comp)
{
    if (!comp) {
        return false;
    }

    bool ok = true;
    if (!p4_firmware_url_extension_looks_valid(comp->firmware_url)) {
        ESP_LOGW(TAG, "P4 manifest warning: firmware URL does not look like an app image (.bin/.app/.ota): %s",
                 comp->firmware_url);
        ok = false;
    }
    if (comp->size_bytes > 0 && comp->size_bytes < CONFIG_MAD_OTA_P4_MIN_IMAGE_BYTES) {
        ESP_LOGW(TAG, "P4 manifest warning: firmware size is suspiciously small for this app: %lld < %d",
                 (long long)comp->size_bytes,
                 (int)CONFIG_MAD_OTA_P4_MIN_IMAGE_BYTES);
        ok = false;
    }
    return ok;
}

static void http_config_common(esp_http_client_config_t *cfg, const char *url)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = CONFIG_MAD_OTA_HTTP_TIMEOUT_MS;
    cfg->buffer_size = 4096;
    cfg->buffer_size_tx = 1024;
    cfg->user_agent = "madP4-ota/0.1";
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
#endif
}

static esp_err_t http_request_to_buffer(const char *url,
                                        const char *method,
                                        const char *post_body,
                                        char **out_body,
                                        int *out_status)
{
    if (!url || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    if (out_status) {
        *out_status = 0;
    }

    esp_http_client_config_t cfg;
    http_config_common(&cfg, url);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    const bool is_post = method && strcasecmp(method, "POST") == 0;
    esp_http_client_set_method(client, is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (is_post) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept", "application/json");
    }

    const int post_len = post_body ? (int)strlen(post_body) : 0;
    ESP_LOGW(TAG, "HTTP DEBUG request: %s %s body_len=%d", is_post ? "POST" : "GET", url, post_len);
    if (is_post) {
        log_payload_chunks("HTTP DEBUG request JSON", post_body ? post_body : "");
    }

    esp_err_t ret = esp_http_client_open(client, post_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open failed for %s: %s", url, esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    if (post_len > 0) {
        const int written = esp_http_client_write(client, post_body, post_len);
        if (written < 0 || written != post_len) {
            ESP_LOGW(TAG, "HTTP POST write failed: written=%d expected=%d", written, post_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    const int64_t content_len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (out_status) {
        *out_status = status;
    }

    ESP_LOGI(TAG, "HTTP %s %s -> status=%d content_len=%lld", is_post ? "POST" : "GET", url, status, (long long)content_len);

    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_len > MAD_OTA_RESPONSE_MAX_BYTES) {
        ESP_LOGW(TAG, "HTTP response too large: %lld", (long long)content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    char *buf = calloc(1, MAD_OTA_RESPONSE_MAX_BYTES + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < MAD_OTA_RESPONSE_MAX_BYTES) {
        const int r = esp_http_client_read(client, buf + total, MAD_OTA_RESPONSE_MAX_BYTES - total);
        if (r < 0) {
            ESP_LOGW(TAG, "HTTP read failed");
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }

    buf[total] = '\0';
    ESP_LOGW(TAG, "HTTP DEBUG response: %s %s status=%d bytes=%d",
             is_post ? "POST" : "GET",
             url,
             status,
             total);
    log_payload_chunks("HTTP DEBUG response BODY", buf);
    *out_body = buf;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static char *build_check_post_body(void)
{
    char device_uid[32];
    make_device_uid(device_uid);

    const char *ssid = wifi_manager_get_connected_ssid();
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    cJSON *versions = cJSON_CreateObject();
    cJSON *network = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    if (!root || !versions || !network || !state) {
        cJSON_Delete(root);
        cJSON_Delete(versions);
        cJSON_Delete(network);
        cJSON_Delete(state);
        return NULL;
    }

    cJSON_AddStringToObject(root, "device_uid", device_uid);
    cJSON_AddStringToObject(root, "product_id", CONFIG_MAD_OTA_PRODUCT_ID);
    cJSON_AddStringToObject(root, "hw_rev", CONFIG_MAD_OTA_HW_REV);
    cJSON_AddStringToObject(root, "channel", CONFIG_MAD_OTA_CHANNEL);
    cJSON_AddStringToObject(root, "idf_app_version", app ? app->version : "unknown");

    /* madUpdateServer resolves components from versions{} and can return several
     * targets in one manifest.  Do not add top-level target_id/current_version
     * here: that filters the response to one target and makes the STM32 button
     * miss STM32F030K6T6 when the normal P4 check happened first.  Also do not
     * send historical aliases such as esp32p4_app/stm32_main.  T5L1 is
     * intentionally omitted on this board: it can exist in a bundle, but this
     * P4+C6+STM32 build does not manage DWIN. */
    cJSON_AddStringToObject(versions, "esp32p4", CONFIG_MAD_OTA_APP_VERSION);
    cJSON_AddStringToObject(versions, CONFIG_MAD_OTA_STM32_TARGET_ID, CONFIG_MAD_OTA_STM32_CURRENT_VERSION);
    cJSON_AddItemToObject(root, "versions", versions);
    ESP_LOGI(TAG, "OTA check versions: esp32p4=%s %s=%s T5L1=omitted; top-level target_id omitted so server can return all relevant components",
             CONFIG_MAD_OTA_APP_VERSION,
             CONFIG_MAD_OTA_STM32_TARGET_ID,
             CONFIG_MAD_OTA_STM32_CURRENT_VERSION);
    cJSON_AddStringToObject(network, "ssid", ssid ? ssid : "");
    char ip[24];
    get_sta_ip_string(ip, sizeof(ip));
    if (ip[0]) {
        cJSON_AddStringToObject(network, "ip", ip);
    }
    cJSON_AddItemToObject(root, "network", network);
    cJSON_AddBoolToObject(state, "safe_machine_state", true);
    cJSON_AddBoolToObject(state, "updates_enabled", true);
    cJSON_AddBoolToObject(state, "service_device", false);
    cJSON_AddBoolToObject(state, "canary_device", false);
    cJSON_AddBoolToObject(state, "locked_channel", false);
    cJSON_AddBoolToObject(state, "allow_rollback", false);
    cJSON_AddItemToObject(root, "state", state);

    char *body = cJSON_PrintUnformatted(root);
    log_payload_chunks("MAD OTA update/check POST JSON", body ? body : "<alloc failed>");
    cJSON_Delete(root);
    return body;
}

static const char *json_get_string(cJSON *obj, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int64_t json_get_int64(cJSON *obj, const char *name, int64_t default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    return default_value;
}

static bool json_get_bool(cJSON *obj, const char *name, bool default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static cJSON *ota_get_manifest_root(cJSON *root)
{
    cJSON *manifest = cJSON_GetObjectItemCaseSensitive(root, "manifest");
    return cJSON_IsObject(manifest) ? manifest : root;
}

static bool ota_response_says_no_update(cJSON *root)
{
    const char *decision = json_get_string(root, "decision");
    const char *status = json_get_string(root, "status");
    const char *result = json_get_string(root, "result");

    if (decision && strcasecmp(decision, "NO_UPDATE") == 0) {
        return true;
    }
    if (result && strcasecmp(result, "NO_UPDATE") == 0) {
        return true;
    }
    if (status && (strcasecmp(status, "no_update") == 0 || strcasecmp(status, "NO_UPDATE") == 0)) {
        return true;
    }

    cJSON *update_available = cJSON_GetObjectItemCaseSensitive(root, "update_available");
    if (cJSON_IsBool(update_available) && !cJSON_IsTrue(update_available)) {
        return true;
    }

    cJSON *components = cJSON_GetObjectItemCaseSensitive(ota_get_manifest_root(root), "components");
    if (cJSON_IsArray(components) && cJSON_GetArraySize(components) == 0) {
        return true;
    }

    return false;
}

static esp_err_t parse_manifest_component_for_target(const char *firmware_base_url,
                                                     const char *json,
                                                     const char *requested_target,
                                                     mad_ota_manifest_component_t *out)
{
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->base_url, firmware_base_url ? firmware_base_url : "", sizeof(out->base_url));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Update/check response is not valid JSON");
        return ESP_FAIL;
    }

    if (ota_response_says_no_update(root)) {
        const char *reason = json_get_string(root, "reason");
        ESP_LOGI(TAG, "Server says NO UPDATE%s%s", reason ? ": " : "", reason ? reason : "");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *manifest = ota_get_manifest_root(root);

    const char *product_id = NULL;
    const char *hw_rev = NULL;
    cJSON *product = cJSON_GetObjectItemCaseSensitive(manifest, "product");
    if (cJSON_IsObject(product)) {
        product_id = json_get_string(product, "product_id");
        hw_rev = json_get_string(product, "hw_rev");
    }
    if (!product_id) {
        product_id = json_get_string(manifest, "product_id");
    }
    if (!hw_rev) {
        hw_rev = json_get_string(manifest, "hw_rev");
    }

    if (product_id && strcmp(product_id, CONFIG_MAD_OTA_PRODUCT_ID) != 0) {
        ESP_LOGW(TAG, "Manifest product_id mismatch: got=%s expected=%s", product_id, CONFIG_MAD_OTA_PRODUCT_ID);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Do not fail on hw_rev mismatch. The server currently uses both full board
     * ids (jc4880p443c-p4-c6-v1) and short lab ids (v1.0). The server-side
     * resolver is the source of truth; the client only logs the difference. */
    if (hw_rev && strcmp(hw_rev, CONFIG_MAD_OTA_HW_REV) != 0) {
        ESP_LOGW(TAG, "Manifest hw_rev differs: got=%s configured=%s; accepting resolver decision", hw_rev, CONFIG_MAD_OTA_HW_REV);
    }

    cJSON *components = cJSON_GetObjectItemCaseSensitive(manifest, "components");
    if (!cJSON_IsArray(components)) {
        /* Some admin/debug endpoints may return a single component object. */
        cJSON *single = cJSON_GetObjectItemCaseSensitive(manifest, "component");
        if (cJSON_IsObject(single)) {
            components = cJSON_CreateArray();
            if (!components) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemReferenceToArray(components, single);
        } else {
            ESP_LOGI(TAG, "Update/check response has no components array; treating as NO UPDATE");
            cJSON_Delete(root);
            return ESP_ERR_NOT_FOUND;
        }
    }

    int manifest_release_id = (int)json_get_int64(manifest, "release_id", 0);
    int manifest_deployment_id = (int)json_get_int64(manifest, "deployment_id", 0);
    cJSON *bundle = cJSON_GetObjectItemCaseSensitive(manifest, "bundle");
    if (cJSON_IsObject(bundle)) {
        if (!manifest_release_id) {
            manifest_release_id = (int)json_get_int64(bundle, "release_id", 0);
        }
        out->mandatory = json_get_bool(bundle, "mandatory", out->mandatory);
        out->allow_downgrade = json_get_bool(bundle, "allow_downgrade", out->allow_downgrade);
        out->rollback = json_get_bool(bundle, "rollback", out->rollback);
        out->auto_update = json_get_bool(bundle, "auto_update", out->auto_update);
        out->auto_rollback = json_get_bool(bundle, "auto_rollback", out->auto_rollback);
    }
    cJSON *rollout = cJSON_GetObjectItemCaseSensitive(manifest, "rollout");
    if (cJSON_IsObject(rollout)) {
        if (!manifest_deployment_id) {
            manifest_deployment_id = (int)json_get_int64(rollout, "deployment_id", 0);
        }
    }

    cJSON *comp = NULL;
    cJSON_ArrayForEach(comp, components) {
        const char *target = json_get_string(comp, "target");
        if (!target) {
            target = json_get_string(comp, "target_id");
        }
        if (requested_target && requested_target[0]) {
            if (!target || strcmp(target, requested_target) != 0) {
                continue;
            }
        } else if (!mad_ota_target_matches(target)) {
            continue;
        }

        strlcpy(out->target, target ? target : (requested_target ? requested_target : mad_ota_target_id()), sizeof(out->target));

        const char *version = json_get_string(comp, "version");
        const char *sha256 = json_get_string(comp, "sha256");
        const char *file_name = json_get_string(comp, "file_name");
        const char *url = json_get_string(comp, "url");
        const char *relative_url = json_get_string(comp, "relative_url");
        if (!relative_url) {
            relative_url = json_get_string(comp, "download_path");
        }

        if (str_is_empty(version)) {
            ESP_LOGW(TAG, "OTA component has empty version");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        strlcpy(out->version, version, sizeof(out->version));
        strlcpy(out->sha256, sha256 ? sha256 : "", sizeof(out->sha256));
        strlcpy(out->file_name, file_name ? file_name : "", sizeof(out->file_name));
        out->size_bytes = json_get_int64(comp, "size_bytes", 0);
        out->reboot_required = json_get_bool(comp, "reboot_required", true);
        out->rollback_supported = json_get_bool(comp, "rollback_supported", true);
        out->mandatory = json_get_bool(comp, "mandatory", out->mandatory);
        out->allow_downgrade = json_get_bool(comp, "allow_downgrade", out->allow_downgrade);
        out->rollback = json_get_bool(comp, "rollback", out->rollback);
        out->auto_update = json_get_bool(comp, "auto_update", out->auto_update);
        out->auto_rollback = json_get_bool(comp, "auto_rollback", out->auto_rollback);
        out->release_id = (int)json_get_int64(comp, "release_id", manifest_release_id);
        out->deployment_id = (int)json_get_int64(comp, "deployment_id", manifest_deployment_id);
        out->firmware_file_id = (int)json_get_int64(comp, "firmware_file_id", 0);

        const char *path_or_url = relative_url ? relative_url : url;
        if (str_is_empty(path_or_url)) {
            ESP_LOGW(TAG, "OTA component has no url/relative_url/download_path");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        /* Prefer relative_url/download_path from the resolver. Absolute url is
         * accepted too, but relative paths let us switch ARD/public firmware
         * hosts on the client side. */
        join_url(out->firmware_url, sizeof(out->firmware_url), firmware_base_url, path_or_url);

        ESP_LOGW(TAG,
                 "OTA resolver selected component: target=%s version=%s release=%d rollout/deployment=%d file=%d size=%lld url=%s sha=%s",
                 out->target,
                 out->version,
                 out->release_id,
                 out->deployment_id,
                 out->firmware_file_id,
                 (long long)out->size_bytes,
                 out->firmware_url,
                 out->sha256);
        (void)p4_manifest_component_looks_installable(out);

        cJSON_Delete(root);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "No component for target %s (configured %s) in resolver response; treating as NO UPDATE", requested_target ? requested_target : mad_ota_target_id(), CONFIG_MAD_OTA_TARGET_ID);
    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t fetch_manifest(const char *base_url, char **out_manifest)
{
    if (!base_url || !out_manifest) {
        return ESP_ERR_INVALID_ARG;
    }

    char check_url[MAD_OTA_URL_MAX_BYTES];
    join_url(check_url, sizeof(check_url), base_url, CONFIG_MAD_OTA_CHECK_PATH);

    char *post_body = build_check_post_body();
    if (!post_body) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Checking OTA manifest via API: %s", check_url);
    esp_err_t ret = http_request_to_buffer(check_url, "POST", post_body, out_manifest, NULL);
    free(post_body);
    if (ret == ESP_OK) {
        return ESP_OK;
    }

#if CONFIG_MAD_OTA_TRY_STATIC_MANIFEST_FALLBACK
    char static_url[MAD_OTA_URL_MAX_BYTES];
    join_url(static_url, sizeof(static_url), base_url, CONFIG_MAD_OTA_STATIC_MANIFEST_PATH);
    ESP_LOGW(TAG, "API manifest failed (%s), trying static manifest: %s", esp_err_to_name(ret), static_url);
    ret = http_request_to_buffer(static_url, "GET", NULL, out_manifest, NULL);
#endif

    return ret;
}

static bool version_needs_update(const char *available_version)
{
    if (str_is_empty(available_version)) {
        return false;
    }
    if (strcmp(available_version, CONFIG_MAD_OTA_APP_VERSION) == 0) {
        return false;
    }
    return true;
}

static char *build_update_event_body(const mad_ota_manifest_component_t *comp,
                                     const char *status,
                                     const char *error_code,
                                     const char *error_text,
                                     int progress_percent)
{
    char device_uid[32];
    make_device_uid(device_uid);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "device_uid", device_uid);
    cJSON_AddStringToObject(root, "product_id", CONFIG_MAD_OTA_PRODUCT_ID);
    cJSON_AddStringToObject(root, "target", (comp && comp->target[0]) ? comp->target : mad_ota_target_id());
    cJSON_AddStringToObject(root, "from_version", CONFIG_MAD_OTA_APP_VERSION);
    if (comp && comp->version[0]) {
        cJSON_AddStringToObject(root, "to_version", comp->version);
    }
    cJSON_AddStringToObject(root, "status", status ? status : "unknown");
    if (comp) {
        if (comp->release_id) {
            cJSON_AddNumberToObject(root, "release_id", comp->release_id);
        }
        if (comp->deployment_id) {
            cJSON_AddNumberToObject(root, "deployment_id", comp->deployment_id);
        }
        if (comp->firmware_file_id) {
            cJSON_AddNumberToObject(root, "firmware_file_id", comp->firmware_file_id);
        }
    }
    if (progress_percent >= 0) {
        cJSON_AddNumberToObject(root, "progress", progress_percent);
    }
    if (error_code) {
        cJSON_AddStringToObject(root, "error_code", error_code);
    } else {
        cJSON_AddNullToObject(root, "error_code");
    }
    if (error_text) {
        cJSON_AddStringToObject(root, "error_text", error_text);
    } else {
        cJSON_AddNullToObject(root, "error_text");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static void post_update_event(const char *path,
                              const mad_ota_manifest_component_t *comp,
                              const char *status,
                              const char *error_code,
                              const char *error_text,
                              int progress_percent)
{
    const char *api_base_url = select_api_base_url();
    char url[MAD_OTA_URL_MAX_BYTES];
    join_url(url, sizeof(url), api_base_url, path);

    char *body = build_update_event_body(comp, status, error_code, error_text, progress_percent);
    if (!body) {
        ESP_LOGW(TAG, "Could not allocate update event JSON for %s", path);
        return;
    }

    char *response = NULL;
    int http_status = 0;
    esp_err_t ret = http_request_to_buffer(url, "POST", body, &response, &http_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Update event POST failed path=%s status=%d ret=%s", path, http_status, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Update event POST OK path=%s status=%d", path, http_status);
    }
    free(response);
    free(body);
}

static void report_start(const mad_ota_manifest_component_t *comp)
{
    post_update_event(CONFIG_MAD_OTA_START_PATH, comp, "started", NULL, NULL, 0);
}

static void report_progress(const mad_ota_manifest_component_t *comp, int progress)
{
    post_update_event(CONFIG_MAD_OTA_PROGRESS_PATH, comp, "progress", NULL, NULL, progress);
}

static void report_result(const mad_ota_manifest_component_t *comp,
                          const char *status,
                          const char *error_code,
                          const char *error_text)
{
    post_update_event(CONFIG_MAD_OTA_REPORT_PATH, comp, status, error_code, error_text, g_mad_ota_state.progress_percent);
}

static void p4_ota_log_phase(int phase, int total_phases, const char *name, const char *detail)
{
    ESP_LOGW(TAG,
             "========== P4 OTA PHASE %d/%d: %s%s%s ==========",
             phase,
             total_phases,
             name ? name : "?",
             detail && detail[0] ? " - " : "",
             detail && detail[0] ? detail : "");
}

static esp_err_t download_and_install_p4(const mad_ota_manifest_component_t *comp)
{
    if (!comp || str_is_empty(comp->firmware_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!valid_sha256_hex(comp->sha256)) {
        ota_set_error("Manifest has invalid/missing sha256 for %s", CONFIG_MAD_OTA_TARGET_ID);
        report_result(comp, "failed", "INVALID_SHA256", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG,
             "Manual/auto P4 install requested: version=%s file=%d size=%lld url=%s",
             comp->version,
             comp->firmware_file_id,
             (long long)comp->size_bytes,
             comp->firmware_url);
    p4_ota_log_phase(1, 6, "PREPARE", "validate manifest and select OTA partition");
    if (!p4_manifest_component_looks_installable(comp)) {
        ota_set_error("Suspicious P4 firmware file: expected .bin/.app/.ota and >= %d bytes, got size=%lld url=%s",
                      (int)CONFIG_MAD_OTA_P4_MIN_IMAGE_BYTES,
                      (long long)comp->size_bytes,
                      comp->firmware_url);
        report_result(comp, "failed", "SUSPICIOUS_P4_IMAGE", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ota_set_error("No OTA update partition found. Check partition table: need ota_0/ota_1 + otadata");
        report_result(comp, "failed", "NO_OTA_PARTITION", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Running partition:    label=%s subtype=%d address=0x%lx size=%lu",
             running ? running->label : "?",
             running ? running->subtype : -1,
             running ? (unsigned long)running->address : 0,
             running ? (unsigned long)running->size : 0);
    ESP_LOGI(TAG, "Boot partition:       label=%s subtype=%d address=0x%lx size=%lu",
             configured ? configured->label : "?",
             configured ? configured->subtype : -1,
             configured ? (unsigned long)configured->address : 0,
             configured ? (unsigned long)configured->size : 0);
    ESP_LOGI(TAG, "Next OTA partition:   label=%s subtype=%d address=0x%lx size=%lu",
             update->label,
             update->subtype,
             (unsigned long)update->address,
             (unsigned long)update->size);

    if (comp->size_bytes > 0 && (uint64_t)comp->size_bytes > update->size) {
        ota_set_error("Firmware too large: manifest=%lld partition=%lu",
                      (long long)comp->size_bytes,
                      (unsigned long)update->size);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_task_wdt_guard_t wdt_guard;
    ota_task_wdt_suspend_for_ota(&wdt_guard, "ESP32-P4 self-OTA");
    ota_set_status(MAD_OTA_STATUS_DOWNLOADING);
    ota_set_progress(0);
    ota_display_quiet_mode(true);
    report_start(comp);

    esp_http_client_config_t cfg;
    http_config_common(&cfg, comp->firmware_url);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_ERR_NO_MEM;
    }

    p4_ota_log_phase(2, 6, "HTTP_OPEN", "open firmware stream");

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ota_set_error("Firmware HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ret;
    }

    const int64_t content_len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Firmware GET %s -> status=%d content_len=%lld", comp->firmware_url, status, (long long)content_len);
    if (status < 200 || status >= 300) {
        ota_set_error("Firmware HTTP status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_FAIL;
    }

    if (content_len > 0 && (uint64_t)content_len > update->size) {
        ota_set_error("Firmware content_len too large: %lld > %lu", (long long)content_len, (unsigned long)update->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_set_status(MAD_OTA_STATUS_DOWNLOADING);
    ota_set_progress(0);
    ota_p4_display_hud_refresh(true);
    p4_ota_log_phase(3, 6, "DOWNLOAD_WRITE", "download firmware and write OTA partition; screen should show DOWNLOADING");

    esp_ota_handle_t ota = 0;
    ESP_LOGW(TAG,
             "OTA: esp_ota_begin start mode=OTA_WITH_SEQUENTIAL_WRITES partition=%s partition_size=%lu content_len=%lld manifest_size=%lld",
             update->label,
             (unsigned long)update->size,
             (long long)content_len,
             (long long)comp->size_bytes);
    const int64_t ota_begin_t0_us = esp_timer_get_time();
    ret = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    const int64_t ota_begin_ms = (esp_timer_get_time() - ota_begin_t0_us) / 1000;
    ESP_LOGW(TAG, "OTA: esp_ota_begin done ret=%s elapsed=%lld ms", esp_err_to_name(ret), (long long)ota_begin_ms);
    if (ret != ESP_OK) {
        ota_set_error("esp_ota_begin failed: %s", esp_err_to_name(ret));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ret;
    }

    size_t chunk_bytes = ota_effective_chunk_bytes();

    /* Keep the OTA transfer buffer in internal RAM when possible. During OTA the
     * panel scans framebuffers from external RAM while flash is being erased and
     * written. Avoiding extra PSRAM traffic here reduces LCD underflow/white
     * flashes on ESP32-P4 + MIPI-DSI panels. */
    uint8_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(chunk_bytes);
    }
    if (!buf) {
        esp_ota_abort(ota);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_ERR_NO_MEM;
    }

    const int throttle_ms = ota_effective_write_throttle_ms();
    const TickType_t throttle_ticks = ota_effective_write_throttle_ticks();

    ESP_LOGW(TAG,
             "OTA UI-active transfer mode fix52: chunk=%u throttle=%dms hud_refresh=%dms configured_chunk=%d configured_throttle=%dms server_report_step=%d%% uart_step=%d%% uart_period=%dms task_prio=%d core=%d",
             (unsigned)chunk_bytes,
             throttle_ms,
             (int)MAD_OTA_P4_DISPLAY_HUD_REFRESH_MS,
             (int)CONFIG_MAD_OTA_DOWNLOAD_CHUNK_BYTES,
             (int)CONFIG_MAD_OTA_WRITE_THROTTLE_MS,
             (int)CONFIG_MAD_OTA_REPORT_STEP_PERCENT,
             (int)MAD_OTA_P4_CONSOLE_PROGRESS_STEP_PERCENT,
             (int)MAD_OTA_P4_CONSOLE_PROGRESS_PERIOD_MS,
             (int)CONFIG_MAD_OTA_TASK_PRIORITY,
             (int)CONFIG_MAD_OTA_TASK_CORE);
    ESP_LOGW(TAG,
             "P4 OTA screen progress note fix52: top-right and center HUD should show DL xx%% during transfer, then VERIFY/REBOOT; UART lines use clear P4 OTA PHASE/progress markers.");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int64_t total = 0;
    int last_server_reported_progress = -1;
    int last_console_progress = -1;
    int64_t transfer_start_us = esp_timer_get_time();
    int64_t last_console_log_us = 0;
    while (true) {
        const int r = esp_http_client_read(client, (char *)buf, (int)chunk_bytes);
        if (r < 0) {
            ota_set_error("Firmware HTTP read failed");
            ret = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }

        mbedtls_sha256_update(&sha, buf, (size_t)r);

        /* Give the display/LVGL and TCP/IP tasks one scheduling point before
         * every flash write as well as after it. This makes P4 self-OTA slower,
         * but greatly reduces long uninterrupted flash/PSRAM/DSI pressure. */
        vTaskDelay(1);

        const int64_t write_t0_us = esp_timer_get_time();
        ret = esp_ota_write(ota, buf, (size_t)r);
        const int64_t write_elapsed_ms = (esp_timer_get_time() - write_t0_us) / 1000;
        if (write_elapsed_ms > 80) {
            ESP_LOGW(TAG, "OTA flash write stall: offset=%lld size=%d elapsed=%lld ms",
                     (long long)total,
                     r,
                     (long long)write_elapsed_ms);
        }
        if (ret != ESP_OK) {
            ota_set_error("esp_ota_write failed at %lld: %s", (long long)total, esp_err_to_name(ret));
            break;
        }

        total += r;
        const int64_t now_us = esp_timer_get_time();
        const int64_t elapsed_ms = (now_us - transfer_start_us) / 1000;
        int rate_kib_s = 0;
        if (elapsed_ms > 0) {
            rate_kib_s = (int)((total * 1000LL) / elapsed_ms / 1024LL);
        }

        if (content_len > 0) {
            int progress = (int)((total * 100) / content_len);
            ota_set_progress(progress);
            ota_p4_display_hud_refresh(false);

            const bool console_percent_due =
                progress >= last_console_progress + MAD_OTA_P4_CONSOLE_PROGRESS_STEP_PERCENT;
            const bool console_time_due =
                last_console_log_us == 0 ||
                now_us - last_console_log_us >= ((int64_t)MAD_OTA_P4_CONSOLE_PROGRESS_PERIOD_MS * 1000LL);
            if (console_percent_due || console_time_due || progress == 100) {
                last_console_progress = progress;
                last_console_log_us = now_us;
                ESP_LOGW(TAG,
                         "P4 OTA DOWNLOAD+WRITE: %3d%%  %lld/%lld bytes  rate=%d KiB/s  chunk=%d  throttle=%dms  elapsed=%lldms",
                         progress,
                         (long long)total,
                         (long long)content_len,
                         rate_kib_s,
                         r,
                         throttle_ms,
                         (long long)elapsed_ms);
            }

            if (progress >= last_server_reported_progress + CONFIG_MAD_OTA_REPORT_STEP_PERCENT || progress == 100) {
                last_server_reported_progress = progress;
                ESP_LOGW(TAG, "P4 OTA server progress report: %d%% read=%lld/%lld",
                         progress,
                         (long long)total,
                         (long long)content_len);
                report_progress(comp, progress);
            }
        } else {
            const bool console_time_due =
                last_console_log_us == 0 ||
                now_us - last_console_log_us >= ((int64_t)MAD_OTA_P4_CONSOLE_PROGRESS_PERIOD_MS * 1000LL);
            if (console_time_due) {
                last_console_log_us = now_us;
                ESP_LOGW(TAG,
                         "P4 OTA DOWNLOAD+WRITE: read=%lld bytes  rate=%d KiB/s  chunk=%d  throttle=%dms  elapsed=%lldms content_len=unknown",
                         (long long)total,
                         rate_kib_s,
                         r,
                         throttle_ms,
                         (long long)elapsed_ms);
            }
        }

        if (throttle_ticks > 0) {
            vTaskDelay(throttle_ticks);
        } else {
            taskYIELD();
        }
    }

    const int64_t transfer_elapsed_ms = (esp_timer_get_time() - transfer_start_us) / 1000;
    ota_set_status(MAD_OTA_STATUS_VERIFYING);
    ota_set_progress(100);
    ota_p4_display_hud_refresh(true);
    p4_ota_log_phase(4, 6, "VERIFY", "download complete; verify size and SHA256");
    ESP_LOGW(TAG,
             "P4 OTA DOWNLOAD+WRITE finished: ret=%s read=%lld expected=%lld elapsed=%lldms; verifying SHA/OTA image",
             esp_err_to_name(ret),
             (long long)total,
             (long long)content_len,
             (long long)transfer_elapsed_ms);

    uint8_t digest[32];
    char digest_hex[65];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    sha256_to_hex(digest, digest_hex);

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        esp_ota_abort(ota);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ret;
    }

    if (content_len > 0 && total != content_len) {
        esp_ota_abort(ota);
        ota_set_error("Firmware size mismatch: read=%lld expected=%lld", (long long)total, (long long)content_len);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_FAIL;
    }

    if (strcasecmp(digest_hex, comp->sha256) != 0) {
        esp_ota_abort(ota);
        ota_set_error("SHA256 mismatch: got=%s expected=%s", digest_hex, comp->sha256);
        report_result(comp, "failed", "SHA256_MISMATCH", (const char *)g_mad_ota_state.last_error);
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "P4 OTA VERIFY: SHA256 OK: %s", digest_hex);

    p4_ota_log_phase(5, 6, "FINALIZE", "esp_ota_end and boot partition switch");
    ESP_LOGW(TAG, "OTA: esp_ota_end start; validating image trailer and metadata");
    const int64_t ota_end_t0_us = esp_timer_get_time();
    ret = esp_ota_end(ota);
    const int64_t ota_end_ms = (esp_timer_get_time() - ota_end_t0_us) / 1000;
    ESP_LOGW(TAG, "OTA: esp_ota_end done ret=%s elapsed=%lld ms", esp_err_to_name(ret), (long long)ota_end_ms);
    if (ret != ESP_OK) {
        ota_set_error("esp_ota_end failed: %s", esp_err_to_name(ret));
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ret;
    }

    ESP_LOGW(TAG, "OTA: esp_ota_set_boot_partition start: label=%s address=0x%lx",
             update->label,
             (unsigned long)update->address);
    ret = esp_ota_set_boot_partition(update);
    ESP_LOGW(TAG, "OTA: esp_ota_set_boot_partition done ret=%s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ota_set_error("esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA failed");
        ota_display_quiet_mode(false);
        return ret;
    }

    ota_set_status(MAD_OTA_STATUS_REBOOT_PENDING);
    ota_set_progress(100);
    ota_p4_display_hud_refresh(true);
    p4_ota_log_phase(6, 6, "REBOOT", "new image installed; reboot into selected OTA partition");
    ESP_LOGW(TAG, "OTA image installed and boot partition switched: version=%s partition=%s address=0x%lx",
             comp->version,
             update->label,
             (unsigned long)update->address);
    ESP_LOGW(TAG, "OTA: sending final progress/report before reboot");
    report_progress(comp, 100);
    report_result(comp, "success", NULL, NULL);
    ESP_LOGW(TAG, "OTA success path complete. Showing reboot screen, then blanking backlight; next boot should load partition %s at 0x%lx",
             update->label,
             (unsigned long)update->address);
    ota_task_wdt_resume_after_ota(&wdt_guard, "ESP32-P4 self-OTA success before reboot");
    ota_p4_show_reboot_message_then_blank_backlight();
    esp_restart();
    return ESP_OK;
}

static esp_err_t download_stm32_image_to_ram(const mad_ota_manifest_component_t *comp, uint8_t **out_image, size_t *out_size)
{
    if (!comp || str_is_empty(comp->firmware_url) || !out_image || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_image = NULL;
    *out_size = 0;

    if (!valid_sha256_hex(comp->sha256)) {
        ota_set_error("Manifest has invalid/missing sha256 for STM32 target");
        report_result(comp, "failed", "INVALID_SHA256", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_INVALID_ARG;
    }
    if (comp->size_bytes <= 0 || comp->size_bytes > CONFIG_MAD_OTA_STM32_MAX_IMAGE_BYTES) {
        ota_set_error("STM32 image size invalid: %lld", (long long)comp->size_bytes);
        report_result(comp, "failed", "INVALID_STM32_SIZE", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_set_status(MAD_OTA_STATUS_DOWNLOADING);
    ota_set_progress(0);
    report_start(comp);

    esp_http_client_config_t cfg;
    http_config_common(&cfg, comp->firmware_url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ota_set_error("STM32 firmware HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    const int64_t content_len = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "STM32 firmware GET %s -> status=%d content_len=%lld", comp->firmware_url, status, (long long)content_len);
    if (status < 200 || status >= 300) {
        ota_set_error("STM32 firmware HTTP status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t expected_len = comp->size_bytes;
    if (content_len > 0) {
        expected_len = content_len;
    }
    if (expected_len <= 0 || expected_len > CONFIG_MAD_OTA_STM32_MAX_IMAGE_BYTES) {
        ota_set_error("STM32 content_len invalid: %lld", (long long)expected_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *image = heap_caps_malloc((size_t)expected_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) {
        image = heap_caps_malloc((size_t)expected_len, MALLOC_CAP_8BIT);
    }
    if (!image) {
        image = malloc((size_t)expected_len);
    }
    if (!image) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    int64_t total = 0;
    int last_reported_progress = -1;
    while (total < expected_len) {
        int want = (int)ota_effective_chunk_bytes();
        if (expected_len - total < want) {
            want = (int)(expected_len - total);
        }
        const int r = esp_http_client_read(client, (char *)image + total, want);
        if (r < 0) {
            ota_set_error("STM32 firmware HTTP read failed");
            ret = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        mbedtls_sha256_update(&sha, image + total, (size_t)r);
        total += r;

        int progress = (int)((total * 100) / expected_len);
        ota_set_progress(progress);
        if (progress >= last_reported_progress + CONFIG_MAD_OTA_REPORT_STEP_PERCENT || progress == 100) {
            last_reported_progress = progress;
            report_progress(comp, progress);
        }
        taskYIELD();
    }

    uint8_t digest[32];
    char digest_hex[65];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    sha256_to_hex(digest, digest_hex);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        free(image);
        return ret;
    }
    if (total != expected_len) {
        free(image);
        ota_set_error("STM32 firmware size mismatch: read=%lld expected=%lld", (long long)total, (long long)expected_len);
        return ESP_FAIL;
    }
    if (strcasecmp(digest_hex, comp->sha256) != 0) {
        free(image);
        ota_set_error("STM32 SHA256 mismatch: got=%s expected=%s", digest_hex, comp->sha256);
        report_result(comp, "failed", "SHA256_MISMATCH", (const char *)g_mad_ota_state.last_error);
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "STM32 firmware SHA256 OK: %s", digest_hex);
    *out_image = image;
    *out_size = (size_t)total;
    return ESP_OK;
}

static esp_err_t download_and_install_stm32(const mad_ota_manifest_component_t *comp)
{
    /* Keep the LCD/LVGL runtime overlay in normal mode during STM32 OTA.
     * The SWD image is small and the successful fix25 path programs it in only
     * a few seconds, so suspending the direct overlay is not worth the visual
     * risk.  In fix26/fix27 this function also enabled ota_display_quiet_mode(),
     * which could expose the LVGL default blue background when the full-screen
     * photo layer was not refreshed at exactly the right moment.  The display
     * quiet mode is still used for ESP32-P4 self-OTA, where flash writes really
     * contend with the MIPI-DSI/DPI path. */
    ota_task_wdt_guard_t wdt_guard;
    ota_task_wdt_suspend_for_ota(&wdt_guard, "STM32 OTA/SWD");
    uint8_t *image = NULL;
    size_t image_size = 0;
    esp_err_t ret = download_stm32_image_to_ram(comp, &image, &image_size);
    if (ret != ESP_OK) {
        ota_task_wdt_resume_after_ota(&wdt_guard, "STM32 OTA/SWD failed");
        return ret;
    }

    ota_set_status(MAD_OTA_STATUS_INSTALLING);
    ota_set_progress(0);

    int max_attempts = CONFIG_MAD_OTA_STM32_FLASH_ATTEMPTS;
    if (max_attempts < 1) {
        max_attempts = 1;
    }
    if (max_attempts > 10) {
        max_attempts = 10;
    }

    ESP_LOGW(TAG,
             "Installing STM32 firmware via SWD: version=%s size=%u attempts=%d flash_base=0x%08lx (download already verified by SHA256; no re-download between attempts)",
             comp->version,
             (unsigned)image_size,
             max_attempts,
             (unsigned long)CONFIG_MAD_OTA_STM32_FLASH_BASE);

    esp_err_t last_flash_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        ota_set_status(MAD_OTA_STATUS_INSTALLING);
        ota_set_progress(0);
        ESP_LOGW(TAG,
                 "STM32 SWD full-image flash attempt %d/%d started: version=%s size=%u",
                 attempt,
                 max_attempts,
                 comp->version,
                 (unsigned)image_size);

        last_flash_ret = stm32_swd_programmer_flash_image(image, image_size, CONFIG_MAD_OTA_STM32_FLASH_BASE);

        if (last_flash_ret == ESP_OK) {
            ESP_LOGW(TAG,
                     "STM32 SWD full-image flash attempt %d/%d succeeded",
                     attempt,
                     max_attempts);
            break;
        }

        ESP_LOGE(TAG,
                 "STM32 SWD full-image flash attempt %d/%d failed: %s",
                 attempt,
                 max_attempts,
                 esp_err_to_name(last_flash_ret));

        if (attempt < max_attempts) {
            ESP_LOGW(TAG,
                     "STM32 SWD retry: keeping downloaded image in RAM, waiting %d ms before next full flash cycle",
                     CONFIG_MAD_OTA_STM32_FLASH_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_MAD_OTA_STM32_FLASH_RETRY_DELAY_MS));
        }
    }

    free(image);
    ret = last_flash_ret;

    if (ret != ESP_OK) {
        ota_set_error("STM32 SWD flash failed after %d full-image attempts: %s", max_attempts, esp_err_to_name(ret));
        report_result(comp, "failed", "STM32_SWD_FLASH_FAILED", (const char *)g_mad_ota_state.last_error);
        ota_task_wdt_resume_after_ota(&wdt_guard, "STM32 OTA/SWD failed");
        return ret;
    }

    ota_task_wdt_resume_after_ota(&wdt_guard, "STM32 OTA/SWD success");
    ota_set_status(MAD_OTA_STATUS_NO_UPDATE);
    ota_set_progress(100);
    report_progress(comp, 100);
    report_result(comp, "success", NULL, NULL);
    ESP_LOGW(TAG, "STM32 firmware installed OK: version=%s", comp->version);
    return ESP_OK;
}

static esp_err_t perform_stm32_check_and_install(void)
{
    const char *api_base_url = select_api_base_url();
    const char *firmware_base_url = select_firmware_base_url();
    const char *ssid = wifi_manager_get_connected_ssid();
    ESP_LOGI(TAG, "STM32 OTA server selected: ssid=\"%s\" api_base=%s firmware_base=%s target=%s",
             ssid ? ssid : "",
             api_base_url,
             firmware_base_url,
             CONFIG_MAD_OTA_STM32_TARGET_ID);

    char *manifest_json = NULL;
    esp_err_t ret = fetch_manifest(api_base_url, &manifest_json);
    if (ret != ESP_OK) {
        ota_set_error("STM32 manifest fetch failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mad_ota_manifest_component_t comp;
    ret = parse_manifest_component_for_target(firmware_base_url, manifest_json, CONFIG_MAD_OTA_STM32_TARGET_ID, &comp);
    free(manifest_json);
    if (ret == ESP_ERR_NOT_FOUND) {
        ota_set_error("No STM32 component offered by server for target=%s", CONFIG_MAD_OTA_STM32_TARGET_ID);
        return ret;
    }
    if (ret != ESP_OK) {
        ota_set_error("STM32 manifest parse failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Keep the UI pinned to the offered STM32 firmware version during the
     * whole install attempt and after a failure.  Previously STM32 failures
     * left available_version empty, so the info strip fell back to
     * FW:<ESP32 app version>, which looked like the update disappeared. */
    strlcpy((char *)g_mad_ota_state.available_version, comp.version, sizeof(g_mad_ota_state.available_version));
    ota_set_status(MAD_OTA_STATUS_UPDATE_AVAILABLE);

    ESP_LOGW(TAG,
             "STM32 OTA update available: target=%s version=%s file=%d url=%s size=%lld sha=%s",
             comp.target,
             comp.version,
             comp.firmware_file_id,
             comp.firmware_url,
             (long long)comp.size_bytes,
             comp.sha256);

    return download_and_install_stm32(&comp);
}

static esp_err_t perform_check_and_optional_install(bool force_install)
{
    const char *api_base_url = select_api_base_url();
    const char *firmware_base_url = select_firmware_base_url();
    const char *ssid = wifi_manager_get_connected_ssid();
    ESP_LOGI(TAG, "OTA server selected: ssid=\"%s\" api_base=%s firmware_base=%s", ssid ? ssid : "", api_base_url, firmware_base_url);

    char *manifest_json = NULL;
    esp_err_t ret = fetch_manifest(api_base_url, &manifest_json);
    if (ret != ESP_OK) {
        if (force_install) {
            ota_set_error("Manifest fetch failed: %s", esp_err_to_name(ret));
        } else {
            ota_set_check_warning("Manifest fetch failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    mad_ota_manifest_component_t comp;
    ret = parse_manifest_component_for_target(firmware_base_url, manifest_json, NULL, &comp);
    free(manifest_json);
    if (ret == ESP_ERR_NOT_FOUND) {
        s_update_component_valid = false;
        strlcpy((char *)g_mad_ota_state.available_version, "", sizeof(g_mad_ota_state.available_version));
        ota_set_status(MAD_OTA_STATUS_NO_UPDATE);
        ESP_LOGI(TAG, "No OTA component offered by server for target=%s (configured %s)", mad_ota_target_id(), CONFIG_MAD_OTA_TARGET_ID);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        if (force_install) {
            ota_set_error("Manifest parse failed: %s", esp_err_to_name(ret));
        } else {
            ota_set_check_warning("Manifest parse failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    strlcpy((char *)g_mad_ota_state.available_version, comp.version, sizeof(g_mad_ota_state.available_version));
    s_update_component = comp;
    s_update_component_valid = true;

    ESP_LOGI(TAG,
             "Manifest target=%s available_version=%s current_version=%s release=%d deployment=%d file=%d auto_update=%d auto_rollback=%d url=%s size=%lld sha=%s",
             comp.target[0] ? comp.target : CONFIG_MAD_OTA_TARGET_ID,
             comp.version,
             CONFIG_MAD_OTA_APP_VERSION,
             comp.release_id,
             comp.deployment_id,
             comp.firmware_file_id,
             (int)comp.auto_update,
             (int)comp.auto_rollback,
             comp.firmware_url,
             (long long)comp.size_bytes,
             comp.sha256);

    if (!version_needs_update(comp.version)) {
        ota_set_status(MAD_OTA_STATUS_NO_UPDATE);
        ESP_LOGI(TAG, "No P4 OTA update needed: current=%s available=%s", CONFIG_MAD_OTA_APP_VERSION, comp.version);
        return ESP_OK;
    }

    ota_set_status(MAD_OTA_STATUS_UPDATE_AVAILABLE);
    ESP_LOGW(TAG, "P4 OTA update available: %s -> %s", CONFIG_MAD_OTA_APP_VERSION, comp.version);

    if (force_install) {
        ESP_LOGW(TAG, "Manual OTA install requested from UI");
        return download_and_install_p4(&s_update_component);
    }

#if CONFIG_MAD_OTA_AUTO_INSTALL
    if (comp.auto_update || comp.mandatory) {
        ESP_LOGW(TAG, "Auto-install allowed locally and requested by server: auto_update=%d mandatory=%d",
                 (int)comp.auto_update,
                 (int)comp.mandatory);
        return download_and_install_p4(&s_update_component);
    }
    ESP_LOGW(TAG, "Server did not request auto_update for this bundle. Press UI update button to install available firmware.");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Local auto-install disabled. Server auto_update=%d mandatory=%d. Press UI UPDATE FW button to install available firmware.",
             (int)comp.auto_update,
             (int)comp.mandatory);
    ESP_LOGW(TAG, "OTA decision: CHECK_ONLY. No download is started because CONFIG_MAD_OTA_AUTO_INSTALL=0 and this was not a manual install.");
    return ESP_OK;
#endif
}

esp_err_t mad_ota_confirm_running_app(void)
{
#if CONFIG_MAD_OTA_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state = 0;
    esp_err_t ret = esp_ota_get_state_partition(running, &state);
    if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_get_state_partition failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Running OTA image is pending verification; marking valid now");
        ret = esp_ota_mark_app_valid_cancel_rollback();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
#else
    ESP_LOGI(TAG, "MAD OTA disabled by sdkconfig");
#endif
    return ESP_OK;
}

static void ota_run_common(bool force_install)
{
    ota_set_status(MAD_OTA_STATUS_WAITING_WIFI);
    strlcpy((char *)g_mad_ota_state.current_version, CONFIG_MAD_OTA_APP_VERSION, sizeof(g_mad_ota_state.current_version));

    const int wait_step_ms = 500;
    int waited_ms = 0;
    while (!g_wifi_state.connected && waited_ms < CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(wait_step_ms));
        waited_ms += wait_step_ms;
    }

    if (!g_wifi_state.connected) {
        if (force_install) {
            ota_set_error("WiFi not connected after %d ms; OTA skipped", CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS);
        } else {
            ota_set_check_warning("WiFi not connected after %d ms; OTA auto-check skipped", CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS);
        }
        return;
    }

    ota_set_status(MAD_OTA_STATUS_CHECKING);
    (void)perform_check_and_optional_install(force_install);
}

static void ota_task_finish(TaskHandle_t *slot, const char *task_name)
{
    if (slot) {
        *slot = NULL;
    }
    if (slot == &s_ota_task_handle) {
        s_ota_task_started = false;
    }

    /* Do not delete OTA helper tasks after HTTPS/mbedTLS work on ESP32-P4 while
     * SPIRAM XIP is enabled.  With IDF 5.4 the FreeRTOS TLSP deletion callback
     * path can reject a valid XIP-mapped pthread cleanup callback as
     * "non-executable" and abort during vTaskDelete().
     *
     * fix46 tried to disable CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS, but ESP-Hosted
     * creates static tasks very early and that configuration hit
     * xPortCheckValidTCBMem() before the display was initialized.  So fix47 keeps
     * the original FreeRTOS/ESP-Hosted configuration and avoids vTaskDelete() only
     * for these short-lived OTA helper tasks.  The task handle is cleared before
     * parking, so the UI can start a new manual OTA task if needed. */
    ESP_LOGW(TAG, "%s finished; parking task instead of vTaskDelete to avoid TLSP/XIP cleanup crash",
             task_name ? task_name : "OTA task");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3600000));
    }
}

static void ota_task(void *arg)
{
    (void)arg;

#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled by sdkconfig");
    ota_task_finish(&s_ota_task_handle, "mad_ota");
    return;
#endif

#if !CONFIG_MAD_OTA_AUTO_CHECK
    ESP_LOGI(TAG, "MAD OTA auto-check disabled by sdkconfig");
    ota_task_finish(&s_ota_task_handle, "mad_ota");
    return;
#endif

    ota_run_common(false);

    ota_task_finish(&s_ota_task_handle, "mad_ota");
}

static void ota_install_task(void *arg)
{
    (void)arg;

#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled by sdkconfig; manual install ignored");
#else
    if (s_update_component_valid && g_mad_ota_state.status == MAD_OTA_STATUS_UPDATE_AVAILABLE) {
        ESP_LOGW(TAG, "Installing previously detected OTA update from UI button");
        (void)download_and_install_p4(&s_update_component);
    } else {
        ota_run_common(true);
    }
#endif

    ota_task_finish(&s_ota_install_task_handle, "mad_ota_install");
}

static void stm32_ota_task(void *arg)
{
    (void)arg;

#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled by sdkconfig; STM32 OTA ignored");
#else
    ota_set_status(MAD_OTA_STATUS_WAITING_WIFI);

    const int wait_step_ms = 500;
    int waited_ms = 0;
    while (!g_wifi_state.connected && waited_ms < CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(wait_step_ms));
        waited_ms += wait_step_ms;
    }

    if (!g_wifi_state.connected) {
        ota_set_error("WiFi not connected after %d ms; STM32 OTA skipped", CONFIG_MAD_OTA_WAIT_WIFI_TIMEOUT_MS);
    } else {
        ota_set_status(MAD_OTA_STATUS_CHECKING);
        (void)perform_stm32_check_and_install();
    }
#endif

    ota_task_finish(&s_stm32_ota_task_handle, "stm32_ota_swd");
}

esp_err_t mad_ota_start_async(void)
{
#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled; task not started");
    return ESP_OK;
#else
    if (s_ota_task_started || s_ota_task_handle) {
        ESP_LOGI(TAG, "MAD OTA task already started");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(ota_task,
                                             "mad_ota",
                                             CONFIG_MAD_OTA_TASK_STACK_SIZE,
                                             NULL,
                                             CONFIG_MAD_OTA_TASK_PRIORITY,
                                             &s_ota_task_handle,
                                             CONFIG_MAD_OTA_TASK_CORE);
    if (ret != pdPASS) {
        s_ota_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ota_task_started = true;
    ESP_LOGI(TAG,
             "MAD OTA task started: product=%s hw=%s app_version=%s channel=%s auto_install=%d",
             CONFIG_MAD_OTA_PRODUCT_ID,
             CONFIG_MAD_OTA_HW_REV,
             CONFIG_MAD_OTA_APP_VERSION,
             CONFIG_MAD_OTA_CHANNEL,
             (int)CONFIG_MAD_OTA_AUTO_INSTALL);
    return ESP_OK;
#endif
}

esp_err_t mad_ota_install_stm32_available_async(void)
{
#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled; STM32 install not started");
    return ESP_OK;
#else
    if (s_stm32_ota_task_handle || s_ota_install_task_handle || s_ota_task_handle) {
        ESP_LOGW(TAG, "OTA check/install already in progress; STM32 install not started");
        return ESP_ERR_INVALID_STATE;
    }

    /* This flag is used by the HUD as "a manual firmware operation was
     * requested".  It must include STM32 OTA too, not only ESP32 self-OTA. */
    s_manual_install_attempted = true;

    BaseType_t ret = xTaskCreatePinnedToCore(stm32_ota_task,
                                             "stm32_ota_swd",
                                             CONFIG_MAD_OTA_TASK_STACK_SIZE,
                                             NULL,
                                             CONFIG_MAD_OTA_TASK_PRIORITY,
                                             &s_stm32_ota_task_handle,
                                             CONFIG_MAD_OTA_TASK_CORE);
    if (ret != pdPASS) {
        s_stm32_ota_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "Manual STM32 OTA/SWD install task started from UI button");
    return ESP_OK;
#endif
}

esp_err_t mad_ota_install_available_async(void)
{
#if !CONFIG_MAD_OTA_ENABLE
    ESP_LOGI(TAG, "MAD OTA disabled; manual install not started");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "Manual MAD OTA install requested: status=%d update_valid=%d check_task=%p install_task=%p available=%s current=%s",
             (int)g_mad_ota_state.status,
             (int)s_update_component_valid,
             (void *)s_ota_task_handle,
             (void *)s_ota_install_task_handle,
             g_mad_ota_state.available_version,
             CONFIG_MAD_OTA_APP_VERSION);
    if (s_ota_install_task_handle || s_ota_task_handle) {
        ESP_LOGW(TAG, "MAD OTA check/install already in progress: check_task=%p install_task=%p",
                 (void *)s_ota_task_handle,
                 (void *)s_ota_install_task_handle);
        return ESP_ERR_INVALID_STATE;
    }

    s_manual_install_attempted = true;

    BaseType_t ret = xTaskCreatePinnedToCore(ota_install_task,
                                             "mad_ota_install",
                                             CONFIG_MAD_OTA_TASK_STACK_SIZE,
                                             NULL,
                                             CONFIG_MAD_OTA_TASK_PRIORITY,
                                             &s_ota_install_task_handle,
                                             CONFIG_MAD_OTA_TASK_CORE);
    if (ret != pdPASS) {
        s_ota_install_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGW(TAG, "Manual MAD OTA install task started from UI button");
    return ESP_OK;
#endif
}

mad_ota_status_t mad_ota_get_status(void)
{
    return g_mad_ota_state.status;
}

int mad_ota_get_progress_percent(void)
{
    return g_mad_ota_state.progress_percent;
}

const char *mad_ota_get_last_error(void)
{
    return (const char *)g_mad_ota_state.last_error;
}

const char *mad_ota_get_current_version(void)
{
    return (const char *)g_mad_ota_state.current_version;
}

const char *mad_ota_get_available_version(void)
{
    return (const char *)g_mad_ota_state.available_version;
}


bool mad_ota_manual_install_was_requested(void)
{
    return s_manual_install_attempted;
}
