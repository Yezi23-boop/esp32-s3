/**
 * @file co5300_panel_defaults.h
 * @brief CO5300 LCD面板默认配置
 * @details
 * 该文件定义面板引脚、分辨率、传输队列和 TE 同步等编译期参数。
 * 上层模块（如 lvgl_port）通过这些宏保持与硬件配置一致。
 */

#pragma once

#include "driver/spi_master.h"
#include "hal/spi_types.h"

/* ========== GPIO引脚定义 ========== */

#define CO5300_PANEL_PIN_PCLK 11 // QSPI 时钟引脚
#define CO5300_PANEL_PIN_CS 12   // QSPI 片选引脚
#define CO5300_PANEL_PIN_D0 4    // QSPI 数据线 D0
#define CO5300_PANEL_PIN_D1 5    // 数据线1
#define CO5300_PANEL_PIN_D2 6    // QSPI 数据线 D2 (WP)
#define CO5300_PANEL_PIN_D3 7    // QSPI 数据线 D3 (HOLD)
#define CO5300_PANEL_PIN_RST 8   // LCD 复位引脚
#define CO5300_PANEL_PIN_TE 13   // TE 同步信号输入引脚

/* ========== SPI配置 ========== */

#define CO5300_PANEL_HOST SPI2_HOST // 绑定的 SPI 主机控制器

/* ========== 显示分辨率 ========== */

#define CO5300_PANEL_H_RES 410 // 面板水平像素数
#define CO5300_PANEL_V_RES 502 // 面板垂直像素数

/* ========== 显示控制 ========== */

#define CO5300_PANEL_DEFAULT_BRIGHTNESS 0xFF // 默认亮度寄存器值（0x00~0xFF）
#define CO5300_PANEL_MAX_TRANSFER_LINES 10   // 单次传输最大行数（控制 DMA/private TX buffer 峰值）

/* TE信号配置 */
#define CO5300_PANEL_USE_TE_SIGNAL 0 // 1: 启用 TE 同步；0: 禁用 TE 同步
#define CO5300_PANEL_TE_MODE 0x00    // TE 模式：0x00=仅 V-Porch；0x01=V-Porch+H-Porch

/* ========== 性能优化 ========== */

#define CO5300_PANEL_OPTIMIZED_PCLK_HZ (50 * 1000 * 1000) // QSPI 像素时钟（Hz）
#define CO5300_PANEL_OPTIMIZED_TRANS_QUEUE_DEPTH 2        // IO 传输队列深度

/* ========== LCD参数 ========== */

#define CO5300_PANEL_BIT_PER_PIXEL 16 // 每像素位宽（RGB565=16bit）

/* ========== 配置宏 ========== */

// 优化的 QSPI IO 配置
// 参数说明：
// - cs: 片选引脚
// - cb: 颜色传输完成回调（通常由 LVGL flush ready 使用）
// - cb_ctx: 回调上下文指针
#define CO5300_PANEL_IO_QSPI_CONFIG_OPTIMIZED(cs, cb, cb_ctx)          \
    {                                                                  \
        .cs_gpio_num = cs,                                             \
        .dc_gpio_num = -1,                                             \
        .spi_mode = 0,                                                 \
        .pclk_hz = CO5300_PANEL_OPTIMIZED_PCLK_HZ,                     \
        .trans_queue_depth = CO5300_PANEL_OPTIMIZED_TRANS_QUEUE_DEPTH, \
        .on_color_trans_done = cb,                                     \
        .user_ctx = cb_ctx,                                            \
        .lcd_cmd_bits = 32,                                            \
        .lcd_param_bits = 8,                                           \
        .flags = {                                                     \
            .quad_mode = true,                                         \
        },                                                             \
    }
