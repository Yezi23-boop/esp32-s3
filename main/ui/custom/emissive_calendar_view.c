#include "emissive_calendar_view.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gui_guider.h"
#include "ui_chinese_fonts.h"

enum {
    kScreenW = 410,
    kScreenH = 502,
    kCardX = 40,
    kCardY = 38,
    kCardW = 330,
    kCardH = 426,
    kCardRadius = 46,
    kDotSize = 42,
    kSwipeExitThresholdPx = 60,
    kSwipeExitMaxDyPx = 50,
};

typedef enum {
    CAL_TONE_NONE = 0,
    CAL_TONE_BLUE,
    CAL_TONE_GREEN,
} calendar_tone_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *days_layer;
    lv_obj_t *date_label;
    int year;
    int month;
    int selected_day;
    int32_t swipe_start_x;
    int32_t swipe_start_y;
    bool swipe_tracking;
} emissive_calendar_state_t;

static lv_point_precise_t s_left_chevron_points[] = {
    {18, 14},
    {13, 20},
    {18, 26},
};

static lv_point_precise_t s_right_chevron_points[] = {
    {16, 14},
    {21, 20},
    {16, 26},
};

static void calendar_refresh(emissive_calendar_state_t *state);

static bool calendar_is_leap_year(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int calendar_days_in_month(int year, int month)
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
    if (month == 2 && calendar_is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

static int calendar_first_weekday_monday(int year, int month)
{
    static const int kMonthOffset[] = {0, 3, 2, 5, 0, 3,
                                      5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) {
        y -= 1;
    }
    int sunday_based = (y + y / 4 - y / 100 + y / 400 +
                        kMonthOffset[month - 1] + 1) %
                       7;
    return (sunday_based + 6) % 7;
}

static calendar_tone_t calendar_tone_for_day(const emissive_calendar_state_t *state,
                                             int day)
{
    if (state->month == 9 &&
        (day == 9 || day == 10 || day == 11 || day == 12)) {
        return CAL_TONE_BLUE;
    }
    if (state->month == 9 && (day == 15 || day == 19 || day == 20)) {
        return CAL_TONE_GREEN;
    }
    if (day == state->selected_day) {
        return CAL_TONE_BLUE;
    }
    return CAL_TONE_NONE;
}

static void calendar_parse_date(const char *date_text,
                                int *year,
                                int *month,
                                int *day)
{
    int y = 2025;
    int m = 9;
    int d = 9;
    if (date_text != NULL) {
        (void)sscanf(date_text, "%d/%d/%d", &y, &m, &d);
    }
    if (y < 2000 || y > 2099) {
        y = 2025;
    }
    if (m < 1 || m > 12) {
        m = 9;
    }
    int max_day = calendar_days_in_month(y, m);
    if (d < 1 || d > max_day) {
        d = 1;
    }
    *year = y;
    *month = m;
    *day = d;
}

static const char *calendar_month_name(int month)
{
    static const char *const kMonthNames[] = {
        "一月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "十一月", "十二月",
    };
    return kMonthNames[month - 1];
}

static void calendar_set_label_style(lv_obj_t *label,
                                     const lv_font_t *font,
                                     lv_color_t color,
                                     lv_text_align_t align)
{
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN);
}

static lv_obj_t *calendar_create_label(lv_obj_t *parent,
                                       const char *text,
                                       int32_t x,
                                       int32_t y,
                                       int32_t w,
                                       int32_t h,
                                       const lv_font_t *font,
                                       lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, text);
    calendar_set_label_style(label, font, color, LV_TEXT_ALIGN_CENTER);
    return label;
}

static lv_obj_t *calendar_create_gradient_wash(lv_obj_t *parent,
                                               lv_color_t color,
                                               lv_color_t grad_color,
                                               lv_grad_dir_t dir,
                                               lv_opa_t opa)
{
    lv_obj_t *wash = lv_obj_create(parent);
    lv_obj_remove_style_all(wash);
    lv_obj_set_pos(wash, 0, 0);
    lv_obj_set_size(wash, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(wash, color, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(wash, grad_color, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(wash, dir, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wash, opa, LV_PART_MAIN);
    lv_obj_clear_flag(wash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(wash, LV_OBJ_FLAG_SCROLLABLE);
    return wash;
}

static void calendar_root_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_event_get_user_data(event);

    if (code == LV_EVENT_DELETE && state != NULL) {
        lv_free(state);
    }
}

static void calendar_swipe_exit_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_event_get_user_data(event);
    if (state == NULL) {
        return;
    }

    lv_point_t p;
    switch (code) {
    case LV_EVENT_PRESSED:
        lv_indev_get_point(lv_indev_get_act(), &p);
        state->swipe_start_x = p.x;
        state->swipe_start_y = p.y;
        state->swipe_tracking = true;
        break;
    case LV_EVENT_RELEASED:
        if (!state->swipe_tracking) {
            break;
        }
        state->swipe_tracking = false;
        lv_indev_get_point(lv_indev_get_act(), &p);
        if ((p.x - state->swipe_start_x) >= kSwipeExitThresholdPx &&
            LV_ABS(p.y - state->swipe_start_y) <= kSwipeExitMaxDyPx) {
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

static void calendar_day_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    if (state == NULL || target == NULL) {
        return;
    }

    lv_obj_t *label = lv_obj_get_child(target, 0);
    if (label == NULL) {
        return;
    }

    int day = atoi(lv_label_get_text(label));
    if (day <= 0) {
        return;
    }

    if (state->date_label != NULL && lv_obj_is_valid(state->date_label)) {
        char buf[16];
        lv_snprintf(buf, sizeof(buf), "%04d/%02d/%02d",
                    state->year, state->month, day);
        lv_label_set_text(state->date_label, buf);
    }
    state->selected_day = day;
    calendar_refresh(state);
}

static void calendar_render_days(emissive_calendar_state_t *state)
{
    lv_obj_clean(state->days_layer);

    const int first_col = calendar_first_weekday_monday(state->year, state->month);
    const int days = calendar_days_in_month(state->year, state->month);
    const int rows = (first_col + days + 6) / 7;
    const int32_t center_x0 = 27;
    const int32_t col_step = 46;
    const int32_t center_y0 = rows <= 5 ? 164 : 150;
    const int32_t row_step = rows <= 5 ? 58 : 50;

    for (int day = 1; day <= days; ++day) {
        int index = first_col + day - 1;
        int col = index % 7;
        int row = index / 7;
        calendar_tone_t tone = calendar_tone_for_day(state, day);

        lv_obj_t *cell = lv_obj_create(state->days_layer);
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, center_x0 + col * col_step - kDotSize / 2,
                       center_y0 + row * row_step - kDotSize / 2);
        lv_obj_set_size(cell, kDotSize, kDotSize);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_radius(cell, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, calendar_swipe_exit_event_cb,
                            LV_EVENT_ALL, state);
        lv_obj_add_event_cb(cell, calendar_day_event_cb, LV_EVENT_CLICKED, state);

        lv_color_t text_color = lv_color_hex(0x1f1d23);
        if (tone == CAL_TONE_BLUE) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xcfe0ff), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            text_color = lv_color_hex(0x007aff);
        } else if (tone == CAL_TONE_GREEN) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xd7ebd8), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
            text_color = lv_color_hex(0x18b957);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        }

        char day_text[3];
        lv_snprintf(day_text, sizeof(day_text), "%d", day);
        lv_obj_t *label = lv_label_create(cell);
        lv_label_set_text(label, day_text);
        calendar_set_label_style(label, &lv_font_montserratMedium_16,
                                 text_color, LV_TEXT_ALIGN_CENTER);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 1);
    }
}

static void calendar_refresh(emissive_calendar_state_t *state)
{
    lv_label_set_text(state->title, calendar_month_name(state->month));
    calendar_render_days(state);
}

static void calendar_prev_month_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_event_get_user_data(event);
    if (state == NULL) {
        return;
    }
    state->month -= 1;
    if (state->month < 1) {
        state->month = 12;
        state->year -= 1;
    }
    calendar_refresh(state);
}

static void calendar_next_month_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_event_get_user_data(event);
    if (state == NULL) {
        return;
    }
    state->month += 1;
    if (state->month > 12) {
        state->month = 1;
        state->year += 1;
    }
    calendar_refresh(state);
}

static lv_obj_t *calendar_create_arrow(lv_obj_t *parent,
                                       int32_t x,
                                       bool is_next,
                                       lv_event_cb_t cb,
                                       emissive_calendar_state_t *state)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, 18);
    lv_obj_set_size(btn, 42, 42);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, state);

    lv_obj_t *line = lv_line_create(btn);
    lv_line_set_points(line,
                       is_next ? s_right_chevron_points : s_left_chevron_points,
                       3);
    lv_obj_set_style_line_width(line, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(line, lv_color_hex(0x342f3a), LV_PART_MAIN);
    lv_obj_set_style_line_opa(line, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    lv_obj_center(line);
    return btn;
}

lv_obj_t *emissive_calendar_view_show(lv_obj_t *anchor,
                                      const char *date_text,
                                      lv_obj_t *date_label)
{
    emissive_calendar_state_t *state =
        (emissive_calendar_state_t *)lv_malloc(sizeof(emissive_calendar_state_t));
    if (state == NULL) {
        return NULL;
    }
    *state = (emissive_calendar_state_t){0};
    calendar_parse_date(date_text, &state->year, &state->month,
                        &state->selected_day);
    state->date_label = date_label;

    lv_obj_t *parent = anchor != NULL ? lv_obj_get_screen(anchor) : NULL;
    if (parent == NULL) {
        parent = lv_screen_active();
    }
    state->root = lv_obj_create(parent);
    if (state->root == NULL) {
        lv_free(state);
        return NULL;
    }
    lv_obj_remove_style_all(state->root);
    lv_obj_set_pos(state->root, 0, 0);
    lv_obj_set_size(state->root, kScreenW, kScreenH);
    lv_obj_clear_flag(state->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(state->root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(state->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(state->root, lv_color_hex(0xf8fbff), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(state->root, lv_color_hex(0xf7fbf2),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(state->root, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(state->root, calendar_root_event_cb, LV_EVENT_ALL, state);
    lv_obj_add_event_cb(state->root, calendar_swipe_exit_event_cb,
                        LV_EVENT_ALL, state);

    (void)calendar_create_gradient_wash(state->root,
                                        lv_color_hex(0xeef8ff),
                                        lv_color_hex(0xfff1f7),
                                        LV_GRAD_DIR_HOR,
                                        LV_OPA_30);
    (void)calendar_create_gradient_wash(state->root,
                                        lv_color_hex(0xfff9fc),
                                        lv_color_hex(0xf0faef),
                                        LV_GRAD_DIR_VER,
                                        LV_OPA_20);

    state->card = lv_obj_create(state->root);
    lv_obj_remove_style_all(state->card);
    lv_obj_set_pos(state->card, kCardX, kCardY);
    lv_obj_set_size(state->card, kCardW, kCardH);
    lv_obj_clear_flag(state->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(state->card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(state->card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state->card, lv_color_hex(0xfff2f8), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(state->card, lv_color_hex(0xfafff4),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(state->card, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(state->card, lv_color_hex(0xffffff),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(state->card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(state->card, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(state->card, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(state->card, lv_color_hex(0xd8cfda),
                                  LV_PART_MAIN);
    lv_obj_add_event_cb(state->card, calendar_swipe_exit_event_cb,
                        LV_EVENT_ALL, state);

    state->title = calendar_create_label(state->card, calendar_month_name(state->month),
                                         0, 22, kCardW, 34,
                                         &lv_font_montserrat_lxgw_tghz_level1_3500_27_4,
                                         lv_color_hex(0x242029));

    (void)calendar_create_arrow(state->card, 14, false,
                                calendar_prev_month_event_cb, state);
    (void)calendar_create_arrow(state->card, kCardW - 56, true,
                                calendar_next_month_event_cb, state);

    static const char *const kWeekdays[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) {
        (void)calendar_create_label(state->card, kWeekdays[i],
                                    27 + i * 46 - 14, 78, 28, 24,
                                    &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                                    lv_color_hex(0x86868b));
    }

    lv_obj_t *line = lv_obj_create(state->card);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 24, 110);
    lv_obj_set_size(line, kCardW - 48, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xe9dce5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_60, LV_PART_MAIN);

    state->days_layer = lv_obj_create(state->card);
    lv_obj_remove_style_all(state->days_layer);
    lv_obj_set_pos(state->days_layer, 0, 0);
    lv_obj_set_size(state->days_layer, kCardW, kCardH);
    lv_obj_clear_flag(state->days_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(state->days_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(state->days_layer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(state->days_layer, calendar_swipe_exit_event_cb,
                        LV_EVENT_ALL, state);

    calendar_render_days(state);
    lv_obj_move_foreground(state->card);
    return state->root;
}
