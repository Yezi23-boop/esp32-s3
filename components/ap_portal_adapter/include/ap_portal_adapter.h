#ifndef AP_PORTAL_ADAPTER_H
#define AP_PORTAL_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AP_PORTAL_MEMORY_WATCH_URL_MAX_BYTES 128
#define AP_PORTAL_MEMORY_WATCH_DEVICE_ID_MAX_BYTES 32
#define AP_PORTAL_MEMORY_WATCH_DEVICE_TOKEN_MAX_BYTES 128

/**
 * @brief AP 门户接收到的 AI Memory Watch endpoint 配置。
 *
 * 该结构只承载 ESP32-S3 到 watch endpoint 的设备级 token。Hermes API key、
 * MiMo key 或 API Server key 不属于门户配置，也不得从该入口进入固件。
 */
typedef struct
{
    char base_url[AP_PORTAL_MEMORY_WATCH_URL_MAX_BYTES];                 /**< watch endpoint 基础地址。 */
    char device_id[AP_PORTAL_MEMORY_WATCH_DEVICE_ID_MAX_BYTES];          /**< 设备 ID。 */
    char device_token[AP_PORTAL_MEMORY_WATCH_DEVICE_TOKEN_MAX_BYTES];    /**< 设备 token，不得写日志。 */
    uint32_t timeout_ms;                                                 /**< 语音请求等待预算；0 表示使用默认值。 */
    bool allow_insecure_http;                                            /**< 开发期是否允许 HTTP 明文 endpoint。 */
} ap_portal_memory_watch_config_t;

/**
 * @brief AI Memory Watch endpoint 配置保存回调。
 *
 * AP 门户只负责解析请求并调用该回调，具体保存到哪个 owner 由主程序注册决定，避免
 * `ap_portal_adapter` 反向依赖 `main/services`。
 */
typedef esp_err_t (*ap_portal_memory_watch_config_cb_t)(
    const ap_portal_memory_watch_config_t *config, void *user_ctx);

/**
 * @brief AI Memory Watch endpoint 配置状态查询回调。
 *
 * 返回 `true` 表示当前运行态或 NVS 已有完整 watch endpoint 配置。
 * 只返回布尔值，不返回配置内容。
 */
typedef bool (*ap_portal_memory_watch_configured_cb_t)(void *user_ctx);

/**
 * @brief 注册 AI Memory Watch endpoint 配置保存回调。
 *
 * @param[in] callback 保存回调；传 `NULL` 表示禁用该门户配置入口。
 * @param[in] user_ctx 透传给回调的用户上下文。
 * @return `ESP_OK` 表示回调已更新。
 */
esp_err_t ap_portal_adapter_set_memory_watch_config_callback(
    ap_portal_memory_watch_config_cb_t callback, void *user_ctx);

/**
 * @brief 注册 AI Memory Watch endpoint 配置状态查询回调。
 *
 * 供 `/api/status` 查询 `memory_watch_endpoint_configured` 布尔字段。
 * 该回调只返回布尔值，不返回配置内容。
 *
 * @param[in] callback 查询回调；传 `NULL` 表示禁用查询。
 * @param[in] user_ctx 透传给回调的用户上下文。
 * @return `ESP_OK` 表示回调已更新。
 */
esp_err_t ap_portal_adapter_set_memory_watch_configured_callback(
    ap_portal_memory_watch_configured_cb_t callback, void *user_ctx);

/**
 * @brief 启动 AP 门户适配层的最小 HTTP 服务器。
 *
 * 当前阶段只提供最小页面与 HTTPD handle 复用接缝，供官方 SoftAP provisioning scheme
 * 在后续启动时复用同一实例，而不是再起第二个 HTTP server。
 *
 * @return `ESP_OK` 表示启动成功；其他错误表示 HTTP server 或路由注册失败。
 */
esp_err_t ap_portal_adapter_start(void);

/**
 * @brief 停止 AP 门户适配层的 HTTP 服务器。
 *
 * 停止前会先把已注册给官方 SoftAP provisioning scheme 的 HTTPD handle 清空，避免后续
 * manager 继续持有悬空服务器句柄。
 *
 * @return `ESP_OK` 表示停止成功或服务器本就未启动；其他错误表示停止失败。
 */
esp_err_t ap_portal_adapter_stop(void);

/**
 * @brief 获取当前 AP 门户 HTTPD 句柄。
 *
 * 该接口用于让上层或调试路径确认当前门户实例是否已创建，以及官方 SoftAP provisioning
 * 将复用哪个 HTTP server。
 *
 * @return 当前 HTTPD 句柄；若门户未启动则返回 `NULL`。
 */
httpd_handle_t ap_portal_adapter_get_httpd_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* AP_PORTAL_ADAPTER_H */
