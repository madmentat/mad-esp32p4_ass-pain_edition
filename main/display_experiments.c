#include "display_experiments.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display_direct_timing.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "images.h"

static const char *TAG = "DISP_EXP";

#define DISPLAY_EXPERIMENT_MAYBE_UNUSED __attribute__((unused))

static display_experiment_backlight_set_cb_t s_backlight_set_cb;
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

void display_experiment_set_backlight_cb(display_experiment_backlight_set_cb_t cb)
{
    s_backlight_set_cb = cb;
}

void display_experiment_backlight_blank_before_switch(void)
{
#if DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_ON_EEZ_SWITCH
    if (s_backlight_set_cb) {
        (void)s_backlight_set_cb(DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_MS));
    }
#endif
}

void display_experiment_backlight_restore_after_switch(void)
{
#if DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_ON_EEZ_SWITCH
    if (s_backlight_set_cb) {
        (void)s_backlight_set_cb(DISPLAY_EXPERIMENT_BACKLIGHT_RESTORE_LEVEL);
    }
#endif
}

IRAM_ATTR static bool DISPLAY_EXPERIMENT_MAYBE_UNUSED
on_color_trans_done_cb(esp_lcd_panel_handle_t panel,
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

IRAM_ATTR static bool DISPLAY_EXPERIMENT_MAYBE_UNUSED
on_refresh_done_cb(esp_lcd_panel_handle_t panel,
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

esp_err_t display_experiment_register_panel_callbacks(esp_lcd_panel_handle_t panel)
{
    return display_direct_register_panel_callbacks(panel);
}

void display_experiment_log_callback_stats(const char *stage)
{
    display_direct_log_callback_stats(stage);
}

typedef struct {
    bool hit;
    uint32_t before;
    uint32_t after;
    int64_t elapsed_us;
    int64_t event_us;
} display_experiment_wait_result_t;

static display_experiment_wait_result_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
wait_for_counter_advance(volatile uint32_t *counter,
                         volatile int64_t *last_event_us,
                         uint32_t before,
                         uint32_t timeout_ms)
{
    display_experiment_wait_result_t result = {
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

typedef struct {
    uint32_t sync_mode;
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
} display_experiment_direct_sync_t;

static uint32_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
display_experiment_phase_for_frame(uint32_t frame, uint32_t *step_index)
{
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_ENABLE
    const uint32_t start_us = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_START_US;
    const uint32_t end_us = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_END_US;
    const uint32_t step_us = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_STEP_US ?
                             DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_STEP_US : 1U;
    const uint32_t frames_per_step = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP ?
                                     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP : 1U;
    const uint32_t span_us = end_us >= start_us ? end_us - start_us : 0U;
    const uint32_t step_count = (span_us / step_us) + 1U;
    const uint32_t idx = (frame / frames_per_step) % step_count;

    if (step_index) {
        *step_index = idx;
    }

    return start_us + (idx * step_us);
#else
    (void)frame;
    if (step_index) {
        *step_index = 0;
    }
    return DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_AFTER_REFRESH_US;
#endif
}

static void DISPLAY_EXPERIMENT_MAYBE_UNUSED
delay_direct_us(uint32_t delay_us)
{
    if (delay_us > 0) {
        esp_rom_delay_us(delay_us);
    }
}

static display_experiment_wait_result_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
wait_next_refresh_from_current(void)
{
    const uint32_t refresh_before = s_refresh_done_count;

    return wait_for_counter_advance(&s_refresh_done_count,
                                    &s_last_refresh_done_us,
                                    refresh_before,
                                    DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_TIMEOUT_MS);
}

static display_experiment_direct_sync_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
display_experiment_wait_before_direct_draw(uint32_t frame, const char *mode, const char *name)
{
    display_experiment_direct_sync_t sync = {
        .sync_mode = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE,
        .phase_us = 0,
        .lead_us = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_LEAD_BEFORE_NEXT_REFRESH_US,
        .skip_refresh_count = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SKIP_REFRESH_COUNT,
        .refresh_counter_before_wait = s_refresh_done_count,
        .refresh_counter_after_wait = s_refresh_done_count,
        .t_before_wait_us = esp_timer_get_time(),
        .t_after_wait_us = 0,
        .wait_us = 0,
        .refresh_interval_ema_us = s_refresh_interval_ema_us,
        .predicted_delay_us = 0,
        .pre_refresh_hit = false,
    };
    uint32_t step_index = 0;
    sync.phase_us = display_experiment_phase_for_frame(frame, &step_index);

#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_ENABLE
    const uint32_t frames_per_step = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP ?
                                     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP : 1U;
    if ((frame % frames_per_step) == 0) {
        ESP_LOGI(TAG,
                 "PHASE_SWEEP: phase_us=%lu step_index=%lu frames_per_step=%lu",
                 (unsigned long)sync.phase_us,
                 (unsigned long)step_index,
                 (unsigned long)frames_per_step);
    }
    ESP_LOGI(TAG,
             "phase-sweep frame=%lu phase_us=%lu image=%s mode=%s",
             (unsigned long)frame,
             (unsigned long)sync.phase_us,
             name,
             mode);
#endif

    switch (sync.sync_mode) {
    case 0:
        break;

    case 1: {
        display_experiment_wait_result_t refresh = wait_next_refresh_from_current();
        sync.pre_refresh_hit = refresh.hit;
        break;
    }

    case 2: {
        display_experiment_wait_result_t refresh = wait_next_refresh_from_current();
        sync.pre_refresh_hit = refresh.hit;
        delay_direct_us(sync.phase_us);
        break;
    }

    case 3: {
        display_experiment_wait_result_t refresh = wait_next_refresh_from_current();
        sync.pre_refresh_hit = refresh.hit;
        for (uint32_t i = 0; i < sync.skip_refresh_count; i++) {
            refresh = wait_next_refresh_from_current();
            sync.pre_refresh_hit = sync.pre_refresh_hit && refresh.hit;
        }
        delay_direct_us(sync.phase_us);
        break;
    }

    case 4: {
        display_experiment_wait_result_t refresh = wait_next_refresh_from_current();
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
        ESP_LOGW(TAG, "Unknown direct sync mode %lu; drawing without pre-sync",
                 (unsigned long)sync.sync_mode);
        sync.sync_mode = 0;
        break;
    }

    sync.t_after_wait_us = esp_timer_get_time();
    sync.wait_us = sync.t_after_wait_us - sync.t_before_wait_us;
    sync.refresh_counter_after_wait = s_refresh_done_count;
    sync.refresh_interval_ema_us = s_refresh_interval_ema_us;

    return sync;
}

void display_experiment_log_panel_framebuffers(esp_lcd_panel_handle_t panel)
{
    display_direct_log_panel_framebuffers(panel,
                                          DISPLAY_DIRECT_NATIVE_H_RES,
                                          DISPLAY_DIRECT_NATIVE_V_RES);
}

lv_display_render_mode_t display_experiment_lvgl_render_mode(void)
{
#if DISPLAY_EXPERIMENT_LVGL_RENDER_MODE == DISPLAY_EXPERIMENT_LVGL_RENDER_FULL
    return LV_DISPLAY_RENDER_MODE_FULL;
#elif DISPLAY_EXPERIMENT_LVGL_RENDER_MODE == DISPLAY_EXPERIMENT_LVGL_RENDER_DIRECT
    return LV_DISPLAY_RENDER_MODE_DIRECT;
#else
    return LV_DISPLAY_RENDER_MODE_PARTIAL;
#endif
}

const char *display_experiment_lvgl_render_mode_name(void)
{
    switch (display_experiment_lvgl_render_mode()) {
    case LV_DISPLAY_RENDER_MODE_FULL:
        return "FULL";
    case LV_DISPLAY_RENDER_MODE_DIRECT:
        return "DIRECT";
    case LV_DISPLAY_RENDER_MODE_PARTIAL:
    default:
        return "PARTIAL";
    }
}

size_t display_experiment_lvgl_min_buffer_pixels(uint16_t h_res, uint16_t v_res)
{
    if (display_experiment_lvgl_render_mode() == LV_DISPLAY_RENDER_MODE_PARTIAL) {
        return 0;
    }

    return (size_t)h_res * (size_t)v_res;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

static void DISPLAY_EXPERIMENT_MAYBE_UNUSED
fill_synthetic_frame(uint16_t *dst, uint16_t w, uint16_t h, bool variant_b)
{
    const uint16_t red = rgb565(240, 20, 20);
    const uint16_t green = rgb565(20, 210, 80);
    const uint16_t blue = rgb565(30, 80, 240);
    const uint16_t yellow = rgb565(255, 220, 20);
    const uint16_t cyan = rgb565(20, 220, 230);
    const uint16_t magenta = rgb565(230, 40, 220);
    const uint16_t white = rgb565(255, 255, 255);
    const uint16_t black = rgb565(0, 0, 0);

    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            uint16_t c;

            if (!variant_b) {
                if (x < w / 4) {
                    c = ((x / 16U) & 1U) ? red : blue;
                } else if (x < w / 2) {
                    c = ((y / 12U) & 1U) ? green : black;
                } else if (x < (w * 3U) / 4U) {
                    c = (((x + y) / 14U) & 1U) ? yellow : magenta;
                } else {
                    c = (((x / 24U) ^ (y / 24U)) & 1U) ? white : cyan;
                }
            } else {
                if (y < h / 4) {
                    c = ((x / 10U) & 1U) ? cyan : black;
                } else if (y < h / 2) {
                    c = ((y / 16U) & 1U) ? magenta : yellow;
                } else if (y < (h * 3U) / 4U) {
                    c = (((w - x + y) / 12U) & 1U) ? red : green;
                } else {
                    c = (((x / 18U) ^ (y / 18U)) & 1U) ? blue : white;
                }
            }

            if ((uint16_t)((x + y) % 97U) < 3U) {
                c = variant_b ? black : white;
            }

            dst[(size_t)y * w + x] = c;
        }
    }
}

static esp_err_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
draw_full_frame(esp_lcd_panel_handle_t panel,
                uint16_t w,
                uint16_t h,
                const void *pixels,
                const char *mode,
                uint32_t frame,
                const char *name)
{
    if (w != DISPLAY_DIRECT_NATIVE_H_RES || h != DISPLAY_DIRECT_NATIVE_V_RES || !pixels) {
        ESP_LOGE(TAG,
                 "%s frame=%lu image=%s invalid direct frame: %ux%u expected %ux%u pixels=%p",
                 mode,
                 (unsigned long)frame,
                 name,
                 w,
                 h,
                 DISPLAY_DIRECT_NATIVE_H_RES,
                 DISPLAY_DIRECT_NATIVE_V_RES,
                 pixels);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t step_index = 0;
    const uint32_t phase_us = display_experiment_phase_for_frame(frame, &step_index);

#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_ENABLE
    const uint32_t frames_per_step = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP ?
                                     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP : 1U;
    if ((frame % frames_per_step) == 0) {
        ESP_LOGI(TAG,
                 "PHASE_SWEEP: phase_us=%lu step_index=%lu frames_per_step=%lu",
                 (unsigned long)phase_us,
                 (unsigned long)step_index,
                 (unsigned long)frames_per_step);
    }
    ESP_LOGI(TAG,
             "phase-sweep frame=%lu phase_us=%lu image=%s mode=%s",
             (unsigned long)frame,
             (unsigned long)phase_us,
             name,
             mode);
#endif

    const display_direct_draw_timing_t timing = {
        .wait_for_refresh = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH != 0,
        .sync_mode = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE,
        .phase_after_refresh_us = phase_us,
        .lead_before_next_refresh_us = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_LEAD_BEFORE_NEXT_REFRESH_US,
        .skip_refresh_count = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SKIP_REFRESH_COUNT,
        .wait_timeout_ms = DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_TIMEOUT_MS,
        .log_timing = true,
    };
    display_direct_draw_result_t result = { 0 };
    esp_err_t ret = display_direct_draw_fullscreen_rgb565_ex(panel,
                                                             pixels,
                                                             &timing,
                                                             mode,
                                                             &result);

    ESP_LOGI(TAG,
             "%s frame=%lu image=%s bytes=%lu sync_mode=%d phase_us=%lu lead_us=%lu "
             "skip_refresh_count=%lu refresh_before=%lu refresh_after=%lu "
             "pre_refresh_hit=%d wait_before_draw_us=%lld predicted_delay_us=%lld "
             "draw_bitmap_us=%lld post_color_hit=%d post_color_wait_us=%lld "
             "post_refresh_hit=%d post_refresh_wait_us=%lld "
             "refresh_interval_ema_us=%lld ret=%s",
             mode,
             (unsigned long)frame,
             name,
             (unsigned long)((size_t)w * h * 2U),
             result.sync_mode,
             (unsigned long)result.phase_after_refresh_us,
             (unsigned long)result.lead_before_next_refresh_us,
             (unsigned long)result.skip_refresh_count,
             (unsigned long)result.refresh_counter_before_wait,
             (unsigned long)result.refresh_counter_after_wait,
             result.pre_refresh_hit ? 1 : 0,
             (long long)result.wait_before_draw_us,
             (long long)result.predicted_delay_us,
             (long long)result.draw_bitmap_us,
             result.post_color_hit ? 1 : 0,
             (long long)result.post_color_wait_us,
             result.post_refresh_hit ? 1 : 0,
             (long long)result.post_refresh_wait_us,
             (long long)result.refresh_interval_ema_us,
             esp_err_to_name(ret));

    display_experiment_log_callback_stats(mode);

    return ret;
}

void display_experiment_run_direct_fullscreen_synthetic(esp_lcd_panel_handle_t panel)
{
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNTHETIC
    const uint16_t w = 480;
    const uint16_t h = 800;
    const size_t bytes = (size_t)w * h * sizeof(uint16_t);

    ESP_LOGW(TAG, "DIRECT FULLSCREEN SYNTHETIC test enabled; LVGL/EEZ will not start");
    display_experiment_log_panel_framebuffers(panel);

    uint16_t *frame_a = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint16_t *frame_b = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    if (!frame_a || !frame_b) {
        ESP_LOGE(TAG, "Failed to allocate synthetic frames: A=%p B=%p bytes_each=%lu",
                 frame_a,
                 frame_b,
                 (unsigned long)bytes);
        return;
    }

    fill_synthetic_frame(frame_a, w, h, false);
    fill_synthetic_frame(frame_b, w, h, true);
    esp_cache_msync(frame_a, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(frame_b, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    uint32_t frame = 0;
    while (1) {
        const bool use_b = (frame & 1U) != 0U;
        (void)draw_full_frame(panel,
                              w,
                              h,
                              use_b ? frame_b : frame_a,
                              "direct-synthetic",
                              frame,
                              use_b ? "B" : "A");
        frame++;
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_DELAY_MS));
    }
#else
    (void)panel;
#endif
}

static void DISPLAY_EXPERIMENT_MAYBE_UNUSED
fill_solid_frame(uint16_t *dst, uint16_t w, uint16_t h, uint16_t color)
{
    const size_t pixels = (size_t)w * h;

    for (size_t i = 0; i < pixels; i++) {
        dst[i] = color;
    }
}

void display_experiment_run_direct_fullscreen_solid_colors(esp_lcd_panel_handle_t panel)
{
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SOLID_COLORS
    const uint16_t w = 480;
    const uint16_t h = 800;
    const size_t bytes = (size_t)w * h * sizeof(uint16_t);

    typedef struct {
        const char *name;
        uint16_t color;
        uint16_t *pixels;
    } solid_frame_t;

    solid_frame_t frames[] = {
        { "red", rgb565(255, 0, 0), NULL },
        { "green", rgb565(0, 255, 0), NULL },
        { "black", rgb565(0, 0, 0), NULL },
        { "white", rgb565(255, 255, 255), NULL },
    };

    ESP_LOGW(TAG, "DIRECT FULLSCREEN SOLID COLORS test enabled; LVGL/EEZ will not start");
    display_experiment_log_panel_framebuffers(panel);

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        frames[i].pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!frames[i].pixels) {
            ESP_LOGE(TAG, "Failed to allocate solid frame %s bytes=%lu",
                     frames[i].name,
                     (unsigned long)bytes);
            return;
        }

        fill_solid_frame(frames[i].pixels, w, h, frames[i].color);
        esp_cache_msync(frames[i].pixels,
                        bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    uint32_t frame = 0;
    while (1) {
        solid_frame_t *solid = &frames[frame % (sizeof(frames) / sizeof(frames[0]))];
        (void)draw_full_frame(panel,
                              w,
                              h,
                              solid->pixels,
                              "direct-solid",
                              frame,
                              solid->name);
        frame++;
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_DELAY_MS));
    }
#else
    (void)panel;
#endif
}

static bool DISPLAY_EXPERIMENT_MAYBE_UNUSED
eez_image_is_direct_compatible(const lv_img_dsc_t *img)
{
    return img &&
           img->header.cf == LV_COLOR_FORMAT_RGB565 &&
           img->header.flags == 0 &&
           img->data != NULL;
}

static void DISPLAY_EXPERIMENT_MAYBE_UNUSED
log_eez_image_info(const char *name,
                   const lv_img_dsc_t *img,
                   uint16_t native_w,
                   uint16_t native_h)
{
    if (!img) {
        ESP_LOGE(TAG, "EEZ image %s: descriptor is NULL", name);
        return;
    }

    const size_t expected_image_bytes =
        (size_t)img->header.w * (size_t)img->header.h * sizeof(uint16_t);
    const size_t expected_native_bytes = (size_t)native_w * native_h * sizeof(uint16_t);

    ESP_LOGI(TAG,
             "EEZ image %s: width=%lu height=%lu stride=%lu data_size=%lu "
             "expected_image_bytes=%lu expected_native_bytes=%lu color_format=%lu flags=0x%lx data=%p",
             name,
             (unsigned long)img->header.w,
             (unsigned long)img->header.h,
             (unsigned long)img->header.stride,
             (unsigned long)img->data_size,
             (unsigned long)expected_image_bytes,
             (unsigned long)expected_native_bytes,
             (unsigned long)img->header.cf,
             (unsigned long)img->header.flags,
             img->data);
}

static size_t DISPLAY_EXPERIMENT_MAYBE_UNUSED
count_pixels_with_color(const uint16_t *pixels, size_t count, uint16_t color)
{
    size_t matches = 0;

    for (size_t i = 0; i < count; i++) {
        if (pixels[i] == color) {
            matches++;
        }
    }

    return matches;
}

static bool DISPLAY_EXPERIMENT_MAYBE_UNUSED
copy_or_rotate_eez_image(uint16_t *dst,
                         uint16_t dst_w,
                         uint16_t dst_h,
                         const lv_img_dsc_t *img,
                         const char *name)
{
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
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ_NO_ROTATION_ONLY
        ESP_LOGE(TAG,
                 "EEZ image %s requires rotation (%ux%u -> %ux%u), but NO_ROTATION_ONLY is enabled",
                 name,
                 src_w,
                 src_h,
                 dst_w,
                 dst_h);
        return false;
#else
        for (uint16_t y = 0; y < src_h; y++) {
            for (uint16_t x = 0; x < src_w; x++) {
                const uint16_t dx = y;
                const uint16_t dy = (uint16_t)(src_w - 1U - x);
                if (dx < dst_w && dy < dst_h) {
                    dst[(size_t)dy * dst_w + dx] = src[(size_t)y * src_w + x];
                    copied_pixels++;
                }
            }
        }
        copy_mode = "rotate-90ccw-to-native";
#endif
    } else {
        ESP_LOGE(TAG,
                 "EEZ image %s has unsupported dimensions %ux%u for native %ux%u",
                 name,
                 src_w,
                 src_h,
                 dst_w,
                 dst_h);
        return false;
    }

    const size_t debug_pixels_left = count_pixels_with_color(dst, dst_pixels, debug_color);

    ESP_LOGI(TAG,
             "EEZ image %s prepared: mode=%s copied_pixels=%lu/%lu debug_magenta_pixels_left=%lu",
             name,
             copy_mode,
             (unsigned long)copied_pixels,
             (unsigned long)dst_pixels,
             (unsigned long)debug_pixels_left);

    if (debug_pixels_left != 0) {
        ESP_LOGW(TAG,
                 "EEZ image %s left %lu debug-magenta pixels; this may indicate copy bounds trouble "
                 "or real source pixels equal to the debug color",
                 name,
                 (unsigned long)debug_pixels_left);
    }

    return true;
}

void display_experiment_run_direct_fullscreen_eez(esp_lcd_panel_handle_t panel)
{
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ
    const uint16_t w = 480;
    const uint16_t h = 800;
    const size_t bytes = (size_t)w * h * sizeof(uint16_t);
    const lv_img_dsc_t *image_a = &img_lil_2;
    const lv_img_dsc_t *image_b = &img_lawson_2;

    ESP_LOGW(TAG, "DIRECT FULLSCREEN EEZ image test enabled; LVGL/EEZ will not start");
    display_experiment_log_panel_framebuffers(panel);
    log_eez_image_info("Lil_2", image_a, w, h);
    log_eez_image_info("Lawson_2", image_b, w, h);

    if (!display_direct_image_is_rgb565_plain(image_a) ||
        !display_direct_image_is_rgb565_plain(image_b)) {
        ESP_LOGE(TAG, "EEZ image descriptors are not direct RGB565/flags=0 compatible");
        return;
    }

    if (!((image_a->header.w == w && image_a->header.h == h) ||
          (image_a->header.w == h && image_a->header.h == w)) ||
        !((image_b->header.w == w && image_b->header.h == h) ||
          (image_b->header.w == h && image_b->header.h == w))) {
        ESP_LOGE(TAG,
                 "Unsupported EEZ dimensions: A=%lux%lu B=%lux%lu native=%ux%u",
                 (unsigned long)image_a->header.w,
                 (unsigned long)image_a->header.h,
                 (unsigned long)image_b->header.w,
                 (unsigned long)image_b->header.h,
                 w,
                 h);
        return;
    }

    uint16_t *frame_a = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    uint16_t *frame_b = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    if (!frame_a || !frame_b) {
        ESP_LOGE(TAG, "Failed to allocate EEZ direct frames: A=%p B=%p bytes_each=%lu",
                 frame_a,
                 frame_b,
                 (unsigned long)bytes);
        return;
    }

    if (!display_direct_prepare_rgb565_native(frame_a,
                                              image_a,
                                              "Lil_2",
                                              DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ_NO_ROTATION_ONLY != 0,
                                              TAG) ||
        !display_direct_prepare_rgb565_native(frame_b,
                                              image_b,
                                              "Lawson_2",
                                              DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ_NO_ROTATION_ONLY != 0,
                                              TAG)) {
        ESP_LOGE(TAG, "Failed to prepare EEZ direct output buffers");
        return;
    }

    esp_cache_msync(frame_a, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(frame_b, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    ESP_LOGI(TAG,
             "EEZ direct images ready: A=Lil_2 %lux%lu B=Lawson_2 %lux%lu native=%ux%u",
             (unsigned long)image_a->header.w,
             (unsigned long)image_a->header.h,
             (unsigned long)image_b->header.w,
             (unsigned long)image_b->header.h,
             w,
             h);

    uint32_t frame = 0;
    while (1) {
        const bool use_b = (frame & 1U) != 0U;
        (void)draw_full_frame(panel,
                              w,
                              h,
                              use_b ? frame_b : frame_a,
                              "direct-eez",
                              frame,
                              use_b ? "Lawson_2" : "Lil_2");
        frame++;
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_DELAY_MS));
    }
#else
    (void)panel;
#endif
}
