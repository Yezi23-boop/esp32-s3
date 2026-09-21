#pragma once

#include "gui_guider.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 使用后台 owner 的只读快照刷新主屏运行时信息。
 *
 * 该接口只在 LVGL 线程调用，不推进电源或天气状态，也不执行网络请求。
 *
 * @param ui GUI Guider 主界面对象。
 */
void main_screen_runtime_refresh(lv_ui *ui);

#ifdef __cplusplus
}
#endif
