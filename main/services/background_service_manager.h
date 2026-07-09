#ifndef BACKGROUND_SERVICE_MANAGER_H
#define BACKGROUND_SERVICE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/power_policy.h"

/*
 * 后台功能开关层：
 * - 区分“用户允许后台运行”和“当前资源策略允许运行”；
 * - 第一阶段只管理危险识别后台能力；
 * - 不直接抢麦克风、不改模型阈值、不直接操作 LVGL。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Safety Monitor 后台目标被阻塞的原因。
     *
     * 该枚举只解释后台管理器为什么没有把 Safety Monitor 目标态设为运行；
     * 启动失败、停止失败等 runtime 异常仍通过 last_error 单独表达。
     */
    typedef enum
    {
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_NONE = 0, /**< 没有阻塞，目标态应运行。 */
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_MANAGER_NOT_READY, /**< 管理器任务尚未接管。 */
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_USER_DISABLED, /**< 用户未开启安全监听。 */
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_POLICY, /**< power_policy 当前不允许运行。 */
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_AUDIO, /**< 前台音频占用麦克风。 */
        BACKGROUND_SERVICE_MANAGER_DANGER_BLOCK_FOREGROUND_RUNTIME, /**< 强前台重任务要求 Safety Monitor 让路。 */
    } background_service_manager_danger_block_reason_t;

    /** 后台服务管理器对外快照。 */
    typedef struct
    {
        bool started;                    /**< 管理器后台任务是否已启动。 */
        bool danger_enabled_by_user;     /**< 用户是否允许危险识别后台运行。 */
        bool danger_allowed_by_policy;   /**< 当前 power_policy 是否允许危险识别运行。 */
        bool danger_should_run;          /**< 管理器合成后的 Safety Monitor 目标态。 */
        bool danger_runtime_running;     /**< Safety Monitor session 最近一次确认的运行状态。 */
        background_service_manager_danger_block_reason_t danger_block_reason; /**< 目标态未运行的主原因。 */
        bool danger_blocked_by_foreground_audio; /**< 前台录音/语音是否正在占用麦克风。 */
        bool danger_blocked_by_foreground_runtime; /**< Hermes/BLE/OTA 等强前台是否要求让路。 */
        power_policy_state_t policy_state; /**< 最近一次使用的整机策略状态。 */
        uint32_t policy_flags;          /**< 最近一次预算 flag，使用 power_policy_flag_t 位图。 */
        esp_err_t last_error;            /**< 最近一次 session 启动、停止或恢复错误码。 */
    } background_service_manager_snapshot_t;

    /**
     * @brief 初始化后台服务管理器。
     * @return ESP_OK 表示初始化成功或已初始化。
     */
    esp_err_t background_service_manager_init(void);

    /**
     * @brief 启动后台服务管理器。
     *
     * 第一阶段会按用户开关和 power_policy 预算启动危险识别后台监听。
     *
     * @return ESP_OK 表示任务已启动或之前已启动。
     */
    esp_err_t background_service_manager_start(void);

    /**
     * @brief 设置危险识别后台功能用户开关。
     * @param[in] enabled true 表示用户允许后台危险识别运行。
     * @return ESP_OK 表示设置成功；其他错误表示底层启动或停止失败。
     */
    esp_err_t background_service_manager_set_danger_detection_enabled(
        bool enabled);

    /**
     * @brief 通知后台管理器前台音频会话是否正在占用麦克风。
     *
     * 前台录音/语音属于比 Safety Monitor 更高优先级的临时会话。该标志为 true
     * 时，后台管理器会暂停危险识别 runtime；恢复为 false 后，再按用户开关和
     * power_policy 预算决定是否重启。
     *
     * @param[in] active true 表示前台音频正在占用麦克风。
     * @param[in] reason 日志原因，可为 NULL。
     * @return ESP_OK 表示状态已记录且策略已同步；其他错误表示停止/恢复失败。
     */
    esp_err_t background_service_manager_set_foreground_audio_active(
        bool active, const char *reason);

    /**
     * @brief 通知后台管理器强前台 runtime gate 可能已经变化。
     *
     * Hermes 页面、BLE/OTA 等强前台 owner acquire/release 后调用本函数；
     * manager task 会重新读取 `foreground_runtime_gate` 快照并合成 Safety Monitor
     * 目标态。gate 本身仍不直接 stop ESP-DL。
     *
     * @return ESP_OK 表示通知已发送或管理器尚未启动无需处理。
     */
    esp_err_t background_service_manager_notify_foreground_runtime_changed(void);

    /**
     * @brief 通知后台管理器 power_policy 预算可能已经变化。
     *
     * 该接口只唤醒后台管理器 task 重新读取 `power_policy_get_budget()`；
     * 不携带最终预算，也不让 power_policy 直接启动或停止 Safety Monitor。
     *
     * @return ESP_OK 表示通知已发送或管理器尚未启动无需处理。
     */
    esp_err_t background_service_manager_notify_policy_changed(void);

    /**
     * @brief 获取后台服务管理器快照。
     * @return 当前后台功能开关、策略许可和最近错误。
     */
    background_service_manager_snapshot_t
    background_service_manager_get_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif // BACKGROUND_SERVICE_MANAGER_H
