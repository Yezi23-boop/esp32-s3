#ifndef MUSIC_HTTP_CLIENT_H
#define MUSIC_HTTP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "music_protocol.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** music-service 请求配置；不包含 Hermes 或网易云私有密钥。 */
    typedef struct
    {
        char base_url[MUSIC_SERVICE_URL_MAX_BYTES];
        char device_id[MUSIC_SERVICE_DEVICE_ID_MAX_BYTES];
        char device_token[MUSIC_SERVICE_DEVICE_TOKEN_MAX_BYTES];
        uint32_t timeout_ms;
        bool allow_insecure_http;
    } music_http_client_config_t;

    /**
     * @brief 仅供 music_service owner 持有的控制面 HTTP/1.1 长连接。
     *
     * 媒体流由 music_stream_player reader 独占另一条 HTTP 连接，二者不共享 handle。
     */
    typedef struct
    {
        void *handle;
        char base_url[MUSIC_SERVICE_URL_MAX_BYTES];
        int64_t last_request_completed_us; /* 最近一次成功控制请求完成时间，单调时钟微秒。 */
    } music_http_control_client_t;

    /** music-service 会话接口返回的最小公共字段。 */
    typedef struct
    {
        int http_status;
        esp_err_t transport_error;
        music_service_state_t state;
        music_service_mode_t mode;
        uint32_t position_ms;
        char music_session_id[MUSIC_SERVICE_SESSION_ID_MAX_BYTES];
        char stream_id[MUSIC_SERVICE_STREAM_ID_MAX_BYTES];
        char source_id[MUSIC_SERVICE_SOURCE_ID_MAX_BYTES];
        char track_id[MUSIC_SERVICE_TRACK_ID_MAX_BYTES];
        char title[MUSIC_SERVICE_TITLE_MAX_BYTES];
        char artist[MUSIC_SERVICE_ARTIST_MAX_BYTES];
        char error_code[MUSIC_SERVICE_ERROR_MAX_BYTES];
    } music_http_session_result_t;

    /** 账户/二维码接口返回的窄状态；二维码位图由调用方提供缓冲区。 */
    typedef struct
    {
        int http_status;
        esp_err_t transport_error;
        music_service_account_state_t state;
        uint64_t expires_at_ms;
        uint16_t qr_size;
        size_t qr_bytes;
        uint8_t *qr_data;
        size_t qr_capacity;
        char login_id[MUSIC_SERVICE_QR_LOGIN_ID_MAX_BYTES];
        char error_code[MUSIC_SERVICE_ERROR_MAX_BYTES];
    } music_http_account_result_t;

    /**
     * @brief 创建一个 music-service 播放会话。
     *
     * 首版只要求服务端支持固定测试流；真实网易云来源仍由服务器决定。
     */
    esp_err_t music_http_client_create_session(
        music_http_control_client_t *control,
        const music_http_client_config_t *config, const char *source_id,
        const char *track_id, const char *command_id,
        music_http_session_result_t *out_result);

    /** @brief 创建会话并向服务端声明指定的选曲模式。 */
    esp_err_t music_http_client_create_session_mode(
        music_http_control_client_t *control,
        const music_http_client_config_t *config, const char *source_id,
        const char *track_id, const char *mode, const char *command_id,
        music_http_session_result_t *out_result);

    /** @brief 读取一个来源的分页歌曲摘要。 */
    esp_err_t music_http_client_fetch_tracks(
        music_http_control_client_t *control,
        const music_http_client_config_t *config, const char *source_id,
        uint32_t offset, music_service_catalog_snapshot_t *out_catalog);

    /** @brief 获取当前网易云账户状态。 */
    esp_err_t music_http_client_get_account(
        const music_http_client_config_t *config,
        music_http_account_result_t *out_result);

    /** @brief 创建二维码登录会话并返回模块位图。 */
    esp_err_t music_http_client_create_qr(
        const music_http_client_config_t *config,
        music_http_account_result_t *out_result);

    /** @brief 轮询二维码登录状态。 */
    esp_err_t music_http_client_poll_qr(
        const music_http_client_config_t *config, const char *login_id,
        music_http_account_result_t *out_result);

    /**
     * @brief 调用 pause/resume/previous/next 或 destroy 会话动作。
     *
     * `action` 只接受服务器契约中的窄白名单，不把通用上游 API 暴露到手表。
     */
    esp_err_t music_http_client_session_command(
        music_http_control_client_t *control,
        const music_http_client_config_t *config, const char *session_id,
        const char *action, const char *mode, const char *command_id,
        music_http_session_result_t *out_result);

    /**
     * @brief 领取一个等待中的 Hermes/远程音乐命令。
     *
     * 该接口只由 `music_service` owner task 调用；没有命令时返回 `ESP_OK`
     * 且 `out_command->available` 为 false。
     */
    esp_err_t music_http_client_poll_remote_command(
        music_http_control_client_t *control,
        const music_http_client_config_t *config,
        music_service_remote_command_t *out_command);

    /**
     * @brief 回传远程命令执行结果和窄音乐快照。
     *
     * `state` 通常为 `executed` 或 `error`；长期设备 token 只放在已有
     * Authorization 头，快照不包含 stream URL 或上游播放地址。
     */
    esp_err_t music_http_client_ack_remote_command(
        music_http_control_client_t *control,
        const music_http_client_config_t *config, const char *command_id,
        const char *state, const char *error_code,
        const music_service_snapshot_t *snapshot);

    /** @brief 关闭控制长连接，在销毁音乐会话时回收 TLS/socket 资源。 */
    void music_http_client_control_reset(music_http_control_client_t *control);

    typedef struct music_http_stream music_http_stream_t;

    /**
     * @brief 打开只携带短时 stream_id capability 的裸 Opus 媒体流。
     *
     * 媒体请求不发送长期 device_token；服务端仍以 device_id 和 capability
     * 归属校验访问权限。
     */
    esp_err_t music_http_client_open_stream(
        const music_http_client_config_t *config, const char *stream_id,
        music_http_stream_t **out_stream);

    /**
     * @brief 读取媒体字节。
     *
     * ESP_ERR_NOT_FOUND 表示正常 EOF，ESP_ERR_HTTP_EAGAIN 表示本次媒体
     * 读取在短超时内没有数据；reader 应继续等待而不是结束播放。
     */
    esp_err_t music_http_client_read_stream(music_http_stream_t *stream,
                                            uint8_t *buffer, size_t capacity,
                                            size_t *out_bytes);

    /** @brief 关闭并释放仅由 reader task 拥有的 HTTPS 媒体连接。 */
    void music_http_client_close_stream(music_http_stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_HTTP_CLIENT_H */
