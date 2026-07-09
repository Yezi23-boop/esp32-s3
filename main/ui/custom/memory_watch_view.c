#include "memory_watch_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_guider.h"
#include "ui_chinese_fonts.h"

#define MEMORY_WATCH_VIEW_MAX_INBOX_ITEMS 20U
#define MEMORY_WATCH_INBOX_CLICK_SLOP 12
#define MEMORY_WATCH_SWIPE_THRESHOLD 70
#define MEMORY_WATCH_SWIPE_MAX_VERTICAL_DRIFT 90
#define MEMORY_WATCH_VIEW_MAX_CONVERSATION_ITEMS 12U

typedef struct
{
    memory_watch_view_t *view;
    size_t index;
    lv_point_t press_point;
    bool pressed;
} memory_watch_view_inbox_row_ctx_t;

struct memory_watch_view
{
    lv_obj_t *screen;
    lv_obj_t *title_label;
    lv_obj_t *status_badge;
    lv_obj_t *inbox_badge;
    lv_obj_t *inbox_badge_label;
    lv_obj_t *connection_dot;
    lv_obj_t *voice_page;
    lv_obj_t *state_label;
    lv_obj_t *conversation_list;
    lv_obj_t *conversation_empty_label;
    lv_obj_t *voice_btn;
    lv_obj_t *voice_label;
    lv_obj_t *inbox_page;
    lv_obj_t *inbox_list;
    lv_obj_t *inbox_empty_label;
    lv_obj_t *detail_page;
    lv_obj_t *detail_time_label;
    lv_obj_t *detail_text_label;
    lv_point_t swipe_start;
    bool swipe_tracking;
    memory_watch_view_inbox_row_ctx_t inbox_row_ctx[MEMORY_WATCH_VIEW_MAX_INBOX_ITEMS];
    memory_watch_view_action_cb_t back_cb;
    memory_watch_view_action_cb_t press_start_cb;
    memory_watch_view_action_cb_t release_send_cb;
    memory_watch_view_action_cb_t slide_cancel_cb;
    memory_watch_view_action_cb_t cancel_waiting_cb;
    memory_watch_view_action_cb_t cancel_clarification_cb;
    memory_watch_view_action_cb_t open_inbox_cb;
    memory_watch_view_action_cb_t open_voice_cb;
    memory_watch_view_action_cb_t inbox_back_cb;
    memory_watch_view_inbox_item_cb_t open_inbox_item_cb;
    void *user_data;
    const memory_watch_view_inbox_item_t *inbox_items;
    size_t inbox_item_count;
    size_t selected_inbox_index;
    const char *detail_body; /**< 详情页完整正文（由 apply_model 更新）。 */
    const memory_watch_view_conversation_item_t *conversation_items;
    size_t conversation_item_count;
    memory_watch_view_page_t page;
    bool pressing;
    bool press_inside;
};

static const lv_coord_t kScreenWidth = 410;
static const lv_coord_t kScreenHeight = 502;
static const lv_coord_t kHeaderHeight = 96;
static const lv_coord_t kVoiceButtonWidth = 244;
static const lv_coord_t kVoiceButtonHeight = 58;

static void memory_watch_view_call(memory_watch_view_t *view,
                                   memory_watch_view_action_cb_t cb)
{
    if (view == NULL || cb == NULL)
    {
        return;
    }
    cb(view->user_data);
}

static void memory_watch_view_set_label(lv_obj_t *label, const char *text)
{
    if (label == NULL)
    {
        return;
    }
    lv_label_set_text(label, text != NULL ? text : "");
}

static void memory_watch_view_set_text_style(lv_obj_t *obj,
                                             lv_color_t color,
                                             const lv_font_t *font)
{
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void memory_watch_view_style_panel(lv_obj_t *obj, lv_color_t bg)
{
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xeaeaea),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void memory_watch_view_style_badge(lv_obj_t *obj, lv_color_t bg,
                                          lv_color_t text)
{
    lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(obj, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(obj, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    memory_watch_view_set_text_style(
        obj, text, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
}

static bool memory_watch_view_point_inside(const lv_area_t *area,
                                           const lv_point_t *point)
{
    if (area == NULL || point == NULL)
    {
        return false;
    }

    return point->x >= area->x1 && point->x <= area->x2 &&
           point->y >= area->y1 && point->y <= area->y2;
}

static bool memory_watch_view_voice_touch_inside(memory_watch_view_t *view)
{
    if (view == NULL || view->voice_btn == NULL)
    {
        return false;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL)
    {
        return view->press_inside;
    }

    lv_point_t point = {0};
    lv_area_t area = {0};
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(view->voice_btn, &area);
    return memory_watch_view_point_inside(&area, &point);
}

static void memory_watch_view_set_voice_label(memory_watch_view_t *view,
                                              const char *text)
{
    if (view == NULL || view->voice_label == NULL)
    {
        return;
    }
    lv_label_set_text(view->voice_label, text != NULL ? text : "");
    lv_obj_center(view->voice_label);
}

static void memory_watch_view_header_back_event(lv_event_t *e)
{
    memory_watch_view_t *view =
        (memory_watch_view_t *)lv_event_get_user_data(e);
    if (view == NULL)
    {
        return;
    }

    if (view->page == MEMORY_WATCH_VIEW_PAGE_VOICE)
    {
        memory_watch_view_call(view, view->back_cb);
    }
    else if (view->page == MEMORY_WATCH_VIEW_PAGE_INBOX)
    {
        memory_watch_view_call(view, view->open_voice_cb);
    }
    else
    {
        memory_watch_view_call(view, view->inbox_back_cb);
    }
}

static void memory_watch_view_open_inbox_event(lv_event_t *e)
{
    memory_watch_view_t *view =
        (memory_watch_view_t *)lv_event_get_user_data(e);
    memory_watch_view_call(view, view != NULL ? view->open_inbox_cb : NULL);
}

static void memory_watch_view_voice_event(lv_event_t *e)
{
    memory_watch_view_t *view =
        (memory_watch_view_t *)lv_event_get_user_data(e);
    if (view == NULL)
    {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)
    {
        view->pressing = true;
        view->press_inside = true;
        memory_watch_view_set_voice_label(view, "松开发送");
        memory_watch_view_call(view, view->press_start_cb);
        return;
    }

    if (code == LV_EVENT_PRESSING && view->pressing)
    {
        view->press_inside = memory_watch_view_voice_touch_inside(view);
        memory_watch_view_set_voice_label(
            view, view->press_inside ? "松开发送" : "松手取消");
        return;
    }

    if (code == LV_EVENT_RELEASED && view->pressing)
    {
        const bool send = memory_watch_view_voice_touch_inside(view);
        view->pressing = false;
        view->press_inside = send;
        memory_watch_view_call(view, send ? view->release_send_cb
                                          : view->slide_cancel_cb);
        return;
    }

    if (code == LV_EVENT_PRESS_LOST && view->pressing)
    {
        view->pressing = false;
        view->press_inside = false;
        memory_watch_view_set_voice_label(view, "按住说话");
        memory_watch_view_call(view, view->slide_cancel_cb);
    }
}

static void memory_watch_view_gesture_event(lv_event_t *e)
{
    memory_watch_view_t *view =
        (memory_watch_view_t *)lv_event_get_user_data(e);
    if (view == NULL)
    {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
    {
        indev = lv_indev_get_act();
    }
    if (indev == NULL)
    {
        return;
    }

    lv_dir_t dir = LV_DIR_NONE;
    if (code == LV_EVENT_GESTURE)
    {
        dir = lv_indev_get_gesture_dir(indev);
    }
    else if (code == LV_EVENT_PRESSED)
    {
        view->swipe_tracking = true;
        lv_indev_get_point(indev, &view->swipe_start);
        return;
    }
    else if (code == LV_EVENT_PRESS_LOST)
    {
        view->swipe_tracking = false;
        return;
    }
    else if (code == LV_EVENT_RELEASED && view->swipe_tracking)
    {
        lv_point_t release_point = {0};
        lv_indev_get_point(indev, &release_point);
        const int32_t dx = release_point.x - view->swipe_start.x;
        const int32_t dy = release_point.y - view->swipe_start.y;
        view->swipe_tracking = false;

        if (abs(dx) < MEMORY_WATCH_SWIPE_THRESHOLD ||
            abs(dy) > MEMORY_WATCH_SWIPE_MAX_VERTICAL_DRIFT)
        {
            return;
        }
        dir = dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT;
    }
    else
    {
        return;
    }

    if (dir == LV_DIR_LEFT && view->page == MEMORY_WATCH_VIEW_PAGE_VOICE)
    {
        memory_watch_view_call(view, view->open_inbox_cb);
    }
    else if (dir == LV_DIR_RIGHT &&
             view->page == MEMORY_WATCH_VIEW_PAGE_INBOX)
    {
        memory_watch_view_call(view, view->open_voice_cb);
    }
    else if (dir == LV_DIR_RIGHT &&
             view->page == MEMORY_WATCH_VIEW_PAGE_INBOX_DETAIL)
    {
        memory_watch_view_call(view, view->inbox_back_cb);
    }
}

static void memory_watch_view_inbox_item_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);
    memory_watch_view_inbox_row_ctx_t *ctx =
        (memory_watch_view_inbox_row_ctx_t *)lv_obj_get_user_data(target);
    if (ctx == NULL || ctx->view == NULL ||
        ctx->view->open_inbox_item_cb == NULL)
    {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (code == LV_EVENT_PRESSED)
    {
        ctx->pressed = true;
        if (indev != NULL)
        {
            lv_indev_get_point(indev, &ctx->press_point);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST)
    {
        ctx->pressed = false;
        return;
    }

    if (code != LV_EVENT_CLICKED)
    {
        return;
    }

    if (ctx->pressed && indev != NULL)
    {
        lv_point_t release_point = {0};
        lv_indev_get_point(indev, &release_point);
        const int32_t dx = release_point.x - ctx->press_point.x;
        const int32_t dy = release_point.y - ctx->press_point.y;
        if (abs(dx) > MEMORY_WATCH_INBOX_CLICK_SLOP ||
            abs(dy) > MEMORY_WATCH_INBOX_CLICK_SLOP)
        {
            ctx->pressed = false;
            return;
        }
    }
    ctx->pressed = false;
    ctx->view->open_inbox_item_cb(ctx->index, ctx->view->user_data);
}

static lv_obj_t *memory_watch_view_create_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, kScreenWidth, kScreenHeight - kHeaderHeight);
    lv_obj_set_pos(page, 0, kHeaderHeight);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return page;
}

static lv_obj_t *memory_watch_view_create_text_button(lv_obj_t *parent,
                                                      const char *text,
                                                      lv_coord_t width,
                                                      lv_coord_t height,
                                                      lv_color_t bg,
                                                      lv_color_t text_color)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xd8d8d4),
                              LV_PART_MAIN | LV_STATE_DISABLED);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    memory_watch_view_set_text_style(
        label, text_color, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8a8984),
                                LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_center(label);
    return button;
}

static lv_color_t memory_watch_view_connection_color(
    memory_watch_view_connection_state_t state)
{
    switch (state)
    {
    case MEMORY_WATCH_VIEW_CONNECTION_ONLINE:
        return lv_color_hex(0x2f8f46);
    case MEMORY_WATCH_VIEW_CONNECTION_OFFLINE:
        return lv_color_hex(0xd84a3a);
    case MEMORY_WATCH_VIEW_CONNECTION_UNKNOWN:
    default:
        return lv_color_hex(0xb8b8b3);
    }
}

static void memory_watch_view_update_connection_dot(
    memory_watch_view_t *view,
    memory_watch_view_connection_state_t state)
{
    if (view == NULL || view->connection_dot == NULL)
    {
        return;
    }
    lv_obj_set_style_bg_color(view->connection_dot,
                              memory_watch_view_connection_color(state),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_coord_t memory_watch_view_message_height(const char *text,
                                                   lv_coord_t text_width)
{
    const size_t len = text != NULL ? strlen(text) : 0U;
    const size_t chars_per_line = text_width > 240 ? 24U : 20U;
    size_t lines = (len / chars_per_line) + 1U;
    if (lines > 5U)
    {
        lines = 5U;
    }
    return (lv_coord_t)(34 + (lines * 18U));
}

static void memory_watch_view_rebuild_conversation(memory_watch_view_t *view)
{
    if (view == NULL || view->conversation_list == NULL)
    {
        return;
    }

    lv_obj_clean(view->conversation_list);
    const size_t item_count =
        view->conversation_item_count < MEMORY_WATCH_VIEW_MAX_CONVERSATION_ITEMS
            ? view->conversation_item_count
            : MEMORY_WATCH_VIEW_MAX_CONVERSATION_ITEMS;
    if (item_count == 0U || view->conversation_items == NULL)
    {
        lv_obj_remove_flag(view->conversation_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(view->conversation_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_coord_t y = 0;
    for (size_t i = 0; i < item_count; ++i)
    {
        const memory_watch_view_conversation_item_t *item =
            &view->conversation_items[i];
        const char *text = item->text != NULL ? item->text : "";

        if (item->role == MEMORY_WATCH_VIEW_CONVERSATION_SYSTEM)
        {
            lv_obj_t *label = lv_label_create(view->conversation_list);
            lv_label_set_text(label, text);
            lv_obj_set_pos(label, 0, y);
            lv_obj_set_size(label, 330, 28);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            memory_watch_view_style_badge(label, lv_color_hex(0xfbf3db),
                                          lv_color_hex(0x956400));
            lv_obj_add_event_cb(label, memory_watch_view_gesture_event,
                                LV_EVENT_ALL, view);
            y += 38;
            continue;
        }

        const bool from_user =
            item->role == MEMORY_WATCH_VIEW_CONVERSATION_USER;
        const lv_coord_t card_w = from_user ? 262 : 306;
        const lv_coord_t card_x = from_user ? 68 : 0;
        const lv_coord_t card_h =
            memory_watch_view_message_height(text, card_w - 24);
        lv_obj_t *card = lv_obj_create(view->conversation_list);
        lv_obj_set_pos(card, card_x, y);
        lv_obj_set_size(card, card_w, card_h);
        memory_watch_view_style_panel(
            card, from_user ? lv_color_hex(0xf7f6f3) : lv_color_hex(0xffffff));
        lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(card, memory_watch_view_gesture_event,
                            LV_EVENT_ALL, view);

        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, card_w - 24);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        memory_watch_view_set_text_style(
            label, lv_color_hex(0x2f3437),
            &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
        y += card_h + 12;
    }

    lv_obj_set_height(view->conversation_list, 252);
    lv_obj_scroll_to_y(view->conversation_list, 32000, LV_ANIM_OFF);
}

static void memory_watch_view_update_inbox_badge(memory_watch_view_t *view,
                                                 uint8_t unread_count)
{
    if (view == NULL || view->inbox_badge_label == NULL)
    {
        return;
    }

    char text[24];
    if (unread_count > 0U)
    {
        snprintf(text, sizeof(text), "收件箱 %u", (unsigned int)unread_count);
    }
    else
    {
        snprintf(text, sizeof(text), "收件箱");
    }
    lv_label_set_text(view->inbox_badge_label, text);
}

static void memory_watch_view_rebuild_inbox_list(memory_watch_view_t *view)
{
    if (view == NULL || view->inbox_list == NULL)
    {
        return;
    }

    lv_obj_clean(view->inbox_list);
    const size_t item_count =
        view->inbox_item_count < MEMORY_WATCH_VIEW_MAX_INBOX_ITEMS
            ? view->inbox_item_count
            : MEMORY_WATCH_VIEW_MAX_INBOX_ITEMS;
    if (item_count == 0U || view->inbox_items == NULL)
    {
        lv_obj_remove_flag(view->inbox_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(view->inbox_empty_label, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < item_count; ++i)
    {
        const memory_watch_view_inbox_item_t *item = &view->inbox_items[i];
        lv_obj_t *row = lv_obj_create(view->inbox_list);
        lv_obj_set_size(row, 330, 74);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * 82U));
        memory_watch_view_style_panel(
            row, item->read ? lv_color_hex(0xffffff) : lv_color_hex(0xf7f6f3));
        lv_obj_set_style_pad_all(row, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

        view->inbox_row_ctx[i].view = view;
        view->inbox_row_ctx[i].index = i;
        lv_obj_set_user_data(row, &view->inbox_row_ctx[i]);
        lv_obj_add_event_cb(row, memory_watch_view_inbox_item_event,
                            LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(row, memory_watch_view_gesture_event,
                            LV_EVENT_ALL, view);

        if (!item->read)
        {
            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_set_size(dot, 7, 7);
            lv_obj_set_pos(dot, 4, 16);
            lv_obj_set_style_radius(dot, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x346538),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(dot, 0,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *time_label = lv_label_create(row);
        lv_label_set_text(time_label,
                          item->created_at != NULL ? item->created_at : "");
        memory_watch_view_set_text_style(
            time_label, lv_color_hex(0x787774),
            &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
        lv_obj_set_pos(time_label, 20, 6);
        lv_obj_set_size(time_label, 292, 18);
        lv_label_set_long_mode(time_label, LV_LABEL_LONG_DOT);

        lv_obj_t *preview_label = lv_label_create(row);
        lv_label_set_text(preview_label, item->text != NULL ? item->text : "");
        memory_watch_view_set_text_style(
            preview_label, lv_color_hex(0x2f3437),
            &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
        lv_obj_set_pos(preview_label, 20, 32);
        lv_obj_set_size(preview_label, 292, 24);
        lv_label_set_long_mode(preview_label, LV_LABEL_LONG_DOT);
    }

    lv_obj_set_height(view->inbox_list, 318);
}

static void memory_watch_view_update_detail(memory_watch_view_t *view)
{
    if (view == NULL || view->detail_time_label == NULL ||
        view->detail_text_label == NULL)
    {
        return;
    }

    if (view->inbox_items == NULL ||
        view->selected_inbox_index >= view->inbox_item_count)
    {
        lv_label_set_text(view->detail_time_label, "暂无消息");
        lv_label_set_text(view->detail_text_label, "");
        return;
    }

    const memory_watch_view_inbox_item_t *item =
        &view->inbox_items[view->selected_inbox_index];
    lv_label_set_text(view->detail_time_label,
                      item->created_at != NULL ? item->created_at : "");
    /* 优先使用 controller 提供的完整 body，回退到 summary preview */
    const char *body = (view->detail_body != NULL &&
                        view->detail_body[0] != '\0')
                           ? view->detail_body
                           : item->text;
    lv_label_set_text(view->detail_text_label,
                      body != NULL ? body : "");
}

static void memory_watch_view_show_page(memory_watch_view_t *view,
                                        memory_watch_view_page_t page)
{
    if (view == NULL)
    {
        return;
    }

    view->page = page;
    lv_obj_add_flag(view->voice_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->inbox_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->detail_page, LV_OBJ_FLAG_HIDDEN);

    if (page == MEMORY_WATCH_VIEW_PAGE_VOICE)
    {
        lv_obj_remove_flag(view->title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->inbox_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->connection_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->status_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->voice_page, LV_OBJ_FLAG_HIDDEN);
    }
    else if (page == MEMORY_WATCH_VIEW_PAGE_INBOX)
    {
        lv_label_set_text(view->title_label, "收件箱");
        lv_obj_remove_flag(view->title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->inbox_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->connection_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->status_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->inbox_page, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text(view->title_label, "消息");
        lv_obj_remove_flag(view->title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->inbox_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->connection_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->status_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->detail_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void memory_watch_view_create_header(memory_watch_view_t *view)
{
    lv_obj_t *back_btn = memory_watch_view_create_text_button(
        view->screen, "<", 52, 44, lv_color_hex(0xffffff),
        lv_color_hex(0x111111));
    lv_obj_set_pos(back_btn, 40, 24);
    memory_watch_view_style_panel(back_btn, lv_color_hex(0xffffff));
    lv_obj_add_event_cb(back_btn, memory_watch_view_header_back_event,
                        LV_EVENT_CLICKED, view);

    view->title_label = lv_label_create(view->screen);
    lv_label_set_text(view->title_label, "Hermes");
    lv_obj_set_pos(view->title_label, 108, 22);
    lv_obj_set_size(view->title_label, 140, 34);
    lv_label_set_long_mode(view->title_label, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        view->title_label, lv_color_hex(0x111111),
        &lv_font_montserrat_lxgw_tghz_level1_3500_27_4);

    view->status_badge = lv_label_create(view->screen);
    lv_obj_set_pos(view->status_badge, 108, 56);
    lv_obj_set_size(view->status_badge, 116, 28);
    lv_label_set_long_mode(view->status_badge, LV_LABEL_LONG_DOT);
    memory_watch_view_style_badge(view->status_badge, lv_color_hex(0xedf3ec),
                                  lv_color_hex(0x346538));

    view->inbox_badge = lv_btn_create(view->screen);
    lv_obj_set_pos(view->inbox_badge, 258, 26);
    lv_obj_set_size(view->inbox_badge, 108, 38);
    memory_watch_view_style_panel(view->inbox_badge, lv_color_hex(0xffffff));
    lv_obj_set_style_pad_all(view->inbox_badge, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(view->inbox_badge, memory_watch_view_open_inbox_event,
                        LV_EVENT_CLICKED, view);

    view->inbox_badge_label = lv_label_create(view->inbox_badge);
    lv_label_set_text(view->inbox_badge_label, "收件箱");
    memory_watch_view_set_text_style(
        view->inbox_badge_label, lv_color_hex(0x2f3437),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
    lv_obj_center(view->inbox_badge_label);

    view->connection_dot = lv_obj_create(view->screen);
    lv_obj_set_pos(view->connection_dot, 244, 41);
    lv_obj_set_size(view->connection_dot, 8, 8);
    lv_obj_set_style_radius(view->connection_dot, 4,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->connection_dot, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->connection_dot, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(view->connection_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(view->connection_dot, LV_OBJ_FLAG_SCROLLABLE);
}

static void memory_watch_view_create_voice_page(memory_watch_view_t *view)
{
    view->voice_page = memory_watch_view_create_page(view->screen);
    lv_obj_add_event_cb(view->voice_page, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    view->state_label = lv_label_create(view->voice_page);
    lv_obj_set_pos(view->state_label, 40, 0);
    lv_obj_set_size(view->state_label, 132, 30);
    lv_label_set_long_mode(view->state_label, LV_LABEL_LONG_DOT);
    memory_watch_view_style_badge(view->state_label, lv_color_hex(0xfbf3db),
                                  lv_color_hex(0x956400));

    view->conversation_list = lv_obj_create(view->voice_page);
    lv_obj_set_pos(view->conversation_list, 40, 40);
    lv_obj_set_size(view->conversation_list, 330, 252);
    lv_obj_set_style_bg_opa(view->conversation_list, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->conversation_list, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(view->conversation_list, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(view->conversation_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view->conversation_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(view->conversation_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(view->conversation_list, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    view->conversation_empty_label = lv_label_create(view->voice_page);
    lv_label_set_text(view->conversation_empty_label, "按住说话开始记录");
    lv_obj_set_pos(view->conversation_empty_label, 40, 142);
    lv_obj_set_size(view->conversation_empty_label, 330, 28);
    lv_label_set_long_mode(view->conversation_empty_label, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        view->conversation_empty_label, lv_color_hex(0x787774),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
    lv_obj_add_event_cb(view->conversation_empty_label,
                        memory_watch_view_gesture_event, LV_EVENT_ALL, view);

    view->voice_btn = memory_watch_view_create_text_button(
        view->voice_page, "按住说话", kVoiceButtonWidth, kVoiceButtonHeight,
        lv_color_hex(0x111111), lv_color_hex(0xffffff));
    lv_obj_set_pos(view->voice_btn, 83, 320);
    view->voice_label = lv_obj_get_child(view->voice_btn, 0);
    lv_obj_add_event_cb(view->voice_btn, memory_watch_view_voice_event,
                        LV_EVENT_ALL, view);
}

static void memory_watch_view_create_inbox_page(memory_watch_view_t *view)
{
    view->inbox_page = memory_watch_view_create_page(view->screen);
    lv_obj_add_event_cb(view->inbox_page, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    lv_obj_t *subtitle = lv_label_create(view->inbox_page);
    lv_label_set_text(subtitle, "Hermes 发来的短消息, 只读查看");
    lv_obj_set_pos(subtitle, 40, 0);
    lv_obj_set_size(subtitle, 330, 28);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        subtitle, lv_color_hex(0x787774),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);

    view->inbox_list = lv_obj_create(view->inbox_page);
    lv_obj_set_pos(view->inbox_list, 40, 42);
    lv_obj_set_size(view->inbox_list, 330, 318);
    lv_obj_set_style_bg_opa(view->inbox_list, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->inbox_list, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(view->inbox_list, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(view->inbox_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view->inbox_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(view->inbox_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(view->inbox_list, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    view->inbox_empty_label = lv_label_create(view->inbox_page);
    lv_label_set_text(view->inbox_empty_label, "暂无收件箱消息");
    lv_obj_set_pos(view->inbox_empty_label, 40, 164);
    lv_obj_set_size(view->inbox_empty_label, 330, 28);
    lv_label_set_long_mode(view->inbox_empty_label, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        view->inbox_empty_label, lv_color_hex(0x787774),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
}

static void memory_watch_view_create_detail_page(memory_watch_view_t *view)
{
    view->detail_page = memory_watch_view_create_page(view->screen);
    lv_obj_add_event_cb(view->detail_page, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    view->detail_time_label = lv_label_create(view->detail_page);
    lv_obj_set_pos(view->detail_time_label, 40, 0);
    lv_obj_set_size(view->detail_time_label, 330, 24);
    lv_label_set_long_mode(view->detail_time_label, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        view->detail_time_label, lv_color_hex(0x787774),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);

    lv_obj_t *detail_card = lv_obj_create(view->detail_page);
    lv_obj_set_pos(detail_card, 40, 38);
    lv_obj_set_size(detail_card, 330, 294);
    memory_watch_view_style_panel(detail_card, lv_color_hex(0xffffff));
    lv_obj_set_style_pad_all(detail_card, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(detail_card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_card, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(detail_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(detail_card, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    view->detail_text_label = lv_label_create(detail_card);
    lv_obj_set_width(view->detail_text_label, 298);
    lv_label_set_long_mode(view->detail_text_label, LV_LABEL_LONG_WRAP);
    memory_watch_view_set_text_style(
        view->detail_text_label, lv_color_hex(0x2f3437),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
    lv_obj_align(view->detail_text_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = lv_label_create(view->detail_page);
    lv_label_set_text(hint, "右滑返回收件箱");
    lv_obj_set_pos(hint, 40, 348);
    lv_obj_set_size(hint, 330, 24);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    memory_watch_view_set_text_style(
        hint, lv_color_hex(0x787774),
        &lv_font_montserrat_lxgw_tghz_level1_3500_16_4);
    lv_obj_add_event_cb(hint, memory_watch_view_gesture_event, LV_EVENT_ALL,
                        view);
}

memory_watch_view_t *memory_watch_view_create(
    const memory_watch_view_config_t *config)
{
    memory_watch_view_t *view =
        (memory_watch_view_t *)calloc(1, sizeof(memory_watch_view_t));
    if (view == NULL)
    {
        return NULL;
    }

    if (config != NULL)
    {
        view->back_cb = config->back_cb;
        view->press_start_cb = config->press_start_cb;
        view->release_send_cb = config->release_send_cb;
        view->slide_cancel_cb = config->slide_cancel_cb;
        view->cancel_waiting_cb = config->cancel_waiting_cb;
        view->cancel_clarification_cb = config->cancel_clarification_cb;
        view->open_inbox_cb = config->open_inbox_cb;
        view->open_voice_cb = config->open_voice_cb;
        view->inbox_back_cb = config->inbox_back_cb;
        view->open_inbox_item_cb = config->open_inbox_item_cb;
        view->user_data = config->user_data;
    }

    view->screen = lv_obj_create(NULL);
    lv_obj_set_size(view->screen, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(view->screen, lv_color_hex(0xfbfbfa),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->screen, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->screen, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(view->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(view->screen, memory_watch_view_gesture_event,
                        LV_EVENT_ALL, view);

    memory_watch_view_create_header(view);
    memory_watch_view_create_voice_page(view);
    memory_watch_view_create_inbox_page(view);
    memory_watch_view_create_detail_page(view);

    const memory_watch_view_model_t initial_model = {
        .top_status_text = "Hermes 待检测",
        .state_text = "待命",
        .user_text = "",
        .reply_text = "按住按钮说话",
        .voice_button_text = "按住说话",
        .page = MEMORY_WATCH_VIEW_PAGE_VOICE,
        .conversation_items = NULL,
        .conversation_item_count = 0,
        .connection_state = MEMORY_WATCH_VIEW_CONNECTION_UNKNOWN,
        .inbox_items = NULL,
        .inbox_item_count = 0,
        .selected_inbox_index = 0,
        .inbox_unread_count = 0,
        .voice_button_enabled = false,
        .cancel_visible = false,
        .cancel_is_clarification = false,
    };
    memory_watch_view_apply_model(view, &initial_model);
    return view;
}

void memory_watch_view_destroy(memory_watch_view_t *view)
{
    if (view == NULL)
    {
        return;
    }

    if (view->screen != NULL)
    {
        lv_obj_delete(view->screen);
    }
    free(view);
}

lv_obj_t *memory_watch_view_get_screen(const memory_watch_view_t *view)
{
    if (view == NULL)
    {
        return NULL;
    }
    return view->screen;
}

void memory_watch_view_apply_model(memory_watch_view_t *view,
                                   const memory_watch_view_model_t *model)
{
    if (view == NULL || model == NULL)
    {
        return;
    }

    view->inbox_items = model->inbox_items;
    view->inbox_item_count = model->inbox_item_count;
    view->selected_inbox_index = model->selected_inbox_index;
    view->detail_body = model->detail_body;
    view->conversation_items = model->conversation_items;
    view->conversation_item_count = model->conversation_item_count;

    memory_watch_view_set_label(view->status_badge, model->top_status_text);
    memory_watch_view_set_label(view->state_label, model->state_text);
    memory_watch_view_update_connection_dot(view, model->connection_state);
    memory_watch_view_rebuild_conversation(view);
    memory_watch_view_update_inbox_badge(view, model->inbox_unread_count);
    memory_watch_view_rebuild_inbox_list(view);
    memory_watch_view_update_detail(view);
    memory_watch_view_show_page(view, model->page);

    if (view->voice_btn != NULL)
    {
        if (model->voice_button_enabled)
        {
            lv_obj_clear_state(view->voice_btn, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(view->voice_btn, LV_STATE_DISABLED);
        }
    }

    if (!view->pressing)
    {
        memory_watch_view_set_voice_label(view, model->voice_button_text);
    }
}
