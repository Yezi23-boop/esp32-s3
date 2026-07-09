#ifndef MAIN_DROPDOWN_CONTROLLER_H
#define MAIN_DROPDOWN_CONTROLLER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "gui_guider.h"

/**
 * @brief 绑定主界面下拉菜单控制器。
 * @param[in] ui 当前 UI 句柄。
 * @return 无返回值。
 */
void main_dropdown_controller_bind(lv_ui *ui);

/**
 * @brief 处理主界面蓝牙按钮点击。
 * @return 无返回值。
 */
void main_dropdown_controller_handle_bluetooth_click(void);

/**
 * @brief 处理主界面 Wi-Fi 按钮点击。
 * @return 无返回值。
 */
void main_dropdown_controller_handle_wifi_click(void);

#ifdef __cplusplus
}
#endif

#endif // MAIN_DROPDOWN_CONTROLLER_H
