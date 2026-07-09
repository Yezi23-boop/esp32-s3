#ifndef WIFI_MANAGEMENT_CONTROLLER_H
#define WIFI_MANAGEMENT_CONTROLLER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "gui_guider.h"

/**
 * @brief 初始化 Wi-Fi 管理页控制器。
 * @param[in] ui 当前 UI 句柄。
 */
void wifi_management_controller_init(lv_ui *ui);

/**
 * @brief 打开 Wi-Fi 管理页。
 * @return 无返回值。
 */
void wifi_management_controller_open(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGEMENT_CONTROLLER_H
