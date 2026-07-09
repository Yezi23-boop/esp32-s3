#ifndef MEMORY_WATCH_WS_CLIENT_H
#define MEMORY_WATCH_WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/memory_watch_voice_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_WATCH_WS_PATH "/v1/watch/ws"
#define MEMORY_WATCH_WS_TEXT_MAX_BYTES MEMORY_WATCH_VOICE_CLIENT_TEXT_MAX_BYTES
#define MEMORY_WATCH_WS_MESSAGE_ID_MAX_BYTES 64U
#define MEMORY_WATCH_WS_TYPE_MAX_BYTES 32U

    /**
     * @brief WebSocket 入站业务事件类型。
     *
     * 原始 JSON frame 名称只在 ws client 内部解析；service 只消费这些
     * turn-level 事件，避免把协议帧细节扩散到 owner task。
     */
    typedef enum
    {
        MEMORY_WATCH_WS_EVENT_UNKNOWN = 0,
        MEMORY_WATCH_WS_EVENT_TURN_ASR_READY,
        MEMORY_WATCH_WS_EVENT_TURN_REPLY_MESSAGE,
        MEMORY_WATCH_WS_EVENT_TURN_ERROR,
    } memory_watch_ws_event_kind_t;

    /**
     * @brief WebSocket 入站 JSON 事件。
     *
     * 文本字段只保存给 UI 和状态机需要的短文本；不得存放 token 或原始响应体。
     */
    typedef struct
    {
        memory_watch_ws_event_kind_t kind; /**< 已映射的业务事件类型。 */
        char type[MEMORY_WATCH_WS_TYPE_MAX_BYTES];
        char request_id[MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES];
        char message_id[MEMORY_WATCH_WS_MESSAGE_ID_MAX_BYTES];
        char role[MEMORY_WATCH_WS_TYPE_MAX_BYTES];
        char status[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
        char text[MEMORY_WATCH_WS_TEXT_MAX_BYTES];
        char error_code[MEMORY_WATCH_VOICE_CLIENT_ERROR_MAX_BYTES];
    } memory_watch_ws_event_t;

    typedef void (*memory_watch_ws_event_cb_t)(
        const memory_watch_ws_event_t *event,
        void *user_ctx);

    typedef void (*memory_watch_ws_disconnect_cb_t)(void *user_ctx);

    /**
     * @brief WebSocket client 运行配置。
     *
     * 复用 watch endpoint 的 base_url/device_id/device_token；device_token 只发给
     * `/v1/watch/ws` 的 auth JSON，不进入日志，也不是 Hermes/MiMo/API key。
     */
    typedef struct
    {
        memory_watch_voice_client_config_t endpoint;
        const char *last_seen_conversation_id;
        memory_watch_ws_event_cb_t event_cb;
        memory_watch_ws_disconnect_cb_t disconnect_cb;
        void *user_ctx;
    } memory_watch_ws_client_config_t;

    /**
     * @brief 建立 `/v1/watch/ws` 连接并完成 auth。
     *
     * 该函数同步执行连接握手，只允许 `memory_watch_service` worker task 调用；
     * UI 不得直接调用。
     */
    esp_err_t memory_watch_ws_client_connect(
        const memory_watch_ws_client_config_t *config);

    /**
     * @brief 发送 audio_start 控制帧。
     */
    esp_err_t memory_watch_ws_client_send_audio_start(
        const char *request_id);

    /**
     * @brief 发送一段 Ogg Opus binary frame。
     */
    esp_err_t memory_watch_ws_client_send_audio_chunk(
        const uint8_t *audio,
        size_t audio_len);

    /**
     * @brief 发送 audio_end 控制帧。
     */
    esp_err_t memory_watch_ws_client_send_audio_end(
        const char *request_id);

    /**
     * @brief 发送一个完整语音 turn：audio_start、binary chunks、audio_end。
     *
     * `memory_watch_service` 优先调用本接口，避免直接理解 WS 控制帧序列。
     */
    esp_err_t memory_watch_ws_client_send_audio_turn(
        const char *request_id,
        const uint8_t *audio,
        size_t audio_len,
        size_t chunk_size);

    /**
     * @brief 发送 ACK，更新 server 侧 last seen。
     */
    esp_err_t memory_watch_ws_client_send_ack(
        const char *message_id);

    /**
     * @brief 关闭当前 WebSocket 连接。
     */
    void memory_watch_ws_client_close(void);

    /**
     * @brief 当前 WS 是否处于连接态。
     */
    bool memory_watch_ws_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_WS_CLIENT_H
