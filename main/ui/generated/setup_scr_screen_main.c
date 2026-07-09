/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#include "lvgl.h"
#include <stdio.h>
#include "gui_guider.h"
#include "events_init.h"
#include "widgets_init.h"
#include "custom.h"



int screen_main_digital_clock_1_min_value = 25;
int screen_main_digital_clock_1_hour_value = 11;
int screen_main_digital_clock_1_sec_value = 50;
void setup_scr_screen_main_function_page(lv_ui *ui)
{
    //Write codes screen_main_Function_viewport
    if (ui->screen_main_Function_main) {
        return;
    }

    ui->screen_main_tileview_1_Function = lv_obj_create(ui->screen_main_Contorl_Con);
    lv_obj_set_pos(ui->screen_main_tileview_1_Function, 410, 0);
    lv_obj_set_size(ui->screen_main_tileview_1_Function, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main_tileview_1_Function, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ui->screen_main_tileview_1_Function, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_tileview_1_Function, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_tileview_1_Function, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_tileview_1_Function, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_tileview_1_Function, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    ui->screen_main_Function_main = lv_obj_create(ui->screen_main_tileview_1_Function);
    lv_obj_set_pos(ui->screen_main_Function_main, 0, 0);
    lv_obj_set_size(ui->screen_main_Function_main, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main_Function_main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ui->screen_main_Function_main, LV_OBJ_FLAG_CLICKABLE);

    //Write style for screen_main_Function_main, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_Function_main, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_Function_main, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_Function_main, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Function_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_1
    ui->screen_main_option_1 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_1, 536, -240);
    lv_obj_set_size(ui->screen_main_option_1, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_1, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_1, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_1, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_1, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_1, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_1, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_1, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_1, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Heart_rate
    ui->screen_main_Heart_rate = lv_image_create(ui->screen_main_option_1);
    lv_obj_set_pos(ui->screen_main_Heart_rate, 13, 4);
    lv_obj_set_size(ui->screen_main_Heart_rate, 70, 70);
    lv_obj_add_flag(ui->screen_main_Heart_rate, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_Heart_rate, &_heart_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_Heart_rate, 50,50);
    lv_image_set_rotation(ui->screen_main_Heart_rate, 0);

    //Write style for screen_main_Heart_rate, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Heart_rate, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_Heart_rate, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_2
    ui->screen_main_option_2 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_2, 536, -80);
    lv_obj_set_size(ui->screen_main_option_2, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_2, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_2, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_2, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_2, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_2, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_2, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_2, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_2, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_2, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Xiao_Zhi
    ui->screen_main_Xiao_Zhi = lv_image_create(ui->screen_main_option_2);
    lv_obj_set_pos(ui->screen_main_Xiao_Zhi, 13, 4);
    lv_obj_set_size(ui->screen_main_Xiao_Zhi, 70, 70);
    lv_obj_add_flag(ui->screen_main_Xiao_Zhi, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_Xiao_Zhi, &_ai_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_Xiao_Zhi, 50,50);
    lv_image_set_rotation(ui->screen_main_Xiao_Zhi, 0);

    //Write style for screen_main_Xiao_Zhi, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Xiao_Zhi, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_Xiao_Zhi, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_4
    ui->screen_main_option_4 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_4, 536, 77);
    lv_obj_set_size(ui->screen_main_option_4, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_4, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_4, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_4, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_4, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_4, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_4, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_4, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_4, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_4, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Game
    ui->screen_main_Game = lv_image_create(ui->screen_main_option_4);
    lv_obj_set_pos(ui->screen_main_Game, 13, 4);
    lv_obj_set_size(ui->screen_main_Game, 70, 70);
    lv_obj_add_flag(ui->screen_main_Game, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_Game, &_game_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_Game, 50,50);
    lv_image_set_rotation(ui->screen_main_Game, 0);

    //Write style for screen_main_Game, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Game, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_Game, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_5
    ui->screen_main_option_5 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_5, 536, 235);
    lv_obj_set_size(ui->screen_main_option_5, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_5, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_5, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_5, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_5, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_5, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_5, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_5, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_5, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_5, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_5, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_5, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_5, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_5, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_5, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_5, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_5, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_5, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_clock
    ui->screen_main_clock = lv_image_create(ui->screen_main_option_5);
    lv_obj_set_pos(ui->screen_main_clock, 11, 3);
    lv_obj_set_size(ui->screen_main_clock, 70, 70);
    lv_obj_add_flag(ui->screen_main_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_clock, &_alarm_clock_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_clock, 50,50);
    lv_image_set_rotation(ui->screen_main_clock, 0);

    //Write style for screen_main_clock, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_clock, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_clock, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_6
    ui->screen_main_option_6 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_6, 534, 392);
    lv_obj_set_size(ui->screen_main_option_6, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_6, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_6, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_6, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_6, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_6, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_6, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_6, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_6, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_6, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_6, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_6, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_6, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_6, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_6, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_6, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_6, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_6, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Microphone
    ui->screen_main_Microphone = lv_image_create(ui->screen_main_option_6);
    lv_obj_set_pos(ui->screen_main_Microphone, 13, 3);
    lv_obj_set_size(ui->screen_main_Microphone, 70, 70);
    lv_obj_add_flag(ui->screen_main_Microphone, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_Microphone, &_Microphone_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_Microphone, 50,50);
    lv_image_set_rotation(ui->screen_main_Microphone, 0);

    //Write style for screen_main_Microphone, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Microphone, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_Microphone, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_7
    ui->screen_main_option_7 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_7, 535, 554);
    lv_obj_set_size(ui->screen_main_option_7, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_7, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_7, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_7, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_7, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_7, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_7, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_7, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_7, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_7, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_7, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_7, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_7, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_7, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_7, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_7, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_7, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_7, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_seting
    ui->screen_main_seting = lv_image_create(ui->screen_main_option_7);
    lv_obj_set_pos(ui->screen_main_seting, 13, 3);
    lv_obj_set_size(ui->screen_main_seting, 70, 70);
    lv_obj_add_flag(ui->screen_main_seting, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_seting, &_set_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_seting, 50,50);
    lv_image_set_rotation(ui->screen_main_seting, 0);

    //Write style for screen_main_seting, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_seting, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_seting, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_option_8
    ui->screen_main_option_8 = lv_obj_create(ui->screen_main_Function_main);
    lv_obj_set_pos(ui->screen_main_option_8, 531, 707);
    lv_obj_set_size(ui->screen_main_option_8, 256, 90);
    lv_obj_set_scrollbar_mode(ui->screen_main_option_8, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_option_8, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_option_8, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_option_8, 18, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_option_8, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_option_8, lv_color_hex(0x947cb6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_option_8, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_option_8, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_option_8, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_option_8, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_option_8, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_option_8, 6, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_option_8, lv_color_hex(0x7c73af), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_option_8, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(ui->screen_main_option_8, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_x(ui->screen_main_option_8, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_option_8, 8, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_user
    ui->screen_main_user = lv_image_create(ui->screen_main_option_8);
    lv_obj_set_pos(ui->screen_main_user, 13, 4);
    lv_obj_set_size(ui->screen_main_user, 70, 70);
    lv_obj_add_flag(ui->screen_main_user, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_user, &_me_RGB565A8_70x70);
    lv_image_set_pivot(ui->screen_main_user, 50,50);
    lv_image_set_rotation(ui->screen_main_user, 0);

    //Write style for screen_main_user, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_user, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_user, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of screen_main function page.
    setup_vertical_scroll(ui->screen_main_Function_main);
    events_init_screen_main_function(ui);
}
void setup_scr_screen_main(lv_ui *ui)
{
    //Write codes screen_main
    ui->screen_main = lv_obj_create(NULL);
    lv_obj_set_size(ui->screen_main, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Contorl_Con
    ui->screen_main_Contorl_Con = lv_obj_create(ui->screen_main);
    lv_obj_set_pos(ui->screen_main_Contorl_Con, 0, 0);
    lv_obj_set_size(ui->screen_main_Contorl_Con, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main_Contorl_Con, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_Contorl_Con, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_Contorl_Con, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_Contorl_Con, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_Contorl_Con, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Contorl_Con, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_tileview_1
    // 运行时性能补丁：保留 GUI Guider 旧句柄，移除全屏 tileview 的持续滚动成本。
    // 左右切页改由 events_init.c 的阈值手势触发，功能页按需创建并缓存。
    ui->screen_main_tileview_1 = ui->screen_main_Contorl_Con;
    ui->screen_main_tileview_1_main = lv_obj_create(ui->screen_main_Contorl_Con);
    lv_obj_set_pos(ui->screen_main_tileview_1_main, 0, 0);
    lv_obj_set_size(ui->screen_main_tileview_1_main, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main_tileview_1_main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ui->screen_main_tileview_1_main, LV_OBJ_FLAG_SCROLLABLE);
    ui->screen_main_tileview_1_Function = NULL;
    ui->screen_main_Function_main = NULL;

    //Write style for screen_main_tileview_1_main, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_tileview_1_main, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_tileview_1_main, lv_color_hex(0xf6f6f6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_tileview_1_main, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_tileview_1_main, 0, LV_PART_MAIN|LV_STATE_DEFAULT);


    //Write codes screen_main_cont_1
    ui->screen_main_cont_1 = lv_obj_create(ui->screen_main_tileview_1_main);
    lv_obj_set_pos(ui->screen_main_cont_1, 0, 0);
    lv_obj_set_size(ui->screen_main_cont_1, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_main_cont_1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ui->screen_main_cont_1, LV_OBJ_FLAG_CLICKABLE);

    //Write style for screen_main_cont_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_cont_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_cont_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_cont_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_src(ui->screen_main_cont_1, &_5_RGB565A8_410x502, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_opa(ui->screen_main_cont_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_image_recolor_opa(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_digital_clock_1
    static bool screen_main_digital_clock_1_timer_enabled = false;
    ui->screen_main_digital_clock_1 = lv_label_create(ui->screen_main_cont_1);
    lv_obj_set_pos(ui->screen_main_digital_clock_1, 95, 96);
    lv_obj_set_size(ui->screen_main_digital_clock_1, 251, 60);
    lv_label_set_text(ui->screen_main_digital_clock_1, "--:--");
    if (!screen_main_digital_clock_1_timer_enabled) {
        lv_timer_create(screen_main_digital_clock_1_timer, 1000, NULL);
        screen_main_digital_clock_1_timer_enabled = true;
    }
    screen_main_digital_clock_1_timer(NULL);

    //Write style for screen_main_digital_clock_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_radius(ui->screen_main_digital_clock_1, 42, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_main_digital_clock_1, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_digital_clock_1, &lv_font_montserratMedium_46, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_main_digital_clock_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->screen_main_digital_clock_1, 7, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_main_digital_clock_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_digital_clock_1, 128, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_digital_clock_1, lv_color_hex(0x585152), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_digital_clock_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_digital_clock_1, 7, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_digital_clock_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_digital_clock_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_digital_clock_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_digital_clock_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);



    //Write codes screen_main_top_grab_area
    ui->screen_main_top_grab_area = lv_obj_create(ui->screen_main);
    lv_obj_set_pos(ui->screen_main_top_grab_area, 0, 0);
    lv_obj_set_size(ui->screen_main_top_grab_area, 410, 70);
    lv_obj_set_scrollbar_mode(ui->screen_main_top_grab_area, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_top_grab_area, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_top_grab_area, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Dropdown_menu
    ui->screen_main_Dropdown_menu = lv_obj_create(ui->screen_main);
    lv_obj_set_pos(ui->screen_main_Dropdown_menu, 0, -200);
    lv_obj_set_size(ui->screen_main_Dropdown_menu, 410, 200);
    lv_obj_set_scrollbar_mode(ui->screen_main_Dropdown_menu, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_main_Dropdown_menu, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Dropdown_menu, 34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_Dropdown_menu, 102, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_Dropdown_menu, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_Dropdown_menu, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Dropdown_menu, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_Wifi
    ui->screen_main_Wifi = lv_imagebutton_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_Wifi, 21, 51);
    lv_obj_set_size(ui->screen_main_Wifi, 76, 76);
    lv_obj_add_flag(ui->screen_main_Wifi, LV_OBJ_FLAG_CHECKABLE);
    lv_imagebutton_set_src(ui->screen_main_Wifi, LV_IMAGEBUTTON_STATE_RELEASED, &_WIFI4_RGB565A8_76x76, NULL, NULL);
    lv_imagebutton_set_src(ui->screen_main_Wifi, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, &_WIFI2_RGB565A8_76x76, NULL, NULL);
    ui->screen_main_Wifi_label = lv_label_create(ui->screen_main_Wifi);
    lv_label_set_text(ui->screen_main_Wifi_label, "");
    lv_label_set_long_mode(ui->screen_main_Wifi_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_main_Wifi_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_main_Wifi, 0, LV_STATE_DEFAULT);

    //Write style for screen_main_Wifi, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_text_color(ui->screen_main_Wifi, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_Wifi, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_main_Wifi, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_Wifi, Part: LV_PART_MAIN, State: LV_STATE_PRESSED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_image_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui->screen_main_Wifi, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_font(ui->screen_main_Wifi, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_STATE_PRESSED);

    //Write style for screen_main_Wifi, Part: LV_PART_MAIN, State: LV_STATE_CHECKED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_image_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui->screen_main_Wifi, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui->screen_main_Wifi, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_STATE_CHECKED);

    //Write style for screen_main_Wifi, Part: LV_PART_MAIN, State: LV_IMAGEBUTTON_STATE_RELEASED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Wifi, 0, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);
    lv_obj_set_style_image_opa(ui->screen_main_Wifi, 255, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);

    //Write codes screen_main_Bluetooth
    ui->screen_main_Bluetooth = lv_imagebutton_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_Bluetooth, 121, 49);
    lv_obj_set_size(ui->screen_main_Bluetooth, 76, 76);
    lv_obj_add_flag(ui->screen_main_Bluetooth, LV_OBJ_FLAG_CHECKABLE);
    lv_imagebutton_set_src(ui->screen_main_Bluetooth, LV_IMAGEBUTTON_STATE_RELEASED, &_langya4_RGB565A8_76x76, NULL, NULL);
    lv_imagebutton_set_src(ui->screen_main_Bluetooth, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, &_langya2_RGB565A8_76x76, NULL, NULL);
    ui->screen_main_Bluetooth_label = lv_label_create(ui->screen_main_Bluetooth);
    lv_label_set_text(ui->screen_main_Bluetooth_label, "");
    lv_label_set_long_mode(ui->screen_main_Bluetooth_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_main_Bluetooth_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_main_Bluetooth, 0, LV_STATE_DEFAULT);

    //Write style for screen_main_Bluetooth, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_text_color(ui->screen_main_Bluetooth, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_Bluetooth, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_main_Bluetooth, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_Bluetooth, Part: LV_PART_MAIN, State: LV_STATE_PRESSED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_image_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui->screen_main_Bluetooth, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_font(ui->screen_main_Bluetooth, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_STATE_PRESSED);

    //Write style for screen_main_Bluetooth, Part: LV_PART_MAIN, State: LV_STATE_CHECKED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_image_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui->screen_main_Bluetooth, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui->screen_main_Bluetooth, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_STATE_CHECKED);

    //Write style for screen_main_Bluetooth, Part: LV_PART_MAIN, State: LV_IMAGEBUTTON_STATE_RELEASED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_Bluetooth, 0, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);
    lv_obj_set_style_image_opa(ui->screen_main_Bluetooth, 255, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);

    //Write codes screen_main_Brightness
    ui->screen_main_Brightness = lv_slider_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_Brightness, 233, 8);
    lv_obj_set_size(ui->screen_main_Brightness, 50, 180);
    lv_slider_set_range(ui->screen_main_Brightness, 0, 100);
    lv_slider_set_mode(ui->screen_main_Brightness, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_value(ui->screen_main_Brightness, 50, LV_ANIM_OFF);

    //Write style for screen_main_Brightness, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_Brightness, 60, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_Brightness, lv_color_hex(0x2195f6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_Brightness, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Brightness, 14, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui->screen_main_Brightness, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_Brightness, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_Brightness, Part: LV_PART_MAIN, State: LV_STATE_FOCUSED.
    lv_obj_set_style_shadow_width(ui->screen_main_Brightness, 0, LV_PART_MAIN|LV_STATE_FOCUSED);

    //Write style for screen_main_Brightness, Part: LV_PART_INDICATOR, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_Brightness, 230, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_Brightness, lv_color_hex(0xffffff), LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_Brightness, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Brightness, 8, LV_PART_INDICATOR|LV_STATE_DEFAULT);

    //Write style for screen_main_Brightness, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_Brightness, 0, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_Brightness, 8, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write codes screen_main_loudness
    ui->screen_main_loudness = lv_slider_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_loudness, 331, 8);
    lv_obj_set_size(ui->screen_main_loudness, 50, 180);
    lv_slider_set_range(ui->screen_main_loudness, 0, 100);
    lv_slider_set_mode(ui->screen_main_loudness, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_value(ui->screen_main_loudness, 50, LV_ANIM_OFF);

    //Write style for screen_main_loudness, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_loudness, 60, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_loudness, lv_color_hex(0x2195f6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_loudness, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_loudness, 14, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui->screen_main_loudness, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_loudness, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_loudness, Part: LV_PART_MAIN, State: LV_STATE_FOCUSED.
    lv_obj_set_style_shadow_width(ui->screen_main_loudness, 0, LV_PART_MAIN|LV_STATE_FOCUSED);

    //Write style for screen_main_loudness, Part: LV_PART_INDICATOR, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_loudness, 230, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_loudness, lv_color_hex(0xffffff), LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_loudness, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_loudness, 8, LV_PART_INDICATOR|LV_STATE_DEFAULT);

    //Write style for screen_main_loudness, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_loudness, 0, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_loudness, 8, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write codes screen_main_slider_1
    ui->screen_main_slider_1 = lv_slider_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_slider_1, 53, 156);
    lv_obj_set_size(ui->screen_main_slider_1, 134, 23);
    lv_slider_set_range(ui->screen_main_slider_1, 0, 100);
    lv_slider_set_mode(ui->screen_main_slider_1, LV_SLIDER_MODE_NORMAL);
    lv_slider_set_value(ui->screen_main_slider_1, 50, LV_ANIM_OFF);

    //Write style for screen_main_slider_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_slider_1, 34, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_slider_1, lv_color_hex(0x00ff8c), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_slider_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_slider_1, 8, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui->screen_main_slider_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_slider_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_slider_1, Part: LV_PART_INDICATOR, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_slider_1, 238, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_slider_1, lv_color_hex(0x00ff70), LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_main_slider_1, LV_GRAD_DIR_NONE, LV_PART_INDICATOR|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_slider_1, 8, LV_PART_INDICATOR|LV_STATE_DEFAULT);

    //Write style for screen_main_slider_1, Part: LV_PART_KNOB, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_main_slider_1, 0, LV_PART_KNOB|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_slider_1, 8, LV_PART_KNOB|LV_STATE_DEFAULT);

    //Write codes screen_main_img_1
    ui->screen_main_img_1 = lv_image_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_img_1, 236, 141);
    lv_obj_set_size(ui->screen_main_img_1, 46, 46);
    lv_obj_add_flag(ui->screen_main_img_1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_main_img_1, &_liandu_RGB565A8_46x46);
    lv_image_set_pivot(ui->screen_main_img_1, 50,50);
    lv_image_set_rotation(ui->screen_main_img_1, 0);

    //Write style for screen_main_img_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_img_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_main_img_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_main_imgbtn_1
    ui->screen_main_imgbtn_1 = lv_imagebutton_create(ui->screen_main_Dropdown_menu);
    lv_obj_set_pos(ui->screen_main_imgbtn_1, 332, 141);
    lv_obj_set_size(ui->screen_main_imgbtn_1, 46, 46);
    lv_obj_add_flag(ui->screen_main_imgbtn_1, LV_OBJ_FLAG_CHECKABLE);
    lv_imagebutton_set_src(ui->screen_main_imgbtn_1, LV_IMAGEBUTTON_STATE_RELEASED, &_shengyin_RGB565A8_46x46, NULL, NULL);
    lv_imagebutton_set_src(ui->screen_main_imgbtn_1, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, &_shengyin3_RGB565A8_46x46, NULL, NULL);
    ui->screen_main_imgbtn_1_label = lv_label_create(ui->screen_main_imgbtn_1);
    lv_label_set_text(ui->screen_main_imgbtn_1_label, "");
    lv_label_set_long_mode(ui->screen_main_imgbtn_1_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui->screen_main_imgbtn_1_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(ui->screen_main_imgbtn_1, 0, LV_STATE_DEFAULT);

    //Write style for screen_main_imgbtn_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_text_color(ui->screen_main_imgbtn_1, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_imgbtn_1, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_main_imgbtn_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write style for screen_main_imgbtn_1, Part: LV_PART_MAIN, State: LV_STATE_PRESSED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_image_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui->screen_main_imgbtn_1, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_font(ui->screen_main_imgbtn_1, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_STATE_PRESSED);

    //Write style for screen_main_imgbtn_1, Part: LV_PART_MAIN, State: LV_STATE_CHECKED.
    lv_obj_set_style_image_recolor_opa(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_image_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui->screen_main_imgbtn_1, lv_color_hex(0xFF33FF), LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_font(ui->screen_main_imgbtn_1, &lv_font_montserratMedium_12, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_STATE_CHECKED);
    lv_obj_set_style_image_recolor_opa(ui->screen_main_imgbtn_1, 0, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);
    lv_obj_set_style_image_opa(ui->screen_main_imgbtn_1, 255, LV_PART_MAIN|LV_IMAGEBUTTON_STATE_RELEASED);

    // --- Antigravity Custom Watch Face UI Redesign Start ---
    // 1. Clear background image and set solid warm-white color (#f6f5f0)
    lv_obj_set_style_bg_image_src(ui->screen_main_cont_1, NULL, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_cont_1, lv_color_hex(0xf6f5f0), LV_PART_MAIN|LV_STATE_DEFAULT);

    // 2. Create date label (top-left, x: 30, y: 24)
    ui->screen_main_date_label = lv_label_create(ui->screen_main_cont_1);
    lv_obj_set_pos(ui->screen_main_date_label, 42, 24);
    lv_obj_set_style_text_color(ui->screen_main_date_label, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_date_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(ui->screen_main_date_label, "周二 06/18");

    // 3. Create battery percentage label and battery progress bar (top-right)
    // Battery Label: x: 332, y: 24
    ui->screen_main_battery_label = lv_label_create(ui->screen_main_cont_1);
    lv_obj_set_pos(ui->screen_main_battery_label, 314, 24);
    lv_obj_set_style_text_color(ui->screen_main_battery_label, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_battery_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(ui->screen_main_battery_label, "80%");

    // Battery Bar container: x: 298, y: 27, size: 28x14, border: 1, radius: 3
    ui->screen_main_battery_bar = lv_obj_create(ui->screen_main_cont_1);
    lv_obj_set_pos(ui->screen_main_battery_bar, 280, 27);
    lv_obj_set_size(ui->screen_main_battery_bar, 28, 14);
    lv_obj_set_scrollbar_mode(ui->screen_main_battery_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(ui->screen_main_battery_bar, 1, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui->screen_main_battery_bar, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_battery_bar, 3, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_battery_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui->screen_main_battery_bar, 1, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Battery Fill bar inside container: size: 24x10, bg_color: #10b981
    ui->screen_main_battery_fill = lv_obj_create(ui->screen_main_battery_bar);
    lv_obj_set_pos(ui->screen_main_battery_fill, 0, 0);
    lv_obj_set_size(ui->screen_main_battery_fill, 24, 10);
    lv_obj_set_scrollbar_mode(ui->screen_main_battery_fill, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(ui->screen_main_battery_fill, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_battery_fill, 1, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_battery_fill, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_battery_fill, lv_color_hex(0x10b981), LV_PART_MAIN|LV_STATE_DEFAULT);

    // 4. Overwrite center large digital clock style (increase to 58px Montserrat, transparent background, centered)
    lv_obj_set_style_bg_opa(ui->screen_main_digital_clock_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_digital_clock_1, &lv_font_montserratMedium_58, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_main_digital_clock_1, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_pos(ui->screen_main_digital_clock_1, 0, 75);
    lv_obj_set_size(ui->screen_main_digital_clock_1, 410, 70);
    lv_obj_set_style_text_align(ui->screen_main_digital_clock_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);

    // 5. Create bottom Weather Bento Card and child components (x: 28, y: 246, size: 354x196, radius: 36)
    ui->screen_main_weather_card = lv_obj_create(ui->screen_main_cont_1);
    lv_obj_set_pos(ui->screen_main_weather_card, 30, 246);
    lv_obj_set_size(ui->screen_main_weather_card, 350, 196);
    lv_obj_set_scrollbar_mode(ui->screen_main_weather_card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(ui->screen_main_weather_card, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_main_weather_card, 36, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_main_weather_card, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_main_weather_card, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui->screen_main_weather_card, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Soft gray shadow for the Bento Card
    lv_obj_set_style_shadow_width(ui->screen_main_weather_card, 20, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui->screen_main_weather_card, lv_color_hex(0xd1d5db), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui->screen_main_weather_card, 100, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(ui->screen_main_weather_card, 4, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Weather Icon (Left-side: size 96x96, pos: 24, 50)
    ui->screen_main_weather_icon = lv_image_create(ui->screen_main_weather_card);
    lv_obj_set_pos(ui->screen_main_weather_icon, 24, 50);
    lv_obj_set_size(ui->screen_main_weather_icon, 96, 96);
    lv_image_set_src(ui->screen_main_weather_icon, &_weather_duoyun_RGB565A8_96x96);
    lv_image_set_scale(ui->screen_main_weather_icon, LV_SCALE_NONE);
    lv_obj_set_style_radius(ui->screen_main_weather_icon, 16, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(ui->screen_main_weather_icon, true, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Weather Temperature: (Right-side: Montserrat 46px, pos: 172, 36)
    ui->screen_main_weather_temp = lv_label_create(ui->screen_main_weather_card);
    lv_obj_set_pos(ui->screen_main_weather_temp, 172, 36);
    lv_obj_set_style_text_color(ui->screen_main_weather_temp, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_weather_temp, &lv_font_montserratMedium_46, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(ui->screen_main_weather_temp, "24");

    // Weather Unit "°C": (Right-side: Chinese 16px, aligned to top-right of temp label)
    lv_obj_t *weather_unit = lv_label_create(ui->screen_main_weather_card);
    lv_obj_set_style_text_color(weather_unit, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(weather_unit, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(weather_unit, "°C");
    lv_obj_align_to(weather_unit, ui->screen_main_weather_temp, LV_ALIGN_OUT_RIGHT_TOP, 2, 4);

    // Weather Status text: (Right-side: Chinese 22px, pos: 172, 94)
    ui->screen_main_weather_text = lv_label_create(ui->screen_main_weather_card);
    lv_obj_set_pos(ui->screen_main_weather_text, 172, 94);
    lv_obj_set_style_text_color(ui->screen_main_weather_text, lv_color_hex(0x1B3024), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_weather_text, &lv_font_montserrat_lxgw_tghz_level1_3500_22_4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(ui->screen_main_weather_text, "多云");

    // High/Low Temperature range: (Right-side: Chinese 16px, pos: 172, 128)
    ui->screen_main_weather_range = lv_label_create(ui->screen_main_weather_card);
    lv_obj_set_pos(ui->screen_main_weather_range, 172, 128);
    lv_obj_set_style_text_color(ui->screen_main_weather_range, lv_color_hex(0x4E6557), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_main_weather_range, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_label_set_text(ui->screen_main_weather_range, "最高 28°C / 最低 19°C");

    // Invoke clock timer once to update date/time and battery stats immediately
    screen_main_digital_clock_1_timer(NULL);
    // --- Antigravity Custom Watch Face UI Redesign End ---

    //Update current screen layout.
    lv_obj_update_layout(ui->screen_main);

    //Init events for screen.
    events_init_screen_main(ui);
}
