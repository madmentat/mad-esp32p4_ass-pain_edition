#include "display_direct_timing.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "p4_project_config.h"

static const char *TAG = "DIRECT_DRAW";

static volatile uint32_t s_color_trans_done_count;
static volatile uint32_t s_refresh_done_count;
static volatile int64_t s_last_color_trans_done_us;
static volatile int64_t s_last_refresh_done_us;
static volatile int64_t s_prev_refresh_done_us;
static volatile int64_t s_last_refresh_interval_us;
static volatile int64_t s_refresh_interval_sum_us;
static volatile int64_t s_refresh_interval_ema_us;
static volatile int64_t s_refresh_interval_min_us;
static volatile int64_t s_refresh_interval_max_us;
static volatile uint32_t s_refresh_interval_count;

typedef struct {
    bool hit;
    uint32_t before;
    uint32_t after;
    int64_t elapsed_us;
    int64_t event_us;
} display_direct_wait_result_t;

typedef struct {
    int sync_mode;
    uint32_t phase_us;
    uint32_t lead_us;
    uint32_t skip_refresh_count;
    uint32_t refresh_counter_before_wait;
    uint32_t refresh_counter_after_wait;
    int64_t t_before_wait_us;
    int64_t t_after_wait_us;
    int64_t wait_us;
    int64_t refresh_interval_ema_us;
    int64_t predicted_delay_us;
    bool pre_refresh_hit;
} display_direct_sync_result_t;

IRAM_ATTR static bool on_color_trans_done_cb(esp_lcd_panel_handle_t panel,
                                             esp_lcd_dpi_panel_event_data_t *edata,
                                             void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    s_color_trans_done_count++;
    s_last_color_trans_done_us = esp_timer_get_time();

    return false;
}

IRAM_ATTR static bool on_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                         esp_lcd_dpi_panel_event_data_t *edata,
                                         void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    const int64_t now_us = esp_timer_get_time();

    s_refresh_done_count++;
    if (s_prev_refresh_done_us != 0) {
        const int64_t interval_us = now_us - s_prev_refresh_done_us;
        s_last_refresh_interval_us = interval_us;
        s_refresh_interval_sum_us += interval_us;
        s_refresh_interval_count++;

        if (s_refresh_interval_min_us == 0 || interval_us < s_refresh_interval_min_us) {
            s_refresh_interval_min_us = interval_us;
        }
        if (interval_us > s_refresh_interval_max_us) {
            s_refresh_interval_max_us = interval_us;
        }
        if (s_refresh_interval_ema_us == 0) {
            s_refresh_interval_ema_us = interval_us;
        } else {
            s_refresh_interval_ema_us = ((s_refresh_interval_ema_us * 7) + interval_us) / 8;
        }
    }

    s_prev_refresh_done_us = now_us;
    s_last_refresh_done_us = now_us;

    return false;
}

esp_err_t display_direct_register_panel_callbacks(esp_lcd_panel_handle_t panel)
{
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done_cb,
        .on_refresh_done = on_refresh_done_cb,
    };

    esp_err_t ret = esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Registered DPI callbacks: on_color_trans_done + on_refresh_done");
    } else {
        ESP_LOGW(TAG, "DPI callback registration failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

void display_direct_log_panel_framebuffers(esp_lcd_panel_handle_t panel,
                                           uint16_t h_res,
                                           uint16_t v_res)
{
    void *fb0 = NULL;
    void *fb1 = NULL;
    const size_t expected_bytes = (size_t)h_res * v_res * sizeof(uint16_t);
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1);

    ESP_LOGI(TAG,
             "esp_lcd_dpi_panel_get_frame_buffer(2): ret=%s fb0=%p fb1=%p expected_bytes_each=%lu",
             esp_err_to_name(ret),
             fb0,
             fb1,
             (unsigned long)expected_bytes);
}

display_direct_callback_stats_t display_direct_get_callback_stats(void)
{
    const uint32_t interval_count = s_refresh_interval_count;
    display_direct_callback_stats_t stats = {
        .color_trans_done_count = s_color_trans_done_count,
        .refresh_done_count = s_refresh_done_count,
        .last_color_trans_done_us = s_last_color_trans_done_us,
        .last_refresh_done_us = s_last_refresh_done_us,
        .last_refresh_interval_us = s_last_refresh_interval_us,
        .refresh_interval_avg_us = interval_count > 0 ? s_refresh_interval_sum_us / interval_count : 0,
        .refresh_interval_ema_us = s_refresh_interval_ema_us,
        .refresh_interval_min_us = s_refresh_interval_min_us,
        .refresh_interval_max_us = s_refresh_interval_max_us,
        .refresh_interval_count = interval_count,
    };

    return stats;
}

void display_direct_log_callback_stats(const char *stage)
{
    const display_direct_callback_stats_t stats = display_direct_get_callback_stats();

    ESP_LOGI(TAG,
             "Callback stats %s: color_trans_done=%lu refresh_done=%lu last_color_us=%lld last_refresh_us=%lld",
             stage ? stage : "",
             (unsigned long)stats.color_trans_done_count,
             (unsigned long)stats.refresh_done_count,
             (long long)stats.last_color_trans_done_us,
             (long long)stats.last_refresh_done_us);
    ESP_LOGI(TAG,
             "refresh timing: last=%lld us avg=%lld us ema=%lld us min=%lld max=%lld count=%lu",
             (long long)stats.last_refresh_interval_us,
             (long long)stats.refresh_interval_avg_us,
             (long long)stats.refresh_interval_ema_us,
             (long long)stats.refresh_interval_min_us,
             (long long)stats.refresh_interval_max_us,
             (unsigned long)stats.refresh_interval_count);
}

static display_direct_wait_result_t wait_for_counter_advance(volatile uint32_t *counter,
                                                             volatile int64_t *last_event_us,
                                                             uint32_t before,
                                                             uint32_t timeout_ms)
{
    display_direct_wait_result_t result = {
        .hit = false,
        .before = before,
        .after = before,
        .elapsed_us = 0,
        .event_us = 0,
    };
    const int64_t t0 = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (*counter == before) {
        if ((esp_timer_get_time() - t0) >= timeout_us) {
            break;
        }
        esp_rom_delay_us(20);
    }

    result.elapsed_us = esp_timer_get_time() - t0;
    result.after = *counter;
    result.hit = result.after != before;
    result.event_us = *last_event_us;

    return result;
}

static void delay_direct_us(uint32_t delay_us)
{
    if (delay_us > 0) {
        esp_rom_delay_us(delay_us);
    }
}

static display_direct_wait_result_t wait_next_refresh_from_current(uint32_t timeout_ms)
{
    const uint32_t refresh_before = s_refresh_done_count;

    return wait_for_counter_advance(&s_refresh_done_count,
                                    &s_last_refresh_done_us,
                                    refresh_before,
                                    timeout_ms);
}

static display_direct_sync_result_t wait_before_direct_draw(const display_direct_draw_timing_t *timing)
{
    const display_direct_draw_timing_t no_sync = {
        .wait_for_refresh = false,
        .sync_mode = 0,
        .phase_after_refresh_us = 0,
        .lead_before_next_refresh_us = 0,
        .skip_refresh_count = 0,
        .wait_timeout_ms = 100,
        .log_timing = false,
    };
    const display_direct_draw_timing_t *cfg = timing ? timing : &no_sync;
    int sync_mode = cfg->sync_mode;

    if (sync_mode < 0) {
        sync_mode = cfg->wait_for_refresh ? 1 : 0;
    }

    display_direct_sync_result_t sync = {
        .sync_mode = sync_mode,
        .phase_us = cfg->phase_after_refresh_us,
        .lead_us = cfg->lead_before_next_refresh_us,
        .skip_refresh_count = cfg->skip_refresh_count,
        .refresh_counter_before_wait = s_refresh_done_count,
        .refresh_counter_after_wait = s_refresh_done_count,
        .t_before_wait_us = esp_timer_get_time(),
        .t_after_wait_us = 0,
        .wait_us = 0,
        .refresh_interval_ema_us = s_refresh_interval_ema_us,
        .predicted_delay_us = 0,
        .pre_refresh_hit = false,
    };

    switch (sync.sync_mode) {
    case 0:
        break;

    case 1: {
        display_direct_wait_result_t refresh = wait_next_refresh_from_current(cfg->wait_timeout_ms);
        sync.pre_refresh_hit = refresh.hit;
        break;
    }

    case 2: {
        display_direct_wait_result_t refresh = wait_next_refresh_from_current(cfg->wait_timeout_ms);
        sync.pre_refresh_hit = refresh.hit;
        delay_direct_us(sync.phase_us);
        break;
    }

    case 3: {
        display_direct_wait_result_t refresh = wait_next_refresh_from_current(cfg->wait_timeout_ms);
        sync.pre_refresh_hit = refresh.hit;
        for (uint32_t i = 0; i < sync.skip_refresh_count; i++) {
            refresh = wait_next_refresh_from_current(cfg->wait_timeout_ms);
            sync.pre_refresh_hit = sync.pre_refresh_hit && refresh.hit;
        }
        delay_direct_us(sync.phase_us);
        break;
    }

    case 4: {
        display_direct_wait_result_t refresh = wait_next_refresh_from_current(cfg->wait_timeout_ms);
        sync.pre_refresh_hit = refresh.hit;

        int64_t period_us = s_refresh_interval_ema_us;
        if (period_us <= 0) {
            period_us = s_last_refresh_interval_us;
        }
        if (period_us <= 0) {
            period_us = 16667;
        }

        int64_t delay_us = period_us - (int64_t)sync.lead_us;
        if (delay_us < 0) {
            delay_us = 0;
        }
        sync.predicted_delay_us = delay_us;
        delay_direct_us((uint32_t)delay_us);
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown direct sync mode %d; drawing without pre-sync", sync.sync_mode);
        sync.sync_mode = 0;
        break;
    }

    sync.t_after_wait_us = esp_timer_get_time();
    sync.wait_us = sync.t_after_wait_us - sync.t_before_wait_us;
    sync.refresh_counter_after_wait = s_refresh_done_count;
    sync.refresh_interval_ema_us = s_refresh_interval_ema_us;

    return sync;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

static void fill_solid_frame(uint16_t *dst, uint16_t w, uint16_t h, uint16_t color)
{
    const size_t pixels = (size_t)w * h;

    for (size_t i = 0; i < pixels; i++) {
        dst[i] = color;
    }
}

static size_t count_pixels_with_color(const uint16_t *pixels, size_t count, uint16_t color)
{
    size_t matches = 0;

    for (size_t i = 0; i < count; i++) {
        if (pixels[i] == color) {
            matches++;
        }
    }

    return matches;
}

bool display_direct_image_is_rgb565_plain(const lv_img_dsc_t *img)
{
    return img &&
           img->header.cf == LV_COLOR_FORMAT_RGB565 &&
           img->header.flags == 0 &&
           img->data != NULL &&
           img->data_size >= (size_t)img->header.w * (size_t)img->header.h * sizeof(uint16_t);
}

bool display_direct_prepare_rgb565_native(uint16_t *dst,
                                          const lv_img_dsc_t *img,
                                          const char *name,
                                          bool no_rotation_only,
                                          const char *tag_name)
{
    const char *log_tag = tag_name ? tag_name : TAG;
    const uint16_t dst_w = DISPLAY_DIRECT_NATIVE_H_RES;
    const uint16_t dst_h = DISPLAY_DIRECT_NATIVE_V_RES;

    if (!dst || !display_direct_image_is_rgb565_plain(img)) {
        ESP_LOGE(log_tag, "RGB565 image %s is not plain/direct-compatible", name ? name : "?");
        return false;
    }

    const uint16_t src_w = (uint16_t)img->header.w;
    const uint16_t src_h = (uint16_t)img->header.h;
    const uint16_t *src = (const uint16_t *)img->data;
    const uint16_t debug_color = rgb565(255, 0, 255);
    const size_t dst_pixels = (size_t)dst_w * dst_h;
    const char *copy_mode = NULL;
    size_t copied_pixels = 0;

    fill_solid_frame(dst, dst_w, dst_h, debug_color);

    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_w * dst_h * sizeof(uint16_t));
        copied_pixels = dst_pixels;
        copy_mode = "native-no-rotation";
    } else if (src_w == dst_h && src_h == dst_w) {
        if (no_rotation_only) {
            ESP_LOGE(log_tag,
                     "RGB565 image %s requires rotation (%ux%u -> %ux%u), but no_rotation_only is enabled",
                     name ? name : "?",
                     src_w,
                     src_h,
                     dst_w,
                     dst_h);
            return false;
        }

        for (uint16_t y = 0; y < src_h; y++) {
            for (uint16_t x = 0; x < src_w; x++) {
                uint16_t dx = y;
                uint16_t dy = (uint16_t)(src_w - 1U - x);

#if P4_DIRECT_BG_MIRROR_Y_AFTER_ROTATE
                dy = (uint16_t)(dst_h - 1U - dy);
#endif
#if P4_DIRECT_BG_MIRROR_X_AFTER_ROTATE
                dx = (uint16_t)(dst_w - 1U - dx);
#endif

                if (dx < dst_w && dy < dst_h) {
                    dst[(size_t)dy * dst_w + dx] = src[(size_t)y * src_w + x];
                    copied_pixels++;
                }
            }
        }
        copy_mode = "rotate-90ccw-to-native";
#if P4_DIRECT_BG_MIRROR_Y_AFTER_ROTATE || P4_DIRECT_BG_MIRROR_X_AFTER_ROTATE
        ESP_LOGI(log_tag,
                 "Direct background mirror after rotate: mirror_y=%d mirror_x=%d",
                 P4_DIRECT_BG_MIRROR_Y_AFTER_ROTATE,
                 P4_DIRECT_BG_MIRROR_X_AFTER_ROTATE);
#endif
    } else {
        ESP_LOGE(log_tag,
                 "RGB565 image %s has unsupported dimensions %ux%u for native %ux%u",
                 name ? name : "?",
                 src_w,
                 src_h,
                 dst_w,
                 dst_h);
        return false;
    }

    const size_t debug_pixels_left = count_pixels_with_color(dst, dst_pixels, debug_color);

    ESP_LOGI(log_tag,
             "RGB565 image %s prepared: mode=%s copied_pixels=%lu/%lu debug_magenta_pixels_left=%lu",
             name ? name : "?",
             copy_mode,
             (unsigned long)copied_pixels,
             (unsigned long)dst_pixels,
             (unsigned long)debug_pixels_left);

    if (debug_pixels_left != 0) {
        ESP_LOGW(log_tag,
                 "RGB565 image %s left %lu debug-magenta pixels; this may indicate copy bounds trouble "
                 "or real source pixels equal to the debug color",
                 name ? name : "?",
                 (unsigned long)debug_pixels_left);
    }

    return true;
}

esp_err_t display_direct_draw_fullscreen_rgb565_ex(
    esp_lcd_panel_handle_t panel,
    const uint16_t *rgb565_native_480x800,
    const display_direct_draw_timing_t *timing,
    const char *tag_name,
    display_direct_draw_result_t *result)
{
    const char *log_tag = tag_name ? tag_name : TAG;
    const display_direct_draw_timing_t default_timing = {
        .wait_for_refresh = false,
        .sync_mode = 0,
        .phase_after_refresh_us = 0,
        .lead_before_next_refresh_us = 0,
        .skip_refresh_count = 0,
        .wait_timeout_ms = 100,
        .log_timing = false,
    };
    const display_direct_draw_timing_t *cfg = timing ? timing : &default_timing;
    const size_t bytes = (size_t)DISPLAY_DIRECT_NATIVE_H_RES *
                         DISPLAY_DIRECT_NATIVE_V_RES *
                         sizeof(uint16_t);

    if (!panel || !rgb565_native_480x800) {
        return ESP_ERR_INVALID_ARG;
    }

    const display_direct_sync_result_t sync = wait_before_direct_draw(cfg);
    const uint32_t color_before_draw = s_color_trans_done_count;
    const uint32_t refresh_before_draw = s_refresh_done_count;

    esp_err_t cache_ret = esp_cache_msync((void *)rgb565_native_480x800,
                                          bytes,
                                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (cache_ret != ESP_OK) {
        ESP_LOGW(log_tag, "esp_cache_msync before direct draw failed: %s", esp_err_to_name(cache_ret));
    }

    const int64_t t0 = esp_timer_get_time();
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel,
                                              0,
                                              0,
                                              DISPLAY_DIRECT_NATIVE_H_RES,
                                              DISPLAY_DIRECT_NATIVE_V_RES,
                                              rgb565_native_480x800);
    const int64_t t1 = esp_timer_get_time();

    display_direct_wait_result_t post_color =
        wait_for_counter_advance(&s_color_trans_done_count,
                                 &s_last_color_trans_done_us,
                                 color_before_draw,
                                 cfg->wait_timeout_ms);
    display_direct_wait_result_t post_refresh =
        wait_for_counter_advance(&s_refresh_done_count,
                                 &s_last_refresh_done_us,
                                 refresh_before_draw,
                                 cfg->wait_timeout_ms);

    if (result) {
        result->sync_mode = sync.sync_mode;
        result->phase_after_refresh_us = sync.phase_us;
        result->lead_before_next_refresh_us = sync.lead_us;
        result->skip_refresh_count = sync.skip_refresh_count;
        result->refresh_counter_before_wait = sync.refresh_counter_before_wait;
        result->refresh_counter_after_wait = sync.refresh_counter_after_wait;
        result->wait_before_draw_us = sync.wait_us;
        result->predicted_delay_us = sync.predicted_delay_us;
        result->draw_bitmap_us = t1 - t0;
        result->post_color_wait_us = post_color.elapsed_us;
        result->post_refresh_wait_us = post_refresh.elapsed_us;
        result->refresh_interval_ema_us = sync.refresh_interval_ema_us;
        result->pre_refresh_hit = sync.pre_refresh_hit;
        result->post_color_hit = post_color.hit;
        result->post_refresh_hit = post_refresh.hit;
        result->ret = ret;
    }

    if (cfg->log_timing) {
        ESP_LOGI(log_tag,
                 "draw fullscreen bytes=%lu sync_mode=%d phase_us=%lu lead_us=%lu "
                 "skip_refresh_count=%lu refresh_before=%lu refresh_after=%lu "
                 "t_before_wait_us=%lld t_draw_start_us=%lld t_draw_end_us=%lld "
                 "pre_refresh_hit=%d wait_before_draw_us=%lld predicted_delay_us=%lld "
                 "draw_bitmap_us=%lld post_color_hit=%d post_color_wait_us=%lld "
                 "post_refresh_hit=%d post_refresh_wait_us=%lld "
                 "refresh_interval_ema_us=%lld ret=%s",
                 (unsigned long)bytes,
                 sync.sync_mode,
                 (unsigned long)sync.phase_us,
                 (unsigned long)sync.lead_us,
                 (unsigned long)sync.skip_refresh_count,
                 (unsigned long)sync.refresh_counter_before_wait,
                 (unsigned long)sync.refresh_counter_after_wait,
                 (long long)sync.t_before_wait_us,
                 (long long)t0,
                 (long long)t1,
                 sync.pre_refresh_hit ? 1 : 0,
                 (long long)sync.wait_us,
                 (long long)sync.predicted_delay_us,
                 (long long)(t1 - t0),
                 post_color.hit ? 1 : 0,
                 (long long)post_color.elapsed_us,
                 post_refresh.hit ? 1 : 0,
                 (long long)post_refresh.elapsed_us,
                 (long long)sync.refresh_interval_ema_us,
                 esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t display_direct_draw_fullscreen_rgb565(
    esp_lcd_panel_handle_t panel,
    const uint16_t *rgb565_native_480x800,
    const display_direct_draw_timing_t *timing,
    const char *tag_name)
{
    return display_direct_draw_fullscreen_rgb565_ex(panel,
                                                    rgb565_native_480x800,
                                                    timing,
                                                    tag_name,
                                                    NULL);
}
