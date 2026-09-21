#ifndef MUSIC_PROTOCOL_H
#define MUSIC_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MUSIC_SERVICE_URL_MAX_BYTES 128U
#define MUSIC_SERVICE_DEVICE_ID_MAX_BYTES 32U
#define MUSIC_SERVICE_DEVICE_TOKEN_MAX_BYTES 128U
#define MUSIC_SERVICE_SESSION_ID_MAX_BYTES 96U
#define MUSIC_SERVICE_STREAM_ID_MAX_BYTES 96U
#define MUSIC_SERVICE_SOURCE_ID_MAX_BYTES 64U
#define MUSIC_SERVICE_TRACK_ID_MAX_BYTES 96U
#define MUSIC_SERVICE_TITLE_MAX_BYTES 128U
#define MUSIC_SERVICE_ARTIST_MAX_BYTES 96U
#define MUSIC_SERVICE_ERROR_MAX_BYTES 64U
#define MUSIC_SERVICE_REMOTE_COMMAND_ID_MAX_BYTES 96U
#define MUSIC_SERVICE_REMOTE_ACTION_MAX_BYTES 32U
#define MUSIC_SERVICE_RING_BYTES (512U * 1024U)
/* 128 kbps 压缩音频约 32 秒缓存；4 KB 起播门槛保持快速响应。 */
#define MUSIC_SERVICE_START_BUFFER_BYTES (4U * 1024U)
#define MUSIC_SERVICE_CATALOG_PAGE_SIZE 10U
#define MUSIC_SERVICE_QR_LOGIN_ID_MAX_BYTES 96U
#define MUSIC_SERVICE_QR_MAX_BYTES 4096U

    /** 在线音乐服务状态；只描述 ESP32 播放控制面的状态。 */
    typedef enum
    {
        MUSIC_SERVICE_STATE_STOPPED = 0,
        MUSIC_SERVICE_STATE_BUFFERING,
        MUSIC_SERVICE_STATE_PLAYING,
        MUSIC_SERVICE_STATE_PAUSED,
        MUSIC_SERVICE_STATE_ERROR,
    } music_service_state_t;

    /** 服务器选曲模式。 */
    typedef enum
    {
        MUSIC_SERVICE_MODE_REPEAT_ONE = 0,
        MUSIC_SERVICE_MODE_REPEAT_ALL,
        MUSIC_SERVICE_MODE_SHUFFLE,
        MUSIC_SERVICE_MODE_ORDER,
        MUSIC_SERVICE_MODE_SMART,
    } music_service_mode_t;

    /**
     * @brief 服务端下发给 music_service owner 的窄命令。
     *
     * 字段只覆盖现有播放控制所需的元数据；媒体 URL、Cookie 和 MCP token
     * 永远不会进入设备命令。`has_volume`/`has_mode` 区分缺省值与显式设置。
     */
    typedef struct
    {
        bool available;
        bool has_mode;
        bool has_volume;
        int volume;
        uint64_t expires_at_ms;
        music_service_mode_t mode;
        char command_id[MUSIC_SERVICE_REMOTE_COMMAND_ID_MAX_BYTES];
        char action[MUSIC_SERVICE_REMOTE_ACTION_MAX_BYTES];
        char source_id[MUSIC_SERVICE_SOURCE_ID_MAX_BYTES];
        char track_id[MUSIC_SERVICE_TRACK_ID_MAX_BYTES];
    } music_service_remote_command_t;

    /** 音乐 service 对 UI 发布的只读快照。 */
    typedef struct
    {
        music_service_state_t state;
        music_service_mode_t mode;
        bool music_active;
        bool endpoint_configured;
        int volume; /**< 当前系统扬声器音量，0~100；由 audio_codec owner 维护。 */
        uint32_t position_ms;
        size_t buffered_bytes;
        char music_session_id[MUSIC_SERVICE_SESSION_ID_MAX_BYTES];
        char stream_id[MUSIC_SERVICE_STREAM_ID_MAX_BYTES];
        char source_id[MUSIC_SERVICE_SOURCE_ID_MAX_BYTES];
        char track_id[MUSIC_SERVICE_TRACK_ID_MAX_BYTES];
        char title[MUSIC_SERVICE_TITLE_MAX_BYTES];
        char artist[MUSIC_SERVICE_ARTIST_MAX_BYTES];
        char error_code[MUSIC_SERVICE_ERROR_MAX_BYTES];
    } music_service_snapshot_t;

    /** 服务端返回的单首歌曲摘要；不包含播放 URL。 */
    typedef struct
    {
        char track_id[MUSIC_SERVICE_TRACK_ID_MAX_BYTES];
        char title[MUSIC_SERVICE_TITLE_MAX_BYTES];
        char artist[MUSIC_SERVICE_ARTIST_MAX_BYTES];
    } music_service_catalog_track_t;

    /** 音乐来源分页快照；由 service owner 更新，UI 只读取副本。 */
    typedef struct
    {
        bool valid;
        bool loading;
        uint32_t generation;
        uint32_t offset;
        uint32_t total;
        size_t track_count;
        char source_id[MUSIC_SERVICE_SOURCE_ID_MAX_BYTES];
        music_service_catalog_track_t tracks[MUSIC_SERVICE_CATALOG_PAGE_SIZE];
    } music_service_catalog_snapshot_t;

    /** 网易云登录状态；二维码数据只在登录页前台短期存在。 */
    typedef enum
    {
        MUSIC_SERVICE_ACCOUNT_UNKNOWN = 0,
        MUSIC_SERVICE_ACCOUNT_LOGGED_OUT,
        MUSIC_SERVICE_ACCOUNT_QR_PENDING,
        MUSIC_SERVICE_ACCOUNT_QR_CONFIRMING,
        MUSIC_SERVICE_ACCOUNT_LOGGED_IN,
        MUSIC_SERVICE_ACCOUNT_EXPIRED,
        MUSIC_SERVICE_ACCOUNT_ERROR,
    } music_service_account_state_t;

    /** 账户登录快照；不包含 Cookie 或上游私有字段。 */
    typedef struct
    {
        music_service_account_state_t state;
        uint64_t expires_at_ms;
        uint16_t qr_size;
        size_t qr_bytes;
        char login_id[MUSIC_SERVICE_QR_LOGIN_ID_MAX_BYTES];
        char error_code[MUSIC_SERVICE_ERROR_MAX_BYTES];
    } music_service_account_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PROTOCOL_H */
