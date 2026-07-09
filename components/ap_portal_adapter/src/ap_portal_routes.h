#ifndef AP_PORTAL_ROUTES_H
#define AP_PORTAL_ROUTES_H

#include "ap_portal_adapter.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设置 AI Memory Watch endpoint 配置保存回调。
 *
 * 该接口只在 `ap_portal_adapter` 内部使用，路由层不会直接依赖上层 service。
 *
 * @param[in] callback 保存回调；传 `NULL` 表示禁用配置入口。
 * @param[in] user_ctx 透传给回调的上下文。
 * @return `ESP_OK` 表示回调已更新。
 */
esp_err_t ap_portal_routes_set_memory_watch_config_callback(
    ap_portal_memory_watch_config_cb_t callback, void *user_ctx);

/**
 * @brief 设置 AI Memory Watch endpoint 配置状态查询回调。
 *
 * 供 `/api/status` 查询 `memory_watch_endpoint_configured` 布尔字段。
 * 该回调只返回布尔值，不返回配置内容。
 *
 * @param[in] callback 查询回调；传 `NULL` 表示禁用查询。
 * @param[in] user_ctx 透传给回调的上下文。
 * @return `ESP_OK` 表示回调已更新。
 */
esp_err_t ap_portal_routes_set_memory_watch_configured_callback(
    ap_portal_memory_watch_configured_cb_t callback, void *user_ctx);

/**
 * @brief 向给定 HTTPD 注册 AP 门户最小路由。
 *
 * 当前阶段只注册根路径和 `favicon.ico`，用于先打通“自定义页面 + 官方 SoftAP provisioning
 * 共用同一 HTTPD handle”的接缝；后续页面资源与设备侧接口会继续在这个模块扩展。
 *
 * @param[in] server 已启动的 HTTPD 句柄。
 * @return `ESP_OK` 表示注册成功；其他错误表示参数非法或路由注册失败。
 */
esp_err_t ap_portal_routes_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* AP_PORTAL_ROUTES_H */
