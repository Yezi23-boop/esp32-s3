#ifndef CAPTIVE_PORTAL_DNS_H
#define CAPTIVE_PORTAL_DNS_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Captive Portal 使用的 DNS 劫持服务。
 *
 * 当前实现会把所有 IPv4 A 记录查询统一回答为 `WIFI_AP_DEF` 的 IPv4 地址，
 * 从而把系统联网探测请求引回本机 SoftAP 门户。
 *
 * @return `ESP_OK` 表示服务已启动或本就处于启动态；其他错误表示启动失败。
 */
esp_err_t captive_portal_dns_start(void);

/**
 * @brief 停止 Captive Portal 使用的 DNS 劫持服务。
 *
 * @return `ESP_OK` 表示服务已停止或本就未启动；其他错误表示停止失败。
 */
esp_err_t captive_portal_dns_stop(void);

/**
 * @brief 查询 DNS 劫持服务当前是否处于运行态。
 *
 * @return `true` 表示服务已启动；`false` 表示服务未启动。
 */
bool captive_portal_dns_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* CAPTIVE_PORTAL_DNS_H */
