#include "mini_games_controller.h"

#include <stdio.h>

#include "app/board_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "features/mini_games/mini_game_2048.h"
#include "features/mini_games/mini_games_progress.h"

#define DISABLE_NEW_GAMES 0

#if !DISABLE_NEW_GAMES
#include "features/mini_games/mini_game_flappy.h"
#include "features/mini_games/mini_game_dino.h"
#include "mini_game_sprites.h"
#endif

#include "lvgl.h"
#include "ui/ui_refresh_policy.h"
#include "ui_chinese_fonts.h"
#include "watch_notification_center.h"

static const char *TAG = "mini_games";

static const lv_coord_t kScreenWidth = 410;
static const lv_coord_t kScreenHeight = 502;
static const lv_coord_t kBoardX = 40;
static const lv_coord_t kBoardY = 96;
static const lv_coord_t kBoardSize = 330;
static const lv_coord_t kTileSize = 72;
static const lv_coord_t kTileGap = 8;
static const lv_coord_t kGestureThresholdPx = 28;
/* 底部悬浮控制栏完全落在 CO5300 的安全显示区内。 */
static const lv_coord_t kControlsY = 434;
static const lv_coord_t kControlsHeight = 44;
static const lv_coord_t kControlLeftX = 40;
static const lv_coord_t kControlLeftWidth = 96;
static const lv_coord_t kControlMiddleX = 144;
static const lv_coord_t kControlMiddleWidth = 122;
static const lv_coord_t kControlRightX = 274;
static const lv_coord_t kControlRightWidth = 96;
/* Flappy 管道从全屏舞台顶部开始，底部仍在悬浮控制栏上方结束。 */
static const lv_coord_t kFlappyPlayfieldY = 0;

typedef enum {
    MINI_GAME_TYPE_NONE = 0, /* 主菜单页面 */
    MINI_GAME_TYPE_2048,     /* 2048 益智 */
    MINI_GAME_TYPE_FLAPPY,   /* 飞翔的小鸟 */
    MINI_GAME_TYPE_DINO      /* 小恐龙跑酷 */
} mini_game_type_t;

static lv_ui *s_ui;
static lv_obj_t *s_screen;
static lv_obj_t *s_menu_cont;
static lv_obj_t *s_game_cont;
static lv_timer_t *s_game_timer;
static mini_game_type_t s_current_game = MINI_GAME_TYPE_NONE;

/* ── 2048 全局静态变量 ── */
static lv_obj_t *s_score_label;
static lv_obj_t *s_best_score_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_state_icon;
static lv_obj_t *s_pause_btn;
static lv_obj_t *s_board;
static lv_obj_t *s_2048_win_overlay;
static lv_obj_t *s_game_controls[3];
static lv_obj_t *s_game_score_panels[2];
static lv_obj_t *s_tile_labels[MINI_GAME_2048_SIZE][MINI_GAME_2048_SIZE];
static mini_game_2048_t s_game;
static bool s_game_initialized;
static bool s_paused;
static bool s_auto_paused;
static bool s_pressing;
static lv_point_t s_press_start;

#if !DISABLE_NEW_GAMES
/* ── Flappy Bird 全局静态变量 ── */
static lv_obj_t *s_flappy_bird_obj;
static lv_obj_t *s_flappy_stage;
static lv_obj_t *s_flappy_pipes_top[FLAPPY_MAX_PIPES];
static lv_obj_t *s_flappy_pipes_bottom[FLAPPY_MAX_PIPES];
static lv_obj_t *s_flappy_score_label;
static lv_obj_t *s_flappy_state_label;
static lv_obj_t *s_flappy_pause_btn;
static mini_game_flappy_t s_game_flappy;
static uint32_t s_flappy_last_score;
static bool s_flappy_game_over_recorded;
static lv_obj_t *s_flappy_clouds[2];
static float s_flappy_clouds_x[2];
static float s_flappy_clouds_y[2];

/* ── 小恐龙跑酷 全局静态变量 ── */
static lv_obj_t *s_dino_obj;
static lv_obj_t *s_dino_obstacles[DINO_MAX_OBSTACLES];
static lv_obj_t *s_dino_score_label;
static lv_obj_t *s_dino_state_label;
static lv_obj_t *s_dino_pause_btn;
static mini_game_dino_t s_game_dino;
static uint32_t s_dino_last_milestone;
static bool s_dino_game_over_recorded;
static lv_obj_t *s_dino_sand_particles[4];
static float s_dino_sand_x[4];
static lv_obj_t *s_dino_cloud;
static float s_dino_cloud_x;
#endif

/* ── 函数前置声明 ── */
static void mini_games_controller_refresh_board(void);
static void mini_games_controller_leave(void);
static void mini_games_controller_setup_menu(void);
static void mini_games_controller_switch_to_game(mini_game_type_t game_type);
static void mini_games_controller_timer_cb(lv_timer_t *timer);

static void mini_games_controller_setup_2048(void);
static void mini_games_controller_refresh_current_game(void);
static void mini_games_controller_submit_current_score(void);

#if !DISABLE_NEW_GAMES
static void mini_games_controller_setup_flappy(void);
static void mini_games_controller_setup_dino(void);

static void mini_games_controller_refresh_flappy(void);
static void mini_games_controller_refresh_dino(void);

static void mini_games_controller_flappy_click_cb(lv_event_t *e);
static void mini_games_controller_flappy_restart_cb(lv_event_t *e);
static void mini_games_controller_flappy_pause_cb(lv_event_t *e);

static void mini_games_controller_dino_click_cb(lv_event_t *e);
static void mini_games_controller_dino_restart_cb(lv_event_t *e);
static void mini_games_controller_dino_pause_cb(lv_event_t *e);
#endif

static void mini_games_controller_game_back_cb(lv_event_t *e);
static void mini_games_controller_2048_continue_cb(lv_event_t *e);

/**
 * @brief 生成随机种子戳。由于未启动真机硬件随机，主要依赖时间微秒。
 */
static uint32_t mini_games_controller_seed(void)
{
    const int64_t now_us = esp_timer_get_time();
    return (uint32_t)now_us ^ (uint32_t)(now_us >> 32);
}

static bool mini_games_controller_is_screen_alive(void)
{
    return s_screen != NULL && lv_obj_is_valid(s_screen);
}

static bool mini_games_controller_is_foreground(void)
{
    return mini_games_controller_is_screen_alive() &&
           lv_screen_active() == s_screen;
}

static void mini_games_controller_set_scale(void *object, int32_t scale)
{
    lv_obj_t *obj = (lv_obj_t *)object;
    if (obj != NULL && lv_obj_is_valid(obj)) {
        lv_obj_set_style_transform_scale(obj, scale,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

/** @brief 用短促缩放表达合并、得分或里程碑，不创建常驻动画。 */
static void mini_games_controller_pulse(lv_obj_t *obj)
{
    if (obj == NULL || !lv_obj_is_valid(obj)) {
        return;
    }

    lv_anim_delete(obj, mini_games_controller_set_scale);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, mini_games_controller_set_scale);
    lv_anim_set_values(&anim, 256, 276);
    lv_anim_set_duration(&anim, 90);
    lv_anim_set_playback_duration(&anim, 110);
    lv_anim_start(&anim);
}

/**
 * @brief 依据游戏运行状态调整底部控制栏可见度。
 *
 * 游戏运行时，完整控件半透明以让出舞台；暂停和结算时恢复实体外观，
 * 保持触控入口清晰可见。
 */
static void mini_games_controller_set_controls_running(bool running)
{
    const lv_opa_t opacity = running ? LV_OPA_50 : LV_OPA_COVER;

    for (uint8_t i = 0; i < 3; ++i) {
        lv_obj_t *button = s_game_controls[i];
        if (button == NULL || !lv_obj_is_valid(button)) {
            continue;
        }

        lv_obj_set_style_opa(button, opacity, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    for (uint8_t i = 0; i < 2; ++i) {
        lv_obj_t *panel = s_game_score_panels[i];
        if (panel == NULL || !lv_obj_is_valid(panel)) {
            continue;
        }

        lv_obj_set_style_opa(panel, opacity, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void mini_games_controller_submit_current_score(void)
{
    switch (s_current_game) {
        case MINI_GAME_TYPE_2048:
            mini_games_progress_submit_high_score(
                MINI_GAMES_PROGRESS_2048,
                mini_game_2048_get_score(&s_game));
            break;
#if !DISABLE_NEW_GAMES
        case MINI_GAME_TYPE_FLAPPY:
            mini_games_progress_submit_high_score(
                MINI_GAMES_PROGRESS_FLAPPY,
                mini_game_flappy_get_score(&s_game_flappy));
            break;
        case MINI_GAME_TYPE_DINO:
            mini_games_progress_submit_high_score(
                MINI_GAMES_PROGRESS_DINO,
                mini_game_dino_get_score(&s_game_dino));
            break;
#endif
        case MINI_GAME_TYPE_NONE:
        default:
            break;
    }
}

static void mini_games_controller_refresh_current_game(void)
{
    if (s_current_game == MINI_GAME_TYPE_2048) {
        mini_games_controller_refresh_board();
#if !DISABLE_NEW_GAMES
    } else if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
        mini_games_controller_refresh_flappy();
    } else if (s_current_game == MINI_GAME_TYPE_DINO) {
        mini_games_controller_refresh_dino();
#endif
    }
}

static void mini_games_controller_clear_game_refs(void)
{
    s_score_label = NULL;
    s_best_score_label = NULL;
    s_state_label = NULL;
    s_state_icon = NULL;
    s_pause_btn = NULL;
    s_board = NULL;
    s_2048_win_overlay = NULL;
    for (uint8_t i = 0; i < 3; ++i) {
        s_game_controls[i] = NULL;
    }
    for (uint8_t i = 0; i < 2; ++i) {
        s_game_score_panels[i] = NULL;
    }

    for (uint8_t row = 0; row < MINI_GAME_2048_SIZE; ++row) {
        for (uint8_t col = 0; col < MINI_GAME_2048_SIZE; ++col) {
            s_tile_labels[row][col] = NULL;
        }
    }
    s_pressing = false;
    s_game_initialized = false;
    s_auto_paused = false;

#if !DISABLE_NEW_GAMES
    s_flappy_bird_obj = NULL;
    s_flappy_stage = NULL;
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        s_flappy_pipes_top[i] = NULL;
        s_flappy_pipes_bottom[i] = NULL;
    }
    s_flappy_score_label = NULL;
    s_flappy_state_label = NULL;
    s_flappy_pause_btn = NULL;
    s_flappy_last_score = 0U;
    s_flappy_game_over_recorded = false;
    s_flappy_clouds[0] = NULL;
    s_flappy_clouds[1] = NULL;

    s_dino_obj = NULL;
    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        s_dino_obstacles[i] = NULL;
    }
    s_dino_score_label = NULL;
    s_dino_state_label = NULL;
    s_dino_pause_btn = NULL;
    s_dino_last_milestone = 0U;
    s_dino_game_over_recorded = false;
    for (uint8_t i = 0; i < 4; ++i) {
        s_dino_sand_particles[i] = NULL;
    }
    s_dino_cloud = NULL;
#endif
}

static void mini_games_controller_reset_refs(void)
{
    s_screen = NULL;
    s_menu_cont = NULL;
    s_game_cont = NULL;
    s_game_timer = NULL;
    s_current_game = MINI_GAME_TYPE_NONE;
    mini_games_controller_clear_game_refs();
}

static lv_color_t mini_games_controller_tile_color(uint16_t value)
{
    switch (value) {
        case 0:
            return lv_color_hex(0xeaeff2);
        case 2:
            return lv_color_hex(0xf3e9dc);
        case 4:
            return lv_color_hex(0xebdcb9);
        case 8:
            return lv_color_hex(0xf2b179);
        case 16:
            return lv_color_hex(0xf59563);
        case 32:
            return lv_color_hex(0xf67c5f);
        case 64:
            return lv_color_hex(0xf65e3b);
        case 128:
            return lv_color_hex(0xedcf72);
        case 256:
            return lv_color_hex(0xedcc61);
        case 512:
            return lv_color_hex(0xedc850);
        case 1024:
            return lv_color_hex(0xedc53f);
        default:
            return lv_color_hex(0x3c8d7d);
    }
}

static lv_color_t mini_games_controller_tile_text_color(uint16_t value)
{
    return value <= 4U ? lv_color_hex(0x293241) : lv_color_hex(0xffffff);
}

static void mini_games_controller_set_paused(bool paused)
{
    s_paused = paused;
    s_auto_paused = false;
    mini_games_controller_refresh_current_game();
}

static void mini_games_controller_new_game(void)
{
    mini_game_2048_reset(&s_game, mini_games_controller_seed());
    s_game_initialized = true;
    s_paused = false;
    s_auto_paused = false;
    mini_games_controller_refresh_board();
}

static void mini_games_controller_screen_delete_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_game_timer != NULL) {
        lv_timer_delete(s_game_timer);
    }
    mini_games_controller_reset_refs();
}

static void mini_games_controller_back_event_cb(lv_event_t *e)
{
    (void)e;
    mini_games_controller_leave();
}

static void mini_games_controller_new_event_cb(lv_event_t *e)
{
    (void)e;
    mini_games_controller_new_game();
}

static void mini_games_controller_2048_continue_cb(lv_event_t *e)
{
    (void)e;
    mini_game_2048_continue(&s_game);
    mini_games_controller_refresh_board();
}

static void mini_games_controller_pause_event_cb(lv_event_t *e)
{
    (void)e;
    mini_games_controller_set_paused(!s_paused);
}

static bool mini_games_controller_get_swipe_direction(
    const lv_point_t *start, const lv_point_t *end,
    mini_game_2048_direction_t *direction)
{
    if (start == NULL || end == NULL || direction == NULL) {
        return false;
    }

    const lv_coord_t dx = (lv_coord_t)(end->x - start->x);
    const lv_coord_t dy = (lv_coord_t)(end->y - start->y);
    const lv_coord_t abs_dx = dx >= 0 ? dx : (lv_coord_t)-dx;
    const lv_coord_t abs_dy = dy >= 0 ? dy : (lv_coord_t)-dy;

    if (abs_dx < kGestureThresholdPx && abs_dy < kGestureThresholdPx) {
        return false;
    }

    if (abs_dx >= abs_dy) {
        *direction = dx > 0 ? MINI_GAME_2048_DIRECTION_RIGHT
                            : MINI_GAME_2048_DIRECTION_LEFT;
    } else {
        *direction = dy > 0 ? MINI_GAME_2048_DIRECTION_DOWN
                            : MINI_GAME_2048_DIRECTION_UP;
    }
    return true;
}

static void mini_games_controller_board_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (!mini_games_controller_is_foreground()) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(lv_indev_get_act(), &s_press_start);
        s_pressing = true;
        return;
    }

    if (!s_pressing) {
        return;
    }

    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }

    s_pressing = false;
    if (s_paused || mini_game_2048_is_game_over(&s_game) ||
        mini_game_2048_is_waiting_for_continue(&s_game)) {
        return;
    }

    lv_point_t end = {0};
    mini_game_2048_direction_t direction = MINI_GAME_2048_DIRECTION_LEFT;
    lv_indev_get_point(lv_indev_get_act(), &end);
    if (!mini_games_controller_get_swipe_direction(&s_press_start, &end,
                                                   &direction)) {
        return;
    }

    const mini_game_2048_move_result_t result =
        mini_game_2048_move(&s_game, direction);
    if (result.just_won || result.game_over) {
        mini_games_controller_submit_current_score();
    }
    if (result.moved || result.game_over || result.just_won) {
        mini_games_controller_refresh_board();
    }
}

/**
 * @brief 创建带 Icon 与文本水平居中的胶囊卡片按钮
 * @note 内部容器 content 会被移除 CLICKABLE 属性以使点击事件能渗透到外部 Button 触发回调。
 */
static lv_obj_t *mini_games_controller_create_icon_button(
    lv_obj_t *parent, const char *icon, lv_color_t icon_color,
    const char *text, lv_color_t text_color,
    lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
    lv_color_t bg_color, bool has_border)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, w, h);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 17, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (has_border) {
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(button, lv_color_hex(0xe5e7eb),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_border_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *content = lv_obj_create(button);
    lv_obj_set_size(content, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(content);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon_lbl = lv_label_create(content);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_color(icon_lbl, icon_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserratMedium_16,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(icon_lbl, 0, 0);

    lv_obj_t *text_lbl = lv_label_create(content);
    lv_label_set_text(text_lbl, text);
    lv_obj_set_style_text_color(text_lbl, text_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(text_lbl, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(text_lbl, icon_lbl, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    return button;
}

static void mini_games_controller_create_tiles(void)
{
    for (uint8_t row = 0; row < MINI_GAME_2048_SIZE; ++row) {
        for (uint8_t col = 0; col < MINI_GAME_2048_SIZE; ++col) {
            lv_obj_t *tile = lv_obj_create(s_board);
            lv_obj_set_size(tile, kTileSize, kTileSize);
            lv_obj_set_pos(tile, (lv_coord_t)(col * (kTileSize + kTileGap)),
                           (lv_coord_t)(row * (kTileSize + kTileGap)));
            lv_obj_set_style_radius(tile, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(tile, 0,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *label = lv_label_create(tile);
            lv_obj_set_style_text_font(label, &lv_font_montserratMedium_27,
                                       LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_center(label);
            s_tile_labels[row][col] = label;
        }
    }
}

static void mini_games_controller_create_2048_win_overlay(void)
{
    s_2048_win_overlay = lv_obj_create(s_game_cont);
    lv_obj_set_pos(s_2048_win_overlay, kBoardX, kBoardY);
    lv_obj_set_size(s_2048_win_overlay, kBoardSize, kBoardSize);
    lv_obj_set_style_bg_color(s_2048_win_overlay, lv_color_hex(0xf8fafc),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_2048_win_overlay, LV_OPA_90,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_2048_win_overlay, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_2048_win_overlay, 16,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_2048_win_overlay, 0,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_2048_win_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_2048_win_overlay);
    lv_label_set_text(title, "你赢了！");
    lv_obj_set_size(title, kBoardSize, 34);
    lv_obj_set_pos(title, 0, 76);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3482e2),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(
        title, &lv_font_montserrat_lxgw_common_5500_22_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *message = lv_label_create(s_2048_win_overlay);
    lv_label_set_text(message, "已经合成 2048");
    lv_obj_set_size(message, kBoardSize, 28);
    lv_obj_set_pos(message, 0, 122);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(message, lv_color_hex(0x475569),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(
        message, &lv_font_montserrat_lxgw_common_5500_16_4,
        LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *continue_btn = mini_games_controller_create_icon_button(
        s_2048_win_overlay, LV_SYMBOL_PLAY, lv_color_hex(0xffffff),
        "继续挑战", lv_color_hex(0xffffff), 15, 176, 145, 46,
        lv_color_hex(0x3482e2), false);
    lv_obj_add_event_cb(continue_btn, mini_games_controller_2048_continue_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *new_btn = mini_games_controller_create_icon_button(
        s_2048_win_overlay, "+", lv_color_hex(0x5ebb70), "新游戏",
        lv_color_hex(0x374151), 170, 176, 145, 46,
        lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(new_btn, mini_games_controller_new_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_2048_win_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ── 统一选择菜单界面搭建 ── */
static void mini_games_controller_menu_btn_cb(lv_event_t *e)
{
    const mini_game_type_t target = (mini_game_type_t)(uintptr_t)lv_event_get_user_data(e);
    mini_games_controller_switch_to_game(target);
}

static void mini_games_controller_setup_menu(void)
{
    if (s_menu_cont == NULL || !lv_obj_is_valid(s_menu_cont)) {
        return;
    }

    /* 菜单大标题 */
    lv_obj_t *title = lv_label_create(s_menu_cont);
    lv_label_set_text(title, "精选小游戏");
    lv_obj_set_style_text_color(title, lv_color_hex(0x1f2937), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_lxgw_common_5500_22_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 40, 40);

    /* 副标题指引 */
    lv_obj_t *sub = lv_label_create(s_menu_cont);
    lv_label_set_text(sub, "请选择一款小游戏开始游玩:");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x6b7280), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(sub, 40, 84);

    /* 选项一：2048 游戏 */
    lv_obj_t *btn_2048 = mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_IMAGE, lv_color_hex(0x3482e2), "2048 益智拼图", lv_color_hex(0x1f2937),
        40, 130, 330, 52, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(btn_2048, mini_games_controller_menu_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)MINI_GAME_TYPE_2048);

#if !DISABLE_NEW_GAMES
    /* 选项二：Flappy Bird */
    lv_obj_t *btn_flappy = mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_AUDIO, lv_color_hex(0x0ea5e9), "飞翔的小鸟 (Flappy)", lv_color_hex(0x1f2937),
        40, 202, 330, 52, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(btn_flappy, mini_games_controller_menu_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)MINI_GAME_TYPE_FLAPPY);

    /* 选项三：Dino Runner */
    lv_obj_t *btn_dino = mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_SETTINGS, lv_color_hex(0xf59e0b), "小恐龙快跑 (Runner)", lv_color_hex(0x1f2937),
        40, 274, 330, 52, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(btn_dino, mini_games_controller_menu_btn_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)MINI_GAME_TYPE_DINO);
#else
    /* 选项二：Flappy Bird (未开放) */
    mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_AUDIO, lv_color_hex(0x9ca3af), "飞翔的小鸟 (未开放)", lv_color_hex(0x9ca3af),
        25, 202, 360, 52, lv_color_hex(0xf3f4f6), true);

    /* 选项三：Dino Runner (未开放) */
    mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_SETTINGS, lv_color_hex(0x9ca3af), "小恐龙快跑 (未开放)", lv_color_hex(0x9ca3af),
        25, 274, 360, 52, lv_color_hex(0xf3f4f6), true);
#endif

    /* 返回手表主屏幕的退出键 */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_menu_cont, LV_SYMBOL_LEFT, lv_color_hex(0xef4444), "返回主菜单", lv_color_hex(0xffffff),
        40, 410, 330, 48, lv_color_hex(0x374151), false);
    lv_obj_add_event_cb(back_btn, mini_games_controller_back_event_cb, LV_EVENT_CLICKED, NULL);
}

/* ── 统一选择菜单切换分发器 ── */
static void mini_games_controller_switch_to_game(mini_game_type_t game_type)
{
    if (!mini_games_controller_is_screen_alive()) {
        return;
    }

    /* 1. 注销高频渲染 Timer */
    if (s_game_timer != NULL) {
        lv_timer_delete(s_game_timer);
        s_game_timer = NULL;
    }

    /* 2. 清理并回收游戏画布的所有内存组件 */
    lv_obj_clean(s_game_cont);
    mini_games_controller_clear_game_refs();

    s_current_game = game_type;
    s_paused = false;
    s_auto_paused = false;

    if (game_type == MINI_GAME_TYPE_NONE) {
        /* 返回主菜单状态 */
        lv_obj_add_flag(s_game_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_menu_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 进入具体小游戏状态 */
        lv_obj_add_flag(s_menu_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_game_cont, LV_OBJ_FLAG_HIDDEN);

        if (game_type == MINI_GAME_TYPE_2048) {
            mini_game_2048_init(&s_game, mini_games_controller_seed());
            s_game_initialized = true;
            mini_games_controller_setup_2048();
            mini_games_controller_refresh_board();
#if !DISABLE_NEW_GAMES
        } else if (game_type == MINI_GAME_TYPE_FLAPPY) {
            mini_game_flappy_init(&s_game_flappy, mini_games_controller_seed());
            s_flappy_last_score = 0U;
            s_flappy_game_over_recorded = false;
            mini_games_controller_setup_flappy();
            mini_games_controller_refresh_flappy();
            /* 启动 40ms 高刷运行 Timer 循环 */
            s_game_timer = lv_timer_create(mini_games_controller_timer_cb, 40, NULL);
        } else if (game_type == MINI_GAME_TYPE_DINO) {
            mini_game_dino_init(&s_game_dino, mini_games_controller_seed());
            s_dino_last_milestone = 0U;
            s_dino_game_over_recorded = false;
            mini_games_controller_setup_dino();
            mini_games_controller_refresh_dino();
            /* 启动 40ms 高刷运行 Timer 循环 */
            s_game_timer = lv_timer_create(mini_games_controller_timer_cb, 40, NULL);
#endif
        }
    }
}

/* ── 统一高频逻辑物理与渲染 Tick 回调 ── */
static void mini_games_controller_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!mini_games_controller_is_foreground() || s_paused) {
        return;
    }

#if !DISABLE_NEW_GAMES
    if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
        if (mini_game_flappy_is_started(&s_game_flappy) &&
            !s_game_flappy.game_over) {
            mini_game_flappy_step(&s_game_flappy);
            mini_games_controller_refresh_flappy();
        }
    } else if (s_current_game == MINI_GAME_TYPE_DINO) {
        if (mini_game_dino_is_started(&s_game_dino) &&
            !s_game_dino.game_over) {
            mini_game_dino_step(&s_game_dino);
            mini_games_controller_refresh_dino();
        }
    }
#endif
}

/* ── 游戏内“返回”键回调 ── */
static void mini_games_controller_game_back_cb(lv_event_t *e)
{
    (void)e;
    mini_games_controller_switch_to_game(MINI_GAME_TYPE_NONE);
}

/* ── 2048 游戏 UI 动态重构挂载 ── */
static void mini_games_controller_setup_2048(void)
{
    /* 顶部只保留悬浮分数，棋盘和操作栏占据主舞台。 */
    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 214, 24);
    lv_obj_set_size(score_card, 74, 54);
    lv_obj_set_style_bg_color(score_card, lv_color_hex(0xffffff),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(score_card, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(score_card, 1,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xe5e7eb),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);
    s_game_score_panels[0] = score_card;

    lv_obj_t *score_hdr = lv_label_create(score_card);
    lv_label_set_text(score_hdr, "得分");
    lv_obj_set_style_text_color(score_hdr, lv_color_hex(0x9ca3af),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(score_hdr, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(score_hdr, LV_ALIGN_TOP_MID, 0, 6);

    s_score_label = lv_label_create(score_card);
    lv_label_set_text(s_score_label, "0");
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0x1f2937),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_score_label, &lv_font_montserratMedium_27,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_score_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *best_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(best_card, 296, 24);
    lv_obj_set_size(best_card, 74, 54);
    lv_obj_set_style_bg_color(best_card, lv_color_hex(0xffffff),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(best_card, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(best_card, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(best_card, 1,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(best_card, lv_color_hex(0xe5e7eb),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(best_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(best_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(best_card, LV_OBJ_FLAG_SCROLLABLE);
    s_game_score_panels[1] = best_card;

    lv_obj_t *best_hdr = lv_label_create(best_card);
    lv_label_set_text(best_hdr, "最高");
    lv_obj_set_style_text_color(best_hdr, lv_color_hex(0x9ca3af),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(best_hdr, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(best_hdr, LV_ALIGN_TOP_MID, 0, 6);

    s_best_score_label = lv_label_create(best_card);
    lv_label_set_text(s_best_score_label, "0");
    lv_obj_set_style_text_color(s_best_score_label, lv_color_hex(0x1f2937),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_best_score_label, &lv_font_montserratMedium_27,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_best_score_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* 非正常状态才显示提示，避免初始画面与棋盘竞争空间。 */
    s_state_icon = lv_obj_create(s_game_cont);
    lv_obj_set_size(s_state_icon, 30, 18);
    lv_obj_set_pos(s_state_icon, 40, 78);
    lv_obj_set_style_radius(s_state_icon, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_state_icon, lv_color_hex(0xdcfce7),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_state_icon, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_state_icon, 0,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_state_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_arrow = lv_label_create(s_state_icon);
    lv_label_set_text(icon_arrow, "<->");
    lv_obj_set_style_text_color(icon_arrow, lv_color_hex(0x10b981),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(icon_arrow, &lv_font_montserratMedium_12,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(icon_arrow);

    s_state_label = lv_label_create(s_game_cont);
    lv_obj_set_size(s_state_label, 230, 18);
    lv_obj_set_pos(s_state_label, 40, 78);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x6b7280),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── 底部安全区悬浮控制栏：返回 / 新游戏 / 暂停 ── */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_LEFT, lv_color_hex(0x10b981), "返回", lv_color_hex(0x374151),
        kControlLeftX, kControlsY, kControlLeftWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[0] = back_btn;
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *new_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "新游戏", lv_color_hex(0xffffff),
        kControlMiddleX, kControlsY, kControlMiddleWidth, kControlsHeight,
        lv_color_hex(0x5ebb70), false);
    s_game_controls[1] = new_btn;
    lv_obj_add_event_cb(new_btn, mini_games_controller_new_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0x3b82f6), "暂停", lv_color_hex(0x374151),
        kControlRightX, kControlsY, kControlRightWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[2] = s_pause_btn;
    lv_obj_add_event_cb(s_pause_btn, mini_games_controller_pause_event_cb,
                        LV_EVENT_CLICKED, NULL);

    /* ── 2048 主舞台：顶部得分与底部操作栏之间保留 8px 间隔 ── */
    s_board = lv_obj_create(s_game_cont);
    lv_obj_set_size(s_board, kBoardSize, kBoardSize);
    lv_obj_set_pos(s_board, kBoardX, kBoardY);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(0xffffff),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_board, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_board, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_board, lv_color_hex(0xe5e7eb),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_board, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_board, kTileGap,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, mini_games_controller_board_event_cb,
                        LV_EVENT_ALL, NULL);

    mini_games_controller_create_tiles();
    mini_games_controller_create_2048_win_overlay();
}

static void mini_games_controller_refresh_board(void)
{
    if (!mini_games_controller_is_screen_alive()) {
        return;
    }

    if (s_score_label != NULL && lv_obj_is_valid(s_score_label)) {
        lv_label_set_text_fmt(s_score_label, "%lu",
                               (unsigned long)mini_game_2048_get_score(&s_game));
    }

    const uint32_t curr_score = mini_game_2048_get_score(&s_game);
    const uint32_t stored_best = mini_games_progress_get_high_score(
        MINI_GAMES_PROGRESS_2048);
    const uint32_t visible_best =
        curr_score > stored_best ? curr_score : stored_best;

    if (s_best_score_label != NULL && lv_obj_is_valid(s_best_score_label)) {
        lv_label_set_text_fmt(s_best_score_label, "%lu",
                              (unsigned long)visible_best);
    }

    if (s_2048_win_overlay != NULL &&
        lv_obj_is_valid(s_2048_win_overlay)) {
        if (mini_game_2048_is_waiting_for_continue(&s_game)) {
            lv_obj_remove_flag(s_2048_win_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_2048_win_overlay);
        } else {
            lv_obj_add_flag(s_2048_win_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_state_label != NULL && lv_obj_is_valid(s_state_label)) {
        if (mini_game_2048_is_game_over(&s_game)) {
            lv_label_set_text(s_state_label, "游戏结束");
            lv_obj_set_pos(s_state_label, 40, 78);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (mini_game_2048_is_waiting_for_continue(&s_game)) {
            lv_label_set_text(s_state_label, "已合成 2048");
            lv_obj_set_pos(s_state_label, 40, 78);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (s_paused) {
            lv_label_set_text(s_state_label,
                              s_auto_paused ? "已自动暂停" : "已暂停");
            lv_obj_set_pos(s_state_label, 40, 78);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_state_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_state_label, LV_OBJ_FLAG_HIDDEN);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_pause_btn != NULL && lv_obj_is_valid(s_pause_btn)) {
        lv_obj_t *content = lv_obj_get_child(s_pause_btn, 0);
        if (content != NULL && lv_obj_is_valid(content)) {
            lv_obj_t *icon_lbl = lv_obj_get_child(content, 0);
            lv_obj_t *text_lbl = lv_obj_get_child(content, 1);
            if (icon_lbl != NULL && text_lbl != NULL) {
                if (s_paused) {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PLAY);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "继续");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                } else {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PAUSE);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x3b82f6), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "暂停");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x374151), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                lv_obj_align_to(text_lbl, icon_lbl, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
            }
        }
    }

    mini_games_controller_set_controls_running(
        !s_paused && !mini_game_2048_is_game_over(&s_game) &&
        !mini_game_2048_is_waiting_for_continue(&s_game));

    for (uint8_t row = 0; row < MINI_GAME_2048_SIZE; ++row) {
        for (uint8_t col = 0; col < MINI_GAME_2048_SIZE; ++col) {
            lv_obj_t *label = s_tile_labels[row][col];
            if (label == NULL || !lv_obj_is_valid(label)) {
                continue;
            }

            lv_obj_t *tile = lv_obj_get_parent(label);
            const uint16_t value = mini_game_2048_get_cell(&s_game, row, col);
            if (value == 0U) {
                lv_label_set_text(label, "");
            } else {
                lv_label_set_text_fmt(label, "%u", (unsigned int)value);
            }

            lv_obj_set_style_bg_color(
                tile, mini_games_controller_tile_color(value),
                LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(
                label, mini_games_controller_tile_text_color(value),
                LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_center(label);
        }
    }
}

#if !DISABLE_NEW_GAMES
/* ── Flappy Bird 游戏 UI 动态重构挂载 ── */
static void mini_games_controller_setup_flappy(void)
{
    /* 全屏天空舞台先创建，后续得分和操作控件作为安全区悬浮层。 */
    lv_obj_t *stage = lv_obj_create(s_game_cont);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_size(stage, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xbae6fd), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, mini_games_controller_flappy_click_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 218, 24);
    lv_obj_set_size(score_card, 152, 48);
    lv_obj_set_style_bg_color(score_card, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(score_card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(score_card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);
    s_game_score_panels[0] = score_card;

    s_flappy_score_label = lv_label_create(score_card);
    lv_label_set_text(s_flappy_score_label, "分数: 0");
    lv_obj_set_style_text_color(s_flappy_score_label, lv_color_hex(0x1f2937), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_flappy_score_label, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_flappy_score_label);

    /* 全屏背景内的局部物理区，子对象默认裁剪在安全窗口中。 */
    lv_obj_t *playfield = lv_obj_create(stage);
    lv_obj_set_pos(playfield, 0, kFlappyPlayfieldY);
    lv_obj_set_size(playfield, FLAPPY_PLAY_AREA_W, FLAPPY_PLAY_AREA_H);
    lv_obj_set_style_bg_opa(playfield, LV_OPA_TRANSP,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(playfield, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(playfield, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(playfield, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(playfield, LV_OBJ_FLAG_CLICKABLE);
    s_flappy_stage = stage;

    /* 3.1 创建背景白云 (先创建的渲染在底层) */
    s_flappy_clouds[0] = lv_image_create(playfield);
    lv_image_set_src(s_flappy_clouds[0], &img_cloud);
    s_flappy_clouds_x[0] = 50.0f;
    s_flappy_clouds_y[0] = 30.0f;
    lv_obj_set_pos(s_flappy_clouds[0], (lv_coord_t)s_flappy_clouds_x[0], (lv_coord_t)s_flappy_clouds_y[0]);
    lv_obj_set_style_image_opa(s_flappy_clouds[0], LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_flappy_clouds[0], LV_OBJ_FLAG_CLICKABLE);

    s_flappy_clouds[1] = lv_image_create(playfield);
    lv_image_set_src(s_flappy_clouds[1], &img_cloud);
    s_flappy_clouds_x[1] = 220.0f;
    s_flappy_clouds_y[1] = 75.0f;
    lv_obj_set_pos(s_flappy_clouds[1], (lv_coord_t)s_flappy_clouds_x[1], (lv_coord_t)s_flappy_clouds_y[1]);
    lv_obj_set_style_image_opa(s_flappy_clouds[1], LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_flappy_clouds[1], LV_OBJ_FLAG_CLICKABLE);

    s_flappy_state_label = lv_label_create(playfield);
    lv_label_set_text(s_flappy_state_label, "轻触屏幕开始飞翔");
    lv_obj_set_size(s_flappy_state_label, FLAPPY_PLAY_AREA_W - 24, 56);
    lv_obj_set_style_text_color(s_flappy_state_label, lv_color_hex(0x0369a1), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_flappy_state_label, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_flappy_state_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_flappy_state_label);

    /* 4. 创建障碍物管道 */
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        s_flappy_pipes_top[i] = lv_obj_create(playfield);
        lv_obj_set_style_bg_color(s_flappy_pipes_top[i], lv_color_hex(0x22c55e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_flappy_pipes_top[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_flappy_pipes_top[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_flappy_pipes_top[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_flappy_pipes_top[i], lv_color_hex(0x16a34a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_flappy_pipes_top[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_flappy_pipes_top[i], LV_OBJ_FLAG_SCROLLABLE);

        s_flappy_pipes_bottom[i] = lv_obj_create(playfield);
        lv_obj_set_style_bg_color(s_flappy_pipes_bottom[i], lv_color_hex(0x22c55e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_flappy_pipes_bottom[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_flappy_pipes_bottom[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_flappy_pipes_bottom[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_flappy_pipes_bottom[i], lv_color_hex(0x16a34a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_flappy_pipes_bottom[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_flappy_pipes_bottom[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 5. 创建小鸟图像对象 (使用像素黄鸟贴图) */
    s_flappy_bird_obj = lv_image_create(playfield);
    lv_image_set_src(s_flappy_bird_obj, &img_bird_mid);
    lv_obj_set_size(s_flappy_bird_obj, 34, 24);
    lv_obj_remove_flag(s_flappy_bird_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_flappy_bird_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* 6. 底部控制栏 */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_LEFT, lv_color_hex(0xef4444), "退出", lv_color_hex(0x374151),
        kControlLeftX, kControlsY, kControlLeftWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[0] = back_btn;
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "重开", lv_color_hex(0xffffff),
        kControlMiddleX, kControlsY, kControlMiddleWidth, kControlsHeight,
        lv_color_hex(0x0ea5e9), false);
    s_game_controls[1] = restart_btn;
    lv_obj_add_event_cb(restart_btn, mini_games_controller_flappy_restart_cb, LV_EVENT_CLICKED, NULL);

    s_flappy_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0x0ea5e9), "暂停", lv_color_hex(0x374151),
        kControlRightX, kControlsY, kControlRightWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[2] = s_flappy_pause_btn;
    lv_obj_add_event_cb(s_flappy_pause_btn, mini_games_controller_flappy_pause_cb, LV_EVENT_CLICKED, NULL);
}

static void mini_games_controller_refresh_flappy(void)
{
    if (s_flappy_bird_obj == NULL || !lv_obj_is_valid(s_flappy_bird_obj)) {
        return;
    }

    /* 根据得分渐变切换日夜与黄昏背景 */
    lv_color_t bg_color = lv_color_hex(0xbae6fd);
    lv_color_t text_color = lv_color_hex(0x0369a1);
    uint8_t cloud_opa_0 = LV_OPA_70;
    uint8_t cloud_opa_1 = LV_OPA_50;

    if (s_game_flappy.score >= 20) {
        bg_color = lv_color_hex(0x0f172a);
        text_color = lv_color_hex(0x38bdf8);
        cloud_opa_0 = LV_OPA_10;
        cloud_opa_1 = LV_OPA_10;
    } else if (s_game_flappy.score >= 10) {
        bg_color = lv_color_hex(0xfdba74);
        text_color = lv_color_hex(0x9a3412);
        cloud_opa_0 = LV_OPA_40;
        cloud_opa_1 = LV_OPA_30;
    }

    if (s_flappy_stage && lv_obj_is_valid(s_flappy_stage)) {
        lv_obj_set_style_bg_color(s_flappy_stage, bg_color,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (s_flappy_state_label && lv_obj_is_valid(s_flappy_state_label)) {
        lv_obj_set_style_text_color(s_flappy_state_label, text_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (s_flappy_clouds[0] && lv_obj_is_valid(s_flappy_clouds[0])) {
        lv_obj_set_style_image_opa(s_flappy_clouds[0], cloud_opa_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (s_flappy_clouds[1] && lv_obj_is_valid(s_flappy_clouds[1])) {
        lv_obj_set_style_image_opa(s_flappy_clouds[1], cloud_opa_1, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    /* 1. 云朵滑动 2.5D 视差计算 */
    if (!s_paused && mini_game_flappy_is_started(&s_game_flappy) &&
        !s_game_flappy.game_over && s_game_flappy.frame_count > 0) {
        // 第一朵云速度 0.3px/帧
        s_flappy_clouds_x[0] -= 0.3f;
        if (s_flappy_clouds_x[0] + 32.0f < 0.0f) {
            s_flappy_clouds_x[0] = (float)FLAPPY_PLAY_AREA_W;
            s_game_flappy.rng_state = s_game_flappy.rng_state * 1103515245 + 12345;
            s_flappy_clouds_y[0] = (float)(20 + (s_game_flappy.rng_state % 50));
        }
        // 第二朵云速度 0.6px/帧
        s_flappy_clouds_x[1] -= 0.6f;
        if (s_flappy_clouds_x[1] + 32.0f < 0.0f) {
            s_flappy_clouds_x[1] = (float)FLAPPY_PLAY_AREA_W;
            s_game_flappy.rng_state = s_game_flappy.rng_state * 1103515245 + 12345;
            s_flappy_clouds_y[1] = (float)(60 + (s_game_flappy.rng_state % 60));
        }
    }

    if (s_flappy_clouds[0] != NULL && lv_obj_is_valid(s_flappy_clouds[0])) {
        lv_obj_set_pos(s_flappy_clouds[0], (lv_coord_t)s_flappy_clouds_x[0], (lv_coord_t)s_flappy_clouds_y[0]);
    }
    if (s_flappy_clouds[1] != NULL && lv_obj_is_valid(s_flappy_clouds[1])) {
        lv_obj_set_pos(s_flappy_clouds[1], (lv_coord_t)s_flappy_clouds_x[1], (lv_coord_t)s_flappy_clouds_y[1]);
    }

    /* 2. 小鸟物理坐标、动作贴图与倾斜角刷新 */
    lv_obj_set_pos(s_flappy_bird_obj, FLAPPY_BIRD_X - 7, (lv_coord_t)s_game_flappy.bird_y - 2);

    int32_t angle = 0;
    const lv_image_dsc_t *bird_src = &img_bird_mid;

    if (s_game_flappy.bird_vy < 0.0f) {
        // 向上起跳：朝上倾斜，角度为速度的线性映射
        angle = (int32_t)(s_game_flappy.bird_vy * 100);
        if (angle < -350) angle = -350; // 最大朝上 35 度
        bird_src = &img_bird_up;
    } else {
        // 向下坠落：随着下坠速度增大，倾斜角度平滑变大，直至垂直朝下 90 度
        angle = (int32_t)(s_game_flappy.bird_vy * 180);
        if (angle > 900) angle = 900; // 最大朝下 90 度
        if (s_game_flappy.bird_vy > 1.5f) {
            bird_src = &img_bird_down;
        } else {
            bird_src = &img_bird_mid;
        }
    }

    lv_image_set_src(s_flappy_bird_obj, bird_src);
    lv_image_set_rotation(s_flappy_bird_obj, angle);

    /* 3. 管道移动 */
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        const flappy_pipe_t *pipe = &s_game_flappy.pipes[i];
        lv_obj_t *top_pipe_obj = s_flappy_pipes_top[i];
        lv_obj_t *bottom_pipe_obj = s_flappy_pipes_bottom[i];

        if (top_pipe_obj != NULL && lv_obj_is_valid(top_pipe_obj)) {
            lv_obj_set_pos(top_pipe_obj, (lv_coord_t)pipe->x, 0);
            lv_obj_set_size(top_pipe_obj, FLAPPY_PIPE_WIDTH, (lv_coord_t)pipe->top_h);
        }
        if (bottom_pipe_obj != NULL && lv_obj_is_valid(bottom_pipe_obj)) {
            lv_obj_set_pos(bottom_pipe_obj, (lv_coord_t)pipe->x, (lv_coord_t)pipe->bottom_y);
            lv_obj_set_size(bottom_pipe_obj, FLAPPY_PIPE_WIDTH, (lv_coord_t)(FLAPPY_PLAY_AREA_H - pipe->bottom_y));
        }
    }

    if (s_flappy_score_label != NULL && lv_obj_is_valid(s_flappy_score_label)) {
        const uint32_t stored_best = mini_games_progress_get_high_score(
            MINI_GAMES_PROGRESS_FLAPPY);
        const uint32_t visible_best = s_game_flappy.score > stored_best
                                          ? s_game_flappy.score
                                          : stored_best;
        lv_label_set_text_fmt(s_flappy_score_label, "分数 %lu 最高 %lu",
                              (unsigned long)s_game_flappy.score,
                              (unsigned long)visible_best);
        if (s_game_flappy.score > s_flappy_last_score) {
            s_flappy_last_score = s_game_flappy.score;
            mini_games_controller_pulse(s_flappy_score_label);
        }
    }

    if (s_flappy_state_label != NULL && lv_obj_is_valid(s_flappy_state_label)) {
        if (s_game_flappy.game_over) {
            const char *medal = "";
            if (s_game_flappy.score >= 40) {
                medal = " 白金";
            } else if (s_game_flappy.score >= 30) {
                medal = " 金牌";
            } else if (s_game_flappy.score >= 20) {
                medal = " 银牌";
            } else if (s_game_flappy.score >= 10) {
                medal = " 铜牌";
            }
            lv_label_set_text_fmt(s_flappy_state_label, "游戏结束！%s\n点击重开", medal);
            lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
            if (!s_flappy_game_over_recorded) {
                s_flappy_game_over_recorded = true;
                mini_games_controller_submit_current_score();
                mini_games_controller_pulse(s_flappy_state_label);
            }
        } else if (s_paused) {
            lv_label_set_text(s_flappy_state_label,
                              s_auto_paused ? "已自动暂停" : "已暂停");
            lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (!mini_game_flappy_is_started(&s_game_flappy)) {
            lv_label_set_text(s_flappy_state_label, "轻触屏幕开始飞翔");
            lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_flappy_pause_btn != NULL && lv_obj_is_valid(s_flappy_pause_btn)) {
        lv_obj_t *content = lv_obj_get_child(s_flappy_pause_btn, 0);
        if (content != NULL && lv_obj_is_valid(content)) {
            lv_obj_t *icon_lbl = lv_obj_get_child(content, 0);
            lv_obj_t *text_lbl = lv_obj_get_child(content, 1);
            if (icon_lbl != NULL && text_lbl != NULL) {
                if (s_paused) {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PLAY);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "继续");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                } else {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PAUSE);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x0ea5e9), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "暂停");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x374151), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                lv_obj_align_to(text_lbl, icon_lbl, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
            }
        }
    }

    mini_games_controller_set_controls_running(
        mini_game_flappy_is_started(&s_game_flappy) && !s_paused &&
        !s_game_flappy.game_over);
}

static void mini_games_controller_flappy_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_FLAPPY && !s_paused &&
        !mini_game_flappy_is_game_over(&s_game_flappy)) {
        mini_game_flappy_jump(&s_game_flappy);
        mini_games_controller_refresh_flappy();
    }
}

static void mini_games_controller_flappy_restart_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
        mini_game_flappy_reset(&s_game_flappy, mini_games_controller_seed());
        s_paused = false;
        s_auto_paused = false;
        s_flappy_last_score = 0U;
        s_flappy_game_over_recorded = false;
        mini_games_controller_refresh_flappy();
    }
}

static void mini_games_controller_flappy_pause_cb(lv_event_t *e)
{
    (void)e;
    if (mini_game_flappy_is_started(&s_game_flappy) &&
        !mini_game_flappy_is_game_over(&s_game_flappy)) {
        mini_games_controller_set_paused(!s_paused);
    }
}

/* ── 小恐龙跑酷 UI 动态重构挂载 ── */
static void mini_games_controller_setup_dino(void)
{
    /* 全屏跑道舞台先创建，顶部只保留得分浮层。 */
    lv_obj_t *stage = lv_obj_create(s_game_cont);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_size(stage, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xfef08a), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    /* 只接收触摸边沿；不能订阅 LV_EVENT_ALL 后在 redraw 回调中改样式。 */
    lv_obj_add_event_cb(stage, mini_games_controller_dino_click_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(stage, mini_games_controller_dino_click_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(stage, mini_games_controller_dino_click_cb,
                        LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 218, 24);
    lv_obj_set_size(score_card, 152, 48);
    lv_obj_set_style_bg_color(score_card, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(score_card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(score_card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);
    s_game_score_panels[0] = score_card;

    s_dino_score_label = lv_label_create(score_card);
    lv_label_set_text(s_dino_score_label, "分数: 0");
    lv_obj_set_style_text_color(s_dino_score_label, lv_color_hex(0x1f2937), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_dino_score_label, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_dino_score_label);

    /* 3.1 绘制天空背景白云 (先创建的渲染在底层) */
    s_dino_cloud = lv_image_create(stage);
    lv_image_set_src(s_dino_cloud, &img_cloud);
    s_dino_cloud_x = 180.0f;
    lv_obj_set_pos(s_dino_cloud, (lv_coord_t)s_dino_cloud_x, 30);
    lv_obj_set_style_image_opa(s_dino_cloud, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_dino_cloud, LV_OBJ_FLAG_CLICKABLE);

    /* 绘制灰色水平地面横线 */
    lv_obj_t *ground = lv_obj_create(stage);
    lv_obj_set_size(ground, DINO_PLAY_AREA_W, 2);
    lv_obj_set_pos(ground, 0, DINO_GROUND_Y);
    lv_obj_set_style_bg_color(ground, lv_color_hex(0x9ca3af), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ground, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ground, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(ground, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ground, LV_OBJ_FLAG_SCROLLABLE);

    /* 3.2 创建地表狂奔飞沙 (极小像素颗粒) */
    for (uint8_t i = 0; i < 4; ++i) {
        s_dino_sand_particles[i] = lv_obj_create(stage);
        lv_obj_set_size(s_dino_sand_particles[i], 3, 2);
        lv_obj_set_style_radius(s_dino_sand_particles[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_dino_sand_particles[i], lv_color_hex(0xa1a1aa), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_dino_sand_particles[i], LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_dino_sand_particles[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_dino_sand_particles[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_dino_sand_particles[i], LV_OBJ_FLAG_SCROLLABLE);

        s_dino_sand_x[i] = (float)(i * 90 + 20);
        lv_obj_set_pos(s_dino_sand_particles[i], (lv_coord_t)s_dino_sand_x[i], DINO_GROUND_Y + 4 + (i % 2) * 6);
    }

    s_dino_state_label = lv_label_create(stage);
    lv_label_set_text(s_dino_state_label, "右屏轻触开始\n左屏按住下蹲");
    lv_obj_set_size(s_dino_state_label, DINO_PLAY_AREA_W - 24, 56);
    lv_obj_set_style_text_color(s_dino_state_label, lv_color_hex(0xa16207), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_dino_state_label, &lv_font_montserrat_lxgw_common_5500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_dino_state_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_dino_state_label);

    /* 4. 创建障碍物图像对象 (默认贴图仙人掌) */
    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        s_dino_obstacles[i] = lv_image_create(stage);
        lv_image_set_src(s_dino_obstacles[i], &img_cactus);
        lv_obj_remove_flag(s_dino_obstacles[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_dino_obstacles[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 5. 创建恐龙图像对象 (使用像素小恐龙贴图) */
    s_dino_obj = lv_image_create(stage);
    lv_image_set_src(s_dino_obj, &img_dino_run1);
    lv_obj_set_size(s_dino_obj, 22, 24);
    lv_obj_remove_flag(s_dino_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dino_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* 6. 底部控制栏 */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_LEFT, lv_color_hex(0xef4444), "退出", lv_color_hex(0x374151),
        kControlLeftX, kControlsY, kControlLeftWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[0] = back_btn;
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "重开", lv_color_hex(0xffffff),
        kControlMiddleX, kControlsY, kControlMiddleWidth, kControlsHeight,
        lv_color_hex(0xf59e0b), false);
    s_game_controls[1] = restart_btn;
    lv_obj_add_event_cb(restart_btn, mini_games_controller_dino_restart_cb, LV_EVENT_CLICKED, NULL);

    s_dino_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0xf59e0b), "暂停", lv_color_hex(0x374151),
        kControlRightX, kControlsY, kControlRightWidth, kControlsHeight,
        lv_color_hex(0xffffff), true);
    s_game_controls[2] = s_dino_pause_btn;
    lv_obj_add_event_cb(s_dino_pause_btn, mini_games_controller_dino_pause_cb, LV_EVENT_CLICKED, NULL);
}

static void mini_games_controller_refresh_dino(void)
{
    if (s_dino_obj == NULL || !lv_obj_is_valid(s_dino_obj)) {
        return;
    }

    /* 每 250 分昼夜交替；约半分钟可完成一次完整手表短局节奏。 */
    bool night_mode = false;
    if (!s_game_dino.game_over) {
        night_mode = (s_game_dino.score / 250) % 2 != 0;
    }

    lv_color_t bg_color = night_mode ? lv_color_hex(0x1f2937) : lv_color_hex(0xfef08a);
    lv_color_t ground_color = night_mode ? lv_color_hex(0xf9fafb) : lv_color_hex(0x9ca3af);
    lv_color_t sand_color = night_mode ? lv_color_hex(0xe5e7eb) : lv_color_hex(0xa1a1aa);
    lv_color_t label_color = night_mode ? lv_color_hex(0xfef08a) : lv_color_hex(0xa16207);
    uint8_t cloud_opa = night_mode ? LV_OPA_10 : LV_OPA_30;

    lv_obj_t *stage = lv_obj_get_parent(s_dino_obj);
    if (stage && lv_obj_is_valid(stage)) {
        lv_obj_set_style_bg_color(stage, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        // 地面线是 stage 的第 1 个子对象
        lv_obj_t *ground = lv_obj_get_child(stage, 1);
        if (ground && lv_obj_is_valid(ground)) {
            lv_obj_set_style_bg_color(ground, ground_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    if (s_dino_state_label && lv_obj_is_valid(s_dino_state_label)) {
        lv_obj_set_style_text_color(s_dino_state_label, label_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (s_dino_cloud && lv_obj_is_valid(s_dino_cloud)) {
        lv_obj_set_style_image_opa(s_dino_cloud, cloud_opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (s_dino_sand_particles[i] && lv_obj_is_valid(s_dino_sand_particles[i])) {
            lv_obj_set_style_bg_color(s_dino_sand_particles[i], sand_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }

    /* 1. 2.5D 天空背景云朵漂移动画 */
    if (!s_paused && mini_game_dino_is_started(&s_game_dino) &&
        !s_game_dino.game_over && s_game_dino.frame_count > 0) {
        s_dino_cloud_x -= 0.2f;
        if (s_dino_cloud_x + 32.0f < 0.0f) {
            s_dino_cloud_x = (float)DINO_PLAY_AREA_W;
        }
    }
    if (s_dino_cloud != NULL && lv_obj_is_valid(s_dino_cloud)) {
        lv_obj_set_pos(s_dino_cloud, (lv_coord_t)s_dino_cloud_x, 30);
    }

    /* 2. 地面沙尘狂奔飞移特效 */
    if (!s_paused && mini_game_dino_is_started(&s_game_dino) &&
        !s_game_dino.game_over && s_game_dino.frame_count > 0) {
        for (uint8_t i = 0; i < 4; ++i) {
            s_dino_sand_x[i] -= s_game_dino.speed;
            if (s_dino_sand_x[i] + 3.0f < 0.0f) {
                s_dino_sand_x[i] = (float)DINO_PLAY_AREA_W;
            }
            if (s_dino_sand_particles[i] != NULL && lv_obj_is_valid(s_dino_sand_particles[i])) {
                lv_obj_set_pos(s_dino_sand_particles[i], (lv_coord_t)s_dino_sand_x[i], DINO_GROUND_Y + 4 + (i % 2) * 6);
            }
        }
    }

    /* 3. 小恐龙物理坐标与跑步/跳跃/死亡动作贴图以及下蹲高度缩放切换 */
    if (s_game_dino.is_ducking && !s_game_dino.game_over) {
        lv_obj_set_size(s_dino_obj, 22, 14);
        lv_obj_set_pos(s_dino_obj, DINO_X - 1, (lv_coord_t)s_game_dino.dino_y + 10);
    } else {
        lv_obj_set_size(s_dino_obj, 22, 24);
        lv_obj_set_pos(s_dino_obj, DINO_X - 1, (lv_coord_t)s_game_dino.dino_y);
    }

    const lv_image_dsc_t *dino_src = &img_dino_run1;
    if (s_game_dino.game_over) {
        dino_src = &img_dino_dead;
    } else if (s_game_dino.is_jumping) {
        dino_src = &img_dino_jump;
    } else {
        // 跑步踏步：每 4 帧交替更换左右腿
        if ((s_game_dino.frame_count / 4) % 2 == 0) {
            dino_src = &img_dino_run1;
        } else {
            dino_src = &img_dino_run2;
        }
    }
    lv_image_set_src(s_dino_obj, dino_src);

    /* 4. 障碍物渲染更新 (区分仙人掌和扑翼翼龙) */
    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        const dino_obstacle_t *obs = &s_game_dino.obstacles[i];
        lv_obj_t *obs_obj = s_dino_obstacles[i];
        if (obs_obj != NULL && lv_obj_is_valid(obs_obj)) {
            if (obs->type == DINO_OBSTACLE_PTERODACTYL) {
                // 中空翼龙：交替拍翅，尺寸 24x20，底部悬空 14 像素
                const lv_image_dsc_t *ptero_src = ((s_game_dino.frame_count / 4) % 2 == 0) ? &img_pterosaur1 : &img_pterosaur2;
                lv_image_set_src(obs_obj, ptero_src);
                lv_obj_set_size(obs_obj, 24, 20);
                lv_obj_set_pos(obs_obj, (lv_coord_t)obs->x, (lv_coord_t)(DINO_GROUND_Y - 14 - 20));
            } else {
                // 仙人掌：尺寸 obs->w x 24，紧贴地面
                lv_image_set_src(obs_obj, &img_cactus);
                lv_obj_set_size(obs_obj, obs->w, 24);
                lv_obj_set_pos(obs_obj, (lv_coord_t)obs->x, (lv_coord_t)(DINO_GROUND_Y - 24));
            }
        }
    }

    if (s_dino_score_label != NULL && lv_obj_is_valid(s_dino_score_label)) {
        const uint32_t stored_best = mini_games_progress_get_high_score(
            MINI_GAMES_PROGRESS_DINO);
        const uint32_t visible_best = s_game_dino.score > stored_best
                                          ? s_game_dino.score
                                          : stored_best;
        lv_label_set_text_fmt(s_dino_score_label, "分数 %lu 最高 %lu",
                              (unsigned long)s_game_dino.score,
                              (unsigned long)visible_best);
        const uint32_t milestone = s_game_dino.score / 100U;
        if (milestone > s_dino_last_milestone) {
            s_dino_last_milestone = milestone;
            mini_games_controller_pulse(s_dino_score_label);
        }
    }

    if (s_dino_state_label != NULL && lv_obj_is_valid(s_dino_state_label)) {
        if (s_game_dino.game_over) {
            lv_label_set_text(s_dino_state_label, "游戏结束！\n点击重开再试");
            lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
            if (!s_dino_game_over_recorded) {
                s_dino_game_over_recorded = true;
                mini_games_controller_submit_current_score();
                mini_games_controller_pulse(s_dino_state_label);
            }
        } else if (s_paused) {
            lv_label_set_text(s_dino_state_label,
                              s_auto_paused ? "已自动暂停" : "已暂停");
            lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (!mini_game_dino_is_started(&s_game_dino)) {
            lv_label_set_text(s_dino_state_label, "右屏轻触开始\n左屏按住下蹲");
            lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_dino_pause_btn != NULL && lv_obj_is_valid(s_dino_pause_btn)) {
        lv_obj_t *content = lv_obj_get_child(s_dino_pause_btn, 0);
        if (content != NULL && lv_obj_is_valid(content)) {
            lv_obj_t *icon_lbl = lv_obj_get_child(content, 0);
            lv_obj_t *text_lbl = lv_obj_get_child(content, 1);
            if (icon_lbl != NULL && text_lbl != NULL) {
                if (s_paused) {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PLAY);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "继续");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x10b981), LV_PART_MAIN | LV_STATE_DEFAULT);
                } else {
                    lv_label_set_text(icon_lbl, LV_SYMBOL_PAUSE);
                    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(0xf59e0b), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(text_lbl, "暂停");
                    lv_obj_set_style_text_color(text_lbl, lv_color_hex(0x374151), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
                lv_obj_align_to(text_lbl, icon_lbl, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
            }
        }
    }

    mini_games_controller_set_controls_running(
        mini_game_dino_is_started(&s_game_dino) && !s_paused &&
        !s_game_dino.game_over);
}

static void mini_games_controller_dino_click_cb(lv_event_t *e)
{
    if (s_current_game != MINI_GAME_TYPE_DINO || s_paused ||
        mini_game_dino_is_game_over(&s_game_dino)) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && code == LV_EVENT_PRESSED) {
        lv_point_t click_point;
        lv_indev_get_point(indev, &click_point);

        // 左半屏按住下蹲，右半屏轻触跳跃；分区保留单手可达性。
        if (click_point.x < 205) {
            mini_game_dino_set_ducking(&s_game_dino, true);
        } else {
            mini_game_dino_jump(&s_game_dino);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        mini_game_dino_set_ducking(&s_game_dino, false);
    }
    /* 游戏 timer 会在事件返回后刷新，避免在 LVGL event/redraw 栈内递归失效。 */
}

static void mini_games_controller_dino_restart_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_DINO) {
        mini_game_dino_reset(&s_game_dino, mini_games_controller_seed());
        s_paused = false;
        s_auto_paused = false;
        s_dino_last_milestone = 0U;
        s_dino_game_over_recorded = false;
        mini_games_controller_refresh_dino();
    }
}

static void mini_games_controller_dino_pause_cb(lv_event_t *e)
{
    (void)e;
    if (mini_game_dino_is_started(&s_game_dino) &&
        !mini_game_dino_is_game_over(&s_game_dino)) {
        mini_games_controller_set_paused(!s_paused);
    }
}
#endif

/* ── 统一的手表页面退出动画路径 ── */
static void mini_games_controller_leave(void)
{
    if (s_ui == NULL || s_ui->screen_main == NULL ||
        !mini_games_controller_is_screen_alive()) {
        return;
    }

    s_paused = false;
    s_auto_paused = false;
    if (s_game_timer != NULL) {
        lv_timer_delete(s_game_timer);
        s_game_timer = NULL;
    }

    lv_screen_load_anim(s_ui->screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0,
                        true);
}

void mini_games_controller_init(lv_ui *ui)
{
    s_ui = ui;
    const esp_err_t err = mini_games_progress_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mini game progress owner start failed: %s",
                 esp_err_to_name(err));
    }
}

void mini_games_controller_open(void)
{
    board_button_clear_events();
    
    /* 确认 s_screen 已经建立，且子画布分层已成功挂载 */
    if (!mini_games_controller_is_screen_alive()) {
        mini_games_controller_reset_refs();

        s_screen = lv_obj_create(NULL);
        lv_obj_set_size(s_screen, kScreenWidth, kScreenHeight);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xf8fafc),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_screen, mini_games_controller_screen_delete_event_cb,
                            LV_EVENT_DELETE, NULL);

        /* 统一菜单画布 */
        s_menu_cont = lv_obj_create(s_screen);
        lv_obj_set_size(s_menu_cont, kScreenWidth, kScreenHeight);
        lv_obj_set_pos(s_menu_cont, 0, 0);
        lv_obj_set_style_bg_opa(s_menu_cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_menu_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_menu_cont, LV_OBJ_FLAG_SCROLLABLE);

        /* 统一游戏画布 (默认为透明，隐藏) */
        s_game_cont = lv_obj_create(s_screen);
        lv_obj_set_size(s_game_cont, kScreenWidth, kScreenHeight);
        lv_obj_set_pos(s_game_cont, 0, 0);
        lv_obj_set_style_bg_opa(s_game_cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_game_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_game_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_game_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_game_cont, LV_OBJ_FLAG_HIDDEN);

        mini_games_controller_setup_menu();
        ESP_LOGI(TAG, "Mini games shell with multi-page structure created");
    }

    mini_games_controller_switch_to_game(MINI_GAME_TYPE_NONE);
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

#ifdef AGENT_PREVIEW_HOST
void mini_games_controller_open_preview_game(uint8_t game_index)
{
    mini_games_controller_open();
    if (game_index >= MINI_GAME_TYPE_2048 && game_index <= MINI_GAME_TYPE_DINO) {
        mini_games_controller_switch_to_game((mini_game_type_t)game_index);
    }
}
#endif

void mini_games_controller_poll_ui(void)
{
    if (!mini_games_controller_is_foreground()) {
        board_button_clear_events();
        return;
    }

    ui_refresh_policy_activity_snapshot_t activity = {0};
    const bool standby = ui_refresh_policy_get_activity_snapshot(&activity) &&
                         activity.standby;
    const bool game_running =
        s_current_game == MINI_GAME_TYPE_2048 ||
#if !DISABLE_NEW_GAMES
        (s_current_game == MINI_GAME_TYPE_FLAPPY &&
         mini_game_flappy_is_started(&s_game_flappy)) ||
        (s_current_game == MINI_GAME_TYPE_DINO &&
         mini_game_dino_is_started(&s_game_dino)) ||
#endif
        false;
    if (game_running && !s_paused &&
        (standby || watch_nc_is_visible())) {
        s_paused = true;
        s_auto_paused = true;
        mini_games_controller_refresh_current_game();
    }

    for (;;) {
        const board_button_event_t event = board_button_consume_event();
        if (event == BOARD_BUTTON_EVENT_NONE) {
            break;
        }

        /* 物理按键长按：退出或返回上级 */
        if (event == BOARD_BUTTON_EVENT_LONG_PRESS) {
            if (s_current_game != MINI_GAME_TYPE_NONE) {
                /* 如果正在玩游戏，退回选择主菜单 */
                mini_games_controller_switch_to_game(MINI_GAME_TYPE_NONE);
            } else {
                /* 如果在主菜单，直接退出手表小游戏返回桌面 */
                mini_games_controller_leave();
            }
            break;
        }

        /* 物理按键短按：游戏暂停与继续 */
        if (event == BOARD_BUTTON_EVENT_SINGLE_CLICK) {
            if (s_current_game == MINI_GAME_TYPE_2048) {
                mini_games_controller_set_paused(!s_paused);
#if !DISABLE_NEW_GAMES
            } else if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
                if (mini_game_flappy_is_started(&s_game_flappy) &&
                    !mini_game_flappy_is_game_over(&s_game_flappy)) {
                    mini_games_controller_set_paused(!s_paused);
                }
            } else if (s_current_game == MINI_GAME_TYPE_DINO) {
                if (mini_game_dino_is_started(&s_game_dino) &&
                    !mini_game_dino_is_game_over(&s_game_dino)) {
                    mini_games_controller_set_paused(!s_paused);
                }
#endif
            }
        }
    }
}
