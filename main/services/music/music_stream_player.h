#ifndef MUSIC_STREAM_PLAYER_H
#define MUSIC_STREAM_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "music_http_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct music_stream_player music_stream_player_t;

    /** 播放器发给 music service owner 的生命周期事件。 */
    typedef enum
    {
        MUSIC_STREAM_PLAYER_EVENT_BUFFERING = 0,
        MUSIC_STREAM_PLAYER_EVENT_PLAYING,
        MUSIC_STREAM_PLAYER_EVENT_ENDED,
        MUSIC_STREAM_PLAYER_EVENT_STOPPED,
        MUSIC_STREAM_PLAYER_EVENT_ERROR,
    } music_stream_player_event_type_t;

    typedef struct
    {
        music_stream_player_event_type_t type;
        esp_err_t error;
        size_t buffered_bytes;
    } music_stream_player_event_t;

    /**
     * @brief 创建并启动长度前缀裸 Opus 的双任务 HTTP 流播放器。
     *
     * reader 与 decoder 分别独占 HTTPS 和音频 decoder，music_service 只拥有
     * 生命周期与事件队列，避免 UI 或 service task 被网络读取阻塞。
     */
    esp_err_t music_stream_player_start(
        const music_http_client_config_t *config, const char *stream_id,
        QueueHandle_t event_queue, music_stream_player_t **out_player);

    /**
     * @brief 请求停止并等待 reader 与 decoder 在释放共享 PSRAM 前退出。
     * @param[in] player 播放 worker。
     * @param[in] timeout_ms 最长等待时间；调用方使用 6000 ms 保持 service 既有边界。
     * @return ESP_OK 表示 HTTP、decoder 与音频输出 owner 已释放。
     */
    esp_err_t music_stream_player_stop(music_stream_player_t *player,
                                       uint32_t timeout_ms);

    /** @brief 判断 worker 是否已经完成停止收敛。 */
    bool music_stream_player_is_stopped(const music_stream_player_t *player);

    /**
     * @brief 释放已停止的 worker 控制块。
     *
     * 调用方正常应先停止或收到终态事件；析构会再次收敛两个 worker，避免
     * 释放仍被任务使用的 PSRAM ring。
     */
    void music_stream_player_release(music_stream_player_t *player);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_STREAM_PLAYER_H */
