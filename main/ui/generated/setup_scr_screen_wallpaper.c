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



void setup_scr_screen_wallpaper(lv_ui *ui)
{
    //Write codes screen_wallpaper
    ui->screen_wallpaper = lv_obj_create(NULL);
    lv_obj_set_size(ui->screen_wallpaper, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_wallpaper, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_wallpaper, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_wallpaper, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_qp1
    ui->screen_wallpaper_img_qp1 = lv_image_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_img_qp1, 0, 0);
    lv_obj_set_size(ui->screen_wallpaper_img_qp1, 410, 502);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_qp1, &_1_RGB565A8_410x502);
    lv_image_set_pivot(ui->screen_wallpaper_img_qp1, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_qp1, 0);

    //Write style for screen_wallpaper_img_qp1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_qp1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_qp1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_qp2
    ui->screen_wallpaper_img_qp2 = lv_image_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_img_qp2, 0, 0);
    lv_obj_set_size(ui->screen_wallpaper_img_qp2, 410, 502);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp2, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_qp2, &_2_RGB565A8_410x502);
    lv_image_set_pivot(ui->screen_wallpaper_img_qp2, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_qp2, 0);

    //Write style for screen_wallpaper_img_qp2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_qp2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_qp2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_qp3
    ui->screen_wallpaper_img_qp3 = lv_image_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_img_qp3, 0, 0);
    lv_obj_set_size(ui->screen_wallpaper_img_qp3, 410, 502);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp3, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_qp3, &_3_RGB565A8_410x502);
    lv_image_set_pivot(ui->screen_wallpaper_img_qp3, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_qp3, 0);

    //Write style for screen_wallpaper_img_qp3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_qp3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_qp3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_qp4
    ui->screen_wallpaper_img_qp4 = lv_image_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_img_qp4, 0, 0);
    lv_obj_set_size(ui->screen_wallpaper_img_qp4, 410, 502);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp4, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->screen_wallpaper_img_qp4, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_qp4, &_4_RGB565A8_410x502);
    lv_image_set_pivot(ui->screen_wallpaper_img_qp4, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_qp4, 0);

    //Write style for screen_wallpaper_img_qp4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_qp4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_qp4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_cont_1
    ui->screen_wallpaper_cont_1 = lv_obj_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_cont_1, 0, 0);
    lv_obj_set_size(ui->screen_wallpaper_cont_1, 410, 502);
    lv_obj_set_scrollbar_mode(ui->screen_wallpaper_cont_1, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_wallpaper_cont_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_wallpaper_cont_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_wallpaper_cont_1, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_wallpaper_cont_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_wallpaper_cont_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_1
    ui->screen_wallpaper_img_1 = lv_image_create(ui->screen_wallpaper_cont_1);
    lv_obj_set_pos(ui->screen_wallpaper_img_1, -347, 171);
    lv_obj_set_size(ui->screen_wallpaper_img_1, 180, 180);
    lv_obj_add_flag(ui->screen_wallpaper_img_1, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_1, &_yuanjiao1_RGB565A8_180x180);
    lv_image_set_pivot(ui->screen_wallpaper_img_1, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_1, 0);

    //Write style for screen_wallpaper_img_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_2
    ui->screen_wallpaper_img_2 = lv_image_create(ui->screen_wallpaper_cont_1);
    lv_obj_set_pos(ui->screen_wallpaper_img_2, -42, 173);
    lv_obj_set_size(ui->screen_wallpaper_img_2, 180, 180);
    lv_obj_add_flag(ui->screen_wallpaper_img_2, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_2, &_yuanjiao2_RGB565A8_180x180);
    lv_image_set_pivot(ui->screen_wallpaper_img_2, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_2, 0);

    //Write style for screen_wallpaper_img_2, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_2, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_2, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_3
    ui->screen_wallpaper_img_3 = lv_image_create(ui->screen_wallpaper_cont_1);
    lv_obj_set_pos(ui->screen_wallpaper_img_3, 265, 173);
    lv_obj_set_size(ui->screen_wallpaper_img_3, 180, 180);
    lv_obj_add_flag(ui->screen_wallpaper_img_3, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_3, &_yuanjiao3_RGB565A8_180x180);
    lv_image_set_pivot(ui->screen_wallpaper_img_3, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_3, 0);

    //Write style for screen_wallpaper_img_3, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_3, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_3, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_img_4
    ui->screen_wallpaper_img_4 = lv_image_create(ui->screen_wallpaper_cont_1);
    lv_obj_set_pos(ui->screen_wallpaper_img_4, 572, 173);
    lv_obj_set_size(ui->screen_wallpaper_img_4, 180, 180);
    lv_obj_add_flag(ui->screen_wallpaper_img_4, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(ui->screen_wallpaper_img_4, &_yuanjiao4_RGB565A8_180x180);
    lv_image_set_pivot(ui->screen_wallpaper_img_4, 50,50);
    lv_image_set_rotation(ui->screen_wallpaper_img_4, 0);

    //Write style for screen_wallpaper_img_4, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_image_recolor_opa(ui->screen_wallpaper_img_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_image_opa(ui->screen_wallpaper_img_4, 255, LV_PART_MAIN|LV_STATE_DEFAULT);

    //Write codes screen_wallpaper_label_1
    ui->screen_wallpaper_label_1 = lv_label_create(ui->screen_wallpaper);
    lv_obj_set_pos(ui->screen_wallpaper_label_1, 140, 422);
    lv_obj_set_size(ui->screen_wallpaper_label_1, 155, 55);
    lv_label_set_text(ui->screen_wallpaper_label_1, "小祁满");
    lv_label_set_long_mode(ui->screen_wallpaper_label_1, LV_LABEL_LONG_WRAP);

    //Write style for screen_wallpaper_label_1, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_border_width(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui->screen_wallpaper_label_1, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui->screen_wallpaper_label_1, &lv_font_SourceHanSerifSC_Regular_22, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui->screen_wallpaper_label_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(ui->screen_wallpaper_label_1, 4, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui->screen_wallpaper_label_1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui->screen_wallpaper_label_1, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_wallpaper_label_1, lv_color_hex(0x2195f6), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_wallpaper_label_1, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(ui->screen_wallpaper_label_1, 14, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui->screen_wallpaper_label_1, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    //The custom code of screen_wallpaper.


    //Update current screen layout.
    lv_obj_update_layout(ui->screen_wallpaper);

    //Init events for screen.
    events_init_screen_wallpaper(ui);
}
