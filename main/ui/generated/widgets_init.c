/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#include "lvgl.h"
#include "system_time.h"
#include "gui_guider.h"
#include "widgets_init.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "custom.h"


__attribute__((unused)) void kb_event_cb (lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);
    if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

__attribute__((unused)) void ta_event_cb (lv_event_t *e) {
#if LV_USE_KEYBOARD
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    lv_obj_t * kb = lv_event_get_user_data(e);

    if(code == LV_EVENT_FOCUSED) {
        if(lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_KEYPAD) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
    } else if(code == LV_EVENT_READY) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_indev_reset(NULL, ta);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_keyboard_set_textarea(kb, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void clock_count(int *hour, int *min, int *sec)
{
    (*sec)++;
    if(*sec == 60)
    {
        *sec = 0;
        (*min)++;
    }
    if(*min == 60)
    {
        *min = 0;
        if(*hour < 12)
        {
            (*hour)++;
        } else {
            (*hour)++;
            *hour = *hour %12;
        }
    }
}

void digital_clock_count(int * hour, int * minute, int * seconds, char * meridiem)
{

    (*seconds)++;
    if(*seconds == 60) {
        *seconds = 0;
        (*minute)++;
    }
    if(*minute == 60) {
        *minute = 0;
        if(*hour < 12) {
            (*hour)++;
        }
        else {
            (*hour)++;
            (*hour) = (*hour) % 12;
        }
    }
    if(*hour == 12 && *seconds == 0 && *minute == 0) {
        if((lv_strcmp(meridiem, "PM") == 0)) {
            lv_strcpy(meridiem, "AM");
        }
        else {
            lv_strcpy(meridiem, "PM");
        }
    }
}

extern int screen_main_digital_clock_1_hour_value;
extern int screen_main_digital_clock_1_min_value;
extern int screen_main_digital_clock_1_sec_value;

void screen_main_digital_clock_1_timer(lv_timer_t *timer)
{
    (void)timer;
    if (lv_obj_is_valid(guider_ui.screen_main_digital_clock_1))
    {
        system_time_local_t now = {0};
        if (system_time_get_local_time(&now) != ESP_OK)
        {
            static bool showed_error = false;
            if (!showed_error) {
                lv_label_set_text(guider_ui.screen_main_digital_clock_1, "--:--");
                if (lv_obj_is_valid(guider_ui.screen_main_date_label))
                {
                    lv_label_set_text(guider_ui.screen_main_date_label, "--/--");
                }
                showed_error = true;
            }
            return;
        }

        /*
         * 主屏时间必须消费 system_time owner 的本地时间快照。
         * GUI Guider 原始计数器只会从固定初值自增，联网校时或 RTC 写回后不会自动纠偏。
         */
        screen_main_digital_clock_1_hour_value = now.hour;
        screen_main_digital_clock_1_min_value = now.min;
        screen_main_digital_clock_1_sec_value = now.sec;

        // 1. 优化时钟刷新：分钟改变时才刷新渲染，大幅降低 1Hz 冗余重绘成本
        static int last_min = -1;
        if (now.min != last_min)
        {
            lv_label_set_text_fmt(guider_ui.screen_main_digital_clock_1, "%02d:%02d", now.hour, now.min);
            last_min = now.min;
        }

        // 2. 优化日期刷新：日期（天）改变时才换算星期并刷新渲染
        if (lv_obj_is_valid(guider_ui.screen_main_date_label))
        {
            static int last_day = -1;
            if (now.day != last_day)
            {
                struct tm t = {0};
                t.tm_year = now.year - 1900;
                t.tm_mon = now.month - 1;
                t.tm_mday = now.day;
                mktime(&t);
                const char *wday_str[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
                int wday = t.tm_wday;
                if (wday < 0 || wday > 6) {
                    wday = 0;
                }
                lv_label_set_text_fmt(guider_ui.screen_main_date_label, "%s %02d/%02d", wday_str[wday], now.month, now.day);
                last_day = now.day;
            }
        }
    }

    main_screen_runtime_refresh(&guider_ui);
}
static lv_obj_t * screen_time_datetext_1_calendar;

void screen_time_datetext_1_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);
    if(code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        char * s = lv_label_get_text(btn);
        if(screen_time_datetext_1_calendar == NULL) {
            screen_time_datetext_1_init_calendar(btn, s);
        }
    }
}

void screen_time_datetext_1_init_calendar(lv_obj_t *obj, char * s)
{
    if (screen_time_datetext_1_calendar == NULL) {
        screen_time_datetext_1_calendar =
            emissive_calendar_view_show(obj, s, guider_ui.screen_time_datetext_1);
        if (screen_time_datetext_1_calendar != NULL) {
            lv_obj_add_event_cb(screen_time_datetext_1_calendar,
                                screen_time_datetext_1_calendar_event_handler,
                                LV_EVENT_DELETE, NULL);
        }
    }
}

void screen_time_datetext_1_calendar_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_DELETE) {
        screen_time_datetext_1_calendar = NULL;
    }
}


