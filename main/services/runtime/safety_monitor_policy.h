#ifndef SAFETY_MONITOR_POLICY_H
#define SAFETY_MONITOR_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/power/power_policy.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        SAFETY_MONITOR_POLICY_BLOCK_NONE = 0,
        SAFETY_MONITOR_POLICY_BLOCK_NOT_READY,
        SAFETY_MONITOR_POLICY_BLOCK_USER_DISABLED,
        SAFETY_MONITOR_POLICY_BLOCK_POWER,
        SAFETY_MONITOR_POLICY_BLOCK_FOREGROUND_AUDIO,
        SAFETY_MONITOR_POLICY_BLOCK_RUNTIME_COORDINATOR,
        SAFETY_MONITOR_POLICY_BLOCK_MUSIC_PLAYBACK,
    } safety_monitor_policy_block_reason_t;

    /** Safety Monitor 用户意图、策略许可和真实运行态快照。 */
    typedef struct
    {
        bool started;
        bool enabled_by_user;
        bool allowed_by_power_policy;
        bool should_run;
        safety_monitor_policy_block_reason_t block_reason;
        bool music_playback_active;
        bool blocked_by_runtime_coordinator;
        power_policy_state_t policy_state;
        uint32_t policy_flags;
    } safety_monitor_policy_snapshot_t;

    esp_err_t safety_monitor_policy_init(void);
    esp_err_t safety_monitor_policy_start(void);
    esp_err_t safety_monitor_policy_set_enabled(bool enabled);
    esp_err_t safety_monitor_policy_set_foreground_audio_active(
        bool active, const char *reason);
    /**
     * @brief 设置音乐播放期间的安全告警策略状态。
     *
     * 音乐是后台播放 owner；active 时由安全策略停止危险检测运行时，且不缓存
     * 或补发音乐期间产生的告警。音乐 service 只报告状态，不直接操作安全检测。
     *
     * @param[in] active true 表示音乐正在输出；false 表示音乐已停止或暂停。
     * @param[in] reason 仅用于诊断日志，可为 NULL。
     * @return `ESP_OK` 表示状态已接受。
     */
    esp_err_t safety_monitor_policy_set_music_active(bool active,
                                                      const char *reason);
    /**
     * @brief 注册 Safety Monitor 为省电 facts+consumer 参与者。
     *
     * 必须在 `power_policy_start()` 之前调用（app_main 的 core policy 组装
     * 阶段）；只登记事实回调和预算变更唤醒，不启动 Safety 运行。
     *
     * @return `ESP_OK` 表示注册成功。
     */
    esp_err_t safety_monitor_policy_register_power_participant(void);
    safety_monitor_policy_snapshot_t safety_monitor_policy_get_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_MONITOR_POLICY_H */
