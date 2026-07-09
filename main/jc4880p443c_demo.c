/*
 * jc4880p443c_demo.c
 *
 * ESP32-P4 + JC4880P443C display/touch bring-up with EEZ LVGL UI.
 *
 * This version keeps the working ST7701 / MIPI-DSI / GT911 path and adds
 * memory, buffer and flush diagnostics for performance tuning.
 *
 * Keep this file encoded as UTF-8 or plain ASCII.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"

#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"

#include "lvgl.h"

#include "jc4880p443c.h"
#include "display_experiments.h"
#include "demo_backlight.h"
#include "wifi_manager.h"
#include "eez_ui_port.h"
#include "ui_background_direct.h"
#include "update/update_manager.h"
#include "stm32_swd_programmer.h"

static const char *TAG = "DEMO";

/*
 * Hardware config
 */
uint16_t DISPLAY_H_RES = 0;
uint16_t DISPLAY_V_RES = 0;

#define PIN_LCD_RST                  (GPIO_NUM_5)
#define PIN_LCD_BACKLIGHT            (GPIO_NUM_23)
#define PIN_MIPI_PHY_PWR_LDO_CHAN    (3)
#define PIN_MIPI_PHY_PWR_VOLTAGE_MV  (2500)

/*
 * Touch pins
 */
#define PIN_TOUCH_SCL                (GPIO_NUM_8)
#define PIN_TOUCH_SDA                (GPIO_NUM_7)
#define PIN_TOUCH_RST                (GPIO_NUM_22)
#define PIN_TOUCH_INT                (GPIO_NUM_21)
#define TOUCH_I2C_PORT               (I2C_NUM_0)
#define TOUCH_I2C_FREQ_HZ            (400000)

/*
 * Backlight PWM
 */
#define BACKLIGHT_LEDC_CH            (0)
#define BACKLIGHT_LEDC_TIMER         (LEDC_TIMER_1)
#define BACKLIGHT_LEDC_MODE          (LEDC_LOW_SPEED_MODE)
#define BACKLIGHT_PWM_FREQ_HZ        (20000)
#define BACKLIGHT_PWM_RESOLUTION     (LEDC_TIMER_10_BIT)

/*
 * LVGL port
 */
#define LVGL_TICK_PERIOD_MS          (2)
#define LVGL_TASK_MAX_DELAY_MS       (500)
#define LVGL_TASK_MIN_DELAY_MS       (1)
#define LVGL_TASK_SUSPEND_DELAY_MS   (20)
#define LVGL_TASK_STACK_SIZE         (10 * 1024)
#define LVGL_TASK_PRIORITY           (4)
#define LVGL_TASK_CORE               (1)

/*
 * Draw buffer candidates.
 *
 * The code tries large buffers first and falls back to smaller ones.
 * For RGB565:
 *   480 * 320 * 2 = 307200 bytes per buffer.
 * Three buffers are allocated: buf1, buf2, rotate_buf.
 */
#define LVGL_BUFFER_WIDTH            (800)
#define LVGL_BUFFER_LINES_TURBO      (256) //Референс 128 - Увеличение помогло
#define LVGL_BUFFER_LINES_LARGE      (192) //Референс 96 - Увеличение помогло
#define LVGL_BUFFER_LINES_FAST       (8)  //Референс 32 - После уменьшения с 40 до 32 Average draw call time:  666 us (было 870 us) При 16 Average draw call time:  354 us
#define LVGL_BUFFER_LINES_MEDIUM     (32)  //Референс 64
#define LVGL_BUFFER_LINES_SAFE       (32)  //Референс 48
#define LVGL_BUFFER_LINES_MIN        (16)  //Референс 32
#define LVGL_FLUSH_SETTLE_MS         (0)

/*
 * Diagnostics
 */
#define FLUSH_LOG_EVERY_N            (50)

/*
 * A "frame batch" is a group of LVGL flushes that belong to one visible redraw.
 * LVGL does not tell us explicitly "this full screen redraw is finished", so we
 * infer it: a batch starts with the first flush after an idle gap and ends when
 * no flushes have arrived for FRAME_LOG_IDLE_GAP_US.
 */
#define FRAME_LOG_IDLE_GAP_US        (20000)
#define FRAME_LOG_MIN_FLUSHES        (2)
#define FRAME_LOG_MIN_BBOX_PERCENT   (10)

/*
 * Global LVGL buffer state
 */
static uint8_t *s_lvgl_rotate_buf = NULL;
static size_t   s_lvgl_buffer_size_bytes = 0;
static size_t   s_lvgl_buffer_pixels = 0;

static int s_backlight_level_pct = 90;

/*
 * Flush diagnostics
 */
static uint32_t s_flush_counter = 0;
static uint32_t s_flush_busy_counter = 0;
static uint32_t s_flush_error_counter = 0;
static int64_t  s_flush_total_us = 0;
static int64_t  s_flush_max_us = 0;

/*
 * Full redraw / frame-batch diagnostics.
 */
static bool     s_frame_batch_active = false;
static uint32_t s_frame_batch_id = 0;
static uint32_t s_frame_flush_count = 0;
static uint32_t s_frame_busy_retries = 0;
static uint32_t s_frame_errors = 0;
static uint64_t s_frame_pixels = 0;
static uint64_t s_frame_bytes = 0;
static int64_t  s_frame_first_us = 0;
static int64_t  s_frame_last_us = 0;
static int64_t  s_frame_draw_total_us = 0;
static int64_t  s_frame_draw_max_us = 0;
static int32_t  s_frame_x1 = 0;
static int32_t  s_frame_y1 = 0;
static int32_t  s_frame_x2 = 0;
static int32_t  s_frame_y2 = 0;

/*
 * Forward declarations
 */
static void log_memory_status(const char *stage);
static void log_buffer_math(const char *stage);
static void log_flush_stats(const char *stage);

static uint32_t lvgl_display_bytes_per_pixel(lv_display_t *disp);
static void frame_batch_note_flush(lv_display_t *disp, const lv_area_t *area,
                                   uint32_t bytes, int64_t flush_us,
                                   int retry_count, esp_err_t ret,
                                   int64_t t_start_us, int64_t t_end_us);
static void frame_batch_maybe_finish(const char *reason);
static void frame_batch_finish(const char *reason);

static esp_err_t backlight_init(void);
static esp_err_t backlight_set(int pct);
static esp_err_t mipi_phy_power_init(void);

static esp_err_t display_init(esp_lcd_panel_handle_t *ret_panel,
                              esp_lcd_panel_io_handle_t *ret_io);

static esp_err_t touch_init(esp_lcd_touch_handle_t *ret_tp);
static void lvgl_touch_register(esp_lcd_touch_handle_t tp);

static esp_err_t lvgl_tick_init(void);
static void lvgl_tick_cb(void *arg);
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map);
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
static void lvgl_task(void *arg);

static bool lvgl_alloc_draw_buffers(size_t pixels, void **buf1, void **buf2,
                                    uint8_t **rotate_buf);
static lv_display_t *lvgl_display_init(esp_lcd_panel_handle_t panel);

/*
 * Memory diagnostics
 */
static void log_memory_status(const char *stage)
{
    multi_heap_info_t internal_info;
    multi_heap_info_t psram_info;
    multi_heap_info_t dma_info;
    multi_heap_info_t default_info;

    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&dma_info, MALLOC_CAP_DMA);
    heap_caps_get_info(&default_info, MALLOC_CAP_DEFAULT);

    ESP_LOGI("MEM", "========== Memory status: %s ==========", stage);

    ESP_LOGI("MEM", "Total free heap:          %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI("MEM", "Minimum free heap:        %lu bytes",
             (unsigned long)esp_get_minimum_free_heap_size());

    ESP_LOGI("MEM", "Default heap free:        %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    ESP_LOGI("MEM", "Default heap largest:     %lu bytes",
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    ESP_LOGI("MEM", "Internal RAM free:        %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI("MEM", "Internal RAM largest:     %lu bytes",
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    ESP_LOGI("MEM", "DMA-capable free:         %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI("MEM", "DMA-capable largest:      %lu bytes",
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    ESP_LOGI("MEM", "PSRAM free:               %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("MEM", "PSRAM largest:            %lu bytes",
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI("MEM", "Default allocated:        %lu bytes",
             (unsigned long)default_info.total_allocated_bytes);
    ESP_LOGI("MEM", "Internal allocated:       %lu bytes",
             (unsigned long)internal_info.total_allocated_bytes);
    ESP_LOGI("MEM", "PSRAM allocated:          %lu bytes",
             (unsigned long)psram_info.total_allocated_bytes);
    ESP_LOGI("MEM", "DMA allocated:            %lu bytes",
             (unsigned long)dma_info.total_allocated_bytes);

    ESP_LOGI("MEM", "Internal blocks: alloc=%lu free=%lu total=%lu",
             (unsigned long)internal_info.allocated_blocks,
             (unsigned long)internal_info.free_blocks,
             (unsigned long)internal_info.total_blocks);
    ESP_LOGI("MEM", "PSRAM blocks:    alloc=%lu free=%lu total=%lu",
             (unsigned long)psram_info.allocated_blocks,
             (unsigned long)psram_info.free_blocks,
             (unsigned long)psram_info.total_blocks);

    ESP_LOGI("MEM", "========================================");
}

static void log_buffer_math(const char *stage)
{
    uint32_t h = DISPLAY_H_RES ? DISPLAY_H_RES : 480;
    uint32_t v = DISPLAY_V_RES ? DISPLAY_V_RES : 800;

    ESP_LOGI("BUF", "========== Buffer math: %s ==========", stage);
    ESP_LOGI("BUF", "Resolution:              %lux%lu",
             (unsigned long)h, (unsigned long)v);
    ESP_LOGI("BUF", "sizeof(lv_color_t):      %lu bytes",
             (unsigned long)sizeof(lv_color_t));
    ESP_LOGI("BUF", "Full RGB565 frame:       %lu bytes",
             (unsigned long)(h * v * 2));
    ESP_LOGI("BUF", "Full RGB888 frame:       %lu bytes",
             (unsigned long)(h * v * 3));
    ESP_LOGI("BUF", "Full ARGB8888 frame:     %lu bytes",
             (unsigned long)(h * v * 4));

    ESP_LOGI("BUF", "LVGL buffer pixels:      %lu",
             (unsigned long)s_lvgl_buffer_pixels);
    ESP_LOGI("BUF", "LVGL buffer bytes:       %lu",
             (unsigned long)s_lvgl_buffer_size_bytes);
    ESP_LOGI("BUF", "LVGL double buffers:     %lu bytes",
             (unsigned long)(s_lvgl_buffer_size_bytes * 2));
    ESP_LOGI("BUF", "LVGL rotate buffer:      %lu bytes",
             (unsigned long)s_lvgl_buffer_size_bytes);
    ESP_LOGI("BUF", "LVGL buffers total:      %lu bytes",
             (unsigned long)(s_lvgl_buffer_size_bytes * 3));
    ESP_LOGI("BUF", "======================================");
}

static void log_flush_stats(const char *stage)
{
    int64_t avg_us = 0;

    if (s_flush_counter > 0) {
        avg_us = s_flush_total_us / (int64_t)s_flush_counter;
    }

    ESP_LOGI("FLUSH", "========== Flush stats: %s ==========", stage);
    ESP_LOGI("FLUSH", "Flush count:             %lu",
             (unsigned long)s_flush_counter);
    ESP_LOGI("FLUSH", "Busy retries:            %lu",
             (unsigned long)s_flush_busy_counter);
    ESP_LOGI("FLUSH", "Flush errors:            %lu",
             (unsigned long)s_flush_error_counter);
    ESP_LOGI("FLUSH", "Average draw call time:  %lld us",
             (long long)avg_us);
    ESP_LOGI("FLUSH", "Max draw call time:      %lld us",
             (long long)s_flush_max_us);
    ESP_LOGI("FLUSH", "=====================================");
}


/*
 * Return bytes per pixel for the current LVGL display color format.
 *
 * sizeof(lv_color_t) can be misleading in LVGL 9 diagnostics, because the
 * display color format is the source of truth for the draw buffer.
 */
static uint32_t lvgl_display_bytes_per_pixel(lv_display_t *disp)
{
    uint32_t bpp = (uint32_t)lv_color_format_get_size(lv_display_get_color_format(disp));

    if (bpp == 0U) {
        bpp = (uint32_t)sizeof(lv_color_t);
    }

    return bpp;
}

static void frame_batch_reset_for_first_flush(const lv_area_t *area,
                                              int64_t t_start_us)
{
    s_frame_batch_active = true;
    s_frame_batch_id++;

    s_frame_flush_count = 0;
    s_frame_busy_retries = 0;
    s_frame_errors = 0;
    s_frame_pixels = 0;
    s_frame_bytes = 0;
    s_frame_first_us = t_start_us;
    s_frame_last_us = t_start_us;
    s_frame_draw_total_us = 0;
    s_frame_draw_max_us = 0;

    s_frame_x1 = area->x1;
    s_frame_y1 = area->y1;
    s_frame_x2 = area->x2;
    s_frame_y2 = area->y2;
}

static void frame_batch_note_flush(lv_display_t *disp, const lv_area_t *area,
                                   uint32_t bytes, int64_t flush_us,
                                   int retry_count, esp_err_t ret,
                                   int64_t t_start_us, int64_t t_end_us)
{
    if (s_frame_batch_active &&
        s_frame_flush_count > 0U &&
        (t_start_us - s_frame_last_us) > FRAME_LOG_IDLE_GAP_US) {
        frame_batch_finish("gap before next flush");
    }

    if (!s_frame_batch_active) {
        frame_batch_reset_for_first_flush(area, t_start_us);
    }

    const uint32_t w = (uint32_t)lv_area_get_width(area);
    const uint32_t h = (uint32_t)lv_area_get_height(area);

    s_frame_flush_count++;
    s_frame_busy_retries += (uint32_t)retry_count;

    if (ret != ESP_OK) {
        s_frame_errors++;
    }

    s_frame_pixels += (uint64_t)w * (uint64_t)h;
    s_frame_bytes += (uint64_t)bytes;
    s_frame_draw_total_us += flush_us;

    if (flush_us > s_frame_draw_max_us) {
        s_frame_draw_max_us = flush_us;
    }

    if (area->x1 < s_frame_x1) {
        s_frame_x1 = area->x1;
    }

    if (area->y1 < s_frame_y1) {
        s_frame_y1 = area->y1;
    }

    if (area->x2 > s_frame_x2) {
        s_frame_x2 = area->x2;
    }

    if (area->y2 > s_frame_y2) {
        s_frame_y2 = area->y2;
    }

    s_frame_last_us = t_end_us;

    (void)disp;
}

static void frame_batch_maybe_finish(const char *reason)
{
    if (!s_frame_batch_active) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    if ((now_us - s_frame_last_us) >= FRAME_LOG_IDLE_GAP_US) {
        frame_batch_finish(reason);
    }
}

static void frame_batch_finish(const char *reason)
{
    if (!s_frame_batch_active) {
        return;
    }

    const uint32_t screen_w = DISPLAY_H_RES ? DISPLAY_H_RES : 480U;
    const uint32_t screen_h = DISPLAY_V_RES ? DISPLAY_V_RES : 800U;
    const uint64_t screen_pixels = (uint64_t)screen_w * (uint64_t)screen_h;

    const uint32_t bbox_w = (s_frame_x2 >= s_frame_x1)
                                ? (uint32_t)(s_frame_x2 - s_frame_x1 + 1)
                                : 0U;
    const uint32_t bbox_h = (s_frame_y2 >= s_frame_y1)
                                ? (uint32_t)(s_frame_y2 - s_frame_y1 + 1)
                                : 0U;
    const uint64_t bbox_pixels = (uint64_t)bbox_w * (uint64_t)bbox_h;

    const uint32_t bbox_percent = (screen_pixels > 0U)
                                      ? (uint32_t)((bbox_pixels * 100ULL) /
                                                   screen_pixels)
                                      : 0U;
    const uint32_t touched_percent = (screen_pixels > 0U)
                                         ? (uint32_t)((s_frame_pixels * 100ULL) /
                                                      screen_pixels)
                                         : 0U;

    const int64_t span_us = s_frame_last_us - s_frame_first_us;
    const int64_t avg_us = (s_frame_flush_count > 0U)
                               ? (s_frame_draw_total_us /
                                  (int64_t)s_frame_flush_count)
                               : 0;

    /*
     * Avoid spamming logs for tiny widget updates. Full-screen transitions and
     * big container redraws will pass this filter.
     */
    if (s_frame_flush_count >= FRAME_LOG_MIN_FLUSHES ||
        bbox_percent >= FRAME_LOG_MIN_BBOX_PERCENT) {
        ESP_LOGI("FRAME", "========== Frame batch #%lu done: %s ==========",
                 (unsigned long)s_frame_batch_id,
                 reason ? reason : "idle");

        ESP_LOGI("FRAME", "Flushes:                 %lu",
                 (unsigned long)s_frame_flush_count);
        ESP_LOGI("FRAME", "Span first->last:        %lld us",
                 (long long)span_us);
        ESP_LOGI("FRAME", "Draw total:              %lld us",
                 (long long)s_frame_draw_total_us);
        ESP_LOGI("FRAME", "Draw avg/max:            %lld / %lld us",
                 (long long)avg_us,
                 (long long)s_frame_draw_max_us);
        ESP_LOGI("FRAME", "Busy retries/errors:     %lu / %lu",
                 (unsigned long)s_frame_busy_retries,
                 (unsigned long)s_frame_errors);

        ESP_LOGI("FRAME", "Bounding box:            x=%ld..%ld y=%ld..%ld (%lux%lu), %lu%% of screen",
                 (long)s_frame_x1,
                 (long)s_frame_x2,
                 (long)s_frame_y1,
                 (long)s_frame_y2,
                 (unsigned long)bbox_w,
                 (unsigned long)bbox_h,
                 (unsigned long)bbox_percent);

        ESP_LOGI("FRAME", "Touched pixels:          %llu, %lu%% of screen",
                 (unsigned long long)s_frame_pixels,
                 (unsigned long)touched_percent);
        ESP_LOGI("FRAME", "Transferred bytes:       %llu",
                 (unsigned long long)s_frame_bytes);
        ESP_LOGI("FRAME", "===============================================");
    }

    s_frame_batch_active = false;
}

/*
 * Backlight
 */
static esp_err_t backlight_init(void)
{
    ESP_LOGI(TAG, "Initializing backlight");

    ledc_timer_config_t lt = {
        .speed_mode      = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_PWM_RESOLUTION,
        .timer_num       = BACKLIGHT_LEDC_TIMER,
        .freq_hz         = BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&lt), TAG, "LEDC timer");

    ledc_channel_config_t lc = {
        .gpio_num   = PIN_LCD_BACKLIGHT,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel    = BACKLIGHT_LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BACKLIGHT_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&lc), TAG, "LEDC channel");

    return ESP_OK;
}

static esp_err_t backlight_set(int pct)
{
    if (pct > 100) {
        pct = 100;
    }

    if (pct < 0) {
        pct = 0;
    }

    s_backlight_level_pct = pct;

    uint32_t duty = (1023U * (uint32_t)pct) / 100U;

    ESP_RETURN_ON_ERROR(ledc_set_duty(BACKLIGHT_LEDC_MODE,
                                      BACKLIGHT_LEDC_CH,
                                      duty),
                        TAG,
                        "set duty");

    ESP_RETURN_ON_ERROR(ledc_update_duty(BACKLIGHT_LEDC_MODE,
                                         BACKLIGHT_LEDC_CH),
                        TAG,
                        "update duty");

    return ESP_OK;
}

esp_err_t demo_backlight_set(int pct)
{
    return backlight_set(pct);
}

int demo_backlight_get(void)
{
    return s_backlight_level_pct;
}

bool demo_screen_backlight_blank_enabled(void)
{
#if DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_ON_EEZ_SWITCH
    return true;
#else
    return false;
#endif
}

int demo_screen_backlight_blank_level_pct(void)
{
    return DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_LEVEL;
}

int demo_screen_backlight_restore_level_pct(void)
{
    return DISPLAY_EXPERIMENT_BACKLIGHT_RESTORE_LEVEL;
}

uint32_t demo_screen_backlight_blank_before_ms(void)
{
    return DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_MS;
}

uint32_t demo_screen_backlight_blank_after_ms(void)
{
    return 0;
}

/*
 * MIPI PHY power
 */
static esp_err_t mipi_phy_power_init(void)
{
    static esp_ldo_channel_handle_t phy_pwr_chan = NULL;

    esp_ldo_channel_config_t cfg = {
        .chan_id    = PIN_MIPI_PHY_PWR_LDO_CHAN,
        .voltage_mv = PIN_MIPI_PHY_PWR_VOLTAGE_MV,
    };

    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &phy_pwr_chan),
                        TAG,
                        "LDO acquire");

    ESP_LOGI(TAG, "MIPI PHY power enabled: LDO channel=%d voltage=%d mV",
             PIN_MIPI_PHY_PWR_LDO_CHAN,
             PIN_MIPI_PHY_PWR_VOLTAGE_MV);

    return ESP_OK;
}

/*
 * Display init: ST7701 over MIPI-DSI
 */
static esp_err_t display_init(esp_lcd_panel_handle_t *ret_panel,
                              esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;

    esp_lcd_dsi_bus_handle_t  dsi_bus = NULL;
    esp_lcd_panel_io_handle_t io      = NULL;
    esp_lcd_panel_handle_t    panel   = NULL;

    const st7701_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    jc4880p443c_get_init_cmds(&init_cmds, &init_cmds_size);

    uint32_t lane_bit_rate = 0;
    uint8_t num_lanes = 0;
    jc4880p443c_get_dsi_config(&lane_bit_rate, &num_lanes);

    uint32_t pclk_mhz = 0;
    uint16_t hbp = 0;
    uint16_t hfp = 0;
    uint16_t vbp = 0;
    uint16_t vfp = 0;
    jc4880p443c_get_timing(&pclk_mhz, &hbp, &hfp, &vbp, &vfp);
    jc4880p443c_get_resolution(&DISPLAY_H_RES, &DISPLAY_V_RES);

    ESP_LOGI(TAG, "Display resolution: %ux%u", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_LOGI(TAG, "DSI config: lanes=%u lane_bit_rate=%lu Mbps",
             (unsigned)num_lanes,
             (unsigned long)lane_bit_rate);
    ESP_LOGI(TAG, "DPI timing: pclk=%lu MHz hbp=%u hfp=%u vbp=%u vfp=%u",
             (unsigned long)pclk_mhz,
             hbp,
             hfp,
             vbp,
             vfp);
    ESP_LOGI(TAG, "ST7701 init command count: %u", init_cmds_size);

    ESP_GOTO_ON_ERROR(backlight_init(), err, TAG, "backlight");
    ESP_GOTO_ON_ERROR(mipi_phy_power_init(), err, TAG, "mipi pwr");

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = num_lanes,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = lane_bit_rate,
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus),
                      err,
                      TAG,
                      "dsi bus");

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io),
                      err,
                      TAG,
                      "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = pclk_mhz,
        .virtual_channel    = 0,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 2,
        .video_timing = {
            .h_size            = DISPLAY_H_RES,
            .v_size            = DISPLAY_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch  = hbp,
            .hsync_front_porch = hfp,
            .vsync_pulse_width = 4,
            .vsync_back_porch  = vbp,
            .vsync_front_porch = vfp,
        },
        .flags.use_dma2d = false,
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds      = init_cmds,
        .init_cmds_size = init_cmds_size,
        .mipi_config    = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_cfg, &panel),
                      err,
                      TAG,
                      "st7701");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), err, TAG, "reset");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), err, TAG, "init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), err, TAG, "on");

    *ret_panel = panel;
    *ret_io    = io;

    ESP_LOGI(TAG, "Display init complete");

    return ESP_OK;

err:
    if (panel) {
        esp_lcd_panel_del(panel);
    }

    if (io) {
        esp_lcd_panel_io_del(io);
    }

    if (dsi_bus) {
        esp_lcd_del_dsi_bus(dsi_bus);
    }

    return ret;
}

/*
 * Touch init: GT911 over I2C
 */
static esp_err_t touch_init(esp_lcd_touch_handle_t *ret_tp)
{
    ESP_LOGI(TAG, "Initializing GT911 touch controller");

    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port          = TOUCH_I2C_PORT,
        .sda_io_num        = PIN_TOUCH_SDA,
        .scl_io_num        = PIN_TOUCH_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t i2c_bus = NULL;

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus),
                        TAG,
                        "I2C master bus creation failed");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_FREQ_HZ;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io),
                        TAG,
                        "Touch panel IO creation failed");

    esp_lcd_touch_io_gt911_config_t gt911_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = DISPLAY_H_RES,
        .y_max        = DISPLAY_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .driver_data = &gt911_cfg,
    };

    esp_lcd_touch_handle_t tp = NULL;

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp),
                        TAG,
                        "GT911 driver creation failed");

    *ret_tp = tp;

    ESP_LOGI(TAG, "GT911 touch initialized (SDA=%d SCL=%d)",
             PIN_TOUCH_SDA,
             PIN_TOUCH_SCL);

    return ESP_OK;
}

/*
 * LVGL tick
 */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static esp_err_t lvgl_tick_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };

    esp_timer_handle_t t = NULL;

    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &t),
                        TAG,
                        "LVGL tick timer create");

    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(t,
                                                 LVGL_TICK_PERIOD_MS * 1000),
                        TAG,
                        "LVGL tick timer start");

    return ESP_OK;
}

/*
 * LVGL flush callback
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    if (ui_background_is_lvgl_flush_suspended()) {
        /* ESP32-P4 self-OTA can temporarily stall flash/cache/PSRAM/DSI hard
         * enough that even a small LVGL status/progress redraw may expose the
         * fallback screen.  When the OTA display guard is active, acknowledge
         * LVGL flushes without sending new rectangles to the panel: the LCD
         * keeps the last known-good frame until reboot or failure recovery. */
        lv_display_flush_ready(disp);
        return;
    }

    int64_t t0 = esp_timer_get_time();

    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

    lv_area_t draw_area = *area;
    uint8_t *draw_map = px_map;
    const lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    if (rotation != LV_DISPLAY_ROTATION_0) {
        lv_area_t rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);

        const lv_color_format_t cf = lv_display_get_color_format(disp);
        const int32_t src_w = lv_area_get_width(area);
        const int32_t src_h = lv_area_get_height(area);
        const uint32_t src_stride = lv_draw_buf_width_to_stride(src_w, cf);
        const uint32_t dest_stride =
            lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        const uint32_t dest_size =
            dest_stride * (uint32_t)lv_area_get_height(&rotated_area);

        if (!s_lvgl_rotate_buf || dest_size > s_lvgl_buffer_size_bytes) {
            ESP_LOGE(TAG,
                     "rotation buffer too small (%lu > %lu)",
                     (unsigned long)dest_size,
                     (unsigned long)s_lvgl_buffer_size_bytes);
            s_flush_error_counter++;
            lv_display_flush_ready(disp);
            return;
        }

        lv_draw_sw_rotate(px_map,
                          s_lvgl_rotate_buf,
                          src_w,
                          src_h,
                          src_stride,
                          dest_stride,
                          rotation,
                          cf);

        draw_area = rotated_area;
        draw_map = s_lvgl_rotate_buf;
    }

    esp_err_t ret = ESP_OK;
    int retry_count = 0;

    for (int i = 0; i < 20; i++) {
        ret = esp_lcd_panel_draw_bitmap(panel,
                                        draw_area.x1,
                                        draw_area.y1,
                                        draw_area.x2 + 1,
                                        draw_area.y2 + 1,
                                        draw_map);

        if (ret == ESP_OK) {
            break;
        }

        if (ret == ESP_ERR_INVALID_STATE) {
            retry_count++;
            s_flush_busy_counter++;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        s_flush_error_counter++;
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(ret));
        break;
    }

    if (LVGL_FLUSH_SETTLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(LVGL_FLUSH_SETTLE_MS));
    }

    int64_t t1 = esp_timer_get_time();
    int64_t dt = t1 - t0;

    const uint32_t w = (uint32_t)lv_area_get_width(&draw_area);
    const uint32_t h = (uint32_t)lv_area_get_height(&draw_area);
    const uint32_t bpp = lvgl_display_bytes_per_pixel(disp);
    const uint32_t bytes = w * h * bpp;

    s_flush_counter++;
    s_flush_total_us += dt;

    if (dt > s_flush_max_us) {
        s_flush_max_us = dt;
    }

    frame_batch_note_flush(disp,
                           &draw_area,
                           bytes,
                           dt,
                           retry_count,
                           ret,
                           t0,
                           t1);

    if ((s_flush_counter % FLUSH_LOG_EVERY_N) == 0 ||
        ret != ESP_OK ||
        retry_count > 0) {
        ESP_LOGI("FLUSH",
                 "#%lu area=%lux%lu bytes=%lu bpp=%lu time=%lld us retries=%d ret=%s",
                 (unsigned long)s_flush_counter,
                 (unsigned long)w,
                 (unsigned long)h,
                 (unsigned long)bytes,
                 (unsigned long)bpp,
                 (long long)dt,
                 retry_count,
                 esp_err_to_name(ret));
    }

    lv_display_flush_ready(disp);
}

/*
 * LVGL touch read callback
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = lv_indev_get_user_data(indev);

    esp_lcd_touch_read_data(tp);

    esp_lcd_touch_point_data_t point;
    uint8_t point_cnt = 0;

    esp_lcd_touch_get_data(tp, &point, &point_cnt, 1);

    if (point_cnt > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_touch_register(esp_lcd_touch_handle_t tp)
{
    lv_indev_t *indev = lv_indev_create();

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, lv_display_get_default());
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    lv_indev_set_user_data(indev, tp);

    ESP_LOGI(TAG, "Touch registered as LVGL input device");
}

/*
 * LVGL task
 */
static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL task on core %d", xPortGetCoreID());

    uint32_t d = LVGL_TASK_MAX_DELAY_MS;
    uint32_t loop_counter = 0;
    bool was_suspended = false;

    while (1) {
        if (ui_background_is_lvgl_task_suspended()) {
            if (!was_suspended) {
                ESP_LOGW(TAG, "LVGL task frozen: lv_timer_handler() disabled for OTA");
                frame_batch_maybe_finish("LVGL task suspended for OTA");
                was_suspended = true;
            }
            vTaskDelay(pdMS_TO_TICKS(LVGL_TASK_SUSPEND_DELAY_MS));
            continue;
        }

        if (was_suspended) {
            ESP_LOGW(TAG, "LVGL task resumed: lv_timer_handler() enabled");
            was_suspended = false;
        }

        d = lv_timer_handler();

        if (d > LVGL_TASK_MAX_DELAY_MS) {
            d = LVGL_TASK_MAX_DELAY_MS;
        }

        if (d < LVGL_TASK_MIN_DELAY_MS) {
            d = LVGL_TASK_MIN_DELAY_MS;
        }

        loop_counter++;

        frame_batch_maybe_finish("idle after redraw");

        if ((loop_counter % 1000U) == 0U) {
            frame_batch_maybe_finish("periodic idle check");
            log_flush_stats("periodic");
            log_memory_status("LVGL task periodic");
        }

        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

/*
 * LVGL display setup
 */
static lv_display_t *lvgl_display_init(esp_lcd_panel_handle_t panel)
{
    lv_init();

    ESP_ERROR_CHECK(lvgl_tick_init());

    lv_display_t *disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);
    assert(disp);

    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_physical_resolution(disp, DISPLAY_H_RES, DISPLAY_V_RES);

    void *buf1 = NULL;
    void *buf2 = NULL;

    const size_t full_mode_pixels =
        display_experiment_lvgl_min_buffer_pixels(DISPLAY_H_RES,
                                                  DISPLAY_V_RES);

    const size_t buffer_candidates[] = {
        full_mode_pixels,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_TURBO,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_LARGE,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_FAST,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_MEDIUM,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_SAFE,
        LVGL_BUFFER_WIDTH * LVGL_BUFFER_LINES_MIN,
    };

    for (size_t i = 0; i < sizeof(buffer_candidates) / sizeof(buffer_candidates[0]); i++) {
        if (buffer_candidates[i] == 0U) {
            continue;
        }

        if (full_mode_pixels > 0U && buffer_candidates[i] != full_mode_pixels) {
            continue;
        }

        ESP_LOGI(TAG,
                 "Trying LVGL buffer candidate: %lu pixels (%lu bytes each)",
                 (unsigned long)buffer_candidates[i],
                 (unsigned long)(buffer_candidates[i] * sizeof(lv_color_t)));

        if (lvgl_alloc_draw_buffers(buffer_candidates[i],
                                    &buf1,
                                    &buf2,
                                    &s_lvgl_rotate_buf)) {
            break;
        }

        ESP_LOGW(TAG,
                 "LVGL buffer candidate failed: %lu pixels",
                 (unsigned long)buffer_candidates[i]);
    }

    assert(buf1 && buf2 && s_lvgl_rotate_buf);

    ESP_LOGI(TAG,
             "LVGL buffers selected: pixels=%lu bytes_each=%lu total_three_buffers=%lu",
             (unsigned long)s_lvgl_buffer_pixels,
             (unsigned long)s_lvgl_buffer_size_bytes,
             (unsigned long)(s_lvgl_buffer_size_bytes * 3U));

    log_buffer_math("after LVGL buffer allocation");
    log_memory_status("after LVGL buffer allocation");

    const lv_display_render_mode_t render_mode =
        display_experiment_lvgl_render_mode();

    ESP_LOGI(TAG,
             "LVGL render mode: %s (%d)",
             display_experiment_lvgl_render_mode_name(),
             (int)render_mode);

    lv_display_set_buffers(disp,
                           buf1,
                           buf2,
                           s_lvgl_buffer_size_bytes,
                           render_mode);

    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_user_data(disp, panel);

    return disp;
}

static bool lvgl_alloc_draw_buffers(size_t pixels, void **buf1, void **buf2,
                                    uint8_t **rotate_buf)
{
    const size_t bytes = pixels * sizeof(lv_color_t);

    *buf1 = NULL;
    *buf2 = NULL;
    *rotate_buf = NULL;

    *buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    *buf2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    *rotate_buf = heap_caps_malloc(bytes, MALLOC_CAP_DMA);

    if (*buf1 && *buf2 && *rotate_buf) {
        s_lvgl_buffer_pixels = pixels;
        s_lvgl_buffer_size_bytes = bytes;

        ESP_LOGI(TAG,
                 "Allocated LVGL buffers: pixels=%lu bytes=%lu buf1=%p buf2=%p rotate=%p",
                 (unsigned long)pixels,
                 (unsigned long)bytes,
                 *buf1,
                 *buf2,
                 *rotate_buf);

        return true;
    }

    ESP_LOGW(TAG,
             "Failed to allocate LVGL buffers: pixels=%lu bytes_each=%lu buf1=%p buf2=%p rotate=%p",
             (unsigned long)pixels,
             (unsigned long)bytes,
             *buf1,
             *buf2,
             *rotate_buf);

    if (*buf1) {
        heap_caps_free(*buf1);
    }

    if (*buf2) {
        heap_caps_free(*buf2);
    }

    if (*rotate_buf) {
        heap_caps_free(*rotate_buf);
    }

    *buf1 = NULL;
    *buf2 = NULL;
    *rotate_buf = NULL;

    return false;
}

/*
 * app_main
 */
void app_main(void)
{
    const int64_t app_t0 = esp_timer_get_time();

    ESP_LOGI(TAG, "EEZ LVGL UI demo - ESP32-P4");
    (void)mad_ota_confirm_running_app();
    log_memory_status("app_main start");
    log_buffer_math("app_main start");

    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;

    /*
     * Display / ST7701 / MIPI-DSI init
     */
    int64_t t0 = esp_timer_get_time();

    esp_err_t ret = display_init(&panel, &io);

    int64_t t1 = esp_timer_get_time();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Halting before LVGL init");
        log_memory_status("display init failed");
        return;
    }

    ESP_LOGI(TAG, "Display init OK in %lld us", (long long)(t1 - t0));
    ESP_LOGI(TAG, "LCD panel handle: %p", panel);
    ESP_LOGI(TAG, "LCD IO handle:    %p", io);

    if (panel == NULL) {
        ESP_LOGE(TAG, "Display init returned NULL panel handle");
        log_memory_status("display panel NULL");
        return;
    }

    log_memory_status("after display init");
    log_buffer_math("after display init");
    display_experiment_log_panel_framebuffers(panel);
    (void)display_experiment_register_panel_callbacks(panel);

    /*
     * Backlight
     */
    t0 = esp_timer_get_time();

    ret = backlight_set(90);

    t1 = esp_timer_get_time();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Backlight set failed: %s", esp_err_to_name(ret));
        log_memory_status("backlight failed");
        return;
    }

    ESP_LOGI(TAG, "Backlight set to 90%% in %lld us", (long long)(t1 - t0));
    log_memory_status("after backlight");
    display_experiment_set_backlight_cb(backlight_set);

#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNTHETIC
    display_experiment_run_direct_fullscreen_synthetic(panel);
    return;
#endif

#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SOLID_COLORS
    display_experiment_run_direct_fullscreen_solid_colors(panel);
    return;
#endif

#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ
    display_experiment_run_direct_fullscreen_eez(panel);
    return;
#endif

    /*
     * LVGL display port init
     */
    t0 = esp_timer_get_time();

    lv_display_t *disp = lvgl_display_init(panel);

    t1 = esp_timer_get_time();

    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL display init failed: disp == NULL");
        log_memory_status("LVGL display init failed");
        return;
    }

    ESP_LOGI(TAG, "LVGL display init OK in %lld us", (long long)(t1 - t0));
    ESP_LOGI(TAG, "LVGL display handle: %p", disp);

    log_memory_status("after LVGL display init");
    log_buffer_math("after LVGL display init");

    /*
     * Touch / GT911 init
     *
     * Non-fatal: if GT911 is not initialized, the UI can still run in
     * display-only mode.
     */
    esp_lcd_touch_handle_t tp = NULL;

    t0 = esp_timer_get_time();

    esp_err_t touch_ret = touch_init(&tp);

    t1 = esp_timer_get_time();

    if (touch_ret == ESP_OK && tp != NULL) {
        ESP_LOGI(TAG, "Touch init OK in %lld us", (long long)(t1 - t0));
        ESP_LOGI(TAG, "Touch handle: %p", tp);

        lvgl_touch_register(tp);
        ESP_LOGI(TAG, "Touch registered as LVGL input device");
    } else {
        ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(touch_ret));
        ESP_LOGW(TAG, "Continuing in display-only mode");
    }

    log_memory_status("after touch init");

    /*
     * EEZ UI init
     */
    t0 = esp_timer_get_time();

    eez_ui_port_init(disp);

    t1 = esp_timer_get_time();

    ESP_LOGI(TAG, "EEZ UI init OK in %lld us", (long long)(t1 - t0));
    log_memory_status("after EEZ UI init");
    log_buffer_math("after EEZ UI init");

    /*
     * WiFi
     *
     * Start asynchronously so the UI appears immediately.  The runtime LED
     * widgets read wifi_manager_get_status() from an LVGL timer and show:
     * red = offline/failed, cyan = connecting, green = connected.
     */
    ret = wifi_manager_start_async();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi async start failed: %s", esp_err_to_name(ret));
    }

    ret = mad_ota_start_async();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MAD OTA async start failed: %s", esp_err_to_name(ret));
    }

    ret = stm32_swd_programmer_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STM32 SWD programmer init failed: %s", esp_err_to_name(ret));
    }

    /*
     * LVGL task
     */
    BaseType_t task_ret = xTaskCreatePinnedToCore(lvgl_task,
                                                  "lvgl",
                                                  LVGL_TASK_STACK_SIZE,
                                                  NULL,
                                                  LVGL_TASK_PRIORITY,
                                                  NULL,
                                                  LVGL_TASK_CORE);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        log_memory_status("LVGL task create failed");
        return;
    }

    ESP_LOGI(TAG,
             "LVGL task created: stack=%d priority=%d core=%d",
             LVGL_TASK_STACK_SIZE,
             LVGL_TASK_PRIORITY,
             LVGL_TASK_CORE);

    log_memory_status("after LVGL task create");
    log_buffer_math("after LVGL task create");
    log_flush_stats("after LVGL task create");

    /*
     * Final summary
     */
    const int64_t app_t1 = esp_timer_get_time();

    ESP_LOGI(TAG, "Running - %dx%d", DISPLAY_H_RES, DISPLAY_V_RES);
    ESP_LOGI(TAG, "app_main completed in %lld us", (long long)(app_t1 - app_t0));

    log_memory_status("app_main end");
    log_buffer_math("app_main end");
}
