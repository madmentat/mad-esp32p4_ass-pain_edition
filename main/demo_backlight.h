#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t demo_backlight_set(int pct);
int demo_backlight_get(void);

bool demo_screen_backlight_blank_enabled(void);
int demo_screen_backlight_blank_level_pct(void);
int demo_screen_backlight_restore_level_pct(void);
uint32_t demo_screen_backlight_blank_before_ms(void);
uint32_t demo_screen_backlight_blank_after_ms(void);

#ifdef __cplusplus
}
#endif
