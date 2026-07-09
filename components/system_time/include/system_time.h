#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        SYSTEM_TIME_SOURCE_NONE = 0,   /**< 尚未建立可信系统时间。 */
        SYSTEM_TIME_SOURCE_RTC,        /**< 系统时间来自 PCF85063ATL RTC。 */
        SYSTEM_TIME_SOURCE_SNTP,       /**< 系统时间来自 SNTP 网络授时。 */
        SYSTEM_TIME_SOURCE_SERVER,     /**< 系统时间来自业务服务器返回的可信时间戳。 */
    } system_time_source_t;

    typedef struct
    {
        bool system_time_valid;       /**< true 表示当前系统时间已进入 TLS 可用时间窗口。 */
        bool rtc_present;             /**< true 表示 I2C 上探测到 PCF85063ATL。 */
        bool rtc_oscillator_stopped;  /**< true 表示 RTC OS 位为 1，RTC 时间不可信。 */
        bool rtc_writeback_ok;        /**< true 表示最近一次可信时间已成功写回 RTC。 */
        system_time_source_t source;  /**< 最近一次建立可信系统时间的来源。 */
        int64_t unix_seconds;         /**< 当前系统 Unix 时间戳，单位为秒。 */
    } system_time_snapshot_t;

    typedef struct
    {
        int year;          /**< 年，例如 2026。 */
        int month;         /**< 月，范围 1-12。 */
        int day;           /**< 日，范围 1-31。 */
        int hour;          /**< 时，24 小时制。 */
        int min;           /**< 分。 */
        int sec;           /**< 秒。 */
        char time_str[64]; /**< 格式化后的本地时间字符串缓存。 */
    } system_time_local_t;

    /**
     * @brief 初始化系统时间核心。
     *
     * 该函数只初始化内部状态和 RTC 驱动，不启动 SNTP，不阻塞联网。
     *
     * @return ESP_OK 表示初始化成功或之前已初始化。
     */
    esp_err_t system_time_init(void);

    /**
     * @brief 尝试使用 RTC 时间设置系统时间。
     *
     * 只有 PCF85063ATL 存在、OS 位为 0 且日历字段合法时，才会调用
     * `settimeofday()`。OS 位为 1 时返回 ESP_ERR_INVALID_STATE。
     *
     * @return ESP_OK 表示已用 RTC 建立系统时间；其他错误表示 RTC 不可用或时间不可信。
     */
    esp_err_t system_time_bootstrap_from_rtc(void);

    /**
     * @brief 启动或重启 SNTP，同步成功后写回 RTC。
     *
     * 该接口是当前项目唯一允许调用 ESP-IDF SNTP API 的入口之一。
     * SNTP 成功后会更新系统时间，并调用 `pcf85063atl_set_time()` 清 RTC OS 位。
     *
     * @param[in] timeout_ms 等待 SNTP 完成的最大时间，单位为毫秒。
     * @return ESP_OK 表示系统时间已通过 SNTP 校准；超时返回 ESP_ERR_TIMEOUT。
     */
    esp_err_t system_time_sync_sntp_and_write_rtc(uint32_t timeout_ms);

    /**
     * @brief 确保当前系统时间可用于 HTTPS/TLS 证书校验。
     *
     * 如果当前系统时间已经有效，直接返回 ESP_OK；否则执行一次 SNTP 同步。
     *
     * @param[in] timeout_ms 等待 SNTP 的最大时间，单位为毫秒。
     * @return ESP_OK 表示系统时间可信；其他错误表示无法在时限内授时。
     */
    esp_err_t system_time_ensure_valid_for_tls(uint32_t timeout_ms);

    /**
     * @brief 应用外部可信 Unix 时间，并可选写回 RTC。
     *
     * 主要供业务服务器返回可信时间戳后使用。该接口仍由系统时间 owner
     * 统一执行 `settimeofday()` 和 RTC 回写，避免业务组件直接修改系统时间。
     *
     * @param[in] source 时间来源，不能为 SYSTEM_TIME_SOURCE_NONE。
     * @param[in] unix_seconds Unix 时间戳，单位为秒。
     * @param[in] write_rtc true 表示同步写回 PCF85063ATL。
     * @return ESP_OK 表示系统时间已更新。
     */
    esp_err_t system_time_apply_unix_time(system_time_source_t source,
                                          int64_t unix_seconds,
                                          bool write_rtc);

    /**
     * @brief 获取系统时间核心最近一次状态快照。
     *
     * getter 只复制内存快照，不访问 I2C，不启动 SNTP。
     *
     * @param[out] out 输出快照。
     * @return ESP_OK 表示复制成功。
     */
    esp_err_t system_time_get_snapshot(system_time_snapshot_t *out);

    /**
     * @brief 读取当前本地时间并格式化。
     *
     * 该接口只读当前系统时间，不触发 SNTP 或 RTC I2C。若系统时间仍不可信，
     * 返回 ESP_ERR_INVALID_STATE。
     *
     * @param[out] out 输出本地时间。
     * @return ESP_OK 表示成功。
     */
    esp_err_t system_time_get_local_time(system_time_local_t *out);

    /**
     * @brief 将系统时间来源转换成日志友好的文本。
     * @param[in] source 时间来源枚举。
     * @return 静态字符串。
     */
    const char *system_time_source_text(system_time_source_t source);

#ifdef __cplusplus
}
#endif
