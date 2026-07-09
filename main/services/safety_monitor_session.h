#ifndef SAFETY_MONITOR_SESSION_H
#define SAFETY_MONITOR_SESSION_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * Safety Monitor 后台会话层：
 * - 收敛危险识别后台 session 的 start/stop、错误恢复、运行确认和失败退避；
 * - 调用方只表达“现在应不应该运行”，不直接理解危险识别 runtime 的内部状态；
 * - 不直接解释 power_policy，也不直接操作 LVGL。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /** Safety Monitor session 对外快照。 */
    typedef struct
    {
        bool runtime_running; /**< 最近一次确认的危险识别 runtime 运行状态。 */
        esp_err_t last_error; /**< 最近一次 start/stop/recover 错误码。 */
    } safety_monitor_session_snapshot_t;

    /**
     * @brief 初始化 Safety Monitor session。
     * @return ESP_OK 表示初始化成功或已初始化。
     */
    esp_err_t safety_monitor_session_init(void);

    /**
     * @brief 按目标状态同步 Safety Monitor session。
     *
     * @param[in] should_run true 表示当前应保持后台危险识别运行。
     * @param[in] reason 调用原因，允许为 NULL，仅用于日志。
     * @return ESP_OK 表示已达到目标状态；其他错误表示启动、恢复或停止失败。
     */
    esp_err_t safety_monitor_session_apply(bool should_run,
                                           const char *reason);

    /**
     * @brief 获取 Safety Monitor session 快照。
     * @return 最近一次确认的 runtime 状态和错误码。
     */
    safety_monitor_session_snapshot_t safety_monitor_session_get_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_MONITOR_SESSION_H
