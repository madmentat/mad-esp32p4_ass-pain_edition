#include "eez_ui_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display_experiments.h"
#include "screens.h"
#include "eez_ui_runtime.h"
#include "ui_background_direct.h"
#include "p4_project_config.h"

#ifndef P4_LVGL_ROTATE_UI_180_FROM_PREVIOUS
#define P4_LVGL_ROTATE_UI_180_FROM_PREVIOUS 1
#endif

#if P4_LVGL_ROTATE_UI_180_FROM_PREVIOUS
#define EEZ_UI_LANDSCAPE_ROTATION LV_DISPLAY_ROTATION_90
#else
#define EEZ_UI_LANDSCAPE_ROTATION LV_DISPLAY_ROTATION_270
#endif
#define EEZ_UI_TICK_PERIOD_MS     10
#define EEZ_UI_BLACKOUT_ON_ROTATION_CHANGE 1
#define EEZ_UI_REFRESH_IMMEDIATELY         1
#define EEZ_UI_FORCE_LANDSCAPE_INSTALL     1
#define EEZ_UI_SUPPRESS_LOAD_INVALIDATION  1
#define EEZ_UI_PORTRAIT_IMAGE_ROTATION     2700

/*
 * LVGL redraws small rectangles under transparent/anti-aliased widget pixels.
 * The full-screen direct background is already correct and must not be mirrored
 * or replaced after page load.  LVGL crop/background provider is used only
 * inside widget redraw rectangles and must stay in the same logical coordinate
 * system as LVGL widgets.
 */
#ifndef EEZ_UI_LVGL_CROP_ROTATE_180
#define EEZ_UI_LVGL_CROP_ROTATE_180 0
#endif

#ifndef EEZ_UI_ENABLE_LVGL_CROP_PROVIDER
#define EEZ_UI_ENABLE_LVGL_CROP_PROVIDER P4_ENABLE_LVGL_STYLE_CROP_PROVIDER
#endif

#ifndef EEZ_UI_HIDE_DIRECT_BG_IMAGE_WITH_OVERLAYS
#define EEZ_UI_HIDE_DIRECT_BG_IMAGE_WITH_OVERLAYS P4_HIDE_DIRECT_BG_IMAGE_WHEN_OVERLAYS_EXIST
#endif

#ifndef EEZ_UI_STATIC_RUNTIME_OVERLAY_NEEDS_CROP_PROVIDER
#define EEZ_UI_STATIC_RUNTIME_OVERLAY_NEEDS_CROP_PROVIDER \
    ((P4_RUNTIME_OVERLAY_TOP_LAYER != 0) && (P4_RUNTIME_OVERLAY_LVGL_VISIBLE != 0))
#endif

static const char *TAG = "EEZ_UI";

typedef struct {
    enum ScreensEnum id;
    int32_t width;
    int32_t height;
} eez_screen_info_t;

#define EEZ_SCREEN_COUNT ((size_t)(_SCREEN_ID_LAST - _SCREEN_ID_FIRST + 1))

static eez_screen_info_t s_screens[EEZ_SCREEN_COUNT];

static lv_display_t *s_disp;
static lv_obj_t *s_black_screen;
static int s_current_screen_id = -1;

static void init_screen_table(void)
{
    for (size_t i = 0; i < EEZ_SCREEN_COUNT; i++) {
        s_screens[i].id = (enum ScreensEnum)(_SCREEN_ID_FIRST + i);
        s_screens[i].width = 0;
        s_screens[i].height = 0;
    }
}

static const eez_screen_info_t *find_screen_info(int screen_id)
{
    if (screen_id < _SCREEN_ID_FIRST || screen_id > _SCREEN_ID_LAST) {
        return NULL;
    }

    return &s_screens[screen_id - _SCREEN_ID_FIRST];
}

static lv_obj_t *get_screen_obj(const eez_screen_info_t *screen)
{
    if (!screen) {
        return NULL;
    }

    return ((lv_obj_t **)&objects)[screen->id - _SCREEN_ID_FIRST];
}

static void capture_generated_screen_sizes(void)
{
    for (size_t i = 0; i < EEZ_SCREEN_COUNT; i++) {
        lv_obj_t *screen = get_screen_obj(&s_screens[i]);
        if (!screen) {
            continue;
        }

        s_screens[i].width = lv_obj_get_width(screen);
        s_screens[i].height = lv_obj_get_height(screen);
        ESP_LOGI(TAG, "EEZ screen %d: %ldx%ld",
                 s_screens[i].id,
                 (long)s_screens[i].width,
                 (long)s_screens[i].height);
    }
}

static void rotate_portrait_fullscreen_image(lv_obj_t *screen,
                                             int32_t phys_w,
                                             int32_t phys_h)
{
    if (!screen || lv_obj_get_child_count(screen) == 0) {
        return;
    }

    lv_obj_t *img = lv_obj_get_child(screen, 0);
    if (!lv_obj_check_type(img, &lv_image_class)) {
        return;
    }

    const int32_t img_w = lv_image_get_src_width(img);
    const int32_t img_h = lv_image_get_src_height(img);
    if (img_w != phys_w || img_h != phys_h) {
        return;
    }

    lv_obj_set_size(screen, phys_h, phys_w);
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_rotation(img, EEZ_UI_PORTRAIT_IMAGE_ROTATION);

#if EEZ_UI_PORTRAIT_IMAGE_ROTATION == 900
    lv_obj_set_pos(img, img_h, 0);
#elif EEZ_UI_PORTRAIT_IMAGE_ROTATION == 2700
    lv_obj_set_pos(img, 0, img_w);
#else
    lv_obj_set_pos(img, 0, 0);
#endif

    ESP_LOGI(TAG, "Rotated portrait image %ldx%ld into landscape frame %ldx%ld",
             (long)img_w, (long)img_h, (long)phys_h, (long)phys_w);
}

static void normalize_generated_screen_orientations(void)
{
#if EEZ_UI_FORCE_LANDSCAPE_INSTALL
    const int32_t phys_w = lv_display_get_original_horizontal_resolution(s_disp);
    const int32_t phys_h = lv_display_get_original_vertical_resolution(s_disp);

    for (size_t i = 0; i < EEZ_SCREEN_COUNT; i++) {
        if (s_screens[i].width == phys_w && s_screens[i].height == phys_h) {
            rotate_portrait_fullscreen_image(get_screen_obj(&s_screens[i]),
                                             phys_w, phys_h);
            s_screens[i].width = phys_h;
            s_screens[i].height = phys_w;
            ESP_LOGI(TAG, "Screen %d forced to landscape %ldx%ld",
                     s_screens[i].id,
                     (long)s_screens[i].width,
                     (long)s_screens[i].height);
        }
    }
#endif
}

static lv_display_rotation_t rotation_for_screen(const eez_screen_info_t *screen)
{
    const int32_t phys_w = lv_display_get_original_horizontal_resolution(s_disp);
    const int32_t phys_h = lv_display_get_original_vertical_resolution(s_disp);

    if (screen->width == phys_w && screen->height == phys_h) {
        return LV_DISPLAY_ROTATION_0;
    }

    if (screen->width == phys_h && screen->height == phys_w) {
        return EEZ_UI_LANDSCAPE_ROTATION;
    }

    ESP_LOGW(TAG, "Screen %d has unexpected size %ldx%ld for display %ldx%ld",
             screen->id, (long)screen->width, (long)screen->height,
             (long)phys_w, (long)phys_h);
    return LV_DISPLAY_ROTATION_0;
}

static void set_screen_orientation(const eez_screen_info_t *screen)
{
    const lv_display_rotation_t rotation = rotation_for_screen(screen);

    if (lv_display_get_rotation(s_disp) != rotation) {
        ESP_LOGI(TAG, "Screen %d %ldx%ld: rotation %d",
                 screen->id, (long)screen->width, (long)screen->height,
                 (int)rotation);
        lv_display_set_rotation(s_disp, rotation);
    }
}

static void configure_screen_root(const eez_screen_info_t *info)
{
    lv_obj_t *screen = get_screen_obj(info);
    if (!screen) {
        return;
    }

    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, info->width, info->height);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
}

static void configure_generated_screens(void)
{
    for (size_t i = 0; i < EEZ_SCREEN_COUNT; i++) {
        configure_screen_root(&s_screens[i]);
    }
}

static void prepare_screen_for_load(const eez_screen_info_t *screen,
                                    lv_obj_t *screen_obj)
{
    lv_obj_set_size(screen_obj,
                    lv_display_get_horizontal_resolution(s_disp),
                    lv_display_get_vertical_resolution(s_disp));
    lv_obj_set_pos(screen_obj, 0, 0);
    lv_obj_update_layout(screen_obj);
    ESP_LOGI(TAG, "Loading screen %d as %ldx%ld",
             screen->id,
             (long)lv_obj_get_width(screen_obj),
             (long)lv_obj_get_height(screen_obj));
}

static void ensure_black_screen(void)
{
    if (s_black_screen) {
        return;
    }

    s_black_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_black_screen);
    lv_obj_set_style_bg_color(s_black_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_black_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_black_screen, LV_OBJ_FLAG_SCROLLABLE);
}

static void show_black_frame(void)
{
    ensure_black_screen();

    lv_obj_set_size(s_black_screen,
                    lv_display_get_horizontal_resolution(s_disp),
                    lv_display_get_vertical_resolution(s_disp));
    lv_screen_load(s_black_screen);

#if EEZ_UI_REFRESH_IMMEDIATELY
    lv_refr_now(s_disp);
#endif
}

#if UI_BACKGROUND_INTERCEPT_ENABLE
typedef struct {
    const lv_img_dsc_t *img;
    lv_obj_t *obj;
    uint32_t child_count;
    uint32_t overlay_count;
} direct_background_info_t;

static bool is_direct_background_descriptor(const lv_img_dsc_t *img)
{
    if (!display_direct_image_is_rgb565_plain(img)) {
        return false;
    }

    const uint32_t src_w = img->header.w;
    const uint32_t src_h = img->header.h;
    const bool native_size =
        src_w == DISPLAY_DIRECT_NATIVE_H_RES &&
        src_h == DISPLAY_DIRECT_NATIVE_V_RES;
    const bool rotated_size =
        src_w == DISPLAY_DIRECT_NATIVE_V_RES &&
        src_h == DISPLAY_DIRECT_NATIVE_H_RES;

    return native_size || rotated_size;
}

static const lv_img_dsc_t *get_direct_background_descriptor(lv_obj_t *obj)
{
    if (!obj || !lv_obj_check_type(obj, &lv_image_class)) {
        return NULL;
    }

    const void *src = lv_image_get_src(obj);
    const lv_img_dsc_t *img = (const lv_img_dsc_t *)src;
    if (!is_direct_background_descriptor(img)) {
        return NULL;
    }

    return img;
}

static bool child_is_empty_image(lv_obj_t *obj)
{
    return obj &&
           lv_obj_check_type(obj, &lv_image_class) &&
           lv_image_get_src(obj) == NULL;
}

static direct_background_info_t find_direct_background(lv_obj_t *screen_obj)
{
    direct_background_info_t info = { 0 };

    if (!screen_obj) {
        return info;
    }

    info.child_count = (uint32_t)lv_obj_get_child_count(screen_obj);

    for (uint32_t i = 0; i < info.child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen_obj, i);
        const lv_img_dsc_t *img = get_direct_background_descriptor(child);
        if (!img) {
            continue;
        }

        if (!info.img) {
            info.img = img;
            info.obj = child;
        } else {
            ESP_LOGW(TAG,
                     "Screen has more than one direct-compatible image; using first one as background");
        }
    }

    for (uint32_t i = 0; i < info.child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen_obj, i);
        if (!child || child == info.obj || child_is_empty_image(child)) {
            continue;
        }
        info.overlay_count++;
    }

    if (!info.img && info.child_count > 0) {
        ESP_LOGW(TAG,
                 "Direct background skipped: no fullscreen plain RGB565 image child found");
    }

    return info;
}

static void clear_screen_lvgl_background_image(lv_obj_t *screen_obj)
{
    if (!screen_obj) {
        return;
    }

    lv_obj_set_style_bg_image_src(screen_obj, NULL, 0);
    lv_obj_set_style_bg_image_opa(screen_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_tiled(screen_obj, false, 0);
}

static void set_screen_plain_background(lv_obj_t *screen_obj, bool transparent)
{
    (void)transparent;
    if (!screen_obj) {
        return;
    }

    /* Keep fallback opaque black.  The real photo is drawn by the direct path,
     * and direct overlay regions are restored from s_clean_native_frame.  If
     * LVGL ever has to redraw the root during OTA/SWD/DSI contention, an
     * opaque black fallback is much less confusing than the default light-blue
     * theme surface. */
    lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_obj, LV_OPA_COVER, 0);
}

#define EEZ_UI_NEEDS_LVGL_OVERLAY_BG_COPY \
    (P4_LVGL_WIDGET_CROP_MIRROR_LR || P4_LVGL_WIDGET_CROP_MIRROR_TB || \
     EEZ_UI_LVGL_CROP_ROTATE_180)

#if EEZ_UI_NEEDS_LVGL_OVERLAY_BG_COPY
static uint16_t *s_lvgl_bg_pixels;
static size_t s_lvgl_bg_capacity_pixels;
static const lv_img_dsc_t *s_lvgl_bg_source;
static lv_img_dsc_t s_lvgl_bg_img;
#endif
static lv_obj_t *s_lvgl_overlay_bg_obj;
static const lv_img_dsc_t *s_lvgl_overlay_bg_original;

#if EEZ_UI_NEEDS_LVGL_OVERLAY_BG_COPY
static void invalidate_cached_lvgl_overlay_background(void)
{
    s_lvgl_bg_source = NULL;
}

static bool ensure_lvgl_overlay_buffer(size_t pixel_count)
{
    if (s_lvgl_bg_pixels && s_lvgl_bg_capacity_pixels >= pixel_count) {
        return true;
    }

    if (s_lvgl_bg_pixels) {
        heap_caps_free(s_lvgl_bg_pixels);
        s_lvgl_bg_pixels = NULL;
        s_lvgl_bg_capacity_pixels = 0;
        invalidate_cached_lvgl_overlay_background();
    }

    s_lvgl_bg_pixels = heap_caps_malloc(pixel_count * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lvgl_bg_pixels) {
        ESP_LOGE(TAG,
                 "Failed to allocate LVGL overlay background buffer pixels=%lu bytes=%lu",
                 (unsigned long)pixel_count,
                 (unsigned long)(pixel_count * sizeof(uint16_t)));
        return false;
    }

    s_lvgl_bg_capacity_pixels = pixel_count;
    ESP_LOGI(TAG,
             "Allocated LVGL overlay background buffer pixels=%lu bytes=%lu ptr=%p",
             (unsigned long)pixel_count,
             (unsigned long)(pixel_count * sizeof(uint16_t)),
             s_lvgl_bg_pixels);
    return true;
}
#endif

static const lv_img_dsc_t *prepare_lvgl_overlay_background_descriptor(const lv_img_dsc_t *img)
{
    if (!display_direct_image_is_rgb565_plain(img)) {
        return NULL;
    }

#if !EEZ_UI_NEEDS_LVGL_OVERLAY_BG_COPY
    return img;
#else
    const uint32_t src_w = img->header.w;
    const uint32_t src_h = img->header.h;
    const size_t pixel_count = (size_t)src_w * (size_t)src_h;
    const uint16_t *src = (const uint16_t *)img->data;

    if (!ensure_lvgl_overlay_buffer(pixel_count)) {
        return NULL;
    }

    if (s_lvgl_bg_source != img) {
        for (uint32_t y = 0; y < src_h; y++) {
            const uint32_t src_y =
#if EEZ_UI_LVGL_CROP_ROTATE_180
                (src_h - 1U - y);
#elif P4_LVGL_WIDGET_CROP_MIRROR_TB
                (src_h - 1U - y);
#else
                y;
#endif
            const size_t dst_row = (size_t)y * src_w;
            const size_t src_row = (size_t)src_y * src_w;

            for (uint32_t x = 0; x < src_w; x++) {
                const uint32_t src_x =
#if EEZ_UI_LVGL_CROP_ROTATE_180
                    (src_w - 1U - x);
#elif P4_LVGL_WIDGET_CROP_MIRROR_LR
                    (src_w - 1U - x);
#else
                    x;
#endif
                s_lvgl_bg_pixels[dst_row + x] = src[src_row + src_x];
            }
        }

        s_lvgl_bg_img = *img;
        s_lvgl_bg_img.data = (const uint8_t *)s_lvgl_bg_pixels;
        s_lvgl_bg_img.data_size = pixel_count * sizeof(uint16_t);
        s_lvgl_bg_source = img;

        ESP_LOGI(TAG,
                 "Prepared LVGL overlay background: %lux%lu mirror_lr=%d mirror_tb=%d rotate180_legacy=%d source=%p overlay=%p",
                 (unsigned long)src_w,
                 (unsigned long)src_h,
                 P4_LVGL_WIDGET_CROP_MIRROR_LR,
                 P4_LVGL_WIDGET_CROP_MIRROR_TB,
                 EEZ_UI_LVGL_CROP_ROTATE_180,
                 img,
                 &s_lvgl_bg_img);
    }

    return &s_lvgl_bg_img;
#endif
}

static void restore_lvgl_overlay_background_image_source(void)
{
    if (s_lvgl_overlay_bg_obj && s_lvgl_overlay_bg_original) {
        lv_image_set_src(s_lvgl_overlay_bg_obj, s_lvgl_overlay_bg_original);
        s_lvgl_overlay_bg_obj = NULL;
        s_lvgl_overlay_bg_original = NULL;
    }
}

static bool set_screen_lvgl_crop_background(lv_obj_t *screen_obj,
                                            const lv_img_dsc_t *img)
{
    if (!screen_obj || !img) {
        return false;
    }

    const lv_img_dsc_t *crop_img = prepare_lvgl_overlay_background_descriptor(img);
    if (!crop_img) {
        clear_screen_lvgl_background_image(screen_obj);
        set_screen_plain_background(screen_obj, false);
        return false;
    }

    /*
     * The full background is already drawn by ui_background_draw_synced().
     * This style background is not for the full page load; load invalidation is
     * suppressed below.  It is only a public-LVGL hook that gives redraws of
     * small widget rectangles the correct image pixels underneath transparent
     * and anti-aliased widget pixels.
     *
     * Important: use the orientation-corrected crop descriptor, not the original
     * EEZ image descriptor. Otherwise LVGL samples the right-looking image in
     * the wrong coordinate system, and widget rectangles duplicate background
     * details from mirrored places.
     */
    lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(screen_obj, crop_img, 0);
    lv_obj_set_style_bg_image_opa(screen_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(screen_obj, false, 0);
    return true;
}

static void restore_background_images_for_lvgl(lv_obj_t *screen_obj)
{
    restore_lvgl_overlay_background_image_source();

    if (!screen_obj) {
        return;
    }

    const uint32_t child_count = (uint32_t)lv_obj_get_child_count(screen_obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen_obj, i);
        if (get_direct_background_descriptor(child)) {
            lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }

    clear_screen_lvgl_background_image(screen_obj);
    set_screen_plain_background(screen_obj, false);
}

static bool sync_direct_background_image_for_lvgl_overlays(const direct_background_info_t *bg)
{
    /*
     * Legacy fallback hook.  Do NOT replace the generated full-screen lv_image
     * source with a mirrored descriptor: LVGL may redraw that whole image after
     * the direct frame and the user sees the photo flip.
     *
     * Correct mode is the style crop provider: hide the generated lv_image, keep
     * the direct background untouched, and let LVGL sample the mirrored crop only
     * when it redraws widget rectangles.
     */
    if (bg && bg->obj) {
        lv_obj_clear_flag(bg->obj, LV_OBJ_FLAG_HIDDEN);
    }
    return false;
}

static void hide_direct_background_image(const direct_background_info_t *bg)
{
    if (bg && bg->obj) {
        lv_obj_add_flag(bg->obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void invalidate_overlay_children(lv_obj_t *screen_obj,
                                        const direct_background_info_t *bg)
{
    if (!screen_obj || !bg) {
        return;
    }

    for (uint32_t i = 0; i < bg->child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen_obj, i);
        if (!child || child == bg->obj || child_is_empty_image(child)) {
            continue;
        }

        lv_obj_invalidate(child);
    }
}

static bool draw_screen_background_direct(int screen_id,
                                          lv_obj_t *screen_obj,
                                          direct_background_info_t *ret_bg)
{
    direct_background_info_t bg = find_direct_background(screen_obj);
    if (ret_bg) {
        *ret_bg = bg;
    }

    if (!bg.img) {
        return false;
    }

    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(s_disp);
    if (!panel) {
        ESP_LOGW(TAG, "Direct background skipped: display has no panel user_data");
        return false;
    }

    esp_err_t ret = ui_background_draw_synced(panel, (ui_background_id_t)screen_id, bg.img);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Direct background failed for screen %d: %s; falling back to LVGL",
                 screen_id,
                 esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG,
             "Direct background drawn for screen %d; child_count=%lu overlay_count=%lu",
             screen_id,
             (unsigned long)bg.child_count,
             (unsigned long)bg.overlay_count);
    return true;
}
#endif

static void eez_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_current_screen_id >= _SCREEN_ID_FIRST &&
        s_current_screen_id <= _SCREEN_ID_LAST) {
        tick_screen_by_id((enum ScreensEnum)s_current_screen_id);
    }
}

void eez_ui_port_init(lv_display_t *disp)
{
    s_disp = disp;
    lv_display_set_default(s_disp);

    ESP_LOGI(TAG,
             "UI background direct intercept: enabled=%d sync_mode=%d phase_us=%lu lead_us=%lu",
             UI_BACKGROUND_INTERCEPT_ENABLE,
             UI_BACKGROUND_DIRECT_SYNC_MODE,
             (unsigned long)UI_BACKGROUND_DIRECT_PHASE_AFTER_REFRESH_US,
             (unsigned long)UI_BACKGROUND_DIRECT_LEAD_BEFORE_NEXT_REFRESH_US);

    init_screen_table();
    create_screens();
    capture_generated_screen_sizes();
    normalize_generated_screen_orientations();
    configure_generated_screens();
    eez_ui_runtime_init();
    eez_ui_port_load_screen(SCREEN_ID_PAGE1);

    lv_timer_create(eez_tick_cb, EEZ_UI_TICK_PERIOD_MS, NULL);
}

void eez_ui_port_load_screen(int screen_id)
{
    if (screen_id == s_current_screen_id) {
        return;
    }

    const eez_screen_info_t *screen = find_screen_info(screen_id);
    if (!screen) {
        ESP_LOGW(TAG, "Ignoring unknown screen id %d", screen_id);
        return;
    }

    lv_obj_t *screen_obj = get_screen_obj(screen);
    if (!screen_obj) {
        ESP_LOGW(TAG, "Screen object %d is not created", screen_id);
        return;
    }

    const bool rotation_changes =
        lv_display_get_rotation(s_disp) != rotation_for_screen(screen);

#if EEZ_UI_BLACKOUT_ON_ROTATION_CHANGE
    if (rotation_changes) {
        show_black_frame();
    }
#endif

    display_experiment_backlight_blank_before_switch();

    set_screen_orientation(screen);
    prepare_screen_for_load(screen, screen_obj);

#if EEZ_UI_BLACKOUT_ON_ROTATION_CHANGE
    if (rotation_changes) {
        show_black_frame();
    }
#endif

#if UI_BACKGROUND_INTERCEPT_ENABLE
    restore_background_images_for_lvgl(screen_obj);
#endif

    bool direct_background_drawn = false;
#if UI_BACKGROUND_INTERCEPT_ENABLE
    direct_background_info_t direct_bg = { 0 };
    direct_background_drawn = draw_screen_background_direct(screen_id,
                                                            screen_obj,
                                                            &direct_bg);
    if (direct_background_drawn) {
        const bool has_generated_overlays = direct_bg.overlay_count > 0U;
        const bool has_static_runtime_overlay =
            (EEZ_UI_STATIC_RUNTIME_OVERLAY_NEEDS_CROP_PROVIDER != 0);
        const bool has_lvgl_overlays = has_generated_overlays || has_static_runtime_overlay;
        const bool use_crop_provider = has_lvgl_overlays &&
                                       (EEZ_UI_ENABLE_LVGL_CROP_PROVIDER != 0);

        if (!has_lvgl_overlays || use_crop_provider ||
            (EEZ_UI_HIDE_DIRECT_BG_IMAGE_WITH_OVERLAYS != 0)) {
            hide_direct_background_image(&direct_bg);
        } else {
            /*
             * Fallback overlay mode: keep the generated lv_image visible.
             * Do not mirror/replace this full-screen image here, otherwise the
             * entire background can visibly flip after the direct frame.
             * Correct crop-only mirroring is handled by the style crop provider
             * when it is enabled.
             */
            const bool synced = sync_direct_background_image_for_lvgl_overlays(&direct_bg);
            ESP_LOGI(TAG,
                     "Screen %d keeps generated lv_image visible for generated=%lu static=%d overlay object(s); LVGL crop provider disabled; overlay_bg_synced=%d",
                     screen_id,
                     (unsigned long)direct_bg.overlay_count,
                     has_static_runtime_overlay,
                     synced);
        }

        if (use_crop_provider) {
            const bool crop_provider_ready =
                set_screen_lvgl_crop_background(screen_obj, direct_bg.img);
            ESP_LOGI(TAG,
                     "Screen %d keeps LVGL bg-image crop provider for generated=%lu static=%d overlay object(s); mirror_lr=%d mirror_tb=%d ready=%d",
                     screen_id,
                     (unsigned long)direct_bg.overlay_count,
                     has_static_runtime_overlay,
                     P4_LVGL_WIDGET_CROP_MIRROR_LR,
                     P4_LVGL_WIDGET_CROP_MIRROR_TB,
                     crop_provider_ready);
        } else {
            clear_screen_lvgl_background_image(screen_obj);
            set_screen_plain_background(screen_obj, !has_lvgl_overlays);
        }
    }
#endif

    s_current_screen_id = screen_id;

#if EEZ_UI_SUPPRESS_LOAD_INVALIDATION
    lv_display_enable_invalidation(s_disp, false);
#endif

    lv_screen_load(screen_obj);

#if UI_BACKGROUND_INTERCEPT_ENABLE
    const bool has_static_runtime_overlay_for_refresh =
        (EEZ_UI_STATIC_RUNTIME_OVERLAY_NEEDS_CROP_PROVIDER != 0);
    const bool has_generated_overlays_for_refresh =
        direct_background_drawn && (direct_bg.overlay_count > 0U);
    const bool needs_lvgl_post_draw =
        !direct_background_drawn ||
        has_generated_overlays_for_refresh ||
        has_static_runtime_overlay_for_refresh;
#endif

#if EEZ_UI_SUPPRESS_LOAD_INVALIDATION
    lv_display_enable_invalidation(s_disp, true);
    if (!direct_background_drawn) {
        lv_obj_invalidate(screen_obj);
    }
#if UI_BACKGROUND_INTERCEPT_ENABLE
    else if (has_generated_overlays_for_refresh) {
        invalidate_overlay_children(screen_obj, &direct_bg);
    }
#endif
#endif

    /*
     * The FPS counter and WiFi status LED live on the persistent LVGL top
     * layer.  Invalidate them only after invalidation has been re-enabled;
     * otherwise the request is lost and the direct full-frame background can
     * leave them erased until their next periodic timer.
     */
    eez_ui_runtime_invalidate_static_overlay();

#if EEZ_UI_REFRESH_IMMEDIATELY
#if UI_BACKGROUND_INTERCEPT_ENABLE
    if (needs_lvgl_post_draw) {
        lv_refr_now(s_disp);
    }
#else
    lv_refr_now(s_disp);
#endif
#endif

    display_experiment_backlight_restore_after_switch();
}

void action_page_switch(lv_event_t *e)
{
    const uintptr_t raw_screen_id = (uintptr_t)lv_event_get_user_data(e);
    eez_ui_port_load_screen((int)raw_screen_id);
}

void action_page2(lv_event_t *e)
{
    (void)e;
}
