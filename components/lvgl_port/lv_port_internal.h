#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "co5300_panel_defaults.h"
#include "lv_port_config.h"
#include "lvgl.h"

#define LV_PORT_TAG "lv_port"

// 跨文件共享状态（定义位于 `lv_port.c`）。
extern lv_display_t *s_display;            // LVGL 显示实例句柄。
extern esp_lcd_panel_handle_t s_panel;     // 底层面板句柄，用于 `draw_bitmap` / `set_gap`。
extern void *s_touch;                      // 触摸驱动句柄，当前仅用于保持初始化结果。
extern int16_t s_last_x;                   // 最近触点 X，抬起态复用该值。
extern int16_t s_last_y;                   // 最近触点 Y，抬起态复用该值。
extern bool s_byte_swap_enabled;           // RGB565 字节序交换开关。
extern volatile int s_flush_pending_count; // 分块 flush 剩余计数，由 flush 回调递减。

#if CO5300_PANEL_USE_TE_SIGNAL
typedef struct
{
    bool frame_start;          /**< true 表示当前即将发送本帧第一块，需先做 TE 同步。 */
    uint32_t flush_count;      /**< 累计 flush 次数。 */
    uint32_t te_sync_count;    /**< TE 同步成功次数。 */
    uint32_t te_timeout_count; /**< TE 同步超时次数。 */
} frame_sync_ctx_t;

extern frame_sync_ctx_t s_frame_ctx;
#endif

void lv_port_disp_init_small(void);                                                  // 初始化双缓冲显示驱动（small 路径）。
void lv_port_disp_init_single(void);                                                 // 初始化显示驱动（single 路径）。
void lv_port_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map); // LVGL flush 回调。
void lv_port_panel_init(void);
void lv_port_touch_init(void);
void lv_port_indev_init(void);
void lv_port_tick_init(void);
