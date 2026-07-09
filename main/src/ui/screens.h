#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_PAGE1 = 1,
    SCREEN_ID_PAGE2 = 2,
    SCREEN_ID_PAGE3 = 3,
    SCREEN_ID_PAGE4 = 4,
    SCREEN_ID_PAGE5 = 5,
    SCREEN_ID_PAGE6 = 6,
    SCREEN_ID_PAGE7 = 7,
    _SCREEN_ID_LAST = 7
};

typedef struct _objects_t {
    lv_obj_t *page1;
    lv_obj_t *page2;
    lv_obj_t *page3;
    lv_obj_t *page4;
    lv_obj_t *page5;
    lv_obj_t *page6;
    lv_obj_t *page7;
    lv_obj_t *lawson_2;
} objects_t;

extern objects_t objects;

void create_screen_page1();
void tick_screen_page1();

void create_screen_page2();
void tick_screen_page2();

void create_screen_page3();
void tick_screen_page3();

void create_screen_page4();
void tick_screen_page4();

void create_screen_page5();
void tick_screen_page5();

void create_screen_page6();
void tick_screen_page6();

void create_screen_page7();
void tick_screen_page7();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/