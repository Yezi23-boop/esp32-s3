#include "mini_games_controller.h"

#include <stdio.h>

#include "app/board_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "features/mini_games/mini_game_2048.h"

#define DISABLE_NEW_GAMES 0

#if !DISABLE_NEW_GAMES
#include "features/mini_games/mini_game_flappy.h"
#include "features/mini_games/mini_game_dino.h"
#include "mini_game_sprites.h"
#endif

#include "lvgl.h"
#include "ui_chinese_fonts.h"

static const char *TAG = "mini_games";

static const lv_coord_t kScreenWidth = 410;
static const lv_coord_t kScreenHeight = 502;
static const lv_coord_t kBoardX = 25;
static const lv_coord_t kBoardY = 122;
static const lv_coord_t kBoardSize = 360;
static const lv_coord_t kTileSize = 82;
static const lv_coord_t kTileGap = 8;
static const lv_coord_t kGestureThresholdPx = 28;

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
static lv_obj_t *s_tile_labels[MINI_GAME_2048_SIZE][MINI_GAME_2048_SIZE];
static mini_game_2048_t s_game;
static uint32_t s_best_score_2048 = 0;
static bool s_game_initialized;
static bool s_paused;
static bool s_pressing;
static lv_point_t s_press_start;

#if !DISABLE_NEW_GAMES
/* ── Flappy Bird 全局静态变量 ── */
static lv_obj_t *s_flappy_bird_obj;
static lv_obj_t *s_flappy_pipes_top[FLAPPY_MAX_PIPES];
static lv_obj_t *s_flappy_pipes_bottom[FLAPPY_MAX_PIPES];
static lv_obj_t *s_flappy_score_label;
static lv_obj_t *s_flappy_state_label;
static lv_obj_t *s_flappy_pause_btn;
static mini_game_flappy_t s_game_flappy;
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

static void mini_games_controller_clear_game_refs(void)
{
    s_score_label = NULL;
    s_best_score_label = NULL;
    s_state_label = NULL;
    s_state_icon = NULL;
    s_pause_btn = NULL;
    s_board = NULL;

    for (uint8_t row = 0; row < MINI_GAME_2048_SIZE; ++row) {
        for (uint8_t col = 0; col < MINI_GAME_2048_SIZE; ++col) {
            s_tile_labels[row][col] = NULL;
        }
    }
    s_pressing = false;
    s_game_initialized = false;

#if !DISABLE_NEW_GAMES
    s_flappy_bird_obj = NULL;
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        s_flappy_pipes_top[i] = NULL;
        s_flappy_pipes_bottom[i] = NULL;
    }
    s_flappy_score_label = NULL;
    s_flappy_state_label = NULL;
    s_flappy_pause_btn = NULL;
    s_flappy_clouds[0] = NULL;
    s_flappy_clouds[1] = NULL;

    s_dino_obj = NULL;
    for (uint8_t i = 0; i < DINO_MAX_OBSTACLES; ++i) {
        s_dino_obstacles[i] = NULL;
    }
    s_dino_score_label = NULL;
    s_dino_state_label = NULL;
    s_dino_pause_btn = NULL;
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
    mini_games_controller_refresh_board();
}

static void mini_games_controller_new_game(void)
{
    mini_game_2048_reset(&s_game, mini_games_controller_seed());
    s_game_initialized = true;
    s_paused = false;
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
    if (s_paused || mini_game_2048_is_game_over(&s_game)) {
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
    if (result.moved || result.game_over) {
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
    lv_obj_set_style_text_font(text_lbl, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
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
    lv_obj_set_style_text_font(title, &lv_font_montserrat_lxgw_tghz_level1_3500_22_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title, 40, 40);

    /* 副标题指引 */
    lv_obj_t *sub = lv_label_create(s_menu_cont);
    lv_label_set_text(sub, "请选择一款小游戏开始游玩:");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x6b7280), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
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
            mini_games_controller_setup_flappy();
            mini_games_controller_refresh_flappy();
            /* 启动 40ms 高刷运行 Timer 循环 */
            s_game_timer = lv_timer_create(mini_games_controller_timer_cb, 40, NULL);
        } else if (game_type == MINI_GAME_TYPE_DINO) {
            mini_game_dino_init(&s_game_dino, mini_games_controller_seed());
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
        if (!s_game_flappy.game_over) {
            mini_game_flappy_step(&s_game_flappy);
            mini_games_controller_refresh_flappy();
        }
    } else if (s_current_game == MINI_GAME_TYPE_DINO) {
        if (!s_game_dino.game_over) {
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
    /* ── 标题 "20" & "48"：左上角 ── */
    lv_obj_t *title_20 = lv_label_create(s_game_cont);
    lv_label_set_text(title_20, "20");
    lv_obj_set_style_text_color(title_20, lv_color_hex(0x3482e2),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_20, &lv_font_montserratMedium_46,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title_20, 40, 20);

    lv_obj_t *title_48 = lv_label_create(s_game_cont);
    lv_label_set_text(title_48, "48");
    lv_obj_set_style_text_color(title_48, lv_color_hex(0x5ebb70),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_48, &lv_font_montserratMedium_46,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(title_48, title_20, LV_ALIGN_OUT_RIGHT_TOP, 0, 0);

    /* ── 分数卡片：Score & Best 并排，白色底板 ── */
    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 195, 20);
    lv_obj_set_size(score_card, 92, 62);
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

    lv_obj_t *score_hdr = lv_label_create(score_card);
    lv_label_set_text(score_hdr, "得分");
    lv_obj_set_style_text_color(score_hdr, lv_color_hex(0x9ca3af),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(score_hdr, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
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
    lv_obj_set_pos(best_card, 278, 20);
    lv_obj_set_size(best_card, 92, 62);
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

    lv_obj_t *best_hdr = lv_label_create(best_card);
    lv_label_set_text(best_hdr, "最高");
    lv_obj_set_style_text_color(best_hdr, lv_color_hex(0x9ca3af),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(best_hdr, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(best_hdr, LV_ALIGN_TOP_MID, 0, 6);

    s_best_score_label = lv_label_create(best_card);
    lv_label_set_text(s_best_score_label, "0");
    lv_obj_set_style_text_color(s_best_score_label, lv_color_hex(0x1f2937),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_best_score_label, &lv_font_montserratMedium_27,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_best_score_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* ── 游戏状态提示（滑动指示器，带有绿色的左右箭头小图标和中文提示） ── */
    s_state_icon = lv_obj_create(s_game_cont);
    lv_obj_set_size(s_state_icon, 30, 18);
    lv_obj_set_pos(s_state_icon, 40, 70);
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
    lv_obj_set_size(s_state_label, 210, 18);
    lv_obj_set_pos(s_state_label, 60, 70);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x6b7280),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── 按钮行：返回 / 新游戏 / 暂停，y=88 ── */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_LEFT, lv_color_hex(0x10b981), "返回", lv_color_hex(0x374151),
        40, 88, 100, 34, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *new_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "新游戏", lv_color_hex(0xffffff),
        148, 88, 114, 34, lv_color_hex(0x5ebb70), false);
    lv_obj_add_event_cb(new_btn, mini_games_controller_new_event_cb,
                        LV_EVENT_CLICKED, NULL);

    s_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0x3b82f6), "暂停", lv_color_hex(0x374151),
        256, 88, 114, 34, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(s_pause_btn, mini_games_controller_pause_event_cb,
                        LV_EVENT_CLICKED, NULL);

    /* ── 游戏棋盘：y=134，底部 494px，白色底板 ── */
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

    uint32_t curr_score = mini_game_2048_get_score(&s_game);
    if (curr_score > s_best_score_2048) {
        s_best_score_2048 = curr_score;
    }

    if (s_best_score_label != NULL && lv_obj_is_valid(s_best_score_label)) {
        lv_label_set_text_fmt(s_best_score_label, "%lu", (unsigned long)s_best_score_2048);
    }

    if (s_state_label != NULL && lv_obj_is_valid(s_state_label)) {
        if (mini_game_2048_is_game_over(&s_game)) {
            lv_label_set_text(s_state_label, "游戏结束");
            lv_obj_set_pos(s_state_label, 40, 70);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (s_paused) {
            lv_label_set_text(s_state_label, "已暂停");
            lv_obj_set_pos(s_state_label, 40, 70);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_add_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(s_state_label, "滑动以移动");
            lv_obj_set_pos(s_state_label, 60, 70);
            if (s_state_icon && lv_obj_is_valid(s_state_icon)) {
                lv_obj_remove_flag(s_state_icon, LV_OBJ_FLAG_HIDDEN);
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
    /* 1. 顶部标题 */
    lv_obj_t *title_lbl = lv_label_create(s_game_cont);
    lv_label_set_text(title_lbl, "飞翔的小鸟");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x0ea5e9), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_lxgw_tghz_level1_3500_22_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title_lbl, 40, 20);

    /* 2. 右侧分数卡片 */
    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 222, 20);
    lv_obj_set_size(score_card, 148, 44);
    lv_obj_set_style_bg_color(score_card, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(score_card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(score_card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);

    s_flappy_score_label = lv_label_create(score_card);
    lv_label_set_text(s_flappy_score_label, "分数: 0");
    lv_obj_set_style_text_color(s_flappy_score_label, lv_color_hex(0x1f2937), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_flappy_score_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_flappy_score_label);

    /* 3. 游戏舞台 (天空背景画布) */
    lv_obj_t *stage = lv_obj_create(s_game_cont);
    lv_obj_set_pos(stage, 40, 68);
    lv_obj_set_size(stage, FLAPPY_PLAY_AREA_W, FLAPPY_PLAY_AREA_H);
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xbae6fd), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(stage, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(stage, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(stage, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, mini_games_controller_flappy_click_cb, LV_EVENT_PRESSED, NULL);

    /* 3.1 创建背景白云 (先创建的渲染在底层) */
    s_flappy_clouds[0] = lv_image_create(stage);
    lv_image_set_src(s_flappy_clouds[0], &img_cloud);
    s_flappy_clouds_x[0] = 50.0f;
    s_flappy_clouds_y[0] = 30.0f;
    lv_obj_set_pos(s_flappy_clouds[0], (lv_coord_t)s_flappy_clouds_x[0], (lv_coord_t)s_flappy_clouds_y[0]);
    lv_obj_set_style_image_opa(s_flappy_clouds[0], LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_flappy_clouds[0], LV_OBJ_FLAG_CLICKABLE);

    s_flappy_clouds[1] = lv_image_create(stage);
    lv_image_set_src(s_flappy_clouds[1], &img_cloud);
    s_flappy_clouds_x[1] = 220.0f;
    s_flappy_clouds_y[1] = 75.0f;
    lv_obj_set_pos(s_flappy_clouds[1], (lv_coord_t)s_flappy_clouds_x[1], (lv_coord_t)s_flappy_clouds_y[1]);
    lv_obj_set_style_image_opa(s_flappy_clouds[1], LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_flappy_clouds[1], LV_OBJ_FLAG_CLICKABLE);

    s_flappy_state_label = lv_label_create(stage);
    lv_label_set_text(s_flappy_state_label, "轻触屏幕开始飞翔");
    lv_obj_set_style_text_color(s_flappy_state_label, lv_color_hex(0x0369a1), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_flappy_state_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_flappy_state_label);

    /* 4. 创建障碍物管道 */
    for (uint8_t i = 0; i < FLAPPY_MAX_PIPES; ++i) {
        s_flappy_pipes_top[i] = lv_obj_create(stage);
        lv_obj_set_style_bg_color(s_flappy_pipes_top[i], lv_color_hex(0x22c55e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_flappy_pipes_top[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_flappy_pipes_top[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_flappy_pipes_top[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_flappy_pipes_top[i], lv_color_hex(0x16a34a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_flappy_pipes_top[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_flappy_pipes_top[i], LV_OBJ_FLAG_SCROLLABLE);

        s_flappy_pipes_bottom[i] = lv_obj_create(stage);
        lv_obj_set_style_bg_color(s_flappy_pipes_bottom[i], lv_color_hex(0x22c55e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_flappy_pipes_bottom[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_flappy_pipes_bottom[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_flappy_pipes_bottom[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_flappy_pipes_bottom[i], lv_color_hex(0x16a34a), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_flappy_pipes_bottom[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_flappy_pipes_bottom[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 5. 创建小鸟图像对象 (使用像素黄鸟贴图) */
    s_flappy_bird_obj = lv_image_create(stage);
    lv_image_set_src(s_flappy_bird_obj, &img_bird_mid);
    lv_obj_set_size(s_flappy_bird_obj, 34, 24);
    lv_obj_remove_flag(s_flappy_bird_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_flappy_bird_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* 6. 底部控制栏 */
    lv_obj_t *back_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_LEFT, lv_color_hex(0xef4444), "退出", lv_color_hex(0x374151),
        40, 412, 100, 34, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "重开", lv_color_hex(0xffffff),
        148, 412, 114, 34, lv_color_hex(0x0ea5e9), false);
    lv_obj_add_event_cb(restart_btn, mini_games_controller_flappy_restart_cb, LV_EVENT_CLICKED, NULL);

    s_flappy_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0x0ea5e9), "暂停", lv_color_hex(0x374151),
        256, 412, 114, 34, lv_color_hex(0xffffff), true);
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

    lv_obj_t *stage = lv_obj_get_parent(s_flappy_bird_obj);
    if (stage && lv_obj_is_valid(stage)) {
        lv_obj_set_style_bg_color(stage, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
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
    if (!s_paused && !s_game_flappy.game_over && s_game_flappy.frame_count > 0) {
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
        lv_label_set_text_fmt(s_flappy_score_label, "分数: %lu", (unsigned long)s_game_flappy.score);
    }

    if (s_flappy_state_label != NULL && lv_obj_is_valid(s_flappy_state_label)) {
        if (s_game_flappy.game_over) {
            const char *medal = "";
            if (s_game_flappy.score >= 30) {
                medal = " [🥇金牌]";
            } else if (s_game_flappy.score >= 15) {
                medal = " [🥈银牌]";
            } else if (s_game_flappy.score >= 5) {
                medal = " [🥉铜牌]";
            }
            lv_label_set_text_fmt(s_flappy_state_label, "游戏结束!%s 点击重开", medal);
            lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (s_paused) {
            lv_label_set_text(s_flappy_state_label, "已暂停");
            lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (s_game_flappy.frame_count > 60) {
                lv_obj_add_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(s_flappy_state_label, "轻触屏幕开始飞翔");
                lv_obj_remove_flag(s_flappy_state_label, LV_OBJ_FLAG_HIDDEN);
            }
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
}

static void mini_games_controller_flappy_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
        mini_game_flappy_jump(&s_game_flappy);
    }
}

static void mini_games_controller_flappy_restart_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_FLAPPY) {
        mini_game_flappy_reset(&s_game_flappy, mini_games_controller_seed());
        s_paused = false;
        mini_games_controller_refresh_flappy();
    }
}

static void mini_games_controller_flappy_pause_cb(lv_event_t *e)
{
    (void)e;
    s_paused = !s_paused;
    mini_games_controller_refresh_flappy();
}

/* ── 小恐龙跑酷 UI 动态重构挂载 ── */
static void mini_games_controller_setup_dino(void)
{
    /* 1. 顶部标题 */
    lv_obj_t *title_lbl = lv_label_create(s_game_cont);
    lv_label_set_text(title_lbl, "小恐龙跑酷");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xf59e0b), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_lxgw_tghz_level1_3500_22_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(title_lbl, 40, 20);

    /* 2. 右侧分数卡片 */
    lv_obj_t *score_card = lv_obj_create(s_game_cont);
    lv_obj_set_pos(score_card, 222, 20);
    lv_obj_set_size(score_card, 148, 44);
    lv_obj_set_style_bg_color(score_card, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(score_card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(score_card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(score_card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(score_card, LV_OBJ_FLAG_SCROLLABLE);

    s_dino_score_label = lv_label_create(score_card);
    lv_label_set_text(s_dino_score_label, "分数: 0");
    lv_obj_set_style_text_color(s_dino_score_label, lv_color_hex(0x1f2937), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_dino_score_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_dino_score_label);

    /* 3. 游戏舞台 (浅黄色跑道背景画布) */
    lv_obj_t *stage = lv_obj_create(s_game_cont);
    lv_obj_set_pos(stage, 40, 68);
    lv_obj_set_size(stage, DINO_PLAY_AREA_W, DINO_PLAY_AREA_H);
    lv_obj_set_style_bg_color(stage, lv_color_hex(0xfef08a), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(stage, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(stage, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(stage, lv_color_hex(0xe5e7eb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage, mini_games_controller_dino_click_cb, LV_EVENT_PRESSED, NULL);

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
    lv_label_set_text(s_dino_state_label, "左屏下蹲 / 右屏跳跃");
    lv_obj_set_style_text_color(s_dino_state_label, lv_color_hex(0xa16207), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_dino_state_label, &lv_font_montserrat_lxgw_tghz_level1_3500_16_4,
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
        40, 412, 100, 34, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(back_btn, mini_games_controller_game_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart_btn = mini_games_controller_create_icon_button(
        s_game_cont, "+", lv_color_hex(0xffffff), "重开", lv_color_hex(0xffffff),
        148, 412, 114, 34, lv_color_hex(0xf59e0b), false);
    lv_obj_add_event_cb(restart_btn, mini_games_controller_dino_restart_cb, LV_EVENT_CLICKED, NULL);

    s_dino_pause_btn = mini_games_controller_create_icon_button(
        s_game_cont, LV_SYMBOL_PAUSE, lv_color_hex(0xf59e0b), "暂停", lv_color_hex(0x374151),
        256, 412, 114, 34, lv_color_hex(0xffffff), true);
    lv_obj_add_event_cb(s_dino_pause_btn, mini_games_controller_dino_pause_cb, LV_EVENT_CLICKED, NULL);
}

static void mini_games_controller_refresh_dino(void)
{
    if (s_dino_obj == NULL || !lv_obj_is_valid(s_dino_obj)) {
        return;
    }

    /* 每 50 分白天与夜晚经典昼夜交替 */
    bool night_mode = false;
    if (!s_game_dino.game_over) {
        night_mode = (s_game_dino.score / 50) % 2 != 0;
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
    if (!s_paused && !s_game_dino.game_over && s_game_dino.frame_count > 0) {
        s_dino_cloud_x -= 0.2f;
        if (s_dino_cloud_x + 32.0f < 0.0f) {
            s_dino_cloud_x = (float)DINO_PLAY_AREA_W;
        }
    }
    if (s_dino_cloud != NULL && lv_obj_is_valid(s_dino_cloud)) {
        lv_obj_set_pos(s_dino_cloud, (lv_coord_t)s_dino_cloud_x, 30);
    }

    /* 2. 地面沙尘狂奔飞移特效 */
    if (!s_paused && !s_game_dino.game_over && s_game_dino.frame_count > 0) {
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
        lv_label_set_text_fmt(s_dino_score_label, "分数: %lu", (unsigned long)s_game_dino.score);
    }

    if (s_dino_state_label != NULL && lv_obj_is_valid(s_dino_state_label)) {
        if (s_game_dino.game_over) {
            lv_label_set_text(s_dino_state_label, "游戏结束! 点击重开再试");
            lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
        } else if (s_paused) {
            lv_label_set_text(s_dino_state_label, "已暂停");
            lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (s_game_dino.frame_count > 60) {
                lv_obj_add_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_label_set_text(s_dino_state_label, "左屏下蹲 / 右屏跳跃");
                lv_obj_remove_flag(s_dino_state_label, LV_OBJ_FLAG_HIDDEN);
            }
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
}

static void mini_games_controller_dino_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game != MINI_GAME_TYPE_DINO) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_point_t click_point;
        lv_indev_get_point(indev, &click_point);
        
        // 绝对 X 坐标 < 205 代表左半屏点击，触发下蹲；否则触发跳跃
        if (click_point.x < 205) {
            mini_game_dino_duck(&s_game_dino);
        } else {
            mini_game_dino_jump(&s_game_dino);
        }
    }
}

static void mini_games_controller_dino_restart_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_game == MINI_GAME_TYPE_DINO) {
        mini_game_dino_reset(&s_game_dino, mini_games_controller_seed());
        s_paused = false;
        mini_games_controller_refresh_dino();
    }
}

static void mini_games_controller_dino_pause_cb(lv_event_t *e)
{
    (void)e;
    s_paused = !s_paused;
    mini_games_controller_refresh_dino();
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

void mini_games_controller_poll_ui(void)
{
    if (!mini_games_controller_is_foreground()) {
        board_button_clear_events();
        return;
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
                s_paused = !s_paused;
                mini_games_controller_refresh_flappy();
            } else if (s_current_game == MINI_GAME_TYPE_DINO) {
                s_paused = !s_paused;
                mini_games_controller_refresh_dino();
#endif
            }
        }
    }
}
