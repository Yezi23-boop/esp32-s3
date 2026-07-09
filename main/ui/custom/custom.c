/*
 * Copyright 2023 NXP
 * NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
 * accordance with the applicable license terms. B                // 设置到底部状态栏位置
                lv_obj_set_tile(guider_ui.screen_tileview_1, guider_ui.screen_tileview_1_botBar, LV_ANIM_OFF);expressly accepting such terms or by downloading, installing,
 * activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
 * comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
 * terms, then you may not retain, install, activate or otherwise use the software.
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include "lvgl.h"
#include "custom.h"
/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

void custom_init(lv_ui *ui)
{
    (void)ui;
}
/*
 * @param e LVGL事件对象，包含事件相关信息
 * @param code 事件类型代码（如LV_EVENT_CLICKED、LV_EVENT_PRESSED等）
 * @param target 目标容器对象，事件将被转发到此容器
 */
// void pass_click_event_to_control_container(lv_event_t *e, lv_event_code_t code, lv_obj_t *target)
// {
//     lv_obj_t *child = lv_event_get_target(e); /* 获取触发事件的子控件对象 */
//     lv_event_send(target, code, child);       /* 将事件转发到指定的目标容器 */
// }