/*
 * Compile-time display transition experiments.
 * Экспериментальные режимы вывода, выбираемые на этапе компиляции.
 *
 * This file is a small switchboard for display-output experiments around
 * fullscreen tearing on JC4880P443C / ST7701S / MIPI-DSI.
 * Этот файл - "пульт переключателей" для диагностики fullscreen tearing /
 * "лесенки" на JC4880P443C / ST7701S / MIPI-DSI.
 *
 * Important usage rules:
 * Важные правила использования:
 * - These are compile-time switches. Change the define, rebuild, flash.
 *   Это compile-time переключатели: поменять define, собрать, прошить.
 * - Direct fullscreen tests run after display/backlight init and before
 *   LVGL/EEZ/touch start. When a direct test is enabled, the normal UI will
 *   not run.
 *   Direct fullscreen тесты запускаются после init дисплея/подсветки и до
 *   LVGL/EEZ/touch. Если direct test включен, штатный UI не стартует.
 * - Enable only one direct fullscreen test at a time:
 *     SYNTHETIC, EEZ, or SOLID_COLORS.
 *   Одновременно включать только один direct fullscreen тест:
 *     SYNTHETIC, EEZ или SOLID_COLORS.
 * - To return to the normal LVGL/EEZ application path, set all direct tests to
 *   0:
 *     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNTHETIC = 0
 *     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ = 0
 *     DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SOLID_COLORS = 0
 *   Чтобы вернуть обычный LVGL/EEZ путь, поставить все direct tests в 0.
 * - The current working-copy defaults may intentionally be left in a lab state
 *   for board testing. Read the individual defaults below before building.
 *   Defaults в рабочей копии могут быть специально оставлены в лабораторном
 *   состоянии для тестов на плате. Перед сборкой проверь значения ниже.
 *
 * Do not use these switches to change ST7701S init, lane rate, pclk, porch, or
 * generated EEZ files. Those are separate experiments with a larger blast
 * radius.
 * Эти переключатели не должны менять ST7701S init, lane rate, pclk, porch или
 * generated EEZ files. Это отдельные эксперименты с большим радиусом риска.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct panel tests.
 * Прямые тесты панели, в обход LVGL/EEZ.
 *
 * Synthetic: draws two generated fullscreen RGB565 frames directly with
 * esp_lcd_panel_draw_bitmap(), bypassing LVGL/EEZ.
 * Use this to answer: "Can the panel path switch two full RGB565 frames without
 * LVGL/EEZ partial redraw artifacts?"
 * Synthetic: рисует две сгенерированные fullscreen RGB565 картинки напрямую
 * через esp_lcd_panel_draw_bitmap(). Нужен, чтобы понять, может ли нижний
 * display path переключать полные кадры без LVGL/EEZ partial redraw.
 *
 * EEZ images: draws two generated EEZ RGB565 image descriptors directly. It
 * supports 480x800 native images and 800x480 landscape images rotated into the
 * native 480x800 panel buffer.
 * Use this to answer: "Are the real generated UI image assets clean when drawn
 * directly, outside LVGL/EEZ screen switching?"
 * EEZ images: рисует реальные generated EEZ RGB565 image descriptors напрямую.
 * Поддерживает native 480x800 и landscape 800x480 с поворотом в native buffer.
 * Нужен, чтобы отделить проблемы самих картинок от LVGL/EEZ screen switching.
 *
 * Solid colors: draws full red, green, black, and white frames. This is the
 * harshest scanout/phase stress test because every pixel changes by a large
 * amount. A tiny seam that is visible here can be invisible on real photos.
 * Solid colors: рисует полный красный, зеленый, черный и белый кадр. Это самый
 * жесткий stress-test scanout/phase: каждый пиксель резко меняется. Маленькая
 * полоса, заметная здесь, может быть невидима на реальных фото.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNTHETIC
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNTHETIC 0
#endif

/*
 * Direct real-image test. In the current lab workflow this may be set to 1 for
 * quick board testing. Set it to 0 before expecting the normal LVGL/EEZ UI.
 * Прямой тест реальных EEZ-картинок. В лабораторном workflow может быть 1 для
 * быстрой проверки платы. Для обычного LVGL/EEZ UI поставить 0.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ 0
#endif

/*
 * Direct solid-color test. Useful for spotting whether a remaining band is
 * scanout timing/phase related rather than image-copy related.
 * Прямой тест однотонных кадров. Помогает понять, остаточная полоса связана с
 * фазой scanout/timing или с copy/rotation bounds у картинок.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SOLID_COLORS
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SOLID_COLORS 0
#endif

/*
 * When enabled, the EEZ direct test accepts only images that are already in
 * native 480x800 layout and copies them without rotation.
 *
 * Set to 1 when you want to prove that no rotate/copy bounds issue is involved.
 * Landscape 800x480 images will intentionally fail with a clear log message.
 * Если включено, direct EEZ test принимает только картинки уже в native 480x800
 * и копирует их без поворота. Включай, когда нужно доказать, что rotate/copy
 * bounds не участвуют. Landscape 800x480 намеренно завершится понятным логом.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ_NO_ROTATION_ONLY
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_EEZ_NO_ROTATION_ONLY 0
#endif

/*
 * Pause between direct fullscreen frame submissions. This affects how easy the
 * transition is to capture on slow-motion video; it does not change the draw
 * timing itself.
 * Пауза между отправками fullscreen кадров в direct tests. Удобна для съемки
 * slow-motion видео; не меняет саму длительность draw_bitmap.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_DELAY_MS
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_DELAY_MS 700
#endif

/*
 * Direct draw sync experiment.
 * Эксперимент синхронизации direct draw относительно refresh boundary.
 *
 * Goal: choose when a fullscreen esp_lcd_panel_draw_bitmap() starts relative to
 * the display refresh boundary. This is for shrinking the residual horizontal
 * color seam seen in solid-color direct tests.
 * Цель: выбрать момент старта fullscreen esp_lcd_panel_draw_bitmap()
 * относительно границы refresh. Это нужно, чтобы уменьшить остаточную
 * горизонтальную полосу в solid-color тесте.
 *
 * Sync modes:
 * Режимы синхронизации:
 *   0 = no sync, draw immediately
 *       Baseline. Usually shows the strongest top/bottom split on solid colors.
 *       Без синхронизации, рисуем сразу. Базовый режим, обычно дает самый
 *       заметный split верх/низ на однотонных цветах.
 *   1 = wait for next refresh_done, then draw immediately
 *       Backward-compatible "WAIT_FOR_REFRESH" behavior. Often much cleaner.
 *       Ждать следующий refresh_done и сразу рисовать. Старое поведение
 *       WAIT_FOR_REFRESH, обычно заметно чище.
 *   2 = wait for next refresh_done, delay PHASE_AFTER_REFRESH_US, then draw
 *       Fixed phase-after-refresh calibration. Sweep or manually tune phase.
 *       Ждать refresh_done, затем микрозадержка PHASE_AFTER_REFRESH_US и draw.
 *       Основной режим ручной/автоматической фазовой калибровки.
 *   3 = wait for next refresh_done, then wait SKIP_REFRESH_COUNT additional
 *       refresh_done events, then delay PHASE_AFTER_REFRESH_US, then draw
 *       Same as mode 2 but allows intentionally skipping one or more frames.
 *       Как mode 2, но можно пропустить дополнительные refresh_done события.
 *       Полезно для проверки стабильности каденции callback'ов.
 *   4 = predicted next-refresh mode: wait refresh_done, estimate refresh
 *       period, delay period - LEAD_BEFORE_NEXT_REFRESH_US, then draw shortly
 *       before the next refresh boundary
 *       Useful if drawing just after refresh_done still leaves a lower stripe.
 *       Предсказание следующей границы refresh: ждем refresh_done, оцениваем
 *       период, затем ждем period - LEAD_BEFORE_NEXT_REFRESH_US и рисуем
 *       незадолго до следующей границы. Полезно, если draw после refresh_done
 *       оставляет нижнюю полоску.
 *
 * DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH is kept for old test
 * instructions. If SYNC_MODE is not defined explicitly, WAIT_FOR_REFRESH=1 maps
 * to sync mode 1 and WAIT_FOR_REFRESH=0 maps to sync mode 0.
 * DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH оставлен для старых
 * инструкций. Если SYNC_MODE явно не задан, WAIT_FOR_REFRESH=1 означает mode 1,
 * а WAIT_FOR_REFRESH=0 означает mode 0.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH 1
#endif

#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE
#if DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_FOR_REFRESH
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE 1
#else
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SYNC_MODE 0
#endif
#endif

/*
 * Fixed delay used by sync modes 2 and 3. Start with small values such as
 * 0/50/100/150/200 us. If the lower stripe shrinks, the best safe point is
 * slightly after refresh_done. If it grows or moves upward, try mode 4.
 * Фиксированная задержка для sync modes 2 и 3. Начинай с 0/50/100/150/200 us.
 * Если нижняя полоса уменьшается, safe point чуть позже refresh_done. Если
 * полоса растет или сдвигается вверх, пробуй mode 4.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_AFTER_REFRESH_US
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_AFTER_REFRESH_US 0
#endif

/*
 * Mode 4 target lead time before the predicted next refresh boundary.
 * Larger lead starts the draw earlier before the next boundary; smaller lead
 * starts it closer to the boundary. Useful values to try: 50, 100, 150, 200,
 * 300, 500, then larger values if the measured refresh period supports it.
 * Lead time для mode 4 перед предсказанной следующей refresh boundary.
 * Больше lead - draw стартует раньше до границы; меньше lead - ближе к границе.
 * Типовые значения для пробы: 50, 100, 150, 200, 300, 500, затем больше, если
 * измеренный refresh period это позволяет.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_LEAD_BEFORE_NEXT_REFRESH_US
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_LEAD_BEFORE_NEXT_REFRESH_US 100
#endif

/*
 * Extra refresh_done events to skip in sync mode 3. Usually 0. Set to 1+ only
 * when checking whether the callback cadence itself is stable across frames.
 * Дополнительные refresh_done события, пропускаемые в sync mode 3. Обычно 0.
 * Значения 1+ нужны только для проверки стабильности callback cadence.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SKIP_REFRESH_COUNT
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_SKIP_REFRESH_COUNT 0
#endif

/*
 * Phase sweep replaces the fixed PHASE_AFTER_REFRESH_US with an automatic
 * sequence:
 *   START_US, START_US + STEP_US, ..., END_US
 *
 * Each phase is held for FRAMES_PER_STEP fullscreen switches. Record slow-motion
 * video and match visible seam quality against PHASE_SWEEP log lines.
 * Phase sweep заменяет фиксированный PHASE_AFTER_REFRESH_US автоматическим
 * перебором:
 *   START_US, START_US + STEP_US, ..., END_US
 *
 * Каждая фаза держится FRAMES_PER_STEP переключений. Сними slow-motion видео и
 * сопоставь качество полосы со строками PHASE_SWEEP в логе.
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_ENABLE
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_ENABLE 0
#endif

#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_START_US
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_START_US 0
#endif

#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_END_US
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_END_US 1000
#endif

#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_STEP_US
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_STEP_US 50
#endif

#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_PHASE_SWEEP_FRAMES_PER_STEP 10
#endif

/*
 * Maximum time to wait for callback counters after a draw. This protects tests
 * from hanging if callbacks are not firing. It is in milliseconds; sub-ms phase
 * delays use esp_rom_delay_us() instead.
 * Максимальное ожидание callback counters после draw. Защищает тесты от
 * зависания, если callbacks не приходят. Единицы - миллисекунды; sub-ms
 * фазовые задержки используют esp_rom_delay_us().
 */
#ifndef DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_TIMEOUT_MS
#define DISPLAY_EXPERIMENT_DIRECT_FULLSCREEN_WAIT_TIMEOUT_MS 100
#endif

/*
 * LVGL render mode experiment:
 * Эксперимент режима рендера LVGL:
 *   0 = PARTIAL, current known-working default
 *       PARTIAL, текущий рабочий default; может давать много stripe flush.
 *   1 = FULL, LVGL renders the full screen before every flush
 *       FULL, LVGL рендерит весь экран перед каждым flush.
 *   2 = DIRECT, LVGL renders into screen-sized buffers
 *       DIRECT, LVGL рендерит в screen-sized buffers.
 *
 * This affects only the normal LVGL path. It has no effect while a direct
 * fullscreen test is enabled, because direct tests return before LVGL starts.
 * Влияет только на обычный LVGL path. Не влияет при включенном direct
 * fullscreen test, потому что direct tests завершают app_main до старта LVGL.
 */
#ifndef DISPLAY_EXPERIMENT_LVGL_RENDER_MODE
#define DISPLAY_EXPERIMENT_LVGL_RENDER_MODE 0
#endif

#define DISPLAY_EXPERIMENT_LVGL_RENDER_PARTIAL 0
#define DISPLAY_EXPERIMENT_LVGL_RENDER_FULL    1
#define DISPLAY_EXPERIMENT_LVGL_RENDER_DIRECT  2

/*
 * Safe diagnostics. These only log panel framebuffer addresses and callback
 * counters. They do not write directly to panel framebuffers.
 * Безопасная диагностика. Только логирует адреса framebuffer'ов и callback
 * counters. Не пишет напрямую в panel framebuffers.
 *
 * FRAMEBUFFER_LOGGING prints the internal DPI framebuffer addresses returned by
 * esp_lcd_dpi_panel_get_frame_buffer().
 * FRAMEBUFFER_LOGGING печатает адреса внутренних DPI framebuffer'ов, которые
 * возвращает esp_lcd_dpi_panel_get_frame_buffer().
 *
 * PANEL_CALLBACK_DIAGNOSTICS registers on_color_trans_done/on_refresh_done and
 * accumulates counters plus refresh timing statistics. Callback code must stay
 * tiny; logging is done later from task context.
 * PANEL_CALLBACK_DIAGNOSTICS регистрирует on_color_trans_done/on_refresh_done
 * и накапливает counters плюс refresh timing statistics. Callback должен
 * оставаться маленьким; логирование идет позже из task context.
 */
#ifndef DISPLAY_EXPERIMENT_FRAMEBUFFER_LOGGING
#define DISPLAY_EXPERIMENT_FRAMEBUFFER_LOGGING 1
#endif

#ifndef DISPLAY_EXPERIMENT_PANEL_CALLBACK_DIAGNOSTICS
#define DISPLAY_EXPERIMENT_PANEL_CALLBACK_DIAGNOSTICS 1
#endif

/*
 * Backlight blanking fallback around EEZ screen switches. Off by default
 * because it can hide tearing instead of fixing the output model.
 *
 * This applies to the EEZ/LVGL screen-switch path, not to direct fullscreen
 * tests. Use only as a fallback/masking experiment after measuring direct draw
 * behavior.
 * Fallback с гашением подсветки вокруг EEZ screen switches. По умолчанию
 * выключен, потому что маскирует tearing вместо исправления модели вывода.
 *
 * Работает только в EEZ/LVGL screen-switch path, не в direct fullscreen tests.
 * Использовать только как fallback/masking experiment после измерения direct
 * draw behavior.
 */
#ifndef DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_ON_EEZ_SWITCH
#define DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_ON_EEZ_SWITCH 0
#endif

#ifndef DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_LEVEL
#define DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_LEVEL 0
#endif

#ifndef DISPLAY_EXPERIMENT_BACKLIGHT_RESTORE_LEVEL
#define DISPLAY_EXPERIMENT_BACKLIGHT_RESTORE_LEVEL 90
#endif

#ifndef DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_MS
#define DISPLAY_EXPERIMENT_BACKLIGHT_BLANK_MS 25
#endif

typedef esp_err_t (*display_experiment_backlight_set_cb_t)(int pct);

void display_experiment_set_backlight_cb(display_experiment_backlight_set_cb_t cb);
void display_experiment_backlight_blank_before_switch(void);
void display_experiment_backlight_restore_after_switch(void);

void display_experiment_log_panel_framebuffers(esp_lcd_panel_handle_t panel);
esp_err_t display_experiment_register_panel_callbacks(esp_lcd_panel_handle_t panel);
void display_experiment_log_callback_stats(const char *stage);

void display_experiment_run_direct_fullscreen_synthetic(esp_lcd_panel_handle_t panel);
void display_experiment_run_direct_fullscreen_solid_colors(esp_lcd_panel_handle_t panel);
void display_experiment_run_direct_fullscreen_eez(esp_lcd_panel_handle_t panel);

lv_display_render_mode_t display_experiment_lvgl_render_mode(void);
const char *display_experiment_lvgl_render_mode_name(void);
size_t display_experiment_lvgl_min_buffer_pixels(uint16_t h_res, uint16_t v_res);

#ifdef __cplusplus
}
#endif
