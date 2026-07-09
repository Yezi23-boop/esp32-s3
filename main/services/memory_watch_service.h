#ifndef MEMORY_WATCH_SERVICE_H
#define MEMORY_WATCH_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/memory_watch_voice_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_WATCH_SERVICE_ID_MAX_BYTES 64
#define MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES 128
#define MEMORY_WATCH_SERVICE_URL_MAX_BYTES 128
#define MEMORY_WATCH_SERVICE_DEVICE_ID_MAX_BYTES 32
#define MEMORY_WATCH_SERVICE_DEVICE_TOKEN_MAX_BYTES 128
#define MEMORY_WATCH_SERVICE_HEALTH_TIMEOUT_MS 5000U
#define MEMORY_WATCH_SERVICE_CONVERSATION_MAX_ITEMS 10U

    /**
     * @brief AI Memory Watch 服务状态。
     *
     * 状态只描述 ESP32-S3 侧的当前交互阶段；长期记忆、ASR 和工具执行结果仍由
     * 服务器侧 Hermes 负责。
     */
    typedef enum
    {
        MEMORY_WATCH_SERVICE_STATE_READY = 0,       /**< 可开始一次新的语音请求。 */
        MEMORY_WATCH_SERVICE_STATE_WAITING_NETWORK, /**< 等待网络 service ready。 */
        MEMORY_WATCH_SERVICE_STATE_RECORDING,       /**< 正在录音。 */
        MEMORY_WATCH_SERVICE_STATE_ENCODING,        /**< 正在封装 Ogg Opus。 */
        MEMORY_WATCH_SERVICE_STATE_UPLOADING,       /**< 正在上传到 watch endpoint。 */
        MEMORY_WATCH_SERVICE_STATE_THINKING,        /**< 已上传，等待 Hermes 最终文本。 */
        MEMORY_WATCH_SERVICE_STATE_NEEDS_CLARIFICATION, /**< Hermes 需要补充说明。 */
        MEMORY_WATCH_SERVICE_STATE_DONE,            /**< 本次交互已完成。 */
        MEMORY_WATCH_SERVICE_STATE_TIMEOUT,         /**< 等待服务器超过手表侧预算。 */
        MEMORY_WATCH_SERVICE_STATE_ERROR,           /**< 本次交互失败。 */
        MEMORY_WATCH_SERVICE_STATE_CANCELED,        /**< 用户取消录音或等待。 */
    } memory_watch_service_state_t;

    /**
     * @brief UI 读取的 AI Memory Watch 快照。
     *
     * getter 只复制该结构，不做网络请求、不读音频、不推进状态机，避免 LVGL
     * 定时刷新路径阻塞。
     */
    typedef struct
    {
        memory_watch_service_state_t state; /**< 当前服务状态。 */
        bool network_ready;                 /**< 最近一次观测到的网络 service ready。 */
        bool endpoint_configured;           /**< 是否已有运行期 watch endpoint 配置。 */
        bool hermes_online;                 /**< 最近一次 `/v1/watch/health` 是否确认 Hermes 在线。 */
        bool request_active;                /**< 是否有未收敛的 active request。 */
        bool clarification_active;          /**< 是否正在回答 Hermes 追问。 */
        esp_err_t last_error;               /**< 最近一次服务层错误。 */
        char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES];      /**< 当前请求 ID。 */
        char clarification_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES]; /**< 当前追问 ID。 */
        char asr_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];       /**< 最近 ASR 文本。 */
        char reply_text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];     /**< 最近 Hermes 回复。 */
        uint32_t conversation_generation; /**< 最近 5 轮本地显示缓存的版本号。 */
    } memory_watch_service_snapshot_t;

    /**
     * @brief Hermes 本地显示缓存消息角色。
     *
     * server conversation 仍是真相源；ESP32 只缓存最近 5 轮，用于页面重建和气泡
     * 点击回到 Hermes 页时快速显示，不作为长期记忆。
     */
    typedef enum
    {
        MEMORY_WATCH_SERVICE_CONVERSATION_USER = 0,   /**< 用户语音/文本经 ASR 后的内容。 */
        MEMORY_WATCH_SERVICE_CONVERSATION_HERMES,     /**< Hermes assistant 回复。 */
        MEMORY_WATCH_SERVICE_CONVERSATION_SYSTEM,     /**< 手表侧状态提示。 */
    } memory_watch_service_conversation_role_t;

    /**
     * @brief Hermes 本地显示缓存条目。
     */
    typedef struct
    {
        memory_watch_service_conversation_role_t role; /**< 消息角色。 */
        char request_id[MEMORY_WATCH_SERVICE_ID_MAX_BYTES]; /**< 关联请求 ID。 */
        char text[MEMORY_WATCH_SERVICE_TEXT_MAX_BYTES];     /**< 短文本展示内容。 */
    } memory_watch_service_conversation_item_t;

    /**
     * @brief AI Memory Watch watch endpoint 运行期配置。
     *
     * `device_token` 只用于 ESP32-S3 到 watch endpoint 的设备鉴权。Hermes
     * API key、MiMo key 或 API Server key 不属于本结构，也不得写入固件源码。
     */
    typedef struct
    {
        const char *base_url;     /**< watch endpoint 基础地址，例如 `https://watch.example.com`。 */
        const char *device_id;    /**< 设备 ID，例如 `watch-001`。 */
        const char *device_token; /**< 运行期配置/NVS 提供的设备 token。 */
        uint32_t timeout_ms;      /**< 语音请求等待预算；0 使用 voice client 默认值。 */
        bool allow_insecure_http; /**< 仅本地开发联调允许明文 HTTP。 */
    } memory_watch_service_endpoint_config_t;

    /**
     * @brief watch endpoint 运行期配置快照。
     *
     * 字符串存放在结构体内部，调用方可以把该结构体按值投递到自己的
     * FreeRTOS queue；不要把 `device_token` 打印到日志。
     */
    typedef struct
    {
        char base_url[MEMORY_WATCH_SERVICE_URL_MAX_BYTES]; /**< endpoint 基础地址。 */
        char device_id[MEMORY_WATCH_SERVICE_DEVICE_ID_MAX_BYTES]; /**< 设备 ID。 */
        char device_token[MEMORY_WATCH_SERVICE_DEVICE_TOKEN_MAX_BYTES]; /**< 设备 token。 */
        uint32_t timeout_ms;      /**< 默认请求等待预算；调用方可按业务覆盖。 */
        bool allow_insecure_http; /**< 是否允许本地开发用明文 HTTP。 */
    } memory_watch_service_endpoint_snapshot_t;

    /**
     * @brief 初始化 AI Memory Watch owner task。
     * @return `ESP_OK` 表示已初始化或本次初始化成功。
     */
    esp_err_t memory_watch_service_init(void);

    /**
     * @brief 配置 watch endpoint。
     *
     * 该接口只复制运行期提供的配置，不持久化、不打印 token，也不触发网络请求。
     *
     * @param[in] config endpoint 配置，字符串不能为空。
     * @return `ESP_OK` 表示已复制配置。
     */
    esp_err_t memory_watch_service_configure_endpoint(
        const memory_watch_service_endpoint_config_t *config);

    /**
     * @brief 保存 watch endpoint 配置到 NVS 并应用到运行期。
     *
     * 该接口只保存 ESP32-S3 到 watch endpoint 的 `device_token`，不保存
     * Hermes API key、MiMo key 或 API Server key；日志也不得打印 token。
     * 如当前已有未完成请求，为避免中途切换服务器配置，将返回
     * `ESP_ERR_INVALID_STATE`。
     *
     * @param[in] config endpoint 配置，字符串不能为空。
     * @return `ESP_OK` 表示已写入 NVS 并应用运行期配置。
     */
    esp_err_t memory_watch_service_save_endpoint_to_nvs(
        const memory_watch_service_endpoint_config_t *config);

    /**
     * @brief 复制当前 watch endpoint 配置快照。
     *
     * 该接口只读 `memory_watch_service` 已持有的 endpoint 配置，不触发网络请求；
     * 中性 watch endpoint 业务服务可用它复用现有配置入口和 NVS 存储。
     *
     * @param[out] out_config 输出配置快照，不能为空。
     * @return `ESP_OK` 表示已配置并复制成功。
     */
    esp_err_t memory_watch_service_copy_endpoint_config(
        memory_watch_service_endpoint_snapshot_t *out_config);

    /**
     * @brief 请求 owner task 检查 `/v1/watch/health`。
     *
     * UI 进入 Hermes 页面时可投递一次该命令；实际 HTTP GET 在 service task
     * 中执行，且 health 请求使用短超时，不阻塞 LVGL 刷新路径。
     *
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_check_health(void);

    /**
     * @brief 告诉 Memory Watch owner Hermes 页面是否处于前台。
     *
     * UI 只发布页面生命周期意图；实际 WebSocket 关闭、后台 conversation polling
     * 由 service/worker 按 owner 规则处理，UI 不直接做网络操作。
     *
     * @param[in] foreground true 表示 Hermes 页面前台可见。
     * @return `ESP_OK` 表示命令已投递或状态已记录。
     */
    esp_err_t memory_watch_service_set_foreground(bool foreground);

    /**
     * @brief 用户按住语音按钮，开始一次录音意图。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_begin_recording(void);

    /**
     * @brief 用户松开发送，提交当前录音。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_send_recording(void);

    /**
     * @brief 发送一条 UTF-8 文本给 watch endpoint。
     *
     * 文本发送复用 Memory Watch 的 owner task、request_id、cancel 和固定 7 字段
     * 响应处理；实际 HTTP POST 在后台 worker task 中执行，UI 不得直接等待网络。
     *
     * @param[in] text 要发送的文本，不能为空，不能包含 CR/LF。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_send_text(const char *text);

    /**
     * @brief 用户滑出按钮或松手取消，丢弃当前录音。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_cancel_recording(void);

    /**
     * @brief 用户取消等待服务器结果。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_cancel_waiting(void);

    /**
     * @brief 用户取消当前 Hermes 追问。
     * @return `ESP_OK` 表示命令已投递到 owner task。
     */
    esp_err_t memory_watch_service_cancel_clarification(void);

    /**
     * @brief 复制当前服务快照。
     * @param[out] out_snapshot 输出快照，不能为空。
     * @return `ESP_OK` 表示成功复制。
     */
    esp_err_t memory_watch_service_get_snapshot(
        memory_watch_service_snapshot_t *out_snapshot);

    /**
     * @brief 复制 Hermes 最近 5 轮本地显示缓存。
     *
     * 该接口只复制 service 内存缓存，不访问网络、不读取 flash/SD，也不推进状态机。
     *
     * @param[out] out_items 输出数组；capacity 为 0 时可以为 NULL。
     * @param[in] capacity 输出数组容量。
     * @param[out] out_count 实际复制数量，不能为空。
     * @return `ESP_OK` 表示成功复制。
     */
    esp_err_t memory_watch_service_copy_conversation_items(
        memory_watch_service_conversation_item_t *out_items,
        size_t capacity,
        size_t *out_count);

    /**
     * @brief 查询当前 watch endpoint 是否已配置。
     *
     * 只返回布尔状态，不返回配置内容；供 `/api/status` 等只读入口使用。
     *
     * @return `true` 表示当前运行态或 NVS 已有完整 watch endpoint 配置。
     */
    bool memory_watch_service_is_endpoint_configured(void);

    /**
     * @brief 获取状态字符串。
     * @param[in] state 服务状态。
     * @return 静态字符串。
     */
    const char *memory_watch_service_state_to_string(
        memory_watch_service_state_t state);

    /**
     * @brief inbox 同步状态。
     */
    typedef enum
    {
        MEMORY_WATCH_INBOX_SYNC_UNCONFIGURED = 0, /**< endpoint 未配置，不发请求。 */
        MEMORY_WATCH_INBOX_SYNC_IDLE,             /**< 空闲，等待下次调度。 */
        MEMORY_WATCH_INBOX_SYNC_POLLING,          /**< inbox worker 正在执行 GET。 */
        MEMORY_WATCH_INBOX_SYNC_READY,            /**< 最近一次成功，store 有效。 */
        MEMORY_WATCH_INBOX_SYNC_RETRY_WAIT,       /**< 临时错误，1 分钟后重试。 */
        MEMORY_WATCH_INBOX_SYNC_AUTH_ERROR,       /**< 401/403，不紧循环，等配置更新。 */
        MEMORY_WATCH_INBOX_SYNC_PROTOCOL_ERROR,   /**< 422/解析失败，等下次人工触发。 */
    } memory_watch_inbox_sync_state_t;

    /**
     * @brief inbox meta — LVGL timer 可低成本读取，不含 item 数组。
     *
     * generation 变化时 controller 才复制 summary 列表。
     */
    typedef struct
    {
        uint32_t generation;                      /**< 每次 store 更新时递增。 */
        size_t item_count;                        /**< 当前 store 中的条目数。 */
        uint8_t unread_count;                     /**< 有效未读数（含待同步已读本地置真）。 */
        memory_watch_inbox_sync_state_t sync_state; /**< 当前同步状态。 */
        int64_t last_success_ms;                  /**< 最近成功轮询时的 esp_timer_get_time() / 1000。 */
    } memory_watch_inbox_meta_t;

    /**
     * @brief inbox summary — 列表页所需字段，不含 body。
     */
    typedef struct
    {
        char notification_id[64]; /**< 最多 63 字节 + '\0'。 */
        char title[64];           /**< 最多 63 字节 + '\0'。 */
        char preview[128];        /**< 最多 127 字节 + '\0'。 */
        char created_at[32];      /**< UTC RFC3339 字符串。 */
        bool read;                /**< 有效已读状态（含本地 pending-read）。 */
    } memory_watch_inbox_summary_t;

    /**
     * @brief 读取 inbox meta（低成本，无 I/O，无 item 拷贝）。
     * @param[out] out_meta 输出 meta，不能为空。
     * @return `ESP_OK` 成功。
     */
    esp_err_t memory_watch_service_get_inbox_meta(
        memory_watch_inbox_meta_t *out_meta);

    /**
     * @brief 拷贝最多 capacity 条 inbox summary（不含 body）。
     *
     * 只有 generation 变化时 controller 才应调用；LVGL timer 只调用
     * `get_inbox_meta`。
     *
     * @param[out] out_summaries 调用方分配的 summary 数组，capacity >= 20。
     * @param[in]  capacity      数组容量（条）。
     * @param[out] out_count     实际拷贝条数。
     * @return `ESP_OK` 成功。
     */
    esp_err_t memory_watch_service_copy_inbox_summaries(
        memory_watch_inbox_summary_t *out_summaries,
        size_t capacity,
        size_t *out_count);

    /**
     * @brief 按 notification_id 拷贝单条完整 inbox item（含 body）。
     *
     * 进入详情时调用；返回后调用方保有副本，service store 可继续更新。
     *
     * @param[in]  notification_id 目标消息 ID。
     * @param[out] out_item        输出完整 item，不能为空。
     * @return `ESP_OK` 成功，`ESP_ERR_NOT_FOUND` 不存在（已被淘汰）。
     */
    esp_err_t memory_watch_service_get_inbox_item(
        const char *notification_id,
        memory_watch_inbox_item_t *out_item);

    /**
     * @brief 请求立即拉取 inbox（打开收件箱、网络恢复等场景）。
     *
     * 多个 poll_now 在 worker 在途时自动合并为一个 pending bit；
     * 不堆积重复 GET。
     *
     * @param[in] reason 调试用描述字符串（如 "open_inbox"/"network_ready"），可为 NULL。
     * @return `ESP_OK` 表示命令已投递。
     */
    esp_err_t memory_watch_service_inbox_poll_now(const char *reason);

    /**
     * @brief 标记单条消息已读（本地立即生效，异步上报服务器）。
     *
     * 调用后 unread_count 立即更新，不等待 HTTP 成功；上报失败时
     * 保留在 pending-read set，网络恢复后重试。
     *
     * @param[in] notification_id 要标记的消息 ID。
     * @return `ESP_OK` 表示命令已投递，`ESP_ERR_NOT_FOUND` 表示 ID 不在当前 store。
     */
    esp_err_t memory_watch_service_inbox_mark_read(
        const char *notification_id);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_SERVICE_H
