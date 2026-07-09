/**
 * @file lv_port.c
 * @brief LVGL 移植层公共状态与总入口
 */

#include "lv_port.h"
#include "lv_port_config.h"
#include "lv_port_internal.h"

/*
 * LVGL 端口层共享状态：
 * 这些变量由显示/输入/tick 子模块共同访问，集中放在单一编译单元避免重复定义。
 */
lv_display_t *s_display = NULL;                      // LVGL 显示对象句柄。
esp_lcd_panel_handle_t s_panel = NULL;               // ESP LCD 面板句柄，由 `co5300_panel` 提供。
void *s_touch = NULL;                                // 触摸驱动句柄，类型由触摸组件内部定义。
int16_t s_last_x = 0;                                // 最近一次触摸点 X，用于 RELEASED 时回填坐标。
int16_t s_last_y = 0;                                // 最近一次触摸点 Y，用于 RELEASED 时回填坐标。
bool s_byte_swap_enabled = LV_PORT_BYTE_SWAP_ENABLE; // RGB565 字节序交换开关。
volatile int s_flush_pending_count = 0;              // 当前 flush 剩余分块数，归零后通知 LVGL `flush_ready`。

#if CO5300_PANEL_USE_TE_SIGNAL
frame_sync_ctx_t s_frame_ctx = {
    .frame_start = true,   // 标记下一块是否属于新帧首块。
    .flush_count = 0,      // 已提交 flush 次数。
    .te_sync_count = 0,    // TE 等待成功次数。
    .te_timeout_count = 0, // TE 等待超时次数。
};
#endif

/**
 * @brief LVGL 端口层初始化入口。
 *
 * 根据 `LV_PORT_FIXED_CHUNK_LINES23` 选择显示缓冲策略：
 * - 非 0：走 `lv_port_disp_init_small()`，偏向片内 DMA 缓冲
 * - 0：走 `lv_port_disp_init_single()`，偏向较大块 PSRAM 缓冲
 *
 * @return 无返回值。
 */
void lv_port_init_small(void)
{
    lv_init();
    lv_port_panel_init();
    lv_port_touch_init();
    if (LV_PORT_FIXED_CHUNK_LINES23)
    {
        lv_port_disp_init_small();
    }
    else
    {
        lv_port_disp_init_single();
    }

    lv_port_indev_init();
    lv_port_tick_init();
}
