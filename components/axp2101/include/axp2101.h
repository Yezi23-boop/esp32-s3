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
        bool vbus_good;         /**< 外部供电是否有效。 */
        bool battery_present;   /**< 电池是否在位。 */
        bool battfet_on;        /**< BATFET 导通状态。 */
        bool charging;          /**< 充电方向判定。 */
        bool discharging;       /**< 放电方向判定。 */
        uint16_t battery_mv;    /**< 电池电压，单位为 mV。 */
        uint16_t vbus_mv;       /**< VBUS 电压，单位为 mV。 */
        uint16_t vsys_mv;       /**< 系统电压，单位为 mV。 */
        int8_t battery_percent; /**< 电量百分比，`-1` 表示未知。 */
    } axp2101_snapshot_t;

    typedef struct
    {
        uint8_t irq0; /**< IRQ bank0 原始位图。 */
        uint8_t irq1; /**< IRQ bank1 原始位图。 */
        uint8_t irq2; /**< IRQ bank2 原始位图。 */
    } axp2101_irq_status_t;

    /**
     * @brief 初始化 AXP2101 驱动并绑定共享 I2C 设备句柄。
     * @return `ESP_OK` 表示成功或之前已初始化；其他错误表示 I2C 设备创建失败。
     */
    esp_err_t axp2101_init(void);

    /**
     * @brief 探测 AXP2101 是否应答。
     * @param[out] present true 表示探测到器件。
     * @return `ESP_OK` 表示探测流程完成；其他错误表示总线访问失败。
     */
    esp_err_t axp2101_probe(bool *present);

    /**
     * @brief 读取一次电源快照。
     * @param[out] snapshot 输出快照。
     * @return `ESP_OK` 表示成功；其他错误表示寄存器读取失败。
     */
    esp_err_t axp2101_read_snapshot(axp2101_snapshot_t *snapshot);

    /**
     * @brief 读取 IRQ 状态寄存器。
     * @param[out] status 输出 IRQ 原始状态。
     * @return `ESP_OK` 表示成功；其他错误表示寄存器读取失败。
     */
    esp_err_t axp2101_read_irq_status(axp2101_irq_status_t *status);

    /**
     * @brief 清除 IRQ 状态寄存器。
     * @param[in] status 待清除的 IRQ 原始状态。
     * @return `ESP_OK` 表示成功；其他错误表示寄存器写入失败。
     *
     * @note 该操作不是原子的：每个 IRQ bank 都通过独立 RW1C 写入清除，
     *       若后续 bank 写失败，前面的 bank 可能已经被清掉。
     */
    esp_err_t axp2101_clear_irq_status(const axp2101_irq_status_t *status);

#ifdef __cplusplus
}
#endif
