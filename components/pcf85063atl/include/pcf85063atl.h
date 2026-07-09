#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        bool oscillator_stopped; /**< true 表示 RTC 曾经停振，当前时间不可信。 */
        uint8_t seconds;         /**< BCD 解码后的秒，范围 0-59。 */
        uint8_t minutes;         /**< BCD 解码后的分，范围 0-59。 */
        uint8_t hours;           /**< BCD 解码后的小时，24 小时制，范围 0-23。 */
        uint8_t days;            /**< BCD 解码后的日期，范围 1-31。 */
        uint8_t weekdays;        /**< 星期字段，按芯片寄存器原始定义解码。 */
        uint8_t months;          /**< BCD 解码后的月份，范围 1-12。 */
        uint8_t years;           /**< BCD 解码后的年份低两位。 */
    } pcf85063atl_time_t;

    typedef struct
    {
        bool oscillator_stopped; /**< true 表示 seconds 寄存器 OS 位为 1，RTC 时间不可信。 */
        bool alarm_flag;         /**< true 表示 Control_2 中 AF 标志置位。 */
        bool timer_flag;         /**< true 表示 Control_2 中 TF 标志置位。 */
        uint8_t control2;        /**< Control_2 原始寄存器值，用于日志和问题定位。 */
        uint8_t seconds_raw;     /**< seconds 原始寄存器值，保留 OS 位和 BCD 秒值。 */
    } pcf85063atl_status_t;

    /**
     * @brief 初始化 PCF85063ATL 最小驱动并绑定共享 I2C 设备句柄。
     * @return ESP_OK 表示成功或之前已初始化；其他错误表示 I2C 设备创建失败。
     */
    esp_err_t pcf85063atl_init(void);

    /**
     * @brief 探测 PCF85063ATL 是否应答。
     * @param[out] present true 表示探测到器件。
     * @return ESP_OK 表示探测流程完成；其他错误表示总线访问失败。
     */
    esp_err_t pcf85063atl_probe(bool *present);

    /**
     * @brief 连续读取 RTC 时间寄存器。
     *
     * 该接口只用于证据采集，不接管系统时间。若 `oscillator_stopped=true`，
     * 调用方必须把读数视为“不可信但可通信”。
     *
     * @param[out] time 输出解码后的时间字段。
     * @return ESP_OK 表示读取成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_read_time(pcf85063atl_time_t *time);

    /**
     * @brief 连续写入 RTC 时间寄存器，并清除 seconds 寄存器 OS 位。
     *
     * 该接口只负责把上层已经判定可信的时间写入芯片。写入 seconds 时会
     * 固定清掉 bit7 OS，表示 RTC 已重新获得可信时间来源。
     *
     * @param[in] time 要写入的时间字段，年份为 2000-2099 的低两位。
     * @return ESP_OK 表示写入成功；ESP_ERR_INVALID_ARG 表示字段越界；
     *         其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_set_time(const pcf85063atl_time_t *time);

    /**
     * @brief 只清除 seconds 寄存器中的 oscillator stopped 标志。
     *
     * 该接口不修改任何日历字段，只在调用方已经确认当前 RTC 时间可信时使用。
     * 普通联网授时应优先调用 `pcf85063atl_set_time()` 一次性写入可信时间。
     *
     * @return ESP_OK 表示清除成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_clear_oscillator_stopped(void);

    /**
     * @brief 读取 RTC 关键状态位和原始诊断寄存器。
     *
     * 上层可用 `oscillator_stopped` 判断当前 RTC 时间是否可信，用 `timer_flag`
     * 和 `alarm_flag` 判断 INT 低电平是否来自 RTC 内部标志。
     *
     * @param[out] status 输出状态快照。
     * @return ESP_OK 表示读取成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_read_status(pcf85063atl_status_t *status);

    /**
     * @brief 读取 Control_2 原始寄存器，观察 AF/TF 等中断标志。
     * @param[out] control2 输出 Control_2 原始值。
     * @return ESP_OK 表示读取成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_read_control2(uint8_t *control2);

    /**
     * @brief 清除 Control_2 中的 alarm/timer 标志位。
     * @return ESP_OK 表示写入成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_clear_interrupt_flags(void);

    /**
     * @brief 配置一次 RTC 倒计时中断。
     *
     * 第一阶段使用 countdown timer 做 `RTC_INT(GPIO39)` 证据闭环：
     * 不依赖 RTC 当前时间是否正确，也不进入 ESP light/deep sleep。
     *
     * @param[in] seconds 倒计时秒数，范围 1-255。
     * @return ESP_OK 表示配置成功；其他错误表示参数或 I2C 访问失败。
     */
    esp_err_t pcf85063atl_arm_countdown_timer(uint8_t seconds);

    /**
     * @brief 停止 RTC countdown timer。
     *
     * 仅关闭 timer 模式寄存器，不主动清除 Control_2 中的 AF/TF 标志。
     * 调用方若已经观测到 TF，应先清标志再停止，避免 INT 保持低电平。
     *
     * @return ESP_OK 表示停止命令写入成功；其他错误表示 I2C 访问失败。
     */
    esp_err_t pcf85063atl_stop_countdown_timer(void);

#ifdef __cplusplus
}
#endif
