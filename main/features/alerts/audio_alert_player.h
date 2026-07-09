#ifndef AUDIO_ALERT_PLAYER_H
#define AUDIO_ALERT_PLAYER_H

#include "esp_err.h"

/*
 * 告警提示音播放器：
 * - 封装一段固定 PCM 资源的单次播放；
 * - 设计目标是“非阻塞触发”，由内部任务完成实际输出；
 * - 若已有播放进行中，重复触发会直接复用当前播放而不是叠加。
 */

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化告警提示音播放器。
     *
     * 当前实现主要用于建立统一入口，便于后续在这里扩展资源装载或设备就绪检查。
     *
     * @return `ESP_OK` 表示初始化成功。
     */
    esp_err_t audio_alert_player_init(void);

    /**
     * @brief 异步触发一次危险提示音播放。
     *
     * 若已有播放任务在运行，则该接口直接返回成功而不叠加第二个播放任务，
     * 以避免提示音在危险状态抖动时反复重入。
     *
     * @return `ESP_OK` 表示播放请求已接受或已有播放正在进行；
     *         `ESP_ERR_INVALID_STATE` 表示模块未初始化；
     *         `ESP_ERR_NO_MEM` 表示无法创建播放任务。
     */
    esp_err_t audio_alert_player_play_warning_once(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_ALERT_PLAYER_H
