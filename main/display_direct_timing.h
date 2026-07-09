#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_DIRECT_NATIVE_H_RES 480
#define DISPLAY_DIRECT_NATIVE_V_RES 800

typedef struct {
    bool wait_for_refresh;
    int sync_mode;
    uint32_t phase_after_refresh_us;
    uint32_t lead_before_next_refresh_us;
    uint32_t skip_refresh_count;
    uint32_t wait_timeout_ms;
    bool log_timing;
} display_direct_draw_timing_t;

typedef struct {
    uint32_t color_trans_done_count;
    uint32_t refresh_done_count;
    int64_t last_color_trans_done_us;
    int64_t last_refresh_done_us;
    int64_t last_refresh_interval_us;
    int64_t refresh_interval_avg_us;
    int64_t refresh_interval_ema_us;
    int64_t refresh_interval_min_us;
    int64_t refresh_interval_max_us;
    uint32_t refresh_interval_count;
} display_direct_callback_stats_t;

typedef struct {
    int sync_mode;
    uint32_t phase_after_refresh_us;
    uint32_t lead_before_next_refresh_us;
    uint32_t skip_refresh_count;
    uint32_t refresh_counter_before_wait;
    uint32_t refresh_counter_after_wait;
    int64_t wait_before_draw_us;
    int64_t predicted_delay_us;
    int64_t draw_bitmap_us;
    int64_t post_color_wait_us;
    int64_t post_refresh_wait_us;
    int64_t refresh_interval_ema_us;
    bool pre_refresh_hit;
    bool post_color_hit;
    bool post_refresh_hit;
    esp_err_t ret;
} display_direct_draw_result_t;

esp_err_t display_direct_register_panel_callbacks(esp_lcd_panel_handle_t panel);
void display_direct_log_panel_framebuffers(esp_lcd_panel_handle_t panel,
                                           uint16_t h_res,
                                           uint16_t v_res);
void display_direct_log_callback_stats(const char *stage);
display_direct_callback_stats_t display_direct_get_callback_stats(void);

bool display_direct_image_is_rgb565_plain(const lv_img_dsc_t *img);
bool display_direct_prepare_rgb565_native(uint16_t *dst,
                                          const lv_img_dsc_t *img,
                                          const char *name,
                                          bool no_rotation_only,
                                          const char *tag_name);

esp_err_t display_direct_draw_fullscreen_rgb565(
    esp_lcd_panel_handle_t panel,
    const uint16_t *rgb565_native_480x800,
    const display_direct_draw_timing_t *timing,
    const char *tag_name);

esp_err_t display_direct_draw_fullscreen_rgb565_ex(
    esp_lcd_panel_handle_t panel,
    const uint16_t *rgb565_native_480x800,
    const display_direct_draw_timing_t *timing,
    const char *tag_name,
    display_direct_draw_result_t *result);

#ifdef __cplusplus
}
#endif
