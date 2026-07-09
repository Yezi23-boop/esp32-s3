/*
 * Copyright 2023 NXP
 * NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
 * accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
 * activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
 * comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
 * terms, then you may not retain, install, activate or otherwise use the software.
 */

#ifndef __CUSTOM_H_
#define __CUSTOM_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include "gui_guider.h"
#include "ai_ui_controller.h"
#include "clock_functions.h"  // 时钟功能模块
#include "emissive_calendar_view.h"
#include "main_dropdown_controller.h"
#include "memory_watch_controller.h"
#include "wifi_management_controller.h"
#include "ui_font_assets.h"
#include "ui_chinese_fonts.h"
#include "scroll_functions.h" // 滚动功能模块
                              //   LV_IMG_DECLARE(_20221103102551_80994_240x280);
    /**
     * 自定义初始化函数
     *
     * @param ui 用户界面对象
     *
     * 功能说明：
     * - 初始化所有自定义功能模块
     * - 配置界面滚动效果
     * - 设置事件处理
     */
    void custom_init(lv_ui *ui);

    /**
     * 传递点击事件到控制容器
     *
     * @param e LVGL事件对象
     * @param code 事件代码
     * @param target 目标对象
     *
     * 功能说明：
     * - 事件转发机制
     * - 支持事件冒泡
     * - 提高代码复用性
    //  */
    // void pass_click_event_to_control_container(lv_event_t *e, lv_event_code_t code, lv_obj_t *target);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_H_ */
