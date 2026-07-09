#ifndef SLEEP_COORDINATOR_H
#define SLEEP_COORDINATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/power_policy.h"

/*
 * sleep_coordinator 只做 sleep 预算 dry-run 观测。
 * 当前手动 Light-sleep 测试路径已移除；后续若接入 ESP-IDF Automatic
 * Light-sleep，应新建独立系统 PM owner，而不是在这里恢复手动测试逻辑。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /** sleep_coordinator 执行模式。 */
    typedef enum
    {
        SLEEP_COORDINATOR_MODE_DISABLED = 0, /**< 禁用 dry-run 采样、日志和计数。 */
        SLEEP_COORDINATOR_MODE_DRY_RUN,      /**< 默认模式，只记录 readiness。 */
    } sleep_coordinator_mode_t;

    /** sleep_coordinator 最近一次只读快照。 */
    typedef struct
    {
        bool initialized;              /**< 是否已初始化。 */
        bool started;                  /**< dry-run 任务是否已启动。 */
        sleep_coordinator_mode_t mode; /**< 当前执行模式。 */
        power_policy_sleep_permission_t sleep_permission; /**< 最近预算许可。 */
        uint32_t sleep_blockers;       /**< 最近 blocker 位图。 */
        uint32_t sleep_interval_hint_ms; /**< 最近 interval hint。 */
        uint32_t budget_version;       /**< 最近观测到的 power_budget 版本。 */
        uint32_t dry_run_count;        /**< 已记录 dry-run 次数。 */
    } sleep_coordinator_snapshot_t;

    /**
     * @brief 初始化 sleep_coordinator。
     * @return ESP_OK 表示初始化成功或已经初始化。
     */
    esp_err_t sleep_coordinator_init(void);

    /**
     * @brief 启动默认 dry-run 后台任务。
     * @return ESP_OK 表示任务已启动或已经启动。
     */
    esp_err_t sleep_coordinator_start(void);

    /**
     * @brief 设置执行模式。
     *
     * 当前只允许 `DISABLED` 和 `DRY_RUN`。该接口保留给调试或后续系统 PM
     * owner 接入前的开关控制，不进入真实 ESP sleep。
     *
     * @param[in] mode 目标模式。
     * @return ESP_OK 表示模式已记录。
     */
    esp_err_t sleep_coordinator_set_mode(sleep_coordinator_mode_t mode);

    /**
     * @brief 读取最近一次 dry-run 快照。
     * @return 当前 sleep_coordinator 快照副本。
     */
    sleep_coordinator_snapshot_t sleep_coordinator_get_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif // SLEEP_COORDINATOR_H
