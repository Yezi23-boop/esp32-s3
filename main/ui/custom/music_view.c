#include "music_view.h"

#include <stdio.h>
#include <string.h>

#include "draw/lv_draw_line.h"
#include "draw/lv_draw_triangle.h"
#include "esp_heap_caps.h"
#include "ui_chinese_fonts.h"

static const lv_coord_t kQrCanvasSize = 220;

static const lv_point_precise_t kMusicDiagonalPoints[] = {
    {285, 0},
    {90, 502},
};
static const lv_point_precise_t kMusicBackPoints[] = {
    {8, 0},
    {0, 5},
    {8, 10},
};
static const lv_point_precise_t kMusicChevronPoints[] = {
    {0, 0},
    {5, 5},
    {0, 10},
};
static const lv_point_precise_t kMusicModePoints[] = {
    {12, 5},
    {11, 2},
    {8, 0},
    {4, 0},
    {1, 2},
    {0, 5},
    {1, 8},
    {4, 10},
    {8, 10},
    {11, 8},
    {9, 8},
};

extern const lv_image_dsc_t _music_artwork_RGB565A8_104x104;

/** UI 仅缓存最近三批目录数据；曲目正文放在 PSRAM。 */
#define MUSIC_VIEW_CATALOG_CACHE_CAPACITY (MUSIC_SERVICE_CATALOG_PAGE_SIZE * 3U)
#define MUSIC_VIEW_CATALOG_SCROLL_THRESHOLD 12
#define MUSIC_VIEW_CATALOG_ROW_HEIGHT 44
#define MUSIC_VIEW_CATALOG_ROW_GAP 8

typedef struct
{
    music_view_t *view;
    uint8_t index;
} music_view_source_context_t;

typedef struct
{
    music_view_t *view;
    uint8_t index;
} music_view_track_context_t;

struct music_view
{
    lv_obj_t *screen;
    lv_obj_t *state_label;
    lv_obj_t *track_label;
    lv_obj_t *artist_label;
    lv_obj_t *state_dot;
    lv_obj_t *artwork;
    lv_obj_t *section_label;
    lv_obj_t *section_kicker;
    lv_obj_t *source_buttons[4];
    lv_obj_t *catalog_list;
    lv_obj_t *track_buttons[MUSIC_VIEW_CATALOG_CACHE_CAPACITY];
    lv_obj_t *account_button;
    lv_obj_t *account_status;
    lv_obj_t *qr_canvas;
    lv_obj_t *track_panel;
    lv_obj_t *previous_button;
    lv_obj_t *next_button;
    uint8_t *qr_buffer;
    lv_obj_t *toggle_button;
    lv_obj_t *toggle_label;
    lv_obj_t *mode_button;
    lv_obj_t *mode_label;
    music_view_source_context_t source_context[4];
    music_view_track_context_t track_context[MUSIC_VIEW_CATALOG_CACHE_CAPACITY];
    music_service_catalog_track_t *catalog_tracks;
    char catalog_source_id[MUSIC_SERVICE_SOURCE_ID_MAX_BYTES];
    size_t catalog_count;
    uint32_t catalog_total;
    uint32_t catalog_next_offset;
    uint32_t catalog_first_offset;
    uint32_t catalog_generation;
    music_view_config_t config;
    bool catalog_visible;
    bool account_visible;
    bool catalog_snapshot_applied;
    bool catalog_load_pending;
    bool playing;
};

static const char *const kSourceLabels[4] = {
    "今日推荐", "我喜欢", "我的歌单", "最近播放",
};

static void music_view_text_style(lv_obj_t *obj, lv_color_t color,
                                  const lv_font_t *font)
{
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void music_view_panel_style(lv_obj_t *obj, lv_color_t background)
{
    lv_obj_set_style_radius(obj, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xdef4e8),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)36,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_obj_t *music_view_button(lv_obj_t *parent, const char *text,
                                   lv_coord_t x, lv_coord_t y,
                                   lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    music_view_panel_style(button, lv_color_hex(0x151d1c));
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_width(label, width - 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    music_view_text_style(label, lv_color_hex(0xeff5f0),
                          &lv_font_montserrat_lxgw_common_5500_16_4);
    lv_obj_center(label);
    return button;
}

static void music_view_line_style(lv_obj_t *line, lv_color_t color,
                                  lv_opa_t opacity, int32_t width)
{
    lv_obj_set_style_line_color(line, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(line, opacity, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(line, width, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(line, true,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *music_view_chevron(lv_obj_t *parent, lv_coord_t x,
                                    lv_coord_t y)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, kMusicChevronPoints,
                       sizeof(kMusicChevronPoints) /
                           sizeof(kMusicChevronPoints[0]));
    lv_obj_set_size(line, 8, 12);
    lv_obj_set_pos(line, x, y);
    music_view_line_style(line, lv_color_hex(0x9eaea6), LV_OPA_90, 1);
    return line;
}

static lv_obj_t *music_view_source_button(lv_obj_t *parent, const char *text,
                                          lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *button = music_view_button(parent, text, x, y, 160, 54);
    lv_obj_set_style_radius(button, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_80,
                            LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_size(label, 118, 20);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_lxgw_music_ui_14_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *accent = lv_obj_create(button);
    lv_obj_set_size(accent, 2, 30);
    lv_obj_set_pos(accent, 0, 12);
    lv_obj_set_style_radius(accent, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0xb5efd0),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(accent, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    music_view_chevron(button, 133, 22);
    return button;
}

static void music_view_icon_draw_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (view == NULL || button == NULL || layer == NULL)
    {
        return;
    }

    lv_area_t coords;
    lv_obj_get_coords(button, &coords);
    const lv_coord_t center_x = (coords.x1 + coords.x2) / 2;
    const lv_coord_t center_y = (coords.y1 + coords.y2) / 2;
    const lv_color_t color = button == view->toggle_button
                                 ? lv_color_hex(0x0b1010)
                                 : lv_color_hex(0x9eaea6);

    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = color;
    triangle.opa = LV_OPA_COVER;

    if (button == view->previous_button)
    {
        triangle.p[0] = (lv_point_precise_t){center_x + 5, center_y - 7};
        triangle.p[1] = (lv_point_precise_t){center_x - 5, center_y};
        triangle.p[2] = (lv_point_precise_t){center_x + 5, center_y + 7};
        lv_draw_triangle(layer, &triangle);
        return;
    }

    if (button == view->next_button)
    {
        triangle.p[0] = (lv_point_precise_t){center_x - 5, center_y - 7};
        triangle.p[1] = (lv_point_precise_t){center_x + 4, center_y};
        triangle.p[2] = (lv_point_precise_t){center_x - 5, center_y + 7};
        lv_draw_triangle(layer, &triangle);

        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.opa = LV_OPA_COVER;
        line.width = 2;
        line.round_start = true;
        line.round_end = true;
        line.p1 = (lv_point_precise_t){center_x + 8, center_y - 7};
        line.p2 = (lv_point_precise_t){center_x + 8, center_y + 7};
        lv_draw_line(layer, &line);
        return;
    }

    if (view->playing)
    {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = color;
        line.opa = LV_OPA_COVER;
        line.width = 4;
        line.round_start = true;
        line.round_end = true;
        line.p1 = (lv_point_precise_t){center_x - 5, center_y - 8};
        line.p2 = (lv_point_precise_t){center_x - 5, center_y + 8};
        lv_draw_line(layer, &line);
        line.p1 = (lv_point_precise_t){center_x + 5, center_y - 8};
        line.p2 = (lv_point_precise_t){center_x + 5, center_y + 8};
        lv_draw_line(layer, &line);
    }
    else
    {
        triangle.p[0] = (lv_point_precise_t){center_x - 5, center_y - 8};
        triangle.p[1] = (lv_point_precise_t){center_x + 7, center_y};
        triangle.p[2] = (lv_point_precise_t){center_x - 5, center_y + 8};
        lv_draw_triangle(layer, &triangle);
    }
}

static lv_obj_t *music_view_icon_button(lv_obj_t *parent, lv_coord_t x,
                                        lv_coord_t y, lv_coord_t size,
                                        music_view_t *view)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_pos(button, x, y);
    music_view_panel_style(button, lv_color_hex(0x151d1c));
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_70,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(button, music_view_icon_draw_event, LV_EVENT_DRAW_MAIN,
                        view);
    return button;
}

static void music_view_back_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view == NULL)
    {
        return;
    }
    if (view->catalog_visible && view->config.catalog_back_cb != NULL)
    {
        view->config.catalog_back_cb(view->config.user_data);
        return;
    }
    if (view->config.back_cb != NULL)
    {
        view->config.back_cb(view->config.user_data);
    }
}

static void music_view_source_event(lv_event_t *event)
{
    music_view_source_context_t *context =
        (music_view_source_context_t *)lv_event_get_user_data(event);
    if (context != NULL && context->view != NULL &&
        context->view->config.source_cb != NULL)
    {
        context->view->config.source_cb(context->index,
                                        context->view->config.user_data);
    }
}

static void music_view_track_event(lv_event_t *event)
{
    music_view_track_context_t *context =
        (music_view_track_context_t *)lv_event_get_user_data(event);
    if (context != NULL && context->view != NULL &&
        context->view->config.track_cb != NULL &&
        context->index < context->view->catalog_count)
    {
        const music_service_catalog_track_t *track =
            &context->view->catalog_tracks[context->index];
        context->view->config.track_cb(context->view->catalog_source_id,
                                       track->track_id,
                                       context->view->config.user_data);
    }
}

static void music_view_catalog_scroll_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view == NULL || !view->catalog_visible || view->catalog_load_pending ||
        view->config.catalog_load_more_cb == NULL ||
        view->catalog_source_id[0] == '\0' || view->catalog_total == 0U ||
        view->catalog_next_offset >= view->catalog_total ||
        lv_obj_get_scroll_bottom(view->catalog_list) >
            MUSIC_VIEW_CATALOG_SCROLL_THRESHOLD)
    {
        return;
    }
    view->catalog_load_pending = true;
    view->config.catalog_load_more_cb(view->catalog_source_id,
                                      view->catalog_next_offset,
                                      view->config.user_data);
}

static void music_view_account_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view != NULL && view->config.account_cb != NULL)
    {
        view->config.account_cb(view->config.user_data);
    }
}

static void music_view_toggle_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view != NULL && view->config.toggle_cb != NULL)
    {
        view->config.toggle_cb(view->config.user_data);
    }
}

static void music_view_previous_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view != NULL && view->config.previous_cb != NULL)
    {
        view->config.previous_cb(view->config.user_data);
    }
}

static void music_view_next_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view != NULL && view->config.next_cb != NULL)
    {
        view->config.next_cb(view->config.user_data);
    }
}

static void music_view_mode_event(lv_event_t *event)
{
    music_view_t *view = (music_view_t *)lv_event_get_user_data(event);
    if (view == NULL || view->config.mode_cb == NULL)
    {
        return;
    }
    /* controller sends the mode command through the service owner. */
    view->config.mode_cb(MUSIC_SERVICE_MODE_REPEAT_ALL,
                         view->config.user_data);
}

static const char *music_view_state_text(const music_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return "状态未知";
    }
    switch (snapshot->state)
    {
    case MUSIC_SERVICE_STATE_BUFFERING:
        return "正在缓冲";
    case MUSIC_SERVICE_STATE_PLAYING:
        return "正在播放";
    case MUSIC_SERVICE_STATE_PAUSED:
        return "已暂停";
    case MUSIC_SERVICE_STATE_ERROR:
        return "播放失败";
    case MUSIC_SERVICE_STATE_STOPPED:
    default:
        return "未播放";
    }
}

static const char *music_view_mode_text(music_service_mode_t mode)
{
    switch (mode)
    {
    case MUSIC_SERVICE_MODE_REPEAT_ONE:
        return "单曲循环";
    case MUSIC_SERVICE_MODE_SHUFFLE:
        return "随机播放";
    case MUSIC_SERVICE_MODE_ORDER:
        return "顺序播放";
    case MUSIC_SERVICE_MODE_SMART:
        return "智能播放";
    case MUSIC_SERVICE_MODE_REPEAT_ALL:
    default:
        return "列表循环";
    }
}

/** 清空滚动目录，保留已经创建的 LVGL 行对象供后续复用。 */
static void music_view_reset_catalog(music_view_t *view)
{
    view->catalog_count = 0U;
    view->catalog_total = 0U;
    view->catalog_next_offset = 0U;
    view->catalog_first_offset = 0U;
    view->catalog_generation = 0U;
    view->catalog_snapshot_applied = false;
    view->catalog_load_pending = false;
    memset(view->catalog_source_id, 0, sizeof(view->catalog_source_id));
    memset(view->catalog_tracks, 0,
           sizeof(*view->catalog_tracks) * MUSIC_VIEW_CATALOG_CACHE_CAPACITY);
    for (size_t i = 0U; i < MUSIC_VIEW_CATALOG_CACHE_CAPACITY; ++i)
    {
        lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 0), "");
        lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 1), "");
        lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 2), "");
        lv_obj_add_flag(view->track_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(view->catalog_list, 0, LV_ANIM_OFF);
}

/** 将 PSRAM 目录缓存映射到固定的可复用歌曲行。 */
static void music_view_render_catalog(music_view_t *view)
{
    for (size_t i = 0U; i < MUSIC_VIEW_CATALOG_CACHE_CAPACITY; ++i)
    {
        if (i < view->catalog_count)
        {
            lv_label_set_text_fmt(lv_obj_get_child(view->track_buttons[i], 0),
                                  "%s", view->catalog_tracks[i].title);
            lv_label_set_text_fmt(lv_obj_get_child(view->track_buttons[i], 1),
                                  "%02u",
                                  (unsigned)(view->catalog_first_offset + i + 1U));
            lv_label_set_text_fmt(lv_obj_get_child(view->track_buttons[i], 2),
                                  "%s", view->catalog_tracks[i].artist);
            lv_obj_remove_flag(view->track_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 0), "");
            lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 1), "");
            lv_label_set_text(lv_obj_get_child(view->track_buttons[i], 2), "");
            lv_obj_add_flag(view->track_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void music_view_set_track_layout(music_view_t *view, bool compact)
{
    if (view == NULL || view->artwork == NULL)
    {
        return;
    }
    if (compact)
    {
        lv_obj_set_size(view->track_panel, 330, 82);
        lv_obj_set_pos(view->track_panel, 40, 122);
        lv_obj_set_size(view->artwork, 60, 60);
        lv_obj_set_pos(view->artwork, 12, 10);
        lv_obj_set_style_radius(view->artwork, 13,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(view->track_label, 84, 10);
        lv_obj_set_size(view->track_label, 230, 24);
        lv_obj_set_style_text_font(
            view->track_label, &lv_font_montserrat_lxgw_common_5500_22_4,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(view->artist_label, 84, 37);
        lv_obj_set_size(view->artist_label, 230, 18);
        lv_obj_set_style_text_font(
            view->artist_label, &lv_font_montserrat_lxgw_common_5500_16_4,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(view->state_dot, 84, 62);
        lv_obj_set_pos(view->state_label, 96, 57);
        lv_obj_set_size(view->state_label, 218, 18);
        return;
    }

    lv_obj_set_size(view->track_panel, 330, 132);
    lv_obj_set_pos(view->track_panel, 40, 78);
    lv_obj_set_size(view->artwork, 104, 104);
    lv_obj_set_pos(view->artwork, 14, 14);
    lv_obj_set_style_radius(view->artwork, 18,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(view->track_label, 134, 20);
    lv_obj_set_size(view->track_label, 180, 30);
    lv_obj_set_style_text_font(
        view->track_label, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(view->artist_label, 134, 53);
    lv_obj_set_size(view->artist_label, 180, 24);
    lv_obj_set_style_text_font(
        view->artist_label, &lv_font_montserrat_lxgw_common_5500_16_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(view->state_dot, 134, 101);
    lv_obj_set_pos(view->state_label, 146, 97);
    lv_obj_set_size(view->state_label, 168, 22);
    lv_obj_set_style_text_font(
        view->state_label, &lv_font_montserrat_lxgw_music_ui_12_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);
}

static const char *music_view_source_label(const char *source_id)
{
    static const char *const source_ids[4] = {
        "today", "liked", "playlists", "recent",
    };
    for (size_t i = 0U; i < 4U; ++i)
    {
        if (source_id != NULL && strcmp(source_id, source_ids[i]) == 0)
        {
            return kSourceLabels[i];
        }
    }
    return "播放队列";
}

music_view_t *music_view_create(const music_view_config_t *config)
{
    music_view_t *view = (music_view_t *)heap_caps_calloc(
        1U, sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (view == NULL)
    {
        return NULL;
    }
    view->catalog_tracks = (music_service_catalog_track_t *)heap_caps_calloc(
        MUSIC_VIEW_CATALOG_CACHE_CAPACITY, sizeof(*view->catalog_tracks),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (view->catalog_tracks == NULL)
    {
        heap_caps_free(view);
        return NULL;
    }
    if (config != NULL)
    {
        view->config = *config;
    }

    view->screen = lv_obj_create(NULL);
    if (view->screen == NULL)
    {
        heap_caps_free(view->catalog_tracks);
        heap_caps_free(view);
        return NULL;
    }
    lv_obj_set_size(view->screen, 410, 502);
    lv_obj_set_style_bg_color(view->screen, lv_color_hex(0x131b1a),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(view->screen, lv_color_hex(0x090d0d),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(view->screen, LV_GRAD_DIR_VER,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->screen, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(view->screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *diagonal = lv_line_create(view->screen);
    lv_line_set_points(diagonal, kMusicDiagonalPoints,
                       sizeof(kMusicDiagonalPoints) /
                           sizeof(kMusicDiagonalPoints[0]));
    lv_obj_set_size(diagonal, 410, 502);
    music_view_line_style(diagonal, lv_color_hex(0xb5efd0), LV_OPA_10, 1);

    lv_obj_t *back = lv_btn_create(view->screen);
    lv_obj_set_size(back, 40, 36);
    lv_obj_set_pos(back, 40, 25);
    music_view_panel_style(back, lv_color_hex(0x151d1c));
    lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(back, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(back, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *back_line = lv_line_create(back);
    lv_line_set_points(back_line, kMusicBackPoints,
                       sizeof(kMusicBackPoints) / sizeof(kMusicBackPoints[0]));
    lv_obj_set_size(back_line, 10, 12);
    lv_obj_set_pos(back_line, 15, 12);
    music_view_line_style(back_line, lv_color_hex(0x9eaea6), LV_OPA_COVER, 1);
    lv_obj_add_event_cb(back, music_view_back_event, LV_EVENT_CLICKED, view);

    lv_obj_t *kicker = lv_label_create(view->screen);
    lv_label_set_text(kicker, "MUSIC");
    lv_obj_set_pos(kicker, 145, 16);
    lv_obj_set_size(kicker, 120, 8);
    lv_obj_set_style_text_align(kicker, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(kicker, 2,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
    music_view_text_style(kicker, lv_color_hex(0x66736d),
                          &lv_font_montserrat_lxgw_music_ui_8_4);

    lv_obj_t *title = lv_label_create(view->screen);
    lv_label_set_text(title, "音乐");
    lv_obj_set_pos(title, 140, 31);
    lv_obj_set_size(title, 130, 16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    music_view_text_style(title, lv_color_hex(0xeff5f0),
                          &lv_font_montserrat_lxgw_common_5500_16_4);
    lv_obj_set_style_text_letter_space(title, 2,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);

    view->account_button = music_view_button(view->screen, "登录", 318, 25,
                                              52, 36);
    lv_obj_set_style_text_font(lv_obj_get_child(view->account_button, 0),
                               &lv_font_montserrat_lxgw_music_ui_12_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(view->account_button, 18,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->account_button, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(view->account_button, music_view_account_event,
                        LV_EVENT_CLICKED, view);

    view->account_status = lv_label_create(view->screen);
    lv_label_set_text(view->account_status, "");
    lv_obj_set_pos(view->account_status, 40, 82);
    lv_obj_set_size(view->account_status, 330, 32);
    lv_obj_set_style_text_align(view->account_status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(view->account_status, LV_LABEL_LONG_DOT);
    music_view_text_style(view->account_status, lv_color_hex(0xa7afbd),
                          &lv_font_montserrat_lxgw_common_5500_16_4);

    view->qr_buffer = heap_caps_calloc(
        (size_t)kQrCanvasSize * (size_t)kQrCanvasSize, 2U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    view->qr_canvas = lv_canvas_create(view->screen);
    if (view->qr_canvas != NULL && view->qr_buffer != NULL)
    {
        lv_canvas_set_buffer(view->qr_canvas, view->qr_buffer, kQrCanvasSize,
                             kQrCanvasSize, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(view->qr_canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_set_pos(view->qr_canvas, 95, 132);
    }
    else
    {
        if (view->qr_canvas != NULL)
        {
            lv_obj_del(view->qr_canvas);
            view->qr_canvas = NULL;
        }
        heap_caps_free(view->qr_buffer);
        view->qr_buffer = NULL;
    }
    lv_obj_add_flag(view->account_status, LV_OBJ_FLAG_HIDDEN);
    if (view->qr_canvas != NULL)
    {
        lv_obj_add_flag(view->qr_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    view->track_panel = lv_obj_create(view->screen);
    lv_obj_set_size(view->track_panel, 330, 132);
    lv_obj_set_pos(view->track_panel, 40, 78);
    music_view_panel_style(view->track_panel, lv_color_hex(0x1c2725));
    lv_obj_set_style_bg_grad_color(view->track_panel, lv_color_hex(0x101817),
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(view->track_panel, LV_GRAD_DIR_VER,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->track_panel, (lv_opa_t)242,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(view->track_panel, 24,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(view->track_panel, 18,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(view->track_panel, LV_OPA_40,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(view->track_panel, lv_color_hex(0x000000),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(view->track_panel, 5,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(view->track_panel, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(view->track_panel, LV_OBJ_FLAG_SCROLLABLE);

    view->artwork = lv_image_create(view->track_panel);
    lv_image_set_src(view->artwork, &_music_artwork_RGB565A8_104x104);
    lv_obj_set_size(view->artwork, 104, 104);
    lv_obj_set_pos(view->artwork, 14, 14);
    lv_obj_set_style_radius(view->artwork, 18,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(view->artwork, true,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->artwork, 1,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(view->artwork, lv_color_hex(0xeff5f0),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(view->artwork, (lv_opa_t)140,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(view->artwork, LV_OBJ_FLAG_SCROLLABLE);

    view->track_label = lv_label_create(view->track_panel);
    lv_label_set_text(view->track_label, "未选择歌曲");
    lv_obj_set_pos(view->track_label, 134, 20);
    lv_obj_set_size(view->track_label, 180, 30);
    lv_label_set_long_mode(view->track_label, LV_LABEL_LONG_DOT);
    music_view_text_style(view->track_label, lv_color_hex(0xeff5f0),
                          &lv_font_montserrat_lxgw_common_5500_22_4);

    view->artist_label = lv_label_create(view->track_panel);
    lv_label_set_text(view->artist_label, "");
    lv_obj_set_pos(view->artist_label, 134, 53);
    lv_obj_set_size(view->artist_label, 180, 24);
    lv_label_set_long_mode(view->artist_label, LV_LABEL_LONG_DOT);
    music_view_text_style(view->artist_label, lv_color_hex(0x9eaea6),
                          &lv_font_montserrat_lxgw_common_5500_16_4);

    view->state_dot = lv_obj_create(view->track_panel);
    lv_obj_set_size(view->state_dot, 6, 6);
    lv_obj_set_pos(view->state_dot, 134, 101);
    lv_obj_set_style_radius(view->state_dot, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(view->state_dot, lv_color_hex(0xb5efd0),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->state_dot, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(view->state_dot, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(view->state_dot,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    view->state_label = lv_label_create(view->track_panel);
    lv_label_set_text(view->state_label, "未播放");
    lv_obj_set_pos(view->state_label, 146, 97);
    lv_obj_set_size(view->state_label, 168, 22);
    lv_label_set_long_mode(view->state_label, LV_LABEL_LONG_DOT);
    music_view_text_style(view->state_label, lv_color_hex(0xb5efd0),
                          &lv_font_montserrat_lxgw_music_ui_12_4);

    view->section_kicker = lv_label_create(view->screen);
    lv_label_set_text(view->section_kicker, "EXPLORE");
    lv_obj_set_pos(view->section_kicker, 40, 306);
    lv_obj_set_size(view->section_kicker, 140, 8);
    lv_obj_set_style_text_letter_space(view->section_kicker, 2,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
    music_view_text_style(view->section_kicker, lv_color_hex(0x66736d),
                          &lv_font_montserrat_lxgw_music_ui_8_4);

    view->section_label = lv_label_create(view->screen);
    lv_label_set_text(view->section_label, "音乐来源");
    lv_obj_set_pos(view->section_label, 40, 319);
    lv_obj_set_size(view->section_label, 170, 20);
    music_view_text_style(view->section_label, lv_color_hex(0xeff5f0),
                          &lv_font_montserrat_lxgw_music_ui_20_4);

    for (uint8_t i = 0; i < 4; ++i)
    {
        view->source_context[i].view = view;
        view->source_context[i].index = i;
        view->source_buttons[i] = music_view_source_button(
            view->screen, kSourceLabels[i],
            (lv_coord_t)(40 + (i % 2U) * 170),
            (lv_coord_t)(354 + (i / 2U) * 64));
        lv_obj_add_event_cb(view->source_buttons[i], music_view_source_event,
                            LV_EVENT_CLICKED, &view->source_context[i]);
    }

    view->catalog_list = lv_obj_create(view->screen);
    lv_obj_set_size(view->catalog_list, 330, 202);
    lv_obj_set_pos(view->catalog_list, 40, 326);
    music_view_panel_style(view->catalog_list, lv_color_hex(0x0b0d12));
    lv_obj_set_style_bg_opa(view->catalog_list, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(view->catalog_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view->catalog_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(view->catalog_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->catalog_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(view->catalog_list, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(view->catalog_list, MUSIC_VIEW_CATALOG_ROW_GAP,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(view->catalog_list, music_view_catalog_scroll_event,
                        LV_EVENT_SCROLL_END, view);

    for (uint8_t i = 0; i < MUSIC_VIEW_CATALOG_CACHE_CAPACITY; ++i)
    {
        view->track_context[i].view = view;
        view->track_context[i].index = i;
        view->track_buttons[i] = music_view_button(
            view->catalog_list, "", 0, 0, 318,
            MUSIC_VIEW_CATALOG_ROW_HEIGHT);
        lv_obj_t *name_label = lv_obj_get_child(view->track_buttons[i], 0);
        lv_obj_set_size(name_label, 250, 18);
        lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 48, 5);
        lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(
            name_label, &lv_font_montserrat_lxgw_common_5500_16_4,
            LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *index_label = lv_label_create(view->track_buttons[i]);
        lv_obj_set_size(index_label, 22, 18);
        lv_obj_align(index_label, LV_ALIGN_TOP_LEFT, 14, 13);
        lv_obj_set_style_text_align(index_label, LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        music_view_text_style(index_label, lv_color_hex(0x66736d),
                              &lv_font_montserrat_lxgw_music_ui_12_4);

        lv_obj_t *artist_label = lv_label_create(view->track_buttons[i]);
        lv_obj_set_size(artist_label, 250, 16);
        lv_obj_align(artist_label, LV_ALIGN_TOP_LEFT, 48, 24);
        lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_LEFT,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        music_view_text_style(artist_label, lv_color_hex(0x9eaea6),
                              &lv_font_montserrat_lxgw_common_5500_16_4);
        music_view_chevron(view->track_buttons[i], 296, 16);
        lv_obj_set_style_radius(view->track_buttons[i], 14,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(view->track_buttons[i], (lv_opa_t)210,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(view->track_buttons[i], music_view_track_event,
                            LV_EVENT_CLICKED, &view->track_context[i]);
        lv_obj_add_flag(view->track_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(view->catalog_list, LV_OBJ_FLAG_HIDDEN);

    view->previous_button = music_view_icon_button(view->screen, 40, 229, 54,
                                                   view);
    lv_obj_add_event_cb(view->previous_button, music_view_previous_event,
                        LV_EVENT_CLICKED, view);
    view->toggle_button = music_view_icon_button(view->screen, 173, 224, 64,
                                                 view);
    lv_obj_set_style_bg_color(view->toggle_button, lv_color_hex(0xb5efd0),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->toggle_button, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    view->toggle_label = lv_label_create(view->toggle_button);
    lv_label_set_text(view->toggle_label, "播放");
    lv_obj_add_flag(view->toggle_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(view->toggle_button, music_view_toggle_event,
                        LV_EVENT_CLICKED, view);
    view->next_button = music_view_icon_button(view->screen, 316, 229, 54,
                                               view);
    lv_obj_add_event_cb(view->next_button, music_view_next_event,
                        LV_EVENT_CLICKED, view);
    view->mode_button = music_view_button(view->screen, "列表循环", 246, 306,
                                          124, 32);
    lv_obj_set_style_radius(view->mode_button, 16,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(view->mode_button, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    view->mode_label = lv_obj_get_child(view->mode_button, 0);
    lv_obj_align(view->mode_label, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_set_size(view->mode_label, 94, 20);
    lv_obj_set_style_text_font(view->mode_label,
                               &lv_font_montserrat_lxgw_music_ui_12_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(view->mode_label, LV_TEXT_ALIGN_LEFT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(view->mode_label, LV_LABEL_LONG_DOT);

    lv_obj_t *mode_line = lv_line_create(view->mode_button);
    lv_line_set_points(mode_line, kMusicModePoints,
                       sizeof(kMusicModePoints) /
                           sizeof(kMusicModePoints[0]));
    lv_obj_set_size(mode_line, 14, 12);
    lv_obj_set_pos(mode_line, 27, 10);
    music_view_line_style(mode_line, lv_color_hex(0x9eaea6), LV_OPA_90, 1);
    lv_obj_add_event_cb(view->mode_button, music_view_mode_event,
                        LV_EVENT_CLICKED, view);

    return view;
}

void music_view_destroy(music_view_t *view)
{
    if (view == NULL)
    {
        return;
    }
    if (view->screen != NULL)
    {
        lv_obj_del(view->screen);
    }
    heap_caps_free(view->qr_buffer);
    heap_caps_free(view->catalog_tracks);
    heap_caps_free(view);
}

lv_obj_t *music_view_get_screen(const music_view_t *view)
{
    return view != NULL ? view->screen : NULL;
}

void music_view_apply_snapshot(music_view_t *view,
                               const music_service_snapshot_t *snapshot)
{
    if (view == NULL || snapshot == NULL)
    {
        return;
    }
    view->playing = snapshot->state == MUSIC_SERVICE_STATE_PLAYING ||
                    snapshot->state == MUSIC_SERVICE_STATE_BUFFERING;
    lv_label_set_text(view->state_label, music_view_state_text(snapshot));
    lv_label_set_text(view->track_label,
                      snapshot->title[0] != '\0' ? snapshot->title : "未选择歌曲");
    lv_label_set_text(view->artist_label,
                      snapshot->artist[0] != '\0' ? snapshot->artist
                                                  : "选择一首音乐开始");
    lv_label_set_text(view->toggle_label,
                      view->playing ? "暂停" : "播放");
    lv_obj_set_style_bg_color(view->state_dot,
                              view->playing ? lv_color_hex(0xb5efd0)
                                            : lv_color_hex(0x66736d),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(view->mode_label, music_view_mode_text(snapshot->mode));
    lv_obj_invalidate(view->toggle_button);
}

void music_view_show_catalog_loading(music_view_t *view, const char *source_id)
{
    if (view == NULL)
    {
        return;
    }
    music_view_reset_catalog(view);
    snprintf(view->catalog_source_id, sizeof(view->catalog_source_id), "%s",
             source_id != NULL ? source_id : "");
    view->catalog_visible = true;
    view->account_visible = false;
    lv_obj_remove_flag(view->track_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->previous_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->toggle_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->next_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(view->previous_button, 40, 217);
    lv_obj_set_size(view->previous_button, 46, 46);
    lv_obj_set_pos(view->toggle_button, 178, 213);
    lv_obj_set_size(view->toggle_button, 54, 54);
    lv_obj_set_pos(view->next_button, 324, 217);
    lv_obj_set_size(view->next_button, 46, 46);
    lv_obj_remove_flag(view->account_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->account_status, LV_OBJ_FLAG_HIDDEN);
    if (view->qr_canvas != NULL)
    {
        lv_obj_add_flag(view->qr_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    music_view_set_track_layout(view, true);
    for (size_t i = 0; i < 4U; ++i)
    {
        lv_obj_add_flag(view->source_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(view->section_kicker, "COLLECTION");
    lv_obj_set_pos(view->section_kicker, 40, 78);
    lv_obj_remove_flag(view->section_kicker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->catalog_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(view->catalog_list, 40, 280);
    lv_obj_set_size(view->catalog_list, 330, 202);
    lv_obj_remove_flag(view->track_buttons[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(view->track_buttons[0], 0),
                      "正在加载歌曲");
    lv_label_set_text(view->section_label, music_view_source_label(source_id));
    lv_obj_set_pos(view->section_label, 40, 91);
    lv_obj_set_size(view->section_label, 270, 26);
    lv_obj_set_style_text_font(
        view->section_label, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(view->section_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->mode_button, LV_OBJ_FLAG_HIDDEN);
}

void music_view_show_sources(music_view_t *view)
{
    if (view == NULL)
    {
        return;
    }
    view->catalog_visible = false;
    view->account_visible = false;
    lv_obj_remove_flag(view->track_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->previous_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->toggle_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view->next_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(view->previous_button, 40, 229);
    lv_obj_set_size(view->previous_button, 54, 54);
    lv_obj_set_pos(view->toggle_button, 173, 224);
    lv_obj_set_size(view->toggle_button, 64, 64);
    lv_obj_set_pos(view->next_button, 316, 229);
    lv_obj_set_size(view->next_button, 54, 54);
    lv_obj_remove_flag(view->account_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->account_status, LV_OBJ_FLAG_HIDDEN);
    if (view->qr_canvas != NULL)
    {
        lv_obj_add_flag(view->qr_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    music_view_set_track_layout(view, false);
    for (size_t i = 0; i < 4U; ++i)
    {
        lv_obj_remove_flag(view->source_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(view->section_kicker, "EXPLORE");
    lv_obj_set_pos(view->section_kicker, 40, 306);
    lv_obj_remove_flag(view->section_kicker, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(view->section_label, "音乐来源");
    lv_obj_set_pos(view->section_label, 40, 319);
    lv_obj_set_size(view->section_label, 170, 20);
    lv_obj_set_style_text_font(view->section_label,
                               &lv_font_montserrat_lxgw_music_ui_20_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(view->section_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->catalog_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(view->mode_button, 246, 306);
    lv_obj_set_size(view->mode_button, 124, 32);
    lv_obj_remove_flag(view->mode_button, LV_OBJ_FLAG_HIDDEN);
}

void music_view_show_account_loading(music_view_t *view)
{
    if (view == NULL)
    {
        return;
    }
    view->account_visible = true;
    view->catalog_visible = false;
    lv_obj_add_flag(view->track_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->state_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->previous_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->next_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->section_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->section_kicker, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(view->account_status, "正在获取二维码");
    lv_obj_remove_flag(view->account_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->account_button, LV_OBJ_FLAG_HIDDEN);
    if (view->qr_canvas != NULL)
    {
        lv_obj_remove_flag(view->qr_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_canvas_fill_bg(view->qr_canvas, lv_color_white(), LV_OPA_COVER);
    }
    for (size_t i = 0; i < 4U; ++i)
    {
        lv_obj_add_flag(view->source_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(view->catalog_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->toggle_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->mode_button, LV_OBJ_FLAG_HIDDEN);
}

void music_view_apply_account(
    music_view_t *view, const music_service_account_snapshot_t *account,
    const uint8_t *qr_data, uint16_t qr_size, size_t qr_bytes)
{
    if (view == NULL || account == NULL || !view->account_visible)
    {
        return;
    }
    const char *text = "登录状态未知";
    switch (account->state)
    {
    case MUSIC_SERVICE_ACCOUNT_QR_PENDING:
        text = "请使用网易云音乐扫描二维码";
        break;
    case MUSIC_SERVICE_ACCOUNT_QR_CONFIRMING:
        text = "已扫码，请在手机上确认";
        break;
    case MUSIC_SERVICE_ACCOUNT_LOGGED_IN:
        text = "登录成功";
        break;
    case MUSIC_SERVICE_ACCOUNT_LOGGED_OUT:
        text = "当前未登录";
        break;
    case MUSIC_SERVICE_ACCOUNT_EXPIRED:
        text = "二维码已过期，请重新获取";
        break;
    case MUSIC_SERVICE_ACCOUNT_ERROR:
        text = "二维码获取失败，请重试";
        break;
    default:
        break;
    }
    lv_label_set_text(view->account_status, text);
    if (view->qr_canvas == NULL || qr_data == NULL || qr_size == 0U ||
        qr_bytes == 0U || qr_size > kQrCanvasSize)
    {
        return;
    }
    lv_canvas_fill_bg(view->qr_canvas, lv_color_white(), LV_OPA_COVER);
    const lv_coord_t scale = kQrCanvasSize / qr_size;
    if (scale == 0)
    {
        return;
    }
    const lv_coord_t drawn = (lv_coord_t)qr_size * scale;
    const lv_coord_t margin = (kQrCanvasSize - drawn) / 2;
    for (uint16_t y = 0; y < qr_size; ++y)
    {
        for (uint16_t x = 0; x < qr_size; ++x)
        {
            const size_t bit = (size_t)y * qr_size + x;
            if (bit / 8U >= qr_bytes ||
                (qr_data[bit / 8U] & (uint8_t)(1U << (7U - bit % 8U))) == 0U)
            {
                continue;
            }
            for (lv_coord_t dy = 0; dy < scale; ++dy)
            {
                for (lv_coord_t dx = 0; dx < scale; ++dx)
                {
                    lv_canvas_set_px(view->qr_canvas, margin + x * scale + dx,
                                     margin + y * scale + dy, lv_color_black(),
                                     LV_OPA_COVER);
                }
            }
        }
    }
}

void music_view_apply_catalog(music_view_t *view,
                              const music_service_catalog_snapshot_t *catalog)
{
    if (view == NULL || catalog == NULL || !view->catalog_visible)
    {
        return;
    }
    if (catalog->loading ||
        (view->catalog_snapshot_applied &&
         catalog->generation == view->catalog_generation))
    {
        return;
    }
    view->catalog_load_pending = false;
    if (!catalog->valid)
    {
        return;
    }
    const bool first_page = catalog->offset == 0U;
    const bool same_source =
        strcmp(view->catalog_source_id, catalog->source_id) == 0;
    if (first_page || !same_source)
    {
        music_view_reset_catalog(view);
        snprintf(view->catalog_source_id, sizeof(view->catalog_source_id), "%s",
                 catalog->source_id);
    }
    else if (catalog->offset != view->catalog_next_offset)
    {
        return;
    }

    const size_t incoming =
        catalog->track_count < MUSIC_SERVICE_CATALOG_PAGE_SIZE
            ? catalog->track_count
            : MUSIC_SERVICE_CATALOG_PAGE_SIZE;
    size_t removed = 0U;
    lv_coord_t old_scroll_y = lv_obj_get_scroll_y(view->catalog_list);
    if (view->catalog_count + incoming > MUSIC_VIEW_CATALOG_CACHE_CAPACITY)
    {
        removed = view->catalog_count < MUSIC_SERVICE_CATALOG_PAGE_SIZE
                      ? view->catalog_count
                      : MUSIC_SERVICE_CATALOG_PAGE_SIZE;
        memmove(view->catalog_tracks, &view->catalog_tracks[removed],
                (view->catalog_count - removed) *
                    sizeof(*view->catalog_tracks));
        view->catalog_count -= removed;
        view->catalog_first_offset += (uint32_t)removed;
    }
    if (incoming > 0U)
    {
        memcpy(&view->catalog_tracks[view->catalog_count], catalog->tracks,
               incoming * sizeof(*view->catalog_tracks));
        view->catalog_count += incoming;
    }
    view->catalog_total = catalog->total;
    view->catalog_next_offset = catalog->offset + (uint32_t)catalog->track_count;
    view->catalog_generation = catalog->generation;
    view->catalog_snapshot_applied = true;
    view->catalog_load_pending = false;
    music_view_render_catalog(view);

    if (removed > 0U)
    {
        const lv_coord_t removed_height =
            (lv_coord_t)removed *
            (MUSIC_VIEW_CATALOG_ROW_HEIGHT + MUSIC_VIEW_CATALOG_ROW_GAP);
        lv_obj_update_layout(view->catalog_list);
        lv_obj_scroll_to_y(view->catalog_list,
                           old_scroll_y > removed_height
                               ? old_scroll_y - removed_height
                               : 0,
                           LV_ANIM_OFF);
    }
}
