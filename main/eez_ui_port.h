#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void eez_ui_port_init(lv_display_t *disp);
void eez_ui_port_load_screen(int screen_id);

#ifdef __cplusplus
}
#endif
