#ifndef BOARD_BUTTON_H
#define BOARD_BUTTON_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_BUTTON_EVENT_NONE = 0,
    BOARD_BUTTON_EVENT_SINGLE_CLICK,
    BOARD_BUTTON_EVENT_LONG_PRESS,
} board_button_event_t;

/**
 * @brief 初始化板载单按键。
 *
 * 该模块只把硬件按键事件转换成 FreeRTOS queue 里的边沿事件，不承担配网、
 * 页面跳转或 LVGL 对象操作。
 *
 * @return `ESP_OK` 表示按键驱动初始化完成。
 */
esp_err_t board_button_init(void);

/**
 * @brief 消费一个待处理的按键事件。
 *
 * @return 最近一次待处理事件；没有事件时返回 `BOARD_BUTTON_EVENT_NONE`。
 */
board_button_event_t board_button_consume_event(void);

/**
 * @brief 清空当前尚未消费的按键事件。
 *
 * 页面切换时调用该接口，可以避免其他页面产生的旧按键事件在小游戏页被误消费。
 */
void board_button_clear_events(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_BUTTON_H */
