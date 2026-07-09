#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display_direct_timing.h"
#include "display_experiments.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Production fullscreen background path.
 *
 * This is deliberately only a thin layer over display_direct_timing.* so the
 * tested laboratory direct-draw sync path and the production background path do
 * not drift apart. Experimental DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_* modes
 * remain standalone lab tests and must not be used as production control flow.
 */
#ifndef UI_BACKGROUND_INTERCEPT_ENABLE
#define UI_BACKGROUND_INTERCEPT_ENABLE 1
#endif

#ifndef UI_BACKGROUND_DIRECT_WAIT_FOR_REFRESH
#define UI_BACKGROUND_DIRECT_WAIT_FOR_REFRESH 1
#endif

#ifndef UI_BACKGROUND_DIRECT_SYNC_MODE
#define UI_BACKGROUND_DIRECT_SYNC_MODE DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE
#endif

#ifndef UI_BACKGROUND_DIRECT_PHASE_AFTER_REFRESH_US
#define UI_BACKGROUND_DIRECT_PHASE_AFTER_REFRESH_US DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_AFTER_REFRESH_US
#endif

#ifndef UI_BACKGROUND_DIRECT_LEAD_BEFORE_NEXT_REFRESH_US
#define UI_BACKGROUND_DIRECT_LEAD_BEFORE_NEXT_REFRESH_US DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_LEAD_BEFORE_NEXT_REFRESH_US
#endif

#ifndef UI_BACKGROUND_DIRECT_SKIP_REFRESH_COUNT
#define UI_BACKGROUND_DIRECT_SKIP_REFRESH_COUNT DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SKIP_REFRESH_COUNT
#endif

#ifndef UI_BACKGROUND_DIRECT_WAIT_TIMEOUT_MS
#define UI_BACKGROUND_DIRECT_WAIT_TIMEOUT_MS DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_TIMEOUT_MS
#endif

#ifndef UI_BACKGROUND_DIRECT_LOG_TIMING
#define UI_BACKGROUND_DIRECT_LOG_TIMING 1
#endif

typedef int ui_background_id_t;

esp_err_t ui_background_draw_synced(esp_lcd_panel_handle_t panel,
                                    ui_background_id_t bg,
                                    const lv_img_dsc_t *img);

/* Redraw only the direct runtime overlay regions (FPS/LED) using the last
 * clean native background and the last panel handle.  This does not redraw the
 * whole page and therefore does not erase LVGL widgets. */
esp_err_t ui_background_redraw_runtime_overlay_regions(void);

/* Suspend small direct overlay redraws while flash/OTA work is running.
 * Full-screen backgrounds already drawn on the panel are left untouched; only
 * the periodic FPS/Wi-Fi native rectangle updates are suppressed. */
void ui_background_set_runtime_overlay_suspended(bool suspended);
bool ui_background_is_runtime_overlay_suspended(void);

/* During ESP32-P4 self-OTA we can completely freeze LVGL flushes so the
 * physical panel keeps the last stable frame and no fallback screen/background
 * can be pushed while flash/PSRAM/DSI are under stress. */
void ui_background_set_lvgl_flush_suspended(bool suspended);
bool ui_background_is_lvgl_flush_suspended(void);

/* fix33: stronger OTA freeze.  Flushing alone still allows LVGL timers,
 * animations, invalidation and touch processing to run and generate redraw
 * pressure while ESP32-P4 self-OTA writes flash.  This flag is consumed by
 * the LVGL task: while set, lv_timer_handler() is not called at all.  Do not
 * call lv_deinit(); keep all objects alive and simply freeze the UI loop. */
void ui_background_set_lvgl_task_suspended(bool suspended);
bool ui_background_is_lvgl_task_suspended(void);

#ifdef __cplusplus
}
#endif
