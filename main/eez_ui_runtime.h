#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime layer for generated EEZ Studio screens.
 *
 * Keep generated files in src/ui untouched.  After create_screens() this
 * module attaches project-owned behaviour, styles, and system widgets to the
 * already-created LVGL objects.
 */
void eez_ui_runtime_init(void);

/*
 * Repaint persistent runtime overlay objects after a direct framebuffer
 * background write or a generated screen switch.  These objects may live on
 * the LVGL top layer in debug mode, but the production visible path is the
 * direct RGB565 overlay baked into the framebuffer.
 */
void eez_ui_runtime_invalidate_static_overlay(void);

/* Native RGB565 direct overlay support.  The direct background renderer calls
 * this before submitting the frame, so FPS/LED are present in the very first
 * displayed frame after a page switch.  Coordinates are native framebuffer
 * coordinates: 480x800 RGB565. */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} eez_runtime_overlay_region_t;

bool eez_ui_runtime_direct_overlay_enabled(void);
size_t eez_ui_runtime_get_direct_overlay_regions(eez_runtime_overlay_region_t *regions,
                                                 size_t max_regions);
void eez_ui_runtime_draw_direct_overlay_rgb565_native(uint16_t *dst,
                                                      int stride_pixels,
                                                      int clip_x,
                                                      int clip_y,
                                                      int clip_w,
                                                      int clip_h);

#ifdef __cplusplus
}
#endif
