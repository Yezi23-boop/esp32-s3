/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#include "events_init.h"
#include <stdio.h>
#include "lvgl.h"

#if LV_USE_GUIDER_SIMULATOR && LV_USE_FREEMASTER
#include "freemaster_client.h"
#endif

#include "custom.h" // 包含自定义函数的头文件
#include "danger_detection_controller.h"
#include "main_dropdown_controller.h"
#include "memory_watch_controller.h"
#include "mini_games_controller.h"
#include "ui_refresh_policy.h"
static int32_t status_bar_y_pos;
static int32_t start_y;
static bool is_dragging;
static bool status_top = 0;

#define SCREEN_MAIN_WIDTH_PX 410
#define SCREEN_MAIN_SWITCH_THRESHOLD_PX 70
#define SCREEN_MAIN_SWITCH_ANIM_MS 160

static int32_t screen_main_swipe_start_x;
static int32_t screen_main_swipe_start_y;
static bool screen_main_swipe_tracking;
static bool screen_main_function_page_active;
static bool screen_main_page_animating;
static bool screen_main_page_target_function;
static uint8_t screen_main_page_anim_pending;

// 平滑收回状态栏到 -height 隐藏处
static void screen_main_dropdown_hide(void)
{
    if (guider_ui.screen_main_Dropdown_menu == NULL) return;
    int32_t status_bar_height = lv_obj_get_height(guider_ui.screen_main_Dropdown_menu);
    if (status_bar_height <= 0) status_bar_height = 200;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, guider_ui.screen_main_Dropdown_menu);
    lv_anim_set_values(&anim, lv_obj_get_y(guider_ui.screen_main_Dropdown_menu), -status_bar_height);
    lv_anim_set_time(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&anim);

    status_top = 0;
    status_bar_y_pos = -status_bar_height;
}

// 平滑展开状态栏到 0 顶端显示处
static void screen_main_dropdown_show(void)
{
    if (guider_ui.screen_main_Dropdown_menu == NULL) return;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, guider_ui.screen_main_Dropdown_menu);
    lv_anim_set_values(&anim, lv_obj_get_y(guider_ui.screen_main_Dropdown_menu), 0);
    lv_anim_set_time(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_start(&anim);

    status_top = 1;
    status_bar_y_pos = 0;
}

static void screen_main_page_anim_completed(lv_anim_t *anim)
{
    (void)anim;

    if (screen_main_page_anim_pending > 0) {
        screen_main_page_anim_pending--;
    }
    if (screen_main_page_anim_pending > 0) {
        return;
    }

    screen_main_page_animating = false;
    screen_main_function_page_active = screen_main_page_target_function;
    if (screen_main_function_page_active) {
        lv_obj_add_flag(guider_ui.screen_main_tileview_1_main, LV_OBJ_FLAG_HIDDEN);
    } else if (guider_ui.screen_main_tileview_1_Function) {
        lv_obj_add_flag(guider_ui.screen_main_tileview_1_Function, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_main_start_x_anim(lv_obj_t *obj, int32_t from, int32_t to)
{
    lv_anim_del(obj, (lv_anim_exec_xcb_t)lv_obj_set_x);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_time(&anim, SCREEN_MAIN_SWITCH_ANIM_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_completed_cb(&anim, screen_main_page_anim_completed);
    lv_anim_start(&anim);
}

static void screen_main_show_function_page(void)
{
    if (screen_main_function_page_active || screen_main_page_animating) {
        return;
    }

    setup_scr_screen_main_function_page(&guider_ui);
    lv_obj_clear_flag(guider_ui.screen_main_tileview_1_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(guider_ui.screen_main_tileview_1_Function, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(guider_ui.screen_main_tileview_1_main, 0);
    lv_obj_set_x(guider_ui.screen_main_tileview_1_Function, SCREEN_MAIN_WIDTH_PX);
    lv_obj_move_foreground(guider_ui.screen_main_top_grab_area);
    lv_obj_move_foreground(guider_ui.screen_main_Dropdown_menu);

    screen_main_page_animating = true;
    screen_main_page_target_function = true;
    screen_main_page_anim_pending = 2;
    screen_main_start_x_anim(guider_ui.screen_main_tileview_1_main, 0, -SCREEN_MAIN_WIDTH_PX);
    screen_main_start_x_anim(guider_ui.screen_main_tileview_1_Function, SCREEN_MAIN_WIDTH_PX, 0);
}

static void screen_main_show_watch_page(void)
{
    if (!screen_main_function_page_active || screen_main_page_animating || !guider_ui.screen_main_tileview_1_Function) {
        return;
    }

    lv_obj_clear_flag(guider_ui.screen_main_tileview_1_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(guider_ui.screen_main_tileview_1_Function, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(guider_ui.screen_main_tileview_1_main, -SCREEN_MAIN_WIDTH_PX);
    lv_obj_set_x(guider_ui.screen_main_tileview_1_Function, 0);
    lv_obj_move_foreground(guider_ui.screen_main_top_grab_area);
    lv_obj_move_foreground(guider_ui.screen_main_Dropdown_menu);

    screen_main_page_animating = true;
    screen_main_page_target_function = false;
    screen_main_page_anim_pending = 2;
    screen_main_start_x_anim(guider_ui.screen_main_tileview_1_main, -SCREEN_MAIN_WIDTH_PX, 0);
    screen_main_start_x_anim(guider_ui.screen_main_tileview_1_Function, 0, SCREEN_MAIN_WIDTH_PX);
}

static bool screen_main_is_horizontal_swipe(int32_t dx, int32_t dy)
{
    return LV_ABS(dx) >= SCREEN_MAIN_SWITCH_THRESHOLD_PX && LV_ABS(dx) > (LV_ABS(dy) * 2);
}

static void screen_main_watch_swipe_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &p);
        screen_main_swipe_start_x = p.x;
        screen_main_swipe_start_y = p.y;
        screen_main_swipe_tracking = true;
        break;
    case LV_EVENT_RELEASED:
        if (!screen_main_swipe_tracking) {
            break;
        }
        screen_main_swipe_tracking = false;
        lv_indev_get_point(lv_indev_get_act(), &p);
        if (screen_main_is_horizontal_swipe(p.x - screen_main_swipe_start_x, p.y - screen_main_swipe_start_y) &&
            p.x < screen_main_swipe_start_x) {
            screen_main_show_function_page();
        }
        break;
    default:
        break;
    }
}

static void screen_main_function_swipe_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &p);
        screen_main_swipe_start_x = p.x;
        screen_main_swipe_start_y = p.y;
        screen_main_swipe_tracking = true;
        break;
    case LV_EVENT_RELEASED:
        if (!screen_main_swipe_tracking) {
            break;
        }
        screen_main_swipe_tracking = false;
        lv_indev_get_point(lv_indev_get_act(), &p);
        if (screen_main_is_horizontal_swipe(p.x - screen_main_swipe_start_x, p.y - screen_main_swipe_start_y) &&
            p.x > screen_main_swipe_start_x) {
            screen_main_show_watch_page();
        }
        break;
    case LV_EVENT_CLICKED:
    {
        if (status_top) {
            screen_main_dropdown_hide();
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_bluetooth_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_CLICKED:
    {
        main_dropdown_controller_handle_bluetooth_click();
        break;
    }
    default:
        break;
    }
}

static void screen_main_wifi_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_CLICKED:
    {
        main_dropdown_controller_handle_wifi_click();
        break;
    }
    default:
        break;
    }
}

static void screen_main_cont_1_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_CLICKED:
    {
        screen_main_dropdown_hide();
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_5_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            ui_load_scr_animation(&guider_ui, &guider_ui.screen_time, guider_ui.screen_time_del, &guider_ui.screen_main_del, setup_scr_screen_time, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, false, true);
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_2_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            ai_ui_open();
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_7_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            ui_load_scr_animation(&guider_ui, &guider_ui.screen_wallpaper, guider_ui.screen_wallpaper_del, &guider_ui.screen_main_del, setup_scr_screen_wallpaper, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, false, true);
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_6_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            danger_detection_ui_open();
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_4_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            mini_games_controller_open();
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_option_8_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static lv_point_t start_p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &start_p);
        break;
    case LV_EVENT_CLICKED:
    {
        lv_point_t curr_p;
        lv_indev_get_point(lv_indev_get_act(), &curr_p);
        if (LV_ABS(curr_p.x - start_p.x) <= 12 && LV_ABS(curr_p.y - start_p.y) <= 12) {
            memory_watch_controller_open();
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_dropdown_drag_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_point_t p;

    switch (code) {
    case LV_EVENT_PRESSED:
    {
        lv_indev_get_point(lv_indev_get_act(), &p);
        int32_t status_bar_height = lv_obj_get_height(guider_ui.screen_main_Dropdown_menu);
        if (status_bar_height <= 0) status_bar_height = 200;

        if (obj == guider_ui.screen_main_top_grab_area || status_top) {
            start_y = p.y;
            is_dragging = true;
        }
        break;
    }
    case LV_EVENT_PRESSING:
    {
        if (is_dragging) {
            lv_indev_get_point(lv_indev_get_act(), &p);
            int32_t drag_distance = p.y - start_y;
            int32_t status_bar_height = lv_obj_get_height(guider_ui.screen_main_Dropdown_menu);
            if (status_bar_height <= 0) status_bar_height = 200;

            int32_t base_y = status_top ? 0 : -status_bar_height;
            status_bar_y_pos = base_y + drag_distance;
            status_bar_y_pos = LV_MAX(status_bar_y_pos, -status_bar_height);
            status_bar_y_pos = LV_MIN(status_bar_y_pos, 0);

            lv_obj_set_pos(guider_ui.screen_main_Dropdown_menu, 0, status_bar_y_pos);
        }
        break;
    }
    case LV_EVENT_RELEASED:
    {
        if (is_dragging) {
            is_dragging = false;
            int32_t status_bar_height = lv_obj_get_height(guider_ui.screen_main_Dropdown_menu);
            if (status_bar_height <= 0) status_bar_height = 200;
            int32_t half_height = status_bar_height / 2;

            if (status_bar_y_pos > -half_height) {
                screen_main_dropdown_show();
            } else {
                screen_main_dropdown_hide();
            }
        }
        break;
    }
    default:
        break;
    }
}

static void screen_main_Brightness_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_VALUE_CHANGED:
    {
        int32_t slider_value = lv_slider_get_value(guider_ui.screen_main_Brightness);
        ui_refresh_policy_set_user_brightness_percent(slider_value);
        int32_t rotation_angle = (slider_value * 360) / 100;
        lv_img_set_pivot(guider_ui.screen_main_img_1, 23, 23);
        lv_img_set_angle(guider_ui.screen_main_img_1, rotation_angle * 10);
        break;
    }
    default:
        break;
    }
}

void events_init_screen_main (lv_ui *ui)
{
    screen_main_swipe_tracking = false;
    screen_main_function_page_active = false;
    screen_main_page_animating = false;
    screen_main_page_target_function = false;
    screen_main_page_anim_pending = 0;

    main_dropdown_controller_bind(ui);
    lv_obj_add_event_cb(ui->screen_main_cont_1, screen_main_cont_1_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_cont_1, screen_main_watch_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_digital_clock_1, screen_main_watch_swipe_event_handler, LV_EVENT_ALL, ui);
    
    lv_obj_add_event_cb(ui->screen_main_top_grab_area, screen_main_dropdown_drag_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Dropdown_menu, screen_main_dropdown_drag_event_handler, LV_EVENT_ALL, ui);
    
    lv_obj_add_event_cb(ui->screen_main_Wifi, screen_main_wifi_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Bluetooth, screen_main_bluetooth_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Brightness, screen_main_Brightness_event_handler, LV_EVENT_ALL, ui);
}

void events_init_screen_main_function (lv_ui *ui)
{
    lv_obj_add_event_cb(ui->screen_main_Function_main, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_1, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Heart_rate, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_2, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Xiao_Zhi, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_4, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Game, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_5, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_clock, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_6, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Microphone, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_7, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_seting, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_8, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_user, screen_main_function_swipe_event_handler, LV_EVENT_ALL, ui);

    lv_obj_add_event_cb(ui->screen_main_option_2, screen_main_option_2_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Xiao_Zhi, screen_main_option_2_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_5, screen_main_option_5_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_6, screen_main_option_6_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Microphone, screen_main_option_6_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_flag(ui->screen_main_option_4, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->screen_main_option_4, screen_main_option_4_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_Game, screen_main_option_4_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_option_7, screen_main_option_7_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_flag(ui->screen_main_option_8, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->screen_main_option_8, screen_main_option_8_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_main_user, screen_main_option_8_event_handler, LV_EVENT_ALL, ui);
}

static void screen_wallpaper_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_SCREEN_LOAD_START:
    {
        setup_horizontal_scroll(guider_ui.screen_wallpaper_cont_1);
        break;
    }
    default:
        break;
    }
}

static void screen_wallpaper_img_1_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_LONG_PRESSED_REPEAT:
    {
        ui_load_scr_animation(&guider_ui, &guider_ui.screen_main, guider_ui.screen_main_del, &guider_ui.screen_wallpaper_del, setup_scr_screen_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, true, true);
        lv_obj_set_style_bg_img_src(guider_ui.screen_main_cont_1, &_1_RGB565A8_410x502, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    }
    default:
        break;
    }
}

static void screen_wallpaper_img_2_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_LONG_PRESSED_REPEAT:
    {
        ui_load_scr_animation(&guider_ui, &guider_ui.screen_main, guider_ui.screen_main_del, &guider_ui.screen_wallpaper_del, setup_scr_screen_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, true, true);
        lv_obj_set_style_bg_img_src(guider_ui.screen_main_cont_1, &_2_RGB565A8_410x502, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    }
    default:
        break;
    }
}

static void screen_wallpaper_img_3_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_LONG_PRESSED_REPEAT:
    {
        ui_load_scr_animation(&guider_ui, &guider_ui.screen_main, guider_ui.screen_main_del, &guider_ui.screen_wallpaper_del, setup_scr_screen_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, true, true);
        lv_obj_set_style_bg_img_src(guider_ui.screen_main_cont_1, &_3_RGB565A8_410x502, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    }
    default:
        break;
    }
}

static void screen_wallpaper_img_4_event_handler (lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_LONG_PRESSED_REPEAT:
    {
        ui_load_scr_animation(&guider_ui, &guider_ui.screen_main, guider_ui.screen_main_del, &guider_ui.screen_wallpaper_del, setup_scr_screen_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 300, true, true);
        lv_obj_set_style_bg_img_src(guider_ui.screen_main_cont_1, &_4_RGB565A8_410x502, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    }
    default:
        break;
    }
}

void events_init_screen_wallpaper (lv_ui *ui)
{
    lv_obj_add_event_cb(ui->screen_wallpaper, screen_wallpaper_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_wallpaper_img_1, screen_wallpaper_img_1_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_wallpaper_img_2, screen_wallpaper_img_2_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_wallpaper_img_3, screen_wallpaper_img_3_event_handler, LV_EVENT_ALL, ui);
    lv_obj_add_event_cb(ui->screen_wallpaper_img_4, screen_wallpaper_img_4_event_handler, LV_EVENT_ALL, ui);
}

void events_init_screen_time (lv_ui *ui)
{
    (void)ui;
}


void events_init(lv_ui *ui)
{

}
