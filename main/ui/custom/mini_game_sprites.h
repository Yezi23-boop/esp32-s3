#ifndef MINI_GAME_SPRITES_H
#define MINI_GAME_SPRITES_H

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#include <stdint.h>

extern const lv_image_dsc_t img_bird_mid;
extern const lv_image_dsc_t img_bird_up;
extern const lv_image_dsc_t img_bird_down;
extern const lv_image_dsc_t img_dino_run1;
extern const lv_image_dsc_t img_dino_run2;
extern const lv_image_dsc_t img_dino_jump;
extern const lv_image_dsc_t img_dino_dead;
extern const lv_image_dsc_t img_cactus;
extern const lv_image_dsc_t img_pterosaur1;
extern const lv_image_dsc_t img_pterosaur2;
extern const lv_image_dsc_t img_cloud;

#endif /* MINI_GAME_SPRITES_H */
