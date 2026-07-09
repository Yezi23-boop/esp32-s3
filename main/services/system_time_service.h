#ifndef SYSTEM_TIME_SERVICE_H
#define SYSTEM_TIME_SERVICE_H

#include <stdint.h>

#include "esp_err.h"
#include "system_time.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 启动系统时间服务。
     *
     * 启动时只尝试 RTC bootstrap，不等待网络 SNTP，避免阻塞 UI 首帧。
     *
     * @return ESP_OK 表示服务已启动或之前已启动。
     */
    esp_err_t system_time_service_start(void);

    /**
     * @brief 通知系统时间服务网络云端依赖已就绪。
     *
     * 该接口只触发后台 SNTP 同步任务，不在调用方上下文阻塞等待。
     *
     * @return ESP_OK 表示通知已接收。
     */
    esp_err_t system_time_service_note_network_ready(void);

    /**
     * @brief 确保系统时间可用于 HTTPS/TLS。
     *
     * official_chat 通过 service 层注入该能力，底层组件不直接依赖 main/services。
     *
     * @param[in] timeout_ms 最大等待时间，单位为毫秒。
     * @return ESP_OK 表示时间已可信。
     */
    esp_err_t system_time_service_ensure_valid_for_tls(uint32_t timeout_ms);

    /**
     * @brief 应用业务服务器返回的可信 Unix 时间。
     *
     * 该接口统一执行 `settimeofday()` 和 RTC 回写，避免业务组件直接改系统时间。
     *
     * @param[in] unix_seconds Unix 时间戳，单位为秒。
     * @return ESP_OK 表示系统时间已更新。
     */
    esp_err_t system_time_service_apply_server_time(int64_t unix_seconds);

    /**
     * @brief 获取系统时间服务状态快照。
     *
     * @param[out] out 输出快照。
     * @return ESP_OK 表示复制成功。
     */
    esp_err_t system_time_service_get_snapshot(system_time_snapshot_t *out);

    /**
     * @brief 读取当前本地时间。
     *
     * 只读当前系统时间，不触发 SNTP 或 RTC I2C。
     *
     * @param[out] out 输出本地时间。
     * @return ESP_OK 表示成功。
     */
    esp_err_t system_time_service_get_local_time(system_time_local_t *out);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_TIME_SERVICE_H
