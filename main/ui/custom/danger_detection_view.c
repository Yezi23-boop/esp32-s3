#include "danger_detection_view.h"

#include <stdlib.h>

#include "gui_guider.h"
#include "ui_chinese_fonts.h"

struct danger_detection_view {
    lv_obj_t *screen;
    lv_obj_t *back_btn;
    lv_obj_t *back_label;
    lv_obj_t *content_layer;
    lv_obj_t *safety_monitor_row;
    lv_obj_t *safety_monitor_label;
    lv_obj_t *safety_monitor_switch;
    lv_obj_t *safety_monitor_knob;
    lv_obj_t *status_label;
    lv_obj_t *category_label;
    lv_obj_t *primary_result_label;
    lv_obj_t *sensitivity_title_label;
    lv_obj_t *sensitivity_row;
    lv_obj_t *sensitivity_buttons[3];
    lv_obj_t *sensitivity_labels[3];
    lv_obj_t *mic_test_row;
    lv_obj_t *mic_test_button;
    lv_obj_t *mic_test_button_label;
    lv_obj_t *mic_test_status_label;
    lv_obj_t *scores_card;
    lv_obj_t *horn_title_label;
    lv_obj_t *horn_confidence_label;
    lv_obj_t *siren_title_label;
    lv_obj_t *siren_confidence_label;
    lv_obj_t *alert_layer;
    lv_obj_t *alert_badge;
    lv_obj_t *alert_icon_label;
    danger_detection_view_action_cb_t back_action_cb;
    danger_detection_view_switch_cb_t safety_monitor_cb;
    danger_detection_view_sensitivity_cb_t sensitivity_cb;
    danger_detection_view_action_cb_t mic_test_cb;
    void *user_data;
    bool updating_switch;
    bool updating_sensitivity;
};

static const lv_coord_t kDangerBackButtonX = 28;
static const lv_coord_t kDangerBackButtonY = 22;
static const lv_coord_t kDangerBackButtonWidth = 96;
static const lv_coord_t kDangerBackButtonHeight = 56;

static void danger_detection_view_back_event(lv_event_t *e)
{
    danger_detection_view_t *view =
        (danger_detection_view_t *)lv_event_get_user_data(e);
    if (view == NULL || view->back_action_cb == NULL) {
        return;
    }

    view->back_action_cb(view->user_data);
}

static void danger_detection_view_switch_event(lv_event_t *e)
{
    danger_detection_view_t *view =
        (danger_detection_view_t *)lv_event_get_user_data(e);
    if (view == NULL || view->safety_monitor_cb == NULL ||
        view->updating_switch) {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    const bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    view->safety_monitor_cb(enabled, view->user_data);
}

static void danger_detection_view_sensitivity_event(lv_event_t *e)
{
    danger_detection_view_t *view =
        (danger_detection_view_t *)lv_event_get_user_data(e);
    if (view == NULL || view->sensitivity_cb == NULL ||
        view->updating_sensitivity) {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    for (int i = 0; i < 3; ++i) {
        if (target == view->sensitivity_buttons[i]) {
            view->sensitivity_cb((danger_detection_view_sensitivity_mode_t)i,
                                 view->user_data);
            return;
        }
    }
}

static void danger_detection_view_mic_test_event(lv_event_t *e)
{
    danger_detection_view_t *view =
        (danger_detection_view_t *)lv_event_get_user_data(e);
    if (view == NULL || view->mic_test_cb == NULL) {
        return;
    }

    view->mic_test_cb(view->user_data);
}

static void danger_detection_view_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }

    lv_label_set_text(label, text != NULL ? text : "");
}

static const char *danger_detection_view_sensitivity_hint(
    danger_detection_view_sensitivity_mode_t mode)
{
    switch (mode) {
        case DANGER_DETECTION_VIEW_SENSITIVITY_CONSERVATIVE:
            return "灵敏度 · 减少误报";
        case DANGER_DETECTION_VIEW_SENSITIVITY_SENSITIVE:
            return "灵敏度 · 更容易触发";
        case DANGER_DETECTION_VIEW_SENSITIVITY_STANDARD:
        default:
            return "灵敏度 · 日常推荐";
    }
}

danger_detection_view_t *danger_detection_view_create(
    const danger_detection_view_config_t *config)
{
    danger_detection_view_t *view =
        (danger_detection_view_t *)calloc(1, sizeof(danger_detection_view_t));
    if (view == NULL) {
        return NULL;
    }

    view->back_action_cb = config != NULL ? config->back_action_cb : NULL;
    view->safety_monitor_cb =
        config != NULL ? config->safety_monitor_cb : NULL;
    view->sensitivity_cb = config != NULL ? config->sensitivity_cb : NULL;
    view->mic_test_cb = config != NULL ? config->mic_test_cb : NULL;
    view->user_data = config != NULL ? config->user_data : NULL;

    view->screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(view->screen);
    lv_obj_set_style_bg_color(view->screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(view->screen, LV_OPA_COVER, 0);

    view->back_btn = lv_btn_create(view->screen);
    lv_obj_remove_style_all(view->back_btn);
    lv_obj_set_size(view->back_btn, kDangerBackButtonWidth,
                    kDangerBackButtonHeight);
    lv_obj_align(view->back_btn, LV_ALIGN_TOP_LEFT, kDangerBackButtonX,
                 kDangerBackButtonY);
    lv_obj_set_style_bg_opa(view->back_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(view->back_btn, danger_detection_view_back_event,
                        LV_EVENT_CLICKED, view);

    view->back_label = lv_label_create(view->back_btn);
    lv_label_set_text(view->back_label, "<");
    lv_obj_set_style_text_font(view->back_label, &lv_font_montserratMedium_27,
                               0);
    lv_obj_center(view->back_label);

    view->content_layer = lv_obj_create(view->screen);
    lv_obj_remove_style_all(view->content_layer);
    lv_obj_set_size(view->content_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(view->content_layer, LV_OBJ_FLAG_CLICKABLE);

    view->safety_monitor_row = lv_obj_create(view->content_layer);
    lv_obj_remove_style_all(view->safety_monitor_row);
    lv_obj_set_size(view->safety_monitor_row, 146, 34);
    lv_obj_align(view->safety_monitor_row, LV_ALIGN_TOP_LEFT, 42, 352);
    lv_obj_set_flex_flow(view->safety_monitor_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->safety_monitor_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(view->safety_monitor_row, 8, 0);

    view->safety_monitor_label = lv_label_create(view->safety_monitor_row);
    lv_label_set_text(view->safety_monitor_label, "安全监听");
    lv_obj_set_style_text_font(view->safety_monitor_label,
                               &lv_font_montserrat_lxgw_common_5500_16_4,
                               0);
    lv_obj_set_style_text_color(view->safety_monitor_label,
                                lv_color_hex(0x111827), 0);

    view->safety_monitor_switch = lv_obj_create(view->safety_monitor_row);
    lv_obj_remove_style_all(view->safety_monitor_switch);
    lv_obj_set_size(view->safety_monitor_switch, 46, 26);
    lv_obj_add_flag(view->safety_monitor_switch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(view->safety_monitor_switch, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_radius(view->safety_monitor_switch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(view->safety_monitor_switch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(view->safety_monitor_switch,
                              lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_bg_color(view->safety_monitor_switch,
                              lv_color_hex(0x111827), LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(view->safety_monitor_switch, 3, 0);
    lv_obj_add_event_cb(view->safety_monitor_switch,
                        danger_detection_view_switch_event,
                        LV_EVENT_VALUE_CHANGED, view);

    view->safety_monitor_knob = lv_obj_create(view->safety_monitor_switch);
    lv_obj_remove_style_all(view->safety_monitor_knob);
    lv_obj_set_size(view->safety_monitor_knob, 20, 20);
    lv_obj_set_style_radius(view->safety_monitor_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(view->safety_monitor_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(view->safety_monitor_knob,
                              lv_color_hex(0xffffff), 0);
    lv_obj_set_style_shadow_width(view->safety_monitor_knob, 6, 0);
    lv_obj_set_style_shadow_opa(view->safety_monitor_knob, LV_OPA_20, 0);
    lv_obj_clear_flag(view->safety_monitor_knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(view->safety_monitor_knob, LV_ALIGN_LEFT_MID, 3, 0);

    view->status_label = lv_label_create(view->content_layer);
    lv_label_set_text(view->status_label, "未开启");
    lv_obj_set_style_text_font(view->status_label,
                               &lv_font_montserrat_lxgw_common_5500_16_4,
                               0);
    lv_obj_align(view->status_label, LV_ALIGN_TOP_MID, 0, 52);

    view->category_label = lv_label_create(view->content_layer);
    lv_label_set_text(view->category_label, "CURRENT: NONE");
    lv_obj_set_style_text_font(view->category_label, &lv_font_montserratMedium_12,
                               0);
    lv_obj_set_style_text_letter_space(view->category_label, 2, 0);
    lv_obj_set_style_text_color(view->category_label, lv_color_hex(0x6b7280), 0);
    lv_obj_align_to(view->category_label, view->status_label, LV_ALIGN_OUT_BOTTOM_MID,
                    0, 10);

    view->primary_result_label = lv_label_create(view->content_layer);
    lv_label_set_text(view->primary_result_label, "NONE");
    lv_obj_set_style_text_font(view->primary_result_label,
                               &lv_font_montserratMedium_58, 0);
    lv_obj_set_style_text_letter_space(view->primary_result_label, 3, 0);
    lv_obj_align(view->primary_result_label, LV_ALIGN_CENTER, 0, -14);

    view->sensitivity_title_label = lv_label_create(view->content_layer);
    lv_label_set_text(view->sensitivity_title_label, "灵敏度 · 日常推荐");
    lv_obj_set_style_text_font(view->sensitivity_title_label,
                               &lv_font_montserrat_lxgw_common_5500_16_4,
                               0);
    lv_obj_set_style_text_color(view->sensitivity_title_label,
                                lv_color_hex(0x111827), 0);
    lv_obj_align(view->sensitivity_title_label, LV_ALIGN_TOP_MID, 0, 286);

    view->sensitivity_row = lv_obj_create(view->content_layer);
    lv_obj_remove_style_all(view->sensitivity_row);
    lv_obj_set_size(view->sensitivity_row, 320, 34);
    lv_obj_align(view->sensitivity_row, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_flex_flow(view->sensitivity_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->sensitivity_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(view->sensitivity_row, 4, 0);

    static const char *const kSensitivityTexts[] = {
        "保守",
        "标准",
        "敏感",
    };
    for (int i = 0; i < 3; ++i) {
        view->sensitivity_buttons[i] = lv_obj_create(view->sensitivity_row);
        lv_obj_remove_style_all(view->sensitivity_buttons[i]);
        lv_obj_set_size(view->sensitivity_buttons[i], 104, 34);
        lv_obj_add_flag(view->sensitivity_buttons[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view->sensitivity_buttons[i], LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_radius(view->sensitivity_buttons[i], 6, 0);
        lv_obj_set_style_bg_opa(view->sensitivity_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(view->sensitivity_buttons[i],
                                  lv_color_hex(0xf3f4f6), 0);
        lv_obj_set_style_bg_color(view->sensitivity_buttons[i],
                                  lv_color_hex(0x111827), LV_STATE_CHECKED);
        lv_obj_add_event_cb(view->sensitivity_buttons[i],
                            danger_detection_view_sensitivity_event,
                            LV_EVENT_CLICKED, view);

        view->sensitivity_labels[i] =
            lv_label_create(view->sensitivity_buttons[i]);
        lv_label_set_text(view->sensitivity_labels[i], kSensitivityTexts[i]);
        lv_obj_set_style_text_font(
            view->sensitivity_labels[i],
            &lv_font_montserrat_lxgw_common_5500_16_4, 0);
        lv_obj_set_style_text_color(view->sensitivity_labels[i],
                                    lv_color_hex(0x374151), 0);
        lv_obj_set_style_text_color(view->sensitivity_labels[i],
                                    lv_color_white(), LV_STATE_CHECKED);
        lv_obj_center(view->sensitivity_labels[i]);
    }

    view->mic_test_row = lv_obj_create(view->content_layer);
    lv_obj_remove_style_all(view->mic_test_row);
    lv_obj_set_size(view->mic_test_row, 174, 34);
    lv_obj_align(view->mic_test_row, LV_ALIGN_TOP_LEFT, 194, 352);
    lv_obj_set_flex_flow(view->mic_test_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->mic_test_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(view->mic_test_row, 6, 0);

    view->mic_test_button = lv_obj_create(view->mic_test_row);
    lv_obj_remove_style_all(view->mic_test_button);
    lv_obj_set_size(view->mic_test_button, 100, 34);
    lv_obj_add_flag(view->mic_test_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(view->mic_test_button, 6, 0);
    lv_obj_set_style_bg_opa(view->mic_test_button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(view->mic_test_button, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_color(view->mic_test_button, lv_color_hex(0x9ca3af),
                              LV_STATE_DISABLED);
    lv_obj_add_event_cb(view->mic_test_button,
                        danger_detection_view_mic_test_event,
                        LV_EVENT_CLICKED, view);

    view->mic_test_button_label = lv_label_create(view->mic_test_button);
    lv_label_set_text(view->mic_test_button_label, "测麦克风");
    lv_obj_set_style_text_font(
        view->mic_test_button_label,
        &lv_font_montserrat_lxgw_common_5500_16_4, 0);
    lv_obj_set_style_text_color(view->mic_test_button_label,
                                lv_color_white(), 0);
    lv_obj_center(view->mic_test_button_label);

    view->mic_test_status_label = lv_label_create(view->mic_test_row);
    lv_label_set_text(view->mic_test_status_label, "未测试");
    lv_obj_set_width(view->mic_test_status_label, 62);
    lv_obj_set_style_text_font(
        view->mic_test_status_label,
        &lv_font_montserrat_lxgw_common_5500_16_4, 0);
    lv_obj_set_style_text_color(view->mic_test_status_label,
                                lv_color_hex(0x6b7280), 0);

    view->scores_card = lv_obj_create(view->content_layer);
    lv_obj_remove_style_all(view->scores_card);
    lv_obj_set_size(view->scores_card, 320, 76);
    lv_obj_align(view->scores_card, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(view->scores_card, 22, 0);
    lv_obj_set_style_bg_color(view->scores_card, lv_color_hex(0xf5f5f5), 0);
    lv_obj_set_style_bg_opa(view->scores_card, LV_OPA_COVER, 0);

    view->horn_title_label = lv_label_create(view->scores_card);
    lv_label_set_text(view->horn_title_label, "HORN");
    lv_obj_set_style_text_font(view->horn_title_label, &lv_font_montserratMedium_12,
                               0);
    lv_obj_set_style_text_letter_space(view->horn_title_label, 2, 0);
    lv_obj_set_style_text_color(view->horn_title_label, lv_color_hex(0x6b7280), 0);
    lv_obj_align(view->horn_title_label, LV_ALIGN_TOP_LEFT, 18, 10);

    view->horn_confidence_label = lv_label_create(view->scores_card);
    lv_label_set_text(view->horn_confidence_label, "--");
    lv_obj_set_style_text_font(view->horn_confidence_label,
                               &lv_font_montserratMedium_27, 0);
    lv_obj_align(view->horn_confidence_label, LV_ALIGN_BOTTOM_LEFT, 18, -8);

    view->siren_title_label = lv_label_create(view->scores_card);
    lv_label_set_text(view->siren_title_label, "SIREN");
    lv_obj_set_style_text_font(view->siren_title_label,
                               &lv_font_montserratMedium_12, 0);
    lv_obj_set_style_text_letter_space(view->siren_title_label, 2, 0);
    lv_obj_set_style_text_color(view->siren_title_label, lv_color_hex(0x6b7280), 0);
    lv_obj_align(view->siren_title_label, LV_ALIGN_TOP_RIGHT, -18, 10);

    view->siren_confidence_label = lv_label_create(view->scores_card);
    lv_label_set_text(view->siren_confidence_label, "--");
    lv_obj_set_style_text_font(view->siren_confidence_label,
                               &lv_font_montserratMedium_27, 0);
    lv_obj_align(view->siren_confidence_label, LV_ALIGN_BOTTOM_RIGHT, -18, -8);

    view->alert_layer = lv_obj_create(view->screen);
    lv_obj_remove_style_all(view->alert_layer);
    lv_obj_set_size(view->alert_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(view->alert_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(view->alert_layer, LV_OBJ_FLAG_HIDDEN);

    view->alert_badge = lv_obj_create(view->alert_layer);
    lv_obj_remove_style_all(view->alert_badge);
    lv_obj_set_size(view->alert_badge, 156, 156);
    lv_obj_set_style_radius(view->alert_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(view->alert_badge, 6, 0);
    lv_obj_set_style_border_color(view->alert_badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(view->alert_badge, LV_OPA_TRANSP, 0);
    lv_obj_center(view->alert_badge);

    view->alert_icon_label = lv_label_create(view->alert_badge);
    lv_label_set_text(view->alert_icon_label, "!");
    lv_obj_set_style_text_font(view->alert_icon_label,
                               &lv_font_montserratMedium_58, 0);
    lv_obj_set_style_text_color(view->alert_icon_label, lv_color_white(), 0);
    lv_obj_center(view->alert_icon_label);

    lv_obj_move_foreground(view->back_btn);

    const danger_detection_view_model_t initial_model = {
        .status_text = "未开启",
        .category_text = "CURRENT: NONE",
        .primary_result_text = "NONE",
        .horn_confidence_text = "--",
        .siren_confidence_text = "--",
        .mic_test_status_text = "未测试",
        .sensitivity_mode = DANGER_DETECTION_VIEW_SENSITIVITY_STANDARD,
        .safety_monitor_enabled = false,
        .mic_test_running = false,
        .alert_visible = false,
    };
    danger_detection_view_apply_model(view, &initial_model);

    return view;
}

void danger_detection_view_destroy(danger_detection_view_t *view)
{
    if (view == NULL) {
        return;
    }

    if (view->screen != NULL) {
        lv_obj_delete(view->screen);
    }
    free(view);
}

lv_obj_t *danger_detection_view_get_screen(danger_detection_view_t *view)
{
    if (view == NULL) {
        return NULL;
    }
    return view->screen;
}

void danger_detection_view_apply_model(
    danger_detection_view_t *view,
    const danger_detection_view_model_t *model)
{
    if (view == NULL || model == NULL) {
        return;
    }

    danger_detection_view_set_text(view->status_label, model->status_text);
    danger_detection_view_set_text(view->category_label, model->category_text);
    danger_detection_view_set_text(view->primary_result_label,
                                   model->primary_result_text);
    danger_detection_view_set_text(view->horn_confidence_label,
                                   model->horn_confidence_text);
    danger_detection_view_set_text(view->siren_confidence_label,
                                   model->siren_confidence_text);
    danger_detection_view_set_text(view->mic_test_status_label,
                                   model->mic_test_status_text);
    danger_detection_view_set_text(
        view->sensitivity_title_label,
        danger_detection_view_sensitivity_hint(model->sensitivity_mode));

    view->updating_switch = true;
    if (model->safety_monitor_enabled) {
        lv_obj_add_state(view->safety_monitor_switch, LV_STATE_CHECKED);
        lv_obj_align(view->safety_monitor_knob, LV_ALIGN_RIGHT_MID, -3, 0);
    } else {
        lv_obj_clear_state(view->safety_monitor_switch, LV_STATE_CHECKED);
        lv_obj_align(view->safety_monitor_knob, LV_ALIGN_LEFT_MID, 3, 0);
    }
    view->updating_switch = false;

    view->updating_sensitivity = true;
    for (int i = 0; i < 3; ++i) {
        if (i == (int)model->sensitivity_mode) {
            lv_obj_add_state(view->sensitivity_buttons[i], LV_STATE_CHECKED);
            lv_obj_set_style_text_color(view->sensitivity_labels[i],
                                        lv_color_white(), 0);
        } else {
            lv_obj_clear_state(view->sensitivity_buttons[i], LV_STATE_CHECKED);
            lv_obj_set_style_text_color(view->sensitivity_labels[i],
                                        lv_color_hex(0x374151), 0);
        }
    }
    view->updating_sensitivity = false;

    if (model->mic_test_running) {
        lv_obj_add_state(view->mic_test_button, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(view->mic_test_status_label,
                                    lv_color_hex(0x111827), 0);
    } else {
        lv_obj_clear_state(view->mic_test_button, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(view->mic_test_status_label,
                                    lv_color_hex(0x6b7280), 0);
    }

    if (model->alert_visible) {
        lv_obj_set_style_bg_color(view->screen, lv_palette_main(LV_PALETTE_RED),
                                  0);
        lv_obj_set_style_text_color(view->back_label, lv_color_white(), 0);
        lv_obj_add_flag(view->content_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(view->alert_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_color(view->screen, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_color(view->back_label, lv_color_black(), 0);
        lv_obj_clear_flag(view->content_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->alert_layer, LV_OBJ_FLAG_HIDDEN);
    }
}
