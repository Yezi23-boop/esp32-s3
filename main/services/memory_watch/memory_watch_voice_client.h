#ifndef MEMORY_WATCH_VOICE_CLIENT_H
#define MEMORY_WATCH_VOICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_WATCH_VOICE_CLIENT_REQUEST_ID_MAX_LEN 96U
#define MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES \
    (MEMORY_WATCH_VOICE_CLIENT_REQUEST_ID_MAX_LEN + 1U)
#define MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES 24U
#define MEMORY_WATCH_VOICE_CLIENT_ACTION_MAX_BYTES 32U
#define MEMORY_WATCH_VOICE_CLIENT_TEXT_MAX_BYTES 256U
#define MEMORY_WATCH_VOICE_CLIENT_ERROR_MAX_BYTES 64U
#define MEMORY_WATCH_VOICE_CLIENT_DEFAULT_TIMEOUT_MS 120000U
#define MEMORY_WATCH_VOICE_CLIENT_MAX_AUDIO_BYTES (6U * 1024U * 1024U)
#define MEMORY_WATCH_VOICE_CLIENT_MAX_TEXT_BYTES 512U

    /**
     * @brief AI Memory Watch 设备侧 HTTP client 配置。
     *
     * `device_token` 只代表 watch endpoint 的设备 token；Hermes API key、
     * API Server key 与 MiMo key 必须只保留在服务器侧。
     */
    typedef struct
    {
        const char *base_url;     /**< watch endpoint 基础地址，例如 `https://watch.example.com`。 */
        const char *device_id;    /**< 服务器 allowlist 中的设备 ID。 */
        const char *device_token; /**< 设备 token，运行期配置/NVS 提供，不硬编码源码。 */
        uint32_t timeout_ms;      /**< HTTP 总等待预算；0 使用 V1 默认 120 秒。 */
        bool allow_insecure_http; /**< 仅开发联调使用；false 时拒绝明文 HTTP 发送 device token。 */
    } memory_watch_voice_client_config_t;

    /**
     * @brief voice-command 请求参数。
     */
    typedef struct
    {
        const char *request_id;       /**< `<device_id>-<boot_id>-<seq>`，最长 96 字符。 */
        const uint8_t *audio;         /**< Ogg Opus 音频数据。 */
        size_t audio_len;             /**< 音频字节数，必须在服务器 6 MiB 上限内。 */
        const char *clarification_id; /**< 可选追问 ID；无追问可传 NULL 或空字符串。 */
        bool has_battery_percent;     /**< 是否上传电量百分比。 */
        int battery_percent;          /**< 电量百分比，0..100。 */
        bool has_charging;            /**< 是否上传充电状态。 */
        bool charging;                /**< 当前是否充电。 */
        bool has_rssi;                /**< 是否上传 Wi-Fi RSSI。 */
        int rssi;                     /**< Wi-Fi RSSI，单位 dBm。 */
        const char *firmware_version; /**< 可选固件版本。 */
        const char *ui_state;         /**< 当前页面状态；NULL 时使用 `ready`。 */
    } memory_watch_voice_client_request_t;

    /**
     * @brief text-command 请求参数。
     *
     * 文本链路用于开发调试、后续手表侧候选文本输入和无麦克风路径；它仍只
     * 面向 watch endpoint，不能绕过服务器直接调用私有大脑接口。
     */
    typedef struct
    {
        const char *request_id;       /**< `<device_id>-<boot_id>-<seq>`，最长 96 字符。 */
        const char *text;             /**< 要发送给 Hermes 的 UTF-8 文本。 */
        const char *clarification_id; /**< 可选追问 ID；无追问可传 NULL 或空字符串。 */
        bool has_battery_percent;     /**< 是否上传电量百分比。 */
        int battery_percent;          /**< 电量百分比，0..100。 */
        bool has_charging;            /**< 是否上传充电状态。 */
        bool charging;                /**< 当前是否充电。 */
        bool has_rssi;                /**< 是否上传 Wi-Fi RSSI。 */
        int rssi;                     /**< Wi-Fi RSSI，单位 dBm。 */
        const char *firmware_version; /**< 可选固件版本。 */
        const char *ui_state;         /**< 当前页面状态；NULL 时使用 `ready`。 */
    } memory_watch_voice_client_text_request_t;

    /**
     * @brief watch endpoint 固定 7 字段响应。
     */
    typedef struct
    {
        int http_status;          /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP/解析错误。 */
        char request_id[MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES];
        char status[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
        char action[MEMORY_WATCH_VOICE_CLIENT_ACTION_MAX_BYTES];
        char asr_text[MEMORY_WATCH_VOICE_CLIENT_TEXT_MAX_BYTES];
        char reply_text[MEMORY_WATCH_VOICE_CLIENT_TEXT_MAX_BYTES];
        char clarification_id[MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES];
        char error_code[MEMORY_WATCH_VOICE_CLIENT_ERROR_MAX_BYTES];
    } memory_watch_voice_client_response_t;

    /**
     * @brief `/v1/watch/health` 设备侧健康检查响应。
     */
    typedef struct
    {
        int http_status;           /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP/解析错误。 */
        char status[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
        char hermes_status[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
        char device_id[MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES];
    } memory_watch_voice_client_health_t;

    /**
     * @brief 危险声音手机通知上报请求。
     *
     * 该结构只面向 watch endpoint 的 `/v1/watch/alerts`，用于把本机已经确认的
     * `Alerting` 事件转发给手机 App 通知链路；不得在模型单窗回调里同步调用。
     */
    typedef struct
    {
        const char *danger_type;    /**< 危险类型，例如 `danger`、`horn`、`siren`。 */
        float danger_prob;          /**< 进入 Alerting 时的 danger 概率，范围 0..1。 */
        uint32_t alert_sequence;    /**< 本机告警序号，用于云端日志去重与排查。 */
        const char *message;        /**< 给手机端展示的短提示文案。 */
        const char *firmware_version; /**< 可选固件版本。 */
    } memory_watch_voice_client_danger_alert_request_t;

    /**
     * @brief `/v1/watch/alerts` 上报结果。
     */
    typedef struct
    {
        int http_status;           /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP 错误。 */
    } memory_watch_voice_client_danger_alert_response_t;

    /**
     * @brief 进入 Hermes 页面时检查 watch endpoint 与 Hermes 在线状态。
     *
     * 该函数同步执行 HTTP GET，只允许 service/worker task 调用；UI 只能读取
     * `memory_watch_service` 发布的 snapshot，不得直接调用本函数。
     *
     * @param[in] config HTTP client 配置。
     * @param[out] out_health 健康检查响应。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应解析成功。
     */
    esp_err_t memory_watch_voice_client_get_health(
        const memory_watch_voice_client_config_t *config,
        memory_watch_voice_client_health_t *out_health);

    /**
     * @brief 上传一次 Ogg Opus 语音请求并解析手表 V1 响应。
     *
     * 该函数同步执行 HTTP 请求，只允许上传 worker task 调用；
     * `memory_watch_service` owner task 后续应只投递请求、消费结果和处理
     * cancel 命令，不得在 owner task 内直接等待 120 秒 HTTP 响应。
     * 也不得在 LVGL timer/getter 或音频采样高频路径中调用。
     *
     * @param[in] config HTTP client 配置。
     * @param[in] request 请求参数。
     * @param[out] out_response 固定 7 字段响应；失败时也会尽量填入 HTTP 状态。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应契约解析成功。
     */
    esp_err_t memory_watch_voice_client_post_voice_command(
        const memory_watch_voice_client_config_t *config,
        const memory_watch_voice_client_request_t *request,
        memory_watch_voice_client_response_t *out_response);

    /**
     * @brief 发送一次文本请求并解析手表 V1 响应。
     *
     * 该函数同步执行 HTTP POST，只允许 service/worker task 调用；UI 只能向
     * `memory_watch_service` 投递文本意图，不能在 LVGL 路径直接等待网络。
     *
     * @param[in] config HTTP client 配置。
     * @param[in] request 文本请求参数。
     * @param[out] out_response 固定 7 字段响应。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应契约解析成功。
     */
    esp_err_t memory_watch_voice_client_post_text_command(
        const memory_watch_voice_client_config_t *config,
        const memory_watch_voice_client_text_request_t *request,
        memory_watch_voice_client_response_t *out_response);

    /**
     * @brief POST 一条危险声音告警给 watch endpoint 手机通知链路。
     *
     * 该函数同步执行 HTTPS POST，只允许 service/worker task 调用。服务器当前可
     * 先不校验 device token，但 client 仍复用 endpoint 配置发送 Authorization
     * 头，后续打开鉴权不需要再改手表侧主流程。
     *
     * @param[in] config HTTP client 配置。
     * @param[in] request 告警请求。
     * @param[out] out_response HTTP 结果。
     * @return `ESP_OK` 表示 HTTP 2xx。
     */
    esp_err_t memory_watch_voice_client_post_danger_alert(
        const memory_watch_voice_client_config_t *config,
        const memory_watch_voice_client_danger_alert_request_t *request,
        memory_watch_voice_client_danger_alert_response_t *out_response);

    /**
     * @brief 通知服务器取消当前等待请求。
     *
     * 取消只负责让手表回到 ready 并尽力通知 voice endpoint；它不承诺撤销
     * Hermes 已经执行的外部工具副作用。
     *
     * @param[in] config HTTP client 配置。
     * @param[in] request_id 要取消的请求 ID。
     * @param[out] out_response 固定 7 字段响应。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应契约解析成功。
     */
    esp_err_t memory_watch_voice_client_cancel_request(
        const memory_watch_voice_client_config_t *config,
        const char *request_id,
        memory_watch_voice_client_response_t *out_response);

    /**
     * @brief inbox 单条消息；字段长度与服务器 UTF-8 字节上限保持一致。
     *
     * 该结构不得放在 task 栈上；20 条约占 15 KiB，必须分配在 PSRAM 或静态段。
     */
    typedef struct
    {
        char notification_id[64]; /**< 最多 63 字节 + '\0'。 */
        char source[24];          /**< 固定为 "hermes"。 */
        char kind[24];            /**< "reminder"/"info"/"warning"。 */
        char created_at[32];      /**< UTC RFC3339，服务端生成。 */
        char title[64];           /**< 最多 63 字节 + '\0'。 */
        char preview[128];        /**< 最多 127 字节 + '\0'。 */
        char body[384];           /**< 最多 383 字节 + '\0'。 */
        bool read;                /**< 服务端已读状态。 */
    } memory_watch_inbox_item_t;

    /**
     * @brief `/v1/watch/sync` 返回的最新未读 inbox 摘要。
     *
     * 这里只承载气泡提示需要的短字段；完整收件箱列表仍走
     * `memory_watch_voice_client_inbox_poll()`，避免后台 idle sync 拉大包。
     */
    typedef struct
    {
        char notification_id[64]; /**< 最新未读 notification_id。 */
        char title[64];           /**< 短标题，允许服务端截断。 */
        char preview[128];        /**< 气泡预览文本，允许服务端截断。 */
        char created_at[32];      /**< UTC RFC3339，服务端生成。 */
    } memory_watch_sync_inbox_summary_t;

    /**
     * @brief inbox 轮询响应（GET /v1/watch/inbox）。
     *
     * items 数组由调用方提供，容量至少为 20；实际条数写入 out_item_count。
     */
    typedef struct
    {
        int http_status;           /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP/解析错误。 */
        uint8_t unread_count;      /**< 服务端返回的未读条数。 */
        size_t item_count;         /**< 实际解析成功的消息条数。 */
    } memory_watch_inbox_poll_result_t;

    /**
     * @brief inbox 标记已读响应（POST /v1/watch/inbox/{id}/read）。
     */
    typedef struct
    {
        int http_status;           /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP/解析错误。 */
        bool read;                 /**< 服务端确认的 read 状态。 */
    } memory_watch_inbox_mark_read_result_t;

    #define MEMORY_WATCH_INBOX_MAX_ITEMS 20U
    /** inbox 轮询响应体最大字节数（20 条满载 ≈ 16 KiB，保留 8 KiB 余量）。 */
    #define MEMORY_WATCH_INBOX_RESPONSE_MAX_BYTES (24U * 1024U)

    /**
     * @brief server conversation 单条消息。
     *
     * 用于 `/v1/watch/sync` 返回的 conversation delta。
     * server conversation 才是真相源；ESP32 只合并最近几条用于显示。
     */
    typedef struct
    {
        char message_id[64]; /**< server 生成的 msg_xxx。 */
        char request_id[MEMORY_WATCH_VOICE_CLIENT_ID_MAX_BYTES];
        char role[16];       /**< "user" 或 "assistant"。 */
        char text[MEMORY_WATCH_VOICE_CLIENT_TEXT_MAX_BYTES];
        char created_at[32]; /**< UTC RFC3339。 */
        char status[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
    } memory_watch_conversation_message_t;

    #define MEMORY_WATCH_CONVERSATION_MAX_MESSAGES 20U

    /**
     * @brief `/v1/watch/sync` 后台同步模式。
     */
    typedef enum
    {
        MEMORY_WATCH_SYNC_MODE_BACKGROUND = 0,          /**< 离页/后台低频同步。 */
        MEMORY_WATCH_SYNC_MODE_FOREGROUND_RECONCILE,   /**< 进入 Hermes 页时补齐最近对话。 */
    } memory_watch_sync_mode_t;

    /**
     * @brief `/v1/watch/sync` 游标参数。
     *
     * `max_messages` 为 0 时使用设备侧默认 10 条；V2.4 的轮询节奏
     * 由 service owner 持有，不从 server response 动态下发。
     */
    typedef struct
    {
        memory_watch_sync_mode_t mode;
        const char *pending_request_id; /**< 可选；后台等待中的 request_id。 */
        const char *after_message_id;   /**< 可选；已显示的最后 message_id。 */
        size_t max_messages;            /**< 请求的 conversation 增量条数，0 使用默认 10。 */
    } memory_watch_voice_client_sync_cursor_t;

    /**
     * @brief `/v1/watch/sync` 聚合响应。
     *
     * ESP32 只理解公开 session_state：none/running/done/error/timeout/canceled。
     * server 内部细分状态不应出现在本结构或 UI/controller。
     */
    typedef struct
    {
        int http_status;           /**< HTTP 状态码，传输失败时为 0。 */
        esp_err_t transport_error; /**< 最近一次 HTTP/解析错误。 */
        bool has_pending;          /**< server 认为 pending_request_id 仍在运行。 */
        char session_state[MEMORY_WATCH_VOICE_CLIENT_STATUS_MAX_BYTES];
        size_t message_count;      /**< 实际解析成功的 conversation messages 条数。 */
        uint8_t inbox_unread_count;
        bool has_latest_unread;
        memory_watch_sync_inbox_summary_t latest_unread;
    } memory_watch_voice_client_sync_result_t;

    #define MEMORY_WATCH_SYNC_DEFAULT_MAX_MESSAGES 10U
    /** `/sync` 响应体最大字节数；按 10 条短 conversation + inbox 摘要预留。 */
    #define MEMORY_WATCH_SYNC_RESPONSE_MAX_BYTES (12U * 1024U)

    /**
     * @brief GET /v1/watch/inbox 拉取最近 20 条快照。
     *
     * 只允许 inbox worker task 调用；调用方必须提供已在 PSRAM 分配的
     * items 数组（capacity >= MEMORY_WATCH_INBOX_MAX_ITEMS）。
     *
     * @param[in]  config     HTTP client 配置。
     * @param[out] items      调用方分配的条目数组，capacity >= 20。
     * @param[in]  capacity   items 数组容量（条）。
     * @param[out] out_result 轮询结果（状态码/条数/未读数）。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应解析成功。
     */
    esp_err_t memory_watch_voice_client_inbox_poll(
        const memory_watch_voice_client_config_t *config,
        memory_watch_inbox_item_t *items,
        size_t capacity,
        memory_watch_inbox_poll_result_t *out_result);

    /**
     * @brief POST /v1/watch/inbox/{notification_id}/read 标记已读。
     *
     * 只允许 inbox worker task 调用；幂等，target 不存在返回 404（视为终态）。
     *
     * @param[in]  config          HTTP client 配置。
     * @param[in]  notification_id 要标记的消息 ID。
     * @param[out] out_result      标记已读结果。
     * @return `ESP_OK` 表示 HTTP 200，`ESP_ERR_NOT_FOUND` 表示 HTTP 404。
     */
    esp_err_t memory_watch_voice_client_inbox_mark_read(
        const memory_watch_voice_client_config_t *config,
        const char *notification_id,
        memory_watch_inbox_mark_read_result_t *out_result);

    /**
     * @brief GET /v1/watch/sync 后台统一同步。
     *
     * 只允许 service/worker task 调用；调用方必须提供 PSRAM 分配的 messages
     * 数组。网络失败只表示本轮同步失败，不能据此清除 pending。
     *
     * @param[in]  config     HTTP client 配置。
     * @param[in]  cursor     同步模式与游标；NULL 使用 background + 默认条数。
     * @param[out] messages   调用方分配的 conversation 增量数组。
     * @param[in]  capacity   messages 数组容量。
     * @param[out] out_result 聚合同步结果。
     * @return `ESP_OK` 表示 HTTP 2xx 且响应契约解析成功。
     */
    esp_err_t memory_watch_voice_client_sync(
        const memory_watch_voice_client_config_t *config,
        const memory_watch_voice_client_sync_cursor_t *cursor,
        memory_watch_conversation_message_t *messages,
        size_t capacity,
        memory_watch_voice_client_sync_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_VOICE_CLIENT_H
