#pragma once

#include "esp_err.h"

/*
 * FT5x06/FT3168 触摸驱动接口：
 * - 当前驱动按单点轮询路径工作，供 LVGL 指针输入回调周期读取；
 * - 底层通过共享 I2C 总线访问触摸芯片，但对上层隐藏总线和寄存器细节；
 * - 接口设计目标是“读取当前点位”，而不是暴露完整手势或中断事件模型。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 FT5x06/FT3168 触摸控制器。
     *
     * 初始化会拉起共享 I2C 总线、配置复位脚并创建触摸设备句柄。
     *
     * @return `ESP_OK` 表示初始化成功或之前已初始化；
     *         其他错误表示 I2C、GPIO 或设备注册失败。
     */
    esp_err_t touch_ft5x06_init(void);

    /**
     * @brief 读取当前触摸点坐标。
     *
     * 当前实现优先服务单点 LVGL 指针输入场景，因此虽然保留数组接口，
     * 实际主要返回第一个有效触点。
     *
     * @param[out] x 输出 X 坐标数组。
     * @param[out] y 输出 Y 坐标数组。
     * @param[out] num_points 输出实际触摸点数量。
     * @param[in] max_points 调用方可接收的最大点数。
     * @return `ESP_OK` 表示读取流程完成；I2C 忙或临时读失败时也会退化为“无触摸”而非直接报错。
     */
    esp_err_t touch_ft5x06_read_points(uint16_t *x, uint16_t *y, uint8_t *num_points, uint8_t max_points);

    /**
     * @brief 获取内部触摸控制器句柄。
     * @param[out] out_handle 输出句柄指针。
     * @return `ESP_OK` 表示成功；
     *         `ESP_ERR_INVALID_ARG` 表示输出参数非法；
     *         `ESP_ERR_INVALID_STATE` 表示驱动尚未初始化。
     */
    esp_err_t touch_ft5x06_get_handle(void **out_handle);

/* 当前板级触摸链路固定使用 I2C0 和如下 GPIO 连接。 */
#define TOUCH_FT5X06_I2C_NUM I2C_NUM_0
#define TOUCH_FT5X06_SCL_GPIO 14
#define TOUCH_FT5X06_SDA_GPIO 15
#define TOUCH_FT5X06_INT_GPIO 38
#define TOUCH_FT5X06_RST_GPIO 9

#ifdef __cplusplus
}
#endif
