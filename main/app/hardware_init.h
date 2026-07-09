#ifndef HARDWARE_INIT_H
#define HARDWARE_INIT_H

#include "esp_err.h"

/*
 * 硬件初始化入口：
 * - 负责把主工程依赖的基础板级能力按正确顺序拉起；
 * - 该层只关心“系统是否具备继续运行的基础条件”，不创建长期后台服务；
 * - 启动后更高层的 service 再分别推进网络、聊天和后台会话。
 */

/**
 * @brief 按主链路顺序初始化基础硬件能力。
 *
 * 当前初始化顺序依次覆盖：
 * - NVS
 * - 音频资源、通用资源 LittleFS 与 SD
 * - audio codec
 * - board power
 * - button
 *
 * @return `ESP_OK` 表示基础硬件初始化成功；
 *         其他错误表示关键基础能力初始化失败。
 *
 * @note 本函数不阻塞等待联网成功，也不启动 LVGL、AI 聊天或 Safety Monitor；
 *       这些运行期能力由 `app_main()` 后续阶段和对应 service 推进。
 */
esp_err_t hardware_init(void);

#endif // HARDWARE_INIT_H
