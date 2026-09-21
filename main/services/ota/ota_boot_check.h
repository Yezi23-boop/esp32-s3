#ifndef OTA_BOOT_CHECK_H
#define OTA_BOOT_CHECK_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 在后台服务启动前确认 PENDING_VERIFY 镜像。
     *
     * 只执行有界的本地启动链检查：running partition、资源文件系统和基础
     * 应用描述可读；不等待 UI、网络、云端或 OTA task。
     *
     * @return ESP_OK 表示无需确认或已成功标记 valid；失败路径会请求 rollback
     *         并重启，若 SDK 拒绝则返回错误。
     */
    esp_err_t ota_boot_check_run(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_BOOT_CHECK_H */
