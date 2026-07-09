#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_veber_1;
extern const lv_img_dsc_t img_frank_1;
extern const lv_img_dsc_t img_lawson_2;
extern const lv_img_dsc_t img_new_lil_2;
extern const lv_img_dsc_t img_miriam_1;
extern const lv_img_dsc_t img_art_deco_2;
extern const lv_img_dsc_t img_helix_nabula_1;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[7];

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/