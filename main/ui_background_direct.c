#include "ui_background_direct.h"

#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "images.h"
#include "eez_ui_runtime.h"

static const char *TAG = "UI_BG";

static uint16_t *s_native_frame;
static uint16_t *s_clean_native_frame;
static uint16_t *s_overlay_scratch;
static size_t s_overlay_scratch_pixels;
static esp_lcd_panel_handle_t s_last_panel;
static volatile bool s_runtime_overlay_suspended;
static volatile bool s_lvgl_flush_suspended;
static volatile bool s_lvgl_task_suspended;
static volatile bool s_runtime_overlay_busy;
static int64_t s_runtime_overlay_retry_after_us;

#define UI_BG_OVERLAY_RETRY_COOLDOWN_US (250 * 1000)

void ui_background_set_runtime_overlay_suspended(bool suspended)
{
    s_runtime_overlay_suspended = suspended;
    if (!suspended) {
        s_runtime_overlay_retry_after_us = 0;
    }
    ESP_LOGI(TAG, "Direct runtime overlay redraw %s", suspended ? "suspended" : "resumed");
}

bool ui_background_is_runtime_overlay_suspended(void)
{
    return s_runtime_overlay_suspended;
}

void ui_background_set_lvgl_flush_suspended(bool suspended)
{
    if (s_lvgl_flush_suspended == suspended) {
        return;
    }
    s_lvgl_flush_suspended = suspended;
    ESP_LOGW(TAG, "LVGL panel flush %s", suspended ? "suspended" : "resumed");
}

bool ui_background_is_lvgl_flush_suspended(void)
{
    return s_lvgl_flush_suspended;
}

void ui_background_set_lvgl_task_suspended(bool suspended)
{
    if (s_lvgl_task_suspended == suspended) {
        return;
    }
    s_lvgl_task_suspended = suspended;
    ESP_LOGW(TAG, "LVGL timer handler %s", suspended ? "suspended" : "resumed");
}

bool ui_background_is_lvgl_task_suspended(void)
{
    return s_lvgl_task_suspended;
}

static const char *image_name_from_descriptor(const lv_img_dsc_t *img)
{
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (images[i].img_dsc == img) {
            return images[i].name;
        }
    }

    return "?";
}

static esp_err_t ensure_native_frame(void)
{
    const size_t bytes = (size_t)DISPLAY_DIRECT_NATIVE_H_RES *
                         DISPLAY_DIRECT_NATIVE_V_RES *
                         sizeof(uint16_t);

    if (!s_native_frame) {
        s_native_frame = heap_caps_malloc(bytes,
                                          MALLOC_CAP_SPIRAM |
                                          MALLOC_CAP_DMA |
                                          MALLOC_CAP_8BIT);
        if (!s_native_frame) {
            ESP_LOGE(TAG, "Failed to allocate native draw frame bytes=%lu",
                     (unsigned long)bytes);
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "Allocated native draw frame %ux%u bytes=%lu ptr=%p",
                 DISPLAY_DIRECT_NATIVE_H_RES,
                 DISPLAY_DIRECT_NATIVE_V_RES,
                 (unsigned long)bytes,
                 s_native_frame);
    }

    if (!s_clean_native_frame) {
        s_clean_native_frame = heap_caps_malloc(bytes,
                                                MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_DMA |
                                                MALLOC_CAP_8BIT);
        if (!s_clean_native_frame) {
            ESP_LOGE(TAG, "Failed to allocate clean native background frame bytes=%lu",
                     (unsigned long)bytes);
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "Allocated clean native background frame %ux%u bytes=%lu ptr=%p",
                 DISPLAY_DIRECT_NATIVE_H_RES,
                 DISPLAY_DIRECT_NATIVE_V_RES,
                 (unsigned long)bytes,
                 s_clean_native_frame);
    }

    return ESP_OK;
}

esp_err_t ui_background_draw_synced(esp_lcd_panel_handle_t panel,
                                    ui_background_id_t bg,
                                    const lv_img_dsc_t *img)
{
    if (!panel || !img) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = image_name_from_descriptor(img);
    esp_err_t ret = ensure_native_frame();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!display_direct_prepare_rgb565_native(s_clean_native_frame,
                                              img,
                                              name,
                                              false,
                                              TAG)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t frame_bytes = (size_t)DISPLAY_DIRECT_NATIVE_H_RES *
                               DISPLAY_DIRECT_NATIVE_V_RES *
                               sizeof(uint16_t);
    memcpy(s_native_frame, s_clean_native_frame, frame_bytes);

    if (eez_ui_runtime_direct_overlay_enabled()) {
        eez_ui_runtime_draw_direct_overlay_rgb565_native(s_native_frame,
                                                         DISPLAY_DIRECT_NATIVE_H_RES,
                                                         0,
                                                         0,
                                                         DISPLAY_DIRECT_NATIVE_H_RES,
                                                         DISPLAY_DIRECT_NATIVE_V_RES);
    }

    s_last_panel = panel;

    const display_direct_draw_timing_t timing = {
        .wait_for_refresh = UI_BACKGROUND_DIRECT_WAIT_FOR_REFRESH != 0,
        .sync_mode = UI_BACKGROUND_DIRECT_SYNC_MODE,
        .phase_after_refresh_us = UI_BACKGROUND_DIRECT_PHASE_AFTER_REFRESH_US,
        .lead_before_next_refresh_us = UI_BACKGROUND_DIRECT_LEAD_BEFORE_NEXT_REFRESH_US,
        .skip_refresh_count = UI_BACKGROUND_DIRECT_SKIP_REFRESH_COUNT,
        .wait_timeout_ms = UI_BACKGROUND_DIRECT_WAIT_TIMEOUT_MS,
        .log_timing = UI_BACKGROUND_DIRECT_LOG_TIMING != 0,
    };
    display_direct_draw_result_t result = { 0 };

    ret = display_direct_draw_fullscreen_rgb565_ex(panel,
                                                   s_native_frame,
                                                   &timing,
                                                   TAG,
                                                   &result);

    ESP_LOGI(TAG,
             "draw bg=%d image=%s sync_mode=%d phase_us=%lu lead_us=%lu "
             "skip_refresh_count=%lu wait_timeout_ms=%lu draw_bitmap_us=%lld ret=%s",
             bg,
             name,
             result.sync_mode,
             (unsigned long)result.phase_after_refresh_us,
             (unsigned long)result.lead_before_next_refresh_us,
             (unsigned long)result.skip_refresh_count,
             (unsigned long)timing.wait_timeout_ms,
             (long long)result.draw_bitmap_us,
             esp_err_to_name(ret));

    return ret;
}


static esp_err_t ensure_overlay_scratch(size_t pixels)
{
    if (s_overlay_scratch && s_overlay_scratch_pixels >= pixels) {
        return ESP_OK;
    }

    if (s_overlay_scratch) {
        heap_caps_free(s_overlay_scratch);
        s_overlay_scratch = NULL;
        s_overlay_scratch_pixels = 0;
    }

    s_overlay_scratch = heap_caps_malloc(pixels * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM |
                                         MALLOC_CAP_DMA |
                                         MALLOC_CAP_8BIT);
    if (!s_overlay_scratch) {
        ESP_LOGE(TAG,
                 "Failed to allocate overlay scratch pixels=%lu bytes=%lu",
                 (unsigned long)pixels,
                 (unsigned long)(pixels * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    s_overlay_scratch_pixels = pixels;
    ESP_LOGI(TAG,
             "Allocated overlay scratch pixels=%lu bytes=%lu ptr=%p",
             (unsigned long)pixels,
             (unsigned long)(pixels * sizeof(uint16_t)),
             s_overlay_scratch);
    return ESP_OK;
}

static void copy_region_from_clean(uint16_t *dst,
                                   int dst_stride,
                                   const uint16_t *src,
                                   int src_stride,
                                   int x,
                                   int y,
                                   int w,
                                   int h)
{
    for (int row = 0; row < h; row++) {
        memcpy(&dst[(size_t)row * (size_t)dst_stride],
               &src[(size_t)(y + row) * (size_t)src_stride + (size_t)x],
               (size_t)w * sizeof(uint16_t));
    }
}

esp_err_t ui_background_redraw_runtime_overlay_regions(void)
{
    if (s_runtime_overlay_suspended) {
        return ESP_OK;
    }

    const int64_t now_us = esp_timer_get_time();
    if (s_runtime_overlay_retry_after_us > 0 && now_us < s_runtime_overlay_retry_after_us) {
        return ESP_OK;
    }

    /* LVGL timers can ask for the FPS/Wi-Fi overlay while the previous native
     * rectangle draw is still in flight.  The ESP32-P4 DSI/DPI driver rejects
     * that with ESP_ERR_INVALID_STATE ("previous draw operation is not
     * finished").  Do not pile up retries: skip this tick and try again after
     * a short cooldown, otherwise OTA/flash work can turn this into a visible
     * blue-background flicker and eventually a task-WDT storm. */
    if (s_runtime_overlay_busy) {
        return ESP_OK;
    }

    if (!s_last_panel || !s_clean_native_frame || !eez_ui_runtime_direct_overlay_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime_overlay_busy = true;

    eez_runtime_overlay_region_t regions[4];
    const size_t region_count = eez_ui_runtime_get_direct_overlay_regions(regions,
                                                                          sizeof(regions) / sizeof(regions[0]));
    esp_err_t final_ret = ESP_OK;

    for (size_t i = 0; i < region_count; i++) {
        eez_runtime_overlay_region_t r = regions[i];
        if (r.w <= 0 || r.h <= 0) {
            continue;
        }
        if (r.x < 0) {
            r.w += r.x;
            r.x = 0;
        }
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.x + r.w > DISPLAY_DIRECT_NATIVE_H_RES) {
            r.w = DISPLAY_DIRECT_NATIVE_H_RES - r.x;
        }
        if (r.y + r.h > DISPLAY_DIRECT_NATIVE_V_RES) {
            r.h = DISPLAY_DIRECT_NATIVE_V_RES - r.y;
        }
        if (r.w <= 0 || r.h <= 0) {
            continue;
        }

        const size_t pixels = (size_t)r.w * (size_t)r.h;
        esp_err_t ret = ensure_overlay_scratch(pixels);
        if (ret != ESP_OK) {
            final_ret = ret;
            continue;
        }

        copy_region_from_clean(s_overlay_scratch,
                               r.w,
                               s_clean_native_frame,
                               DISPLAY_DIRECT_NATIVE_H_RES,
                               r.x,
                               r.y,
                               r.w,
                               r.h);

        eez_ui_runtime_draw_direct_overlay_rgb565_native(s_overlay_scratch,
                                                         r.w,
                                                         r.x,
                                                         r.y,
                                                         r.w,
                                                         r.h);

        ret = esp_cache_msync(s_overlay_scratch,
                              pixels * sizeof(uint16_t),
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                              ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_cache_msync overlay region failed: %s", esp_err_to_name(ret));
        }

        ret = esp_lcd_panel_draw_bitmap(s_last_panel,
                                        r.x,
                                        r.y,
                                        r.x + r.w,
                                        r.y + r.h,
                                        s_overlay_scratch);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Overlay region draw failed region=%lu %d,%d %dx%d: %s",
                     (unsigned long)i,
                     r.x,
                     r.y,
                     r.w,
                     r.h,
                     esp_err_to_name(ret));
            final_ret = ret;

            if (ret == ESP_ERR_INVALID_STATE) {
                s_runtime_overlay_retry_after_us = esp_timer_get_time() +
                                                   UI_BG_OVERLAY_RETRY_COOLDOWN_US;
                break;
            }
        }
    }

    s_runtime_overlay_busy = false;
    return final_ret;
}
