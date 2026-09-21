#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/ota/ota_update_plan.h"
#include "services/ota/ota_transport.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 独立 OTA 维护会话状态。
     *
     * 阶段 1/2 只推进到 READY，不创建网络连接，也不写入 Flash；后续阶段
     * 复用相同状态快照承载 manifest、下载和回滚状态。
     */
    typedef enum
    {
        OTA_SERVICE_STATE_IDLE = 0,
        OTA_SERVICE_STATE_PREPARING,
        OTA_SERVICE_STATE_QUIESCING,
        OTA_SERVICE_STATE_QUIESCED,
        OTA_SERVICE_STATE_READY,
        OTA_SERVICE_STATE_NO_UPDATE,
        OTA_SERVICE_STATE_DOWNLOADING,
        OTA_SERVICE_STATE_STAGED,
        OTA_SERVICE_STATE_VERIFYING,
        OTA_SERVICE_STATE_RESTARTING,
        OTA_SERVICE_STATE_FAILED,
        OTA_SERVICE_STATE_PENDING_VERIFY,
        OTA_SERVICE_STATE_VALID,
        OTA_SERVICE_STATE_ROLLED_BACK,
    } ota_service_state_t;

    /** OTA service 只读快照，UI 只能读取副本。 */
    typedef struct
    {
        ota_service_state_t state;
        uint32_t generation;
        size_t bytes_received;
        size_t image_size;
        uint8_t progress_percent;
        esp_err_t last_error;
        bool task_started;
        bool maintenance_active;
        ota_update_source_t source;
        bool update_available;
        bool delta_available;
        char target_version[OTA_UPDATE_VERSION_MAX];
    } ota_service_snapshot_t;

    /**
     * @brief 初始化并创建 OTA owner task。
     *
     * task 会先等待真实的 `ui_first_frame_ready`，避免在显示基础设施未完成
     * 时抢占前台资源。task 栈位于 internal RAM，以满足 Flash 写入时的 cache
     * 冻结约束；服务本身只编排状态和 owner API。
     */
    esp_err_t ota_service_start(void);

    /**
     * @brief 请求进入 OTA 维护准备阶段。
     *
     * 请求只入队，不在 UI 线程申请 gate、等待 ACK 或执行 I/O。
     */
    esp_err_t ota_service_request_prepare(void);

    /**
     * @brief 按编译期选择的 provider 检查更新。
     *
     * 检查只访问 provider，不申请前台 owner，也不写入 Flash。
     */
    esp_err_t ota_service_request_check(void);

    /**
     * @brief 用户确认后开始维护并下载、校验备用槽。
     *
     * 成功后状态为 `STAGED`，此接口不更新 `otadata`。
     * @return ESP_OK 表示命令已入队。
     */
    esp_err_t ota_service_request_download(void);

    /**
     * @brief 激活已完成校验的备用槽并立即重启。
     *
     * 仅 `STAGED` 状态可调用；这是唯一会更新启动选择的 OTA service 接口。
     * @return ESP_OK 表示命令已入队。
     */
    esp_err_t ota_service_request_activate(void);

    /**
     * @brief 设置开发期下载故障注入点。
     *
     * 仅允许在 IDLE 且尚未进入维护时设置；产品默认值为 NONE。
     */
    esp_err_t ota_service_set_fault_mode(ota_transport_fault_mode_t mode);

    /**
     * @brief 设置 finish 成功到 restart 之间的测试保持时间。
     *
     * 默认 0；仅用于电源复位窗口故障测试，单位毫秒。
     */
    esp_err_t ota_service_set_restart_hold_ms(uint32_t hold_ms);

    /**
     * @brief 请求取消当前维护会话。
     *
     * service task 收到后负责反向完成 quiesce 并释放 OTA gate。
     */
    esp_err_t ota_service_request_cancel(void);

    /**
     * @brief 获取 OTA 状态快照。
     * @param[out] out_snapshot 输出副本，不能为 NULL。
     * @return ESP_OK 表示成功。
     */
    esp_err_t ota_service_get_snapshot(ota_service_snapshot_t *out_snapshot);

    /**
     * @brief 将状态枚举转换为日志/UI 文本。
     * @param[in] state OTA 状态。
     * @return 静态字符串，调用方不得释放。
     */
    const char *ota_service_state_text(ota_service_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */
