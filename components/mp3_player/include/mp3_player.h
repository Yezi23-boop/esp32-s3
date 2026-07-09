#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include "esp_err.h"
#include "audio_player.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化 MP3 播放器。
     *
     * @return `ESP_OK` 表示成功；
     *         其他错误表示底层 `audio_player` 创建或回调注册失败。
     *
     * @note 必须在 `audio_codec_init()` 之后调用。
     */
    esp_err_t mp3_player_init(void);

    /**
     * @brief 播放音频文件，支持 MP3 和 WAV。
     *
     * `audio_player` 会自动识别文件格式，并在成功后接管文件句柄生命周期。
     *
     * @param[in] file_path 文件路径，例如 `"/spiffs/music.mp3"`。
     * @return `ESP_OK` 表示成功；
     *         其他错误表示路径非法、打开文件失败或底层播放启动失败。
     */
    esp_err_t mp3_player_play_file(const char *file_path);

    /**
     * @brief 暂停播放。
     * @return 底层 `audio_player_pause()` 返回值。
     */
    esp_err_t mp3_player_pause(void);

    /**
     * @brief 恢复播放。
     * @return 底层 `audio_player_resume()` 返回值。
     */
    esp_err_t mp3_player_resume(void);

    /**
     * @brief 停止播放。
     * @return 底层 `audio_player_stop()` 返回值。
     */
    esp_err_t mp3_player_stop(void);

    /**
     * @brief 反初始化 MP3 播放器。
     * @return 底层 `audio_player_delete()` 返回值。
     */
    esp_err_t mp3_player_deinit(void);

    /**
     * @brief 获取播放器状态。
     * @return 当前 `audio_player` 状态枚举。
     */
    audio_player_state_t mp3_player_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // MP3_PLAYER_H
