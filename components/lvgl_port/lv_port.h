
/**
 * @file lv_port.h
 * @brief LVGL 移植层对外接口
 * @details
 * - 该头文件只暴露端口层初始化入口，隐藏显示/触摸/tick 的内部状态。
 * - 适合上层应用在启动阶段一次性调用。
 */

#ifndef _LV_PORT_H_
#define _LV_PORT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief 初始化 LVGL 端口层。
 *
 * 调用顺序固定为：
 * 1. `lv_init`
 * 2. 面板初始化
 * 3. 触摸初始化
 * 4. 显示缓冲注册
 * 5. 输入设备注册
 * 6. tick 定时器启动
 *
 * @return 无返回值。
 *
 * @note 该入口应只在系统启动阶段调用一次；重复初始化会放大底层显示和输入资源竞争风险。
 */
void lv_port_init_small(void);

#endif
