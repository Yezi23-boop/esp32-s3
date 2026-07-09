#ifndef MINI_GAMES_CONTROLLER_H
#define MINI_GAMES_CONTROLLER_H

#include "gui_guider.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化小游戏 UI 控制器。
 *
 * @param[in] ui GUI Guider 全局 UI 树。
 */
void mini_games_controller_init(lv_ui *ui);

/**
 * @brief 打开 2048 小游戏页面。
 */
void mini_games_controller_open(void);

/**
 * @brief 在 LVGL 线程轮询小游戏前台事件。
 *
 * 当前只消费板载按键 pending 事件：短按暂停/继续，长按退出页面。
 */
void mini_games_controller_poll_ui(void);

#ifdef __cplusplus
}
#endif

#endif /* MINI_GAMES_CONTROLLER_H */
