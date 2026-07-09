#include "eez_ui_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "demo_backlight.h"
#include "display_direct_timing.h"
#include "p4_project_config.h"
#include "ui_background_direct.h"
#include "screens.h"
#include "wifi_manager.h"
#include "update/update_manager.h"
#include "stm32_swd_programmer.h"

#ifndef EEZ_UI_ENABLE_WIDGET_TUNING
#define EEZ_UI_ENABLE_WIDGET_TUNING 1
#endif

#ifndef EEZ_UI_ENABLE_WIFI_LED
#define EEZ_UI_ENABLE_WIFI_LED 1
#endif

#ifndef EEZ_UI_ENABLE_FPS_COUNTER
#define EEZ_UI_ENABLE_FPS_COUNTER P4_ENABLE_FPS_COUNTER
#endif

#ifndef EEZ_UI_FPS_COUNTER_UPDATE_MS
#define EEZ_UI_FPS_COUNTER_UPDATE_MS P4_FPS_COUNTER_UPDATE_MS
#endif

#ifndef EEZ_UI_FPS_COUNTER_X
#define EEZ_UI_FPS_COUNTER_X P4_FPS_COUNTER_X
#endif

#ifndef EEZ_UI_FPS_COUNTER_Y
#define EEZ_UI_FPS_COUNTER_Y P4_FPS_COUNTER_Y
#endif

#ifndef EEZ_UI_BRIGHTNESS_SLIDER_MIN
#define EEZ_UI_BRIGHTNESS_SLIDER_MIN 5
#endif

#ifndef EEZ_UI_BRIGHTNESS_SLIDER_MAX
#define EEZ_UI_BRIGHTNESS_SLIDER_MAX 100
#endif

/*
 * Diagnostic/demo switch: individual LVGL objects can be rotated by style
 * transforms, but it is disabled by default because the clickable/input box
 * and layout usually still need manual adjustment after a 90 degree turn.
 */
#ifndef EEZ_UI_ROTATE_BUTTONS_90_DEG
#define EEZ_UI_ROTATE_BUTTONS_90_DEG 0
#endif

#define EEZ_UI_WIFI_LED_TIMER_MS 250U
#define EEZ_UI_WIFI_LED_SIZE     18
#define EEZ_UI_WIFI_LED_MARGIN   10
#define EEZ_UI_MAX_STATUS_LEDS   ((size_t)(_SCREEN_ID_LAST - _SCREEN_ID_FIRST + 1))

#ifndef EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE
#define EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE P4_DIRECT_RUNTIME_OVERLAY_ENABLE
#endif

#ifndef EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
#define EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE P4_RUNTIME_OVERLAY_LVGL_VISIBLE
#endif

#ifndef EEZ_UI_OTA_HUD_W
#define EEZ_UI_OTA_HUD_W 360
#endif

#ifndef EEZ_UI_OTA_HUD_H
#define EEZ_UI_OTA_HUD_H 96
#endif

#ifndef EEZ_UI_OTA_HUD_RADIUS
#define EEZ_UI_OTA_HUD_RADIUS 18
#endif

#ifndef EEZ_UI_OTA_HUD_X
#define EEZ_UI_OTA_HUD_X ((P4_UI_LOGICAL_WIDTH - EEZ_UI_OTA_HUD_W) / 2)
#endif

#ifndef EEZ_UI_OTA_HUD_Y
#define EEZ_UI_OTA_HUD_Y ((P4_UI_LOGICAL_HEIGHT - EEZ_UI_OTA_HUD_H) / 2)
#endif

#ifndef EEZ_UI_DIRECT_FPS_BOX_W
#define EEZ_UI_DIRECT_FPS_BOX_W P4_DIRECT_FPS_BOX_W
#endif

#ifndef EEZ_UI_DIRECT_FPS_BOX_H
#define EEZ_UI_DIRECT_FPS_BOX_H P4_DIRECT_FPS_BOX_H
#endif

static const char *TAG = "EEZ_RUNTIME";

#if EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
static lv_obj_t *s_runtime_overlay;
#endif
static lv_obj_t *s_wifi_led;
static lv_timer_t *s_wifi_led_timer;
static lv_obj_t *s_fps_label;
static lv_timer_t *s_fps_timer;
static lv_timer_t *s_static_overlay_bootstrap_timer;
static lv_obj_t *s_swd_probe_button;
static lv_obj_t *s_swd_probe_button_label;
static lv_timer_t *s_swd_probe_button_timer;

static char s_direct_fps_text[24] = "FPS: --";
static char s_direct_wifi_text[64] = "WI-FI: SCANNING";
/* Firmware/status text is drawn as colored segments in the direct overlay:
 *   FW / UPDATE prefix: yellow
 *   version number:     red
 *   availability state: purple
 */
static char s_direct_fw_prefix[16] = "FW";
static char s_direct_fw_version[32] = "--";
static char s_direct_fw_status[32] = "";
static float s_direct_fps_value;
static wifi_manager_connection_status_t s_direct_wifi_status = WIFI_MANAGER_STATUS_IDLE;

static lv_obj_t *screen_obj_by_id(enum ScreensEnum screen_id)
{
    if (screen_id < _SCREEN_ID_FIRST || screen_id > _SCREEN_ID_LAST) {
        return NULL;
    }

    return ((lv_obj_t **)&objects)[screen_id - _SCREEN_ID_FIRST];
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static lv_obj_t *runtime_overlay_root(void)
{
#if P4_RUNTIME_OVERLAY_TOP_LAYER && EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    if (s_runtime_overlay) {
        return s_runtime_overlay;
    }

    /*
     * Use LVGL's built-in top layer directly.  The previous implementation
     * created an additional full-screen transparent container here while the
     * code was still running on ESP-IDF's small main_task stack.  Creating and
     * sizing that extra object was enough to push LVGL label/style allocation
     * over the stack guard on ESP32-P4.
     *
     * The top layer already persists across screen switches, so there is no
     * need for our own full-screen parent object.
     */
    s_runtime_overlay = lv_layer_top();
    if (!s_runtime_overlay) {
        s_runtime_overlay = lv_screen_active();
    }

    if (s_runtime_overlay) {
        ESP_LOGI(TAG, "Static runtime overlay will use LVGL top layer directly: obj=%p", s_runtime_overlay);
    }

    return s_runtime_overlay;
#else
    return NULL;
#endif
}

#if EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
static void runtime_overlay_sync_size(void)
{
    /* No-op: s_runtime_overlay is lv_layer_top(), whose size/lifetime is owned by LVGL. */
}
#endif

void eez_ui_runtime_invalidate_static_overlay(void)
{
#if EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    runtime_overlay_sync_size();
    if (s_runtime_overlay) {
        lv_obj_invalidate(s_runtime_overlay);
    }
    if (s_wifi_led) {
        lv_obj_invalidate(s_wifi_led);
    }
    if (s_fps_label) {
        lv_obj_invalidate(s_fps_label);
    }
#endif
#if EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE
    ui_background_redraw_runtime_overlay_regions();
#endif
}

#if P4_LVGL_WIDGET_POS_MIRROR_X || P4_LVGL_WIDGET_POS_MIRROR_Y
static bool runtime_child_is_fullscreen_background(lv_obj_t *screen, lv_obj_t *child)
{
    if (!screen || !child || !lv_obj_check_type(child, &lv_image_class)) {
        return false;
    }

    const int32_t screen_w = lv_obj_get_width(screen);
    const int32_t screen_h = lv_obj_get_height(screen);
    const int32_t child_w = lv_obj_get_width(child);
    const int32_t child_h = lv_obj_get_height(child);
    const int32_t src_w = lv_image_get_src_width(child);
    const int32_t src_h = lv_image_get_src_height(child);

    if ((child_w == screen_w && child_h == screen_h) ||
        (src_w == screen_w && src_h == screen_h) ||
        (src_w == screen_h && src_h == screen_w)) {
        return true;
    }

    return false;
}
#endif

static void apply_position_only_mirror_to_top_level_widgets(lv_obj_t *screen)
{
#if P4_LVGL_WIDGET_POS_MIRROR_X || P4_LVGL_WIDGET_POS_MIRROR_Y
    if (!screen) {
        return;
    }

    lv_obj_update_layout(screen);
    const int32_t screen_w = lv_obj_get_width(screen);
    const int32_t screen_h = lv_obj_get_height(screen);
    const uint32_t child_count = (uint32_t)lv_obj_get_child_count(screen);

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (!child || runtime_child_is_fullscreen_background(screen, child)) {
            continue;
        }

        const int32_t old_x = lv_obj_get_x(child);
        const int32_t old_y = lv_obj_get_y(child);
        const int32_t w = lv_obj_get_width(child);
        const int32_t h = lv_obj_get_height(child);
        int32_t new_x = old_x;
        int32_t new_y = old_y;

#if P4_LVGL_WIDGET_POS_MIRROR_X
        new_x = screen_w - old_x - w;
#endif
#if P4_LVGL_WIDGET_POS_MIRROR_Y
        new_y = screen_h - old_y - h;
#endif

        if (new_x != old_x || new_y != old_y) {
            ESP_LOGI(TAG,
                     "Position-only mirror child=%p pos %ld,%ld -> %ld,%ld size=%ldx%ld screen=%ldx%ld mirror_x=%d mirror_y=%d",
                     child,
                     (long)old_x,
                     (long)old_y,
                     (long)new_x,
                     (long)new_y,
                     (long)w,
                     (long)h,
                     (long)screen_w,
                     (long)screen_h,
                     P4_LVGL_WIDGET_POS_MIRROR_X,
                     P4_LVGL_WIDGET_POS_MIRROR_Y);
            lv_obj_set_pos(child, new_x, new_y);
        }
    }
#else
    (void)screen;
#endif
}

#if EEZ_UI_ENABLE_WIDGET_TUNING
static void brightness_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    if (!slider) {
        return;
    }

    const int pct = (int)lv_slider_get_value(slider);
    esp_err_t ret = demo_backlight_set(pct);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Brightness slider failed to set %d%%: %s",
                 pct,
                 esp_err_to_name(ret));
    }
}

static void tune_brightness_slider(lv_obj_t *slider)
{
    if (!slider) {
        return;
    }

    lv_slider_set_range(slider,
                        EEZ_UI_BRIGHTNESS_SLIDER_MIN,
                        EEZ_UI_BRIGHTNESS_SLIDER_MAX);

    const int pct = clamp_int(demo_backlight_get(),
                              EEZ_UI_BRIGHTNESS_SLIDER_MIN,
                              EEZ_UI_BRIGHTNESS_SLIDER_MAX);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_set_style_bg_color(slider, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(slider, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(slider, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFD166), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(slider, 8, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB | LV_STATE_DEFAULT);

    ESP_LOGI(TAG, "Brightness slider attached: %d..%d current=%d%% obj=%p",
             EEZ_UI_BRIGHTNESS_SLIDER_MIN,
             EEZ_UI_BRIGHTNESS_SLIDER_MAX,
             pct,
             slider);
}

static void tune_arc(lv_obj_t *arc)
{
    if (!arc) {
        return;
    }

    lv_obj_set_style_arc_color(arc, lv_color_hex(0x101820), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_arc_color(arc, lv_color_hex(0xFFD166), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(arc, lv_color_hex(0xFFFFFF), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_DEFAULT);
}

static void tune_button(lv_obj_t *button)
{
    if (!button) {
        return;
    }

    lv_obj_set_style_bg_color(button, lv_color_hex(0x1D3557), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x457B9D), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(button, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);

#if EEZ_UI_ROTATE_BUTTONS_90_DEG
    lv_obj_set_style_transform_pivot_x(button, lv_obj_get_width(button) / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_pivot_y(button, lv_obj_get_height(button) / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_angle(button, 900, LV_PART_MAIN | LV_STATE_DEFAULT);
#endif
}

static void tune_label(lv_obj_t *label)
{
    if (!label) {
        return;
    }

    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void tune_widgets_recursive(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    if (lv_obj_check_type(obj, &lv_slider_class)) {
        tune_brightness_slider(obj);
    } else if (lv_obj_check_type(obj, &lv_arc_class)) {
        tune_arc(obj);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        tune_button(obj);
    } else if (lv_obj_check_type(obj, &lv_label_class)) {
        tune_label(obj);
    }

    const uint32_t child_count = (uint32_t)lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        tune_widgets_recursive(lv_obj_get_child(obj, i));
    }
}
#else
static void tune_widgets_recursive(lv_obj_t *obj)
{
    (void)obj;
}
#endif

static void update_direct_info_texts(void)
{
    const wifi_manager_connection_status_t wifi_status = wifi_manager_get_status();
    const mad_ota_status_t ota_status = mad_ota_get_status();

    if (wifi_status == WIFI_MANAGER_STATUS_CONNECTED) {
        const char *ssid = wifi_manager_get_connected_ssid();
        if (ssid && ssid[0]) {
            snprintf(s_direct_wifi_text, sizeof(s_direct_wifi_text), "WI-FI: %.32s", ssid);
        } else {
            snprintf(s_direct_wifi_text, sizeof(s_direct_wifi_text), "WI-FI: CONNECTED");
        }
    } else if (wifi_status == WIFI_MANAGER_STATUS_CONNECTING || wifi_status == WIFI_MANAGER_STATUS_IDLE) {
        snprintf(s_direct_wifi_text, sizeof(s_direct_wifi_text), "WI-FI: SCANNING");
    } else {
        snprintf(s_direct_wifi_text, sizeof(s_direct_wifi_text), "WI-FI: OFFLINE");
    }

    const char *current_version = mad_ota_get_current_version();
    const char *raw_available_version = mad_ota_get_available_version();
    if (!current_version || !current_version[0]) {
        current_version = "--";
    }
    const bool has_available_version = raw_available_version && raw_available_version[0];
    const char *available_version = has_available_version ? raw_available_version : current_version;

    switch (ota_status) {
    case MAD_OTA_STATUS_UPDATE_AVAILABLE:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "AVAILABLE");
        return;

    case MAD_OTA_STATUS_DOWNLOADING:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "DL %d%%", mad_ota_get_progress_percent());
        return;

    case MAD_OTA_STATUS_INSTALLING:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "INSTALL %d%%", mad_ota_get_progress_percent());
        return;

    case MAD_OTA_STATUS_VERIFYING:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "VERIFY");
        return;

    case MAD_OTA_STATUS_REBOOT_PENDING:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "REBOOT");
        return;

    case MAD_OTA_STATUS_FAILED:
        if (has_available_version) {
            /* Preserve the last offered firmware version after failed flashing or
             * transient errors.  Do not fall back to FW:<current ESP32 version>,
             * because that looks like the update disappeared. */
            snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "UPDATE");
            snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", available_version);
            snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "FAILED");
            return;
        }
        if (mad_ota_manual_install_was_requested()) {
            snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "FW");
            snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", current_version);
            snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "FAILED");
            return;
        }
        break;

    case MAD_OTA_STATUS_NO_UPDATE:
        snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "FW");
        snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", current_version);
        snprintf(s_direct_fw_status, sizeof(s_direct_fw_status), "NO UPDATE");
        return;

    case MAD_OTA_STATUS_IDLE:
    default:
        break;
    }

    snprintf(s_direct_fw_prefix, sizeof(s_direct_fw_prefix), "FW");
    snprintf(s_direct_fw_version, sizeof(s_direct_fw_version), "%.20s", current_version);
    s_direct_fw_status[0] = '\0';
}

#if EEZ_UI_ENABLE_WIFI_LED
static lv_color_t wifi_led_color_for_state(wifi_manager_connection_status_t status)
{
    switch (status) {
    case WIFI_MANAGER_STATUS_CONNECTED:
        return lv_color_hex(0x00D45A);  /* green */

    case WIFI_MANAGER_STATUS_CONNECTING:
        return lv_color_hex(0x00C8FF);  /* cyan */

    case WIFI_MANAGER_STATUS_FAILED:
    case WIFI_MANAGER_STATUS_DISCONNECTED:
    case WIFI_MANAGER_STATUS_IDLE:
    default:
        return lv_color_hex(0xE53935);  /* red */
    }
}

static void style_wifi_led(lv_obj_t *led, wifi_manager_connection_status_t status)
{
    if (!led) {
        return;
    }

    const lv_color_t color = wifi_led_color_for_state(status);

    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(led, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(led, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(led, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(led, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(led, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(led, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(led, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_wifi_leds(void)
{
    const wifi_manager_connection_status_t status = wifi_manager_get_status();
    s_direct_wifi_status = status;
    update_direct_info_texts();

#if EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    if (s_wifi_led) {
        style_wifi_led(s_wifi_led, status);
        lv_obj_invalidate(s_wifi_led);
    }
#endif
#if EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE
    ui_background_redraw_runtime_overlay_regions();
#endif
}

static void wifi_led_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_wifi_leds();
}

static void create_static_wifi_led(void)
{
#if !EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    s_direct_wifi_status = wifi_manager_get_status();
    return;
#endif
    if (s_wifi_led) {
        return;
    }

    lv_obj_t *parent = runtime_overlay_root();
    if (!parent) {
        ESP_LOGW(TAG, "Cannot create static WiFi LED: no LVGL top layer/active screen");
        return;
    }

    lv_obj_t *led = lv_obj_create(parent);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, EEZ_UI_WIFI_LED_SIZE, EEZ_UI_WIFI_LED_SIZE);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(led, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(led, LV_ALIGN_BOTTOM_MID, 0, -EEZ_UI_WIFI_LED_MARGIN);
    lv_obj_move_foreground(led);

    s_wifi_led = led;
    style_wifi_led(s_wifi_led, wifi_manager_get_status());

    ESP_LOGI(TAG, "Static WiFi status LED attached to top layer obj=%p", led);
}

#else
static void update_wifi_leds(void)
{
}

static void create_static_wifi_led(void)
{
}
#endif


#if EEZ_UI_ENABLE_FPS_COUNTER
static lv_color_t fps_color_for_value(float fps)
{
    if (fps < 30.0f) {
        return lv_color_hex(0xE53935);  /* red */
    }
    if (fps <= 40.0f) {
        return lv_color_hex(0xFFD166);  /* yellow */
    }
    return lv_color_hex(0x00D45A);      /* green */
}

static float current_display_fps(void)
{
    const display_direct_callback_stats_t stats = display_direct_get_callback_stats();
    int64_t interval_us = stats.refresh_interval_ema_us;
    if (interval_us <= 0) {
        interval_us = stats.last_refresh_interval_us;
    }
    if (interval_us <= 0) {
        return 0.0f;
    }
    return 1000000.0f / (float)interval_us;
}

static void style_fps_label(lv_obj_t *label, float fps)
{
    if (!label) {
        return;
    }

    const lv_color_t color = fps_color_for_value(fps);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(label, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(label, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(label, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(label, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(label, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(label, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(label, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(label, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_fps_labels(void)
{
    const float fps = current_display_fps();
    s_direct_fps_value = fps;
    if (fps > 0.1f) {
        snprintf(s_direct_fps_text, sizeof(s_direct_fps_text), "FPS: %.1f", (double)fps);
    } else {
        snprintf(s_direct_fps_text, sizeof(s_direct_fps_text), "FPS: --");
    }

#if EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    if (s_fps_label) {
        lv_label_set_text(s_fps_label, s_direct_fps_text);
        style_fps_label(s_fps_label, fps);
        lv_obj_invalidate(s_fps_label);
    }
#endif
#if EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE
    ui_background_redraw_runtime_overlay_regions();
#endif
}

static void fps_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_fps_labels();
}

static void create_static_fps_label(void)
{
#if !EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE
    update_fps_labels();
    return;
#endif
    if (s_fps_label) {
        return;
    }

    lv_obj_t *parent = runtime_overlay_root();
    if (!parent) {
        ESP_LOGW(TAG, "Cannot create static FPS counter: no LVGL top layer/active screen");
        return;
    }

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "FPS: --");
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(label, EEZ_UI_FPS_COUNTER_X, EEZ_UI_FPS_COUNTER_Y);
    lv_obj_move_foreground(label);

    s_fps_label = label;
    style_fps_label(s_fps_label, 0.0f);

    ESP_LOGI(TAG,
             "Static FPS counter attached to top layer obj=%p logical_pos=%d,%d thresholds=<30 red, 30..40 yellow, >40 green",
             label,
             EEZ_UI_FPS_COUNTER_X,
             EEZ_UI_FPS_COUNTER_Y);
}

#else
static void update_fps_labels(void)
{
}

static void create_static_fps_label(void)
{
}
#endif


#if EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE
static uint16_t direct_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

static uint16_t direct_fps_color_rgb565(float fps)
{
    if (fps < 30.0f) {
        return direct_rgb565(229, 57, 53);
    }
    if (fps <= 40.0f) {
        return direct_rgb565(255, 209, 102);
    }
    return direct_rgb565(0, 212, 90);
}

static uint16_t direct_wifi_color_rgb565(wifi_manager_connection_status_t status)
{
    switch (status) {
    case WIFI_MANAGER_STATUS_CONNECTED:
        return direct_rgb565(0, 212, 90);
    case WIFI_MANAGER_STATUS_CONNECTING:
        return direct_rgb565(0, 200, 255);
    case WIFI_MANAGER_STATUS_FAILED:
    case WIFI_MANAGER_STATUS_DISCONNECTED:
    case WIFI_MANAGER_STATUS_IDLE:
    default:
        return direct_rgb565(229, 57, 53);
    }
}

static uint16_t direct_blend_rgb565(uint16_t bg, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    const uint8_t br = (uint8_t)(((bg >> 11) & 0x1F) << 3);
    const uint8_t bgc = (uint8_t)(((bg >> 5) & 0x3F) << 2);
    const uint8_t bb = (uint8_t)((bg & 0x1F) << 3);
    const uint16_t inv = (uint16_t)(255U - alpha);
    const uint8_t rr = (uint8_t)(((uint16_t)r * alpha + (uint16_t)br * inv + 127U) / 255U);
    const uint8_t gg = (uint8_t)(((uint16_t)g * alpha + (uint16_t)bgc * inv + 127U) / 255U);
    const uint8_t bl = (uint8_t)(((uint16_t)b * alpha + (uint16_t)bb * inv + 127U) / 255U);
    return direct_rgb565(rr, gg, bl);
}

typedef struct {
    char ch;
    uint8_t rows[7];
} font5x7_t;

static const font5x7_t s_font5x7[] = {
    { ' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00} },
    { '-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00} },
    { '.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C} },
    { ':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00} },
    { '/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10} },
    { '%', {0x19,0x19,0x02,0x04,0x08,0x13,0x13} },
    { '#', {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A} },
    { '_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F} },
    { '0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E} },
    { '1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E} },
    { '2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F} },
    { '3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E} },
    { '4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02} },
    { '5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E} },
    { '6', {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E} },
    { '7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08} },
    { '8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E} },
    { '9', {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E} },
    { 'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E} },
    { 'C', {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F} },
    { 'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E} },
    { 'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F} },
    { 'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10} },
    { 'G', {0x0F,0x10,0x10,0x13,0x11,0x11,0x0F} },
    { 'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E} },
    { 'J', {0x07,0x02,0x02,0x02,0x12,0x12,0x0C} },
    { 'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11} },
    { 'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F} },
    { 'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11} },
    { 'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11} },
    { 'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10} },
    { 'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D} },
    { 'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11} },
    { 'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E} },
    { 'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04} },
    { 'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04} },
    { 'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A} },
    { 'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11} },
    { 'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04} },
    { 'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F} },
    { 'a', {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F} },
    { 'b', {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E} },
    { 'c', {0x00,0x00,0x0F,0x10,0x10,0x10,0x0F} },
    { 'd', {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F} },
    { 'e', {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E} },
    { 'f', {0x06,0x09,0x08,0x1E,0x08,0x08,0x08} },
    { 'g', {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01} },
    { 'h', {0x10,0x10,0x1E,0x11,0x11,0x11,0x11} },
    { 'i', {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E} },
    { 'j', {0x02,0x00,0x06,0x02,0x02,0x12,0x0C} },
    { 'k', {0x10,0x10,0x12,0x14,0x18,0x14,0x12} },
    { 'l', {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E} },
    { 'm', {0x00,0x00,0x1A,0x15,0x15,0x15,0x15} },
    { 'n', {0x00,0x00,0x1E,0x11,0x11,0x11,0x11} },
    { 'o', {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E} },
    { 'p', {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10} },
    { 'q', {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01} },
    { 'r', {0x00,0x00,0x16,0x18,0x10,0x10,0x10} },
    { 's', {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E} },
    { 't', {0x08,0x08,0x1E,0x08,0x08,0x09,0x06} },
    { 'u', {0x00,0x00,0x11,0x11,0x11,0x13,0x0D} },
    { 'v', {0x00,0x00,0x11,0x11,0x11,0x0A,0x04} },
    { 'w', {0x00,0x00,0x11,0x15,0x15,0x15,0x0A} },
    { 'x', {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11} },
    { 'y', {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E} },
    { 'z', {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F} },
};

static const uint8_t *font_rows_for_char(char ch)
{
    for (size_t i = 0; i < sizeof(s_font5x7) / sizeof(s_font5x7[0]); i++) {
        if (s_font5x7[i].ch == ch) {
            return s_font5x7[i].rows;
        }
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
        for (size_t i = 0; i < sizeof(s_font5x7) / sizeof(s_font5x7[0]); i++) {
            if (s_font5x7[i].ch == ch) {
                return s_font5x7[i].rows;
            }
        }
    }
    return s_font5x7[0].rows;
}

static bool logical_to_native(int lx, int ly, int *nx, int *ny)
{
    if (lx < 0 || ly < 0 || lx >= P4_UI_LOGICAL_WIDTH || ly >= P4_UI_LOGICAL_HEIGHT) {
        return false;
    }

    /* This is the same logical 800x480 -> native 480x800 transform used by
     * display_direct_prepare_rgb565_native() for the full-screen background. */
    *nx = ly;
    *ny = P4_UI_LOGICAL_WIDTH - 1 - lx;
    return *nx >= 0 && *ny >= 0 &&
           *nx < DISPLAY_DIRECT_NATIVE_H_RES && *ny < DISPLAY_DIRECT_NATIVE_V_RES;
}

static void put_native_clipped(uint16_t *dst,
                               int stride,
                               int clip_x,
                               int clip_y,
                               int clip_w,
                               int clip_h,
                               int nx,
                               int ny,
                               uint16_t color)
{
    if (!dst || nx < clip_x || ny < clip_y ||
        nx >= clip_x + clip_w || ny >= clip_y + clip_h) {
        return;
    }
    dst[(size_t)(ny - clip_y) * (size_t)stride + (size_t)(nx - clip_x)] = color;
}

static void put_logical_clipped(uint16_t *dst,
                                int stride,
                                int clip_x,
                                int clip_y,
                                int clip_w,
                                int clip_h,
                                int lx,
                                int ly,
                                uint16_t color)
{
    int nx = 0;
    int ny = 0;
    if (!logical_to_native(lx, ly, &nx, &ny)) {
        return;
    }
    put_native_clipped(dst, stride, clip_x, clip_y, clip_w, clip_h, nx, ny, color);
}

static void put_logical_alpha_clipped(uint16_t *dst,
                                      int stride,
                                      int clip_x,
                                      int clip_y,
                                      int clip_w,
                                      int clip_h,
                                      int lx,
                                      int ly,
                                      uint8_t r,
                                      uint8_t g,
                                      uint8_t b,
                                      uint8_t alpha)
{
    int nx = 0;
    int ny = 0;
    if (!logical_to_native(lx, ly, &nx, &ny) ||
        nx < clip_x || ny < clip_y ||
        nx >= clip_x + clip_w || ny >= clip_y + clip_h) {
        return;
    }

    uint16_t *px = &dst[(size_t)(ny - clip_y) * (size_t)stride + (size_t)(nx - clip_x)];
    *px = direct_blend_rgb565(*px, r, g, b, alpha);
}

static bool rounded_rect_contains(int x, int y, int w, int h, int radius)
{
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return false;
    }
    if (radius <= 0) {
        return true;
    }

    if (x >= radius && x < w - radius) {
        return true;
    }
    if (y >= radius && y < h - radius) {
        return true;
    }

    int cx = x < radius ? radius : (w - radius - 1);
    int cy = y < radius ? radius : (h - radius - 1);
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void fill_logical_rounded_rect_alpha(uint16_t *dst,
                                            int stride,
                                            int clip_x,
                                            int clip_y,
                                            int clip_w,
                                            int clip_h,
                                            int lx,
                                            int ly,
                                            int w,
                                            int h,
                                            int radius,
                                            uint8_t r,
                                            uint8_t g,
                                            uint8_t b,
                                            uint8_t alpha)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!rounded_rect_contains(x, y, w, h, radius)) {
                continue;
            }
            put_logical_alpha_clipped(dst, stride, clip_x, clip_y, clip_w, clip_h,
                                      lx + x, ly + y, r, g, b, alpha);
        }
    }
}

static void draw_logical_rounded_rect_outline(uint16_t *dst,
                                              int stride,
                                              int clip_x,
                                              int clip_y,
                                              int clip_w,
                                              int clip_h,
                                              int lx,
                                              int ly,
                                              int w,
                                              int h,
                                              int radius,
                                              int thickness,
                                              uint16_t color)
{
    if (thickness <= 0) {
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!rounded_rect_contains(x, y, w, h, radius)) {
                continue;
            }
            const int ix = x - thickness;
            const int iy = y - thickness;
            const bool inside_inner = rounded_rect_contains(ix, iy,
                                                            w - thickness * 2,
                                                            h - thickness * 2,
                                                            radius - thickness);
            if (!inside_inner) {
                put_logical_clipped(dst, stride, clip_x, clip_y, clip_w, clip_h,
                                    lx + x, ly + y, color);
            }
        }
    }
}

static void fill_logical_rect(uint16_t *dst,
                              int stride,
                              int clip_x,
                              int clip_y,
                              int clip_w,
                              int clip_h,
                              int lx,
                              int ly,
                              int w,
                              int h,
                              uint16_t color)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            put_logical_clipped(dst, stride, clip_x, clip_y, clip_w, clip_h, lx + x, ly + y, color);
        }
    }
}

static void draw_logical_rect_outline(uint16_t *dst,
                                      int stride,
                                      int clip_x,
                                      int clip_y,
                                      int clip_w,
                                      int clip_h,
                                      int lx,
                                      int ly,
                                      int w,
                                      int h,
                                      uint16_t color)
{
    fill_logical_rect(dst, stride, clip_x, clip_y, clip_w, clip_h, lx, ly, w, 1, color);
    fill_logical_rect(dst, stride, clip_x, clip_y, clip_w, clip_h, lx, ly + h - 1, w, 1, color);
    fill_logical_rect(dst, stride, clip_x, clip_y, clip_w, clip_h, lx, ly, 1, h, color);
    fill_logical_rect(dst, stride, clip_x, clip_y, clip_w, clip_h, lx + w - 1, ly, 1, h, color);
}

static void draw_char_5x7(uint16_t *dst,
                          int stride,
                          int clip_x,
                          int clip_y,
                          int clip_w,
                          int clip_h,
                          int lx,
                          int ly,
                          char ch,
                          int scale,
                          uint16_t color)
{
    const uint8_t *rows = font_rows_for_char(ch);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if ((rows[row] & (1U << (4 - col))) == 0) {
                continue;
            }
            fill_logical_rect(dst, stride, clip_x, clip_y, clip_w, clip_h,
                              lx + col * scale,
                              ly + row * scale,
                              scale,
                              scale,
                              color);
        }
    }
}

static void draw_text_5x7(uint16_t *dst,
                          int stride,
                          int clip_x,
                          int clip_y,
                          int clip_w,
                          int clip_h,
                          int lx,
                          int ly,
                          const char *text,
                          int scale,
                          uint16_t color)
{
    int x = lx;
    while (text && *text) {
        draw_char_5x7(dst, stride, clip_x, clip_y, clip_w, clip_h, x, ly, *text, scale, color);
        x += 6 * scale;
        text++;
    }
}

static void draw_text_5x7_width(uint16_t *dst,
                                int stride,
                                int clip_x,
                                int clip_y,
                                int clip_w,
                                int clip_h,
                                int lx,
                                int ly,
                                int max_w,
                                const char *text,
                                int scale,
                                uint16_t color)
{
    int x = lx;
    const int char_w = 5 * scale;
    const int advance = 6 * scale;
    const int right = lx + max_w;
    while (text && *text && x + char_w <= right) {
        draw_char_5x7(dst, stride, clip_x, clip_y, clip_w, clip_h, x, ly, *text, scale, color);
        x += advance;
        text++;
    }
}

static int draw_text_5x7_segment(uint16_t *dst,
                                 int stride,
                                 int clip_x,
                                 int clip_y,
                                 int clip_w,
                                 int clip_h,
                                 int lx,
                                 int ly,
                                 int right,
                                 const char *text,
                                 int scale,
                                 uint16_t color)
{
    int x = lx;
    const int char_w = 5 * scale;
    const int advance = 6 * scale;
    while (text && *text && x + char_w <= right) {
        draw_char_5x7(dst, stride, clip_x, clip_y, clip_w, clip_h, x, ly, *text, scale, color);
        x += advance;
        text++;
    }
    return x;
}

static void native_bounds_for_logical_rect(int lx, int ly, int w, int h, eez_runtime_overlay_region_t *r)
{
    int xs[4];
    int ys[4];
    logical_to_native(lx, ly, &xs[0], &ys[0]);
    logical_to_native(lx + w - 1, ly, &xs[1], &ys[1]);
    logical_to_native(lx, ly + h - 1, &xs[2], &ys[2]);
    logical_to_native(lx + w - 1, ly + h - 1, &xs[3], &ys[3]);

    int min_x = xs[0];
    int max_x = xs[0];
    int min_y = ys[0];
    int max_y = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }

    const int pad = 2;
    min_x -= pad;
    min_y -= pad;
    max_x += pad;
    max_y += pad;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= DISPLAY_DIRECT_NATIVE_H_RES) max_x = DISPLAY_DIRECT_NATIVE_H_RES - 1;
    if (max_y >= DISPLAY_DIRECT_NATIVE_V_RES) max_y = DISPLAY_DIRECT_NATIVE_V_RES - 1;

    r->x = min_x;
    r->y = min_y;
    r->w = max_x - min_x + 1;
    r->h = max_y - min_y + 1;
}

bool eez_ui_runtime_direct_overlay_enabled(void)
{
    return true;
}

static bool direct_ota_hud_visible(void)
{
    if (!mad_ota_p4_self_ota_display_hold_active()) {
        return false;
    }
    const mad_ota_status_t status = mad_ota_get_status();
    return status == MAD_OTA_STATUS_DOWNLOADING ||
           status == MAD_OTA_STATUS_INSTALLING ||
           status == MAD_OTA_STATUS_VERIFYING ||
           status == MAD_OTA_STATUS_REBOOT_PENDING ||
           status == MAD_OTA_STATUS_FAILED;
}

static void draw_direct_ota_hud_rgb565_native(uint16_t *dst,
                                              int stride_pixels,
                                              int clip_x,
                                              int clip_y,
                                              int clip_w,
                                              int clip_h)
{
    if (!direct_ota_hud_visible()) {
        return;
    }

    const mad_ota_status_t status = mad_ota_get_status();
    int progress = mad_ota_get_progress_percent();
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    const int x = EEZ_UI_OTA_HUD_X;
    const int y = EEZ_UI_OTA_HUD_Y;
    const int w = EEZ_UI_OTA_HUD_W;
    const int h = EEZ_UI_OTA_HUD_H;
    const int radius = EEZ_UI_OTA_HUD_RADIUS;

    const uint16_t border = direct_rgb565(255, 216, 64);
    const uint16_t text = direct_rgb565(255, 255, 255);
    const uint16_t accent = direct_rgb565(70, 190, 255);
    const uint16_t fail = direct_rgb565(255, 76, 76);

    fill_logical_rounded_rect_alpha(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                    x, y, w, h, radius,
                                    0, 0, 0, 190);
    draw_logical_rounded_rect_outline(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                      x, y, w, h, radius, 2,
                                      status == MAD_OTA_STATUS_FAILED ? fail : border);

    const char *title = "UPDATING ESP32P4";
    if (status == MAD_OTA_STATUS_DOWNLOADING) {
        title = "DOWNLOADING ESP32P4";
    } else if (status == MAD_OTA_STATUS_INSTALLING) {
        title = "INSTALLING ESP32P4";
    } else if (status == MAD_OTA_STATUS_VERIFYING) {
        title = "VERIFYING ESP32P4";
    } else if (status == MAD_OTA_STATUS_REBOOT_PENDING) {
        title = "REBOOTING";
    } else if (status == MAD_OTA_STATUS_FAILED) {
        title = "ESP32P4 OTA FAILED";
    }

    draw_text_5x7_width(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                        x + 22, y + 16, w - 44,
                        title, 3,
                        status == MAD_OTA_STATUS_FAILED ? fail : text);

    char pct[24];
    int pct_scale = 4;
    if (status == MAD_OTA_STATUS_REBOOT_PENDING) {
        snprintf(pct, sizeof(pct), "wait...");
        pct_scale = 3;
    } else {
        snprintf(pct, sizeof(pct), "%d%%", progress);
    }
    const int pct_w = (int)strlen(pct) * 6 * pct_scale;
    draw_text_5x7(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                  x + (w - pct_w) / 2, y + 46,
                  pct, pct_scale,
                  status == MAD_OTA_STATUS_FAILED ? fail : accent);

    const int bar_x = x + 28;
    const int bar_y = y + h - 18;
    const int bar_w = w - 56;
    const int bar_h = 8;
    const int fill_w = (bar_w * progress + 99) / 100;
    for (int yy = 0; yy < bar_h; ++yy) {
        for (int xx = 0; xx < bar_w; ++xx) {
            put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                      bar_x + xx, bar_y + yy,
                                      60, 80, 100, 180);
        }
        for (int xx = 0; xx < fill_w; ++xx) {
            put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                      bar_x + xx, bar_y + yy,
                                      70, 190, 255, 230);
        }
    }
}

size_t eez_ui_runtime_get_direct_overlay_regions(eez_runtime_overlay_region_t *regions,
                                                 size_t max_regions)
{
    size_t n = 0;
    if (regions && max_regions > n) {
        native_bounds_for_logical_rect(P4_DIRECT_INFO_STRIP_X,
                                       P4_DIRECT_INFO_STRIP_Y,
                                       P4_DIRECT_INFO_STRIP_W,
                                       P4_DIRECT_INFO_STRIP_H,
                                       &regions[n++]);
    }

    if (regions && max_regions > n) {
        const int led_x = (P4_UI_LOGICAL_WIDTH - EEZ_UI_WIFI_LED_SIZE) / 2;
        const int led_y = P4_UI_LOGICAL_HEIGHT - EEZ_UI_WIFI_LED_SIZE - EEZ_UI_WIFI_LED_MARGIN;
        native_bounds_for_logical_rect(led_x,
                                       led_y,
                                       EEZ_UI_WIFI_LED_SIZE,
                                       EEZ_UI_WIFI_LED_SIZE,
                                       &regions[n++]);
    }

    if (direct_ota_hud_visible() && regions && max_regions > n) {
        native_bounds_for_logical_rect(EEZ_UI_OTA_HUD_X,
                                       EEZ_UI_OTA_HUD_Y,
                                       EEZ_UI_OTA_HUD_W,
                                       EEZ_UI_OTA_HUD_H,
                                       &regions[n++]);
    }

    return n;
}

void eez_ui_runtime_draw_direct_overlay_rgb565_native(uint16_t *dst,
                                                      int stride_pixels,
                                                      int clip_x,
                                                      int clip_y,
                                                      int clip_w,
                                                      int clip_h)
{
    if (!dst || clip_w <= 0 || clip_h <= 0) {
        return;
    }

    update_direct_info_texts();
    const mad_ota_status_t ota_status = mad_ota_get_status();

    const uint16_t white = direct_rgb565(255, 255, 255);
    const uint16_t fps_color = direct_fps_color_rgb565(s_direct_fps_value);
    const uint16_t net_color = direct_rgb565(80, 180, 255);
    const uint16_t fw_prefix_color = direct_rgb565(255, 216, 64);   /* yellow: FW / UPDATE */
    const uint16_t fw_version_color = direct_rgb565(255, 76, 76);   /* red: firmware version */
    const uint16_t fw_status_color = direct_rgb565(190, 105, 255);  /* purple: availability/status */
    const uint16_t led_color = direct_wifi_color_rgb565(s_direct_wifi_status);

    const int strip_x = P4_DIRECT_INFO_STRIP_X;
    const int strip_y = P4_DIRECT_INFO_STRIP_Y;
    const int fps_x = EEZ_UI_FPS_COUNTER_X;
    const int fps_y = EEZ_UI_FPS_COUNTER_Y;
    const int net_x = P4_DIRECT_NET_TEXT_X;
    const int net_y = strip_y;
    const int fw_x = P4_DIRECT_FW_TEXT_X;
    const int fw_y = strip_y;

    /* One shared translucent info strip instead of two separate pill capsules.
     * Keep the background visible under it; this is a status ribbon, not a block. */
    fill_logical_rounded_rect_alpha(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                    strip_x, strip_y,
                                    P4_DIRECT_INFO_STRIP_W, P4_DIRECT_INFO_STRIP_H,
                                    P4_DIRECT_INFO_STRIP_RADIUS,
                                    0, 0, 0, P4_DIRECT_OVERLAY_BG_ALPHA);
    /* Draw only long horizontal borders for the royal info strip.
     * Do not draw the left/right vertical edge: on the rotated/native framebuffer
     * it appears as a lonely short border segment at the screen edge. */
    for (int x = strip_x; x < strip_x + P4_DIRECT_INFO_STRIP_W; ++x) {
        put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                  x, strip_y, 80, 180, 255, P4_DIRECT_INFO_STRIP_BORDER_ALPHA);
        put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                  x, strip_y + P4_DIRECT_INFO_STRIP_H - 1, 80, 180, 255, P4_DIRECT_INFO_STRIP_BORDER_ALPHA);
    }
    draw_text_5x7(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                  fps_x + 7, fps_y + 4, s_direct_fps_text, 2, fps_color);

    /* Vertical separators inside the full-width royal strip. */
    for (int y = strip_y + 4; y < strip_y + P4_DIRECT_INFO_STRIP_H - 4; ++y) {
        put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                  P4_DIRECT_NET_TEXT_X - 10, y, 80, 180, 255, 120);
        put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                  P4_DIRECT_FW_TEXT_X - 10, y, 80, 180, 255, 120);
    }

    draw_text_5x7_width(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                         net_x, net_y + 4, P4_DIRECT_NET_TEXT_W,
                         s_direct_wifi_text, 2, net_color);
    const int fw_text_y = fw_y + 4;
    const int fw_right = fw_x + P4_DIRECT_FW_TEXT_W;
    int fw_cursor = fw_x;
    fw_cursor = draw_text_5x7_segment(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                      fw_cursor, fw_text_y, fw_right,
                                      s_direct_fw_prefix, 2, fw_prefix_color);
    fw_cursor += 12;
    fw_cursor = draw_text_5x7_segment(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                      fw_cursor, fw_text_y, fw_right,
                                      s_direct_fw_version, 2, fw_version_color);
    if (s_direct_fw_status[0] && fw_cursor + 12 < fw_right) {
        fw_cursor += 12;
        draw_text_5x7_segment(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                              fw_cursor, fw_text_y, fw_right,
                              s_direct_fw_status, 2, fw_status_color);
    }

    /* Full-width OTA progress line at the bottom of the info strip.
     * It is visible only while a user-requested/allowed update is actually
     * downloading or being written. In idle/check/no-update states the strip
     * stays clean and only shows the installed firmware version. */
    if (ota_status == MAD_OTA_STATUS_DOWNLOADING || ota_status == MAD_OTA_STATUS_INSTALLING || ota_status == MAD_OTA_STATUS_VERIFYING) {
        int progress = mad_ota_get_progress_percent();
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;

        const int bar_x = P4_DIRECT_INFO_STRIP_X;
        const int bar_y = P4_DIRECT_INFO_STRIP_Y + P4_DIRECT_INFO_STRIP_H - 4;
        const int bar_w = P4_DIRECT_INFO_STRIP_W;
        const int bar_h = 4;
        const int fill_w = (bar_w * progress + 99) / 100;

        for (int yy = 0; yy < bar_h; ++yy) {
            for (int xx = 0; xx < bar_w; ++xx) {
                put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                          bar_x + xx, bar_y + yy,
                                          70, 120, 150, 80);
            }
            for (int xx = 0; xx < fill_w; ++xx) {
                put_logical_alpha_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                          bar_x + xx, bar_y + yy,
                                          70, 190, 255, 220);
            }
        }
    }

    draw_direct_ota_hud_rgb565_native(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h);

    const int led_x = (P4_UI_LOGICAL_WIDTH - EEZ_UI_WIFI_LED_SIZE) / 2;
    const int led_y = P4_UI_LOGICAL_HEIGHT - EEZ_UI_WIFI_LED_SIZE - EEZ_UI_WIFI_LED_MARGIN;
    const int cx = led_x + EEZ_UI_WIFI_LED_SIZE / 2;
    const int cy = led_y + EEZ_UI_WIFI_LED_SIZE / 2;
    const int r_outer = EEZ_UI_WIFI_LED_SIZE / 2;
    const int r_inner = r_outer - 3;

    for (int y = led_y; y < led_y + EEZ_UI_WIFI_LED_SIZE; y++) {
        for (int x = led_x; x < led_x + EEZ_UI_WIFI_LED_SIZE; x++) {
            const int dx = x - cx;
            const int dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= r_outer * r_outer) {
                put_logical_clipped(dst, stride_pixels, clip_x, clip_y, clip_w, clip_h,
                                    x, y, d2 <= r_inner * r_inner ? led_color : white);
            }
        }
    }
}
#else
bool eez_ui_runtime_direct_overlay_enabled(void)
{
    return false;
}

size_t eez_ui_runtime_get_direct_overlay_regions(eez_runtime_overlay_region_t *regions,
                                                 size_t max_regions)
{
    (void)regions;
    (void)max_regions;
    return 0;
}

void eez_ui_runtime_draw_direct_overlay_rgb565_native(uint16_t *dst,
                                                      int stride_pixels,
                                                      int clip_x,
                                                      int clip_y,
                                                      int clip_w,
                                                      int clip_h)
{
    (void)dst;
    (void)stride_pixels;
    (void)clip_x;
    (void)clip_y;
    (void)clip_w;
    (void)clip_h;
}
#endif


static void ota_update_button_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = mad_ota_install_available_async();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Manual OTA install request failed: %s", esp_err_to_name(ret));
    }
    eez_ui_runtime_invalidate_static_overlay();
}

static void swd_probe_button_sync(void)
{
    if (!s_swd_probe_button_label) {
        return;
    }

    const stm32_swd_status_t status = stm32_swd_programmer_get_status();
    const stm32_swd_state_t *state = stm32_swd_programmer_get_state();

    switch (status) {
    case STM32_SWD_STATUS_BUSY:
        lv_label_set_text(s_swd_probe_button_label, "STM32 BUSY");
        lv_obj_set_style_text_color(s_swd_probe_button_label, lv_color_hex(0xFFD840), LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    case STM32_SWD_STATUS_OK:
        lv_label_set_text(s_swd_probe_button_label, "STM32 OK");
        lv_obj_set_style_text_color(s_swd_probe_button_label, lv_color_hex(0x7ED957), LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    case STM32_SWD_STATUS_FAILED:
        lv_label_set_text(s_swd_probe_button_label, "STM32 FAIL");
        lv_obj_set_style_text_color(s_swd_probe_button_label, lv_color_hex(0xFF4C4C), LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    case STM32_SWD_STATUS_IDLE:
    default:
        lv_label_set_text(s_swd_probe_button_label, "STM32 FW");
        lv_obj_set_style_text_color(s_swd_probe_button_label, lv_color_hex(0xB469FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    }

    (void)state;
}

static void swd_probe_button_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    swd_probe_button_sync();
}

static void swd_probe_button_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = mad_ota_install_stm32_available_async();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "STM32 OTA/SWD install request failed: %s", esp_err_to_name(ret));
    }
    swd_probe_button_sync();
}

static void create_swd_probe_button(lv_obj_t *anchor_button)
{
    lv_obj_t *parent = NULL;
    if (anchor_button) {
        parent = lv_obj_get_parent(anchor_button);
    }
    if (!parent) {
        parent = screen_obj_by_id(SCREEN_ID_PAGE1);
    }
    if (!parent) {
        ESP_LOGW(TAG, "Cannot create STM32 firmware button: no page1 root");
        return;
    }

    if (!s_swd_probe_button) {
        s_swd_probe_button = lv_button_create(parent);
        if (!s_swd_probe_button) {
            return;
        }

        lv_obj_set_style_bg_color(s_swd_probe_button, lv_color_hex(0x160B24), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_swd_probe_button, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_swd_probe_button, lv_color_hex(0x3B185F), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_swd_probe_button, lv_color_hex(0xB469FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(s_swd_probe_button, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_swd_probe_button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_swd_probe_button, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(s_swd_probe_button, swd_probe_button_event_cb, LV_EVENT_CLICKED, NULL);

        s_swd_probe_button_label = lv_label_create(s_swd_probe_button);
        if (s_swd_probe_button_label) {
            lv_obj_center(s_swd_probe_button_label);
        }

        if (!s_swd_probe_button_timer) {
            s_swd_probe_button_timer = lv_timer_create(swd_probe_button_timer_cb, 250, NULL);
        }
    }

    lv_obj_set_size(s_swd_probe_button, 130, 42);
    if (anchor_button) {
        lv_obj_align_to(s_swd_probe_button, anchor_button, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    } else {
        /* Fallback only if generated page1 was changed and UPDATE FW was not found. */
        lv_obj_align(s_swd_probe_button, LV_ALIGN_TOP_RIGHT, -144, 110);
    }

    swd_probe_button_sync();
    ESP_LOGI(TAG, "STM32 firmware button created/aligned under UPDATE FW: obj=%p anchor=%p", s_swd_probe_button, anchor_button);
}

static void tune_page1_update_button(void)
{
    lv_obj_t *screen = screen_obj_by_id(SCREEN_ID_PAGE1);
    if (!screen) {
        return;
    }

    const uint32_t child_count = lv_obj_get_child_count(screen);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (!child || lv_obj_get_child_count(child) == 0) {
            continue;
        }

        /* Generated page1 currently has one interactive button with one label child.
         * Avoid depending on LVGL internal class symbols so this survives minor LVGL changes. */
        lv_obj_set_size(child, 130, 50);
        lv_obj_set_style_bg_color(child, lv_color_hex(0x0B2A3A), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(child, LV_OPA_60, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(child, lv_color_hex(0x50B4FF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(child, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(child, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(child, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(child, ota_update_button_event_cb, LV_EVENT_CLICKED, NULL);

        if (lv_obj_get_child_count(child) > 0) {
            lv_obj_t *label = lv_obj_get_child(child, 0);
            if (label) {
                lv_label_set_text(label, "UPDATE FW");
                lv_obj_set_style_text_color(label, lv_color_hex(0xFFD840), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }

        create_swd_probe_button(child);
        ESP_LOGI(TAG, "Page1 OTA update button bound: obj=%p", child);
        return;
    }

    ESP_LOGW(TAG, "Page1 OTA update button not found");
}

/*
 * Page binding table.  Generated screen content may change in EEZ Studio; this
 * table is the stable project-owned layer where we decide what runtime features
 * each page receives after regeneration.
 */
typedef struct {
    enum ScreensEnum screen_id;
    bool apply_widget_styles;
} eez_ui_page_profile_t;

static const eez_ui_page_profile_t s_page_profiles[] = {
    { SCREEN_ID_PAGE1, true },
    { SCREEN_ID_PAGE2, true },
    { SCREEN_ID_PAGE3, true },
    { SCREEN_ID_PAGE4, true },
    { SCREEN_ID_PAGE5, true },
    { SCREEN_ID_PAGE6, true },
    { SCREEN_ID_PAGE7, true },
};

static void static_overlay_bootstrap_timer_cb(lv_timer_t *timer)
{
    if (timer) {
        lv_timer_delete(timer);
    }
    s_static_overlay_bootstrap_timer = NULL;

#if EEZ_UI_ENABLE_WIFI_LED
    create_static_wifi_led();
#endif

#if EEZ_UI_ENABLE_FPS_COUNTER
    create_static_fps_label();
#endif

    eez_ui_runtime_invalidate_static_overlay();
    update_wifi_leds();

#if EEZ_UI_ENABLE_WIFI_LED
    if (!s_wifi_led_timer) {
        s_wifi_led_timer = lv_timer_create(wifi_led_timer_cb, EEZ_UI_WIFI_LED_TIMER_MS, NULL);
    }
#endif

#if EEZ_UI_ENABLE_FPS_COUNTER
    update_fps_labels();
    if (!s_fps_timer) {
        s_fps_timer = lv_timer_create(fps_timer_cb, EEZ_UI_FPS_COUNTER_UPDATE_MS, NULL);
    }
#endif

    ESP_LOGI(TAG, "Static FPS/WiFi overlay created from LVGL task context");
}

static void schedule_static_overlay_bootstrap(void)
{
#if EEZ_UI_ENABLE_WIFI_LED || EEZ_UI_ENABLE_FPS_COUNTER
    if (!s_static_overlay_bootstrap_timer && (!s_wifi_led || !s_fps_label)) {
        /*
         * Defer persistent overlay creation to lv_timer_handler(), i.e. the
         * dedicated LVGL task with a 10 KiB stack, instead of doing it during
         * eez_ui_port_init() on ESP-IDF's small main_task stack.
         */
        s_static_overlay_bootstrap_timer = lv_timer_create(static_overlay_bootstrap_timer_cb, 1, NULL);
        ESP_LOGI(TAG, "Static FPS/WiFi overlay creation deferred to LVGL task");
    }
#endif
}

void eez_ui_runtime_init(void)
{
    for (size_t i = 0; i < sizeof(s_page_profiles) / sizeof(s_page_profiles[0]); i++) {
        const eez_ui_page_profile_t *profile = &s_page_profiles[i];
        lv_obj_t *screen = screen_obj_by_id(profile->screen_id);
        if (!screen) {
            ESP_LOGW(TAG, "Screen %d is not created; runtime binding skipped", profile->screen_id);
            continue;
        }

        apply_position_only_mirror_to_top_level_widgets(screen);

        if (profile->apply_widget_styles) {
            tune_widgets_recursive(screen);
        }
    }

    tune_page1_update_button();
    if (!s_swd_probe_button) {
        create_swd_probe_button(NULL);
    }

    schedule_static_overlay_bootstrap();

    ESP_LOGI(TAG,
             "EEZ runtime layer initialized: logical=%dx%d lvgl_ui_180=%d pos_mirror_x=%d pos_mirror_y=%d fps_counter=%d direct_overlay=%d lvgl_overlay_visible=%d",
             P4_UI_LOGICAL_WIDTH,
             P4_UI_LOGICAL_HEIGHT,
             P4_LVGL_ROTATE_UI_180_FROM_PREVIOUS,
             P4_LVGL_WIDGET_POS_MIRROR_X,
             P4_LVGL_WIDGET_POS_MIRROR_Y,
             EEZ_UI_ENABLE_FPS_COUNTER,
             EEZ_UI_DIRECT_RUNTIME_OVERLAY_ENABLE,
             EEZ_UI_RUNTIME_OVERLAY_LVGL_VISIBLE);
}
