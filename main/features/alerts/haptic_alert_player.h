#ifndef HAPTIC_ALERT_PLAYER_H
#define HAPTIC_ALERT_PLAYER_H

#include "esp_err.h"

/*
 * 危险告警触觉提醒播放器：
 * - 拥有震动模式和异步执行；
 * - 不拥有 DS2413 硬件枚举、危险识别状态机或 UI 展示；
 * - 第一版只提供首次危险强震，持续提醒后续另行接入。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化触觉提醒播放器。
     *
     * @return ESP_OK 表示初始化成功或已初始化。
     */
    esp_err_t haptic_alert_player_init(void);

    /**
     * @brief 异步触发首次危险强震。
     *
     * 若已有震动任务在运行，则直接返回成功，避免同一次 Alerting 期间重复强震。
     *
     * @return ESP_OK 表示请求已接受、已去重或当前预算禁止震动；
     *         ESP_ERR_INVALID_STATE 表示模块未初始化；
     *         ESP_ERR_NO_MEM 表示无法创建震动任务。
     */
    esp_err_t haptic_alert_player_play_initial_danger_once(void);

#ifdef __cplusplus
}
#endif

#endif // HAPTIC_ALERT_PLAYER_H
