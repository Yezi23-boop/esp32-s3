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
#include "system_time.h"
#include "custom.h"

enum {
    kScreenTimeSwipeExitThresholdPx = 60,
    kScreenTimeSwipeExitMaxDyPx = 50,
};

static int32_t s_screen_time_swipe_start_x;
static int32_t s_screen_time_swipe_start_y;
static bool s_screen_time_swipe_tracking;

static void screen_time_open_calendar_on_loaded(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCREEN_LOADED) {
        return;
    }

    lv_ui *ui = (lv_ui *)lv_event_get_user_data(e);
    if (ui == NULL || ui->screen_time_datetext_1 == NULL) {
        return;
    }

    system_time_local_t now = {0};
    if (system_time_get_local_time(&now) == ESP_OK) {
        char date_text[16];
        lv_snprintf(date_text, sizeof(date_text), "%04d/%02d/%02d",
                    now.year, now.month, now.day);
        lv_label_set_text(ui->screen_time_datetext_1, date_text);
    }

    screen_time_datetext_1_init_calendar(
        ui->screen_time_datetext_1,
        (char *)lv_label_get_text(ui->screen_time_datetext_1));
}

static void screen_time_swipe_exit_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;

    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &p);
        s_screen_time_swipe_start_x = p.x;
        s_screen_time_swipe_start_y = p.y;
        s_screen_time_swipe_tracking = true;
        break;
    case LV_EVENT_RELEASED:
        if (!s_screen_time_swipe_tracking) {
            break;
        }
        s_screen_time_swipe_tracking = false;
        lv_indev_get_point(lv_indev_get_act(), &p);
        if ((p.x - s_screen_time_swipe_start_x) >= kScreenTimeSwipeExitThresholdPx &&
            LV_ABS(p.y - s_screen_time_swipe_start_y) <= kScreenTimeSwipeExitMaxDyPx) {
            ui_load_scr_animation(&guider_ui, &guider_ui.screen_main,
                                  guider_ui.screen_main_del,
                                  &guider_ui.screen_time_del,
                                  setup_scr_screen_main,
                                  LV_SCR_LOAD_ANIM_FADE_ON, 300, 300,
                                  true, true);
        }
        break;
    default:
        break;
    }
}


void setup_scr_screen_time(lv_ui *ui)
{
    //Write codes screen_time
    ui->screen_time = lv_obj_create(NULL);
    lv_obj_set_size(ui->screen_time, 410, 502);
    lv_obj_clear_flag(ui->screen_time, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ui->screen_time, LV_SCROLLBAR_MODE_OFF);

    //Write style for screen_time, Part: LV_PART_MAIN, State: LV_STATE_DEFAULT.
    lv_obj_set_style_bg_opa(ui->screen_time, 255, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui->screen_time, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(ui->screen_time, LV_GRAD_DIR_NONE, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Hidden date anchor used by the custom calendar view.
    ui->screen_time_datetext_1 = lv_label_create(ui->screen_time);
    lv_obj_set_pos(ui->screen_time_datetext_1, 0, 0);
    lv_obj_set_size(ui->screen_time_datetext_1, 1, 1);
    lv_label_set_text(ui->screen_time_datetext_1, "2025/10/27");
    lv_obj_add_flag(ui->screen_time_datetext_1, LV_OBJ_FLAG_HIDDEN);

    //The custom code of screen_time.


    //Update current screen layout.
    lv_obj_update_layout(ui->screen_time);

    lv_obj_add_event_cb(ui->screen_time, screen_time_open_calendar_on_loaded,
                        LV_EVENT_SCREEN_LOADED, ui);
    lv_obj_add_event_cb(ui->screen_time, screen_time_swipe_exit_event_cb,
                        LV_EVENT_ALL, ui);

}
