/**
 * @file audio_app.h
 * @brief 音频应用层接口 (录音/播放控制)
 */

#ifndef AUDIO_APP_H
#define AUDIO_APP_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化音频应用层录音控制入口。
     *
     * 当前实现主要保留统一初始化入口，便于在应用层集中追加录音目录检查、
     * 运行时参数准备或资源探测逻辑，而不把这些工作散落到 UI 事件处理里。
     *
     * @return `ESP_OK` 表示初始化成功。
     */
    esp_err_t audio_app_init(void);

    /**
     * @brief 启动一次新的录音任务。
     *
     * 该接口只负责创建后台录音任务并缓存输出路径，不会阻塞等待录音结束。
     *
     * @param[in] filename 录音文件保存路径，例如 `"/sdcard/record.wav"`。
     * @return `ESP_OK` 表示任务创建成功；
     *         `ESP_ERR_INVALID_STATE` 表示当前已有录音任务在运行；
     *         `ESP_ERR_INVALID_ARG` 表示参数非法；
     *         其他错误表示任务创建失败。
     *
     * @note 仅允许在任务上下文调用；函数会修改全局录音状态和输出路径缓存。
     */
    esp_err_t audio_app_start_record(const char *filename);

    /**
     * @brief 请求停止当前录音。
     *
     * 当前实现通过清除全局录音标志，让后台录音任务自然退出并回填 WAV 头，
     * 以避免在文件写入中途强行销毁任务导致文件损坏。
     *
     * @return `ESP_OK` 表示停止请求已接受；当前未录音时也返回 `ESP_OK`。
     */
    esp_err_t audio_app_stop_record(void);

    /**
     * @brief 查询录音任务是否仍在运行。
     * @return true 表示后台录音任务仍持有录音循环。
     */
    bool audio_app_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_APP_H
