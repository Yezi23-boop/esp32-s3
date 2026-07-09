#include "axp2101.h"

#include <stddef.h>

#include "axp2101_regs.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "i2c_manager.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif

static const char *TAG = "axp2101";

static bool s_ready = false;             // 驱动是否已初始化。
static bool s_voltage_adc_ready = false; // ADC 通道是否已完成能力使能。
static uint8_t s_voltage_adc_mask = 0;   // 最近一次写入的 ADC 通道掩码。

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static i2c_master_dev_handle_t s_dev_handle = NULL; // IDF 5.3+ 下的 I2C 设备句柄。
#endif

static esp_err_t axp2101_read_bytes(uint8_t reg, uint8_t *data, size_t len);
static esp_err_t axp2101_write_bytes(uint8_t reg, const uint8_t *data, size_t len);
static esp_err_t axp2101_ensure_voltage_adc_channels(void);
static uint16_t axp2101_decode_h5l8(const uint8_t *data);
static uint16_t axp2101_decode_h6l8(const uint8_t *data);

/**
 * @brief 初始化 AXP2101 驱动。
 * @return `ESP_OK` 表示成功或之前已初始化；其他错误表示 I2C 设备创建失败。
 */
esp_err_t axp2101_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_manager_init(), TAG, "i2c manager init failed");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared i2c bus is not ready");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR_7BIT,
        .scl_speed_hz = I2C_MANAGER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev_handle), TAG,
        "add axp2101 device failed");
#endif

    s_ready = true;
    return ESP_OK;
}

/**
 * @brief 探测 AXP2101 是否应答。
 * @param[out] present true 表示探测到器件。
 * @return `ESP_OK` 表示探测流程完成；其他错误表示总线访问失败。
 */
esp_err_t axp2101_probe(bool *present)
{
    ESP_RETURN_ON_FALSE(present != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "present is null");
    *present = false;

    ESP_RETURN_ON_ERROR(axp2101_init(), TAG, "init failed before probe");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared i2c bus is not ready");

    esp_err_t ret = i2c_master_probe(bus_handle, AXP2101_I2C_ADDR_7BIT, 50);
    if (ret == ESP_OK)
    {
        *present = true;
        return ESP_OK;
    }
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_FAIL)
    {
        return ESP_OK;
    }
    return ret;
#else
    uint8_t value = 0;
    esp_err_t ret = axp2101_read_bytes(AXP2101_REG_STATUS0, &value, 1);
    if (ret == ESP_OK)
    {
        *present = true;
        return ESP_OK;
    }
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_FAIL)
    {
        return ESP_OK;
    }
    return ret;
#endif
}

/**
 * @brief 读取一次 AXP2101 电源快照。
 * @param[out] snapshot 输出快照。
 * @return `ESP_OK` 表示成功；其他错误表示寄存器读取失败。
 */
esp_err_t axp2101_read_snapshot(axp2101_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "snapshot is null");
    ESP_RETURN_ON_ERROR(axp2101_init(), TAG, "init failed before snapshot");

    axp2101_snapshot_t out = {0};
    uint8_t raw[2] = {0}; // 双字节电压原始数据缓存。
    uint8_t status0 = 0;  // STATUS0 原始值。
    uint8_t status2 = 0;  // STATUS2 原始值。
    uint8_t percent = 0;  // 电量百分比原始值。

    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_STATUS0, &status0, 1),
                        TAG, "read status0 failed");
    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_STATUS2, &status2, 1),
                        TAG, "read status2 failed");

    out.vbus_good = (status0 & AXP2101_STATUS0_VBUS_GOOD) != 0;
    out.battery_present = (status0 & AXP2101_STATUS0_BATTERY_PRESENT) != 0;
    out.battfet_on = (status0 & AXP2101_STATUS0_BATFET_ON) != 0;
    uint8_t bat_dir = (status2 >> AXP2101_STATUS2_BAT_DIR_SHIFT) &
                      AXP2101_STATUS2_BAT_DIR_MASK;
    out.charging = bat_dir == AXP2101_STATUS2_BAT_DIR_CHARGE;
    out.discharging = bat_dir == AXP2101_STATUS2_BAT_DIR_DISCHARGE;

    ESP_RETURN_ON_ERROR(axp2101_ensure_voltage_adc_channels(), TAG,
                        "enable voltage adc channels failed");

    if (out.battery_present)
    {
        ESP_RETURN_ON_ERROR(
            axp2101_read_bytes(AXP2101_REG_BATTERY_H, raw, sizeof(raw)), TAG,
            "read battery voltage failed");
        out.battery_mv = axp2101_decode_h5l8(raw);
        ESP_RETURN_ON_ERROR(
            axp2101_read_bytes(AXP2101_REG_BAT_PERCENT, &percent, 1), TAG,
            "read battery percentage failed");
        out.battery_percent = (int8_t)percent;
    }
    else
    {
        out.battery_mv = 0;
        out.battery_percent = -1;
    }

    if (out.vbus_good)
    {
        ESP_RETURN_ON_ERROR(
            axp2101_read_bytes(AXP2101_REG_VBUS_H, raw, sizeof(raw)), TAG,
            "read vbus voltage failed");
        out.vbus_mv = axp2101_decode_h6l8(raw);
    }
    else
    {
        out.vbus_mv = 0;
    }

    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_VSYS_H, raw, sizeof(raw)),
                        TAG, "read vsys voltage failed");
    out.vsys_mv = axp2101_decode_h6l8(raw);

    *snapshot = out;
    return ESP_OK;
}

/**
 * @brief 读取 IRQ 状态寄存器。
 * @param[out] status 输出 IRQ 原始状态。
 * @return `ESP_OK` 表示成功；其他错误表示寄存器读取失败。
 */
esp_err_t axp2101_read_irq_status(axp2101_irq_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "status is null");
    ESP_RETURN_ON_ERROR(axp2101_init(), TAG, "init failed before irq read");

    axp2101_irq_status_t out = {0};
    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_IRQ0, &out.irq0, 1), TAG,
                        "read irq0 failed");
    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_IRQ1, &out.irq1, 1), TAG,
                        "read irq1 failed");
    ESP_RETURN_ON_ERROR(axp2101_read_bytes(AXP2101_REG_IRQ2, &out.irq2, 1), TAG,
                        "read irq2 failed");

    *status = out;
    return ESP_OK;
}

/**
 * @brief 清除 IRQ 状态寄存器。
 * @param[in] status 待清除的 IRQ 原始状态。
 * @return `ESP_OK` 表示成功；其他错误表示寄存器写入失败。
 */
esp_err_t axp2101_clear_irq_status(const axp2101_irq_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "status is null");
    ESP_RETURN_ON_ERROR(axp2101_init(), TAG, "init failed before irq clear");

    uint8_t irq0 = status->irq0;
    uint8_t irq1 = status->irq1;
    uint8_t irq2 = status->irq2;

    ESP_RETURN_ON_ERROR(axp2101_write_bytes(AXP2101_REG_IRQ0, &irq0, 1), TAG,
                        "clear irq0 failed");
    ESP_RETURN_ON_ERROR(axp2101_write_bytes(AXP2101_REG_IRQ1, &irq1, 1), TAG,
                        "clear irq1 failed");
    ESP_RETURN_ON_ERROR(axp2101_write_bytes(AXP2101_REG_IRQ2, &irq2, 1), TAG,
                        "clear irq2 failed");

    return ESP_OK;
}

/**
 * @brief 从 AXP2101 读取连续寄存器。
 * @param[in] reg 起始寄存器地址。
 * @param[out] data 输出缓冲区。
 * @param[in] len 读取长度，单位为字节。
 * @return `ESP_OK` 表示成功；其他错误表示 I2C 访问失败。
 */
static esp_err_t axp2101_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "axp2101 device not ready");
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len, 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP2101_I2C_ADDR_7BIT << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP2101_I2C_ADDR_7BIT << 1) | I2C_MASTER_READ,
                          true);
    if (len > 1)
    {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

/**
 * @brief 向 AXP2101 写入寄存器。
 * @param[in] reg 目标寄存器地址。
 * @param[in] data 待写入数据。
 * @param[in] len 写入长度，单位为字节。
 * @return `ESP_OK` 表示成功；其他错误表示 I2C 访问失败或当前写入长度不受支持。
 *
 * @note 当前实现仅支持单字节写入，因为当前主链路只需要配置少量控制寄存器。
 */
static esp_err_t axp2101_write_bytes(uint8_t reg, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");
    ESP_RETURN_ON_FALSE(len == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "only single-byte writes are supported");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "axp2101 device not ready");

    uint8_t buffer[2] = {reg, data[0]};
    return i2c_master_transmit(s_dev_handle, buffer, sizeof(buffer), 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AXP2101_I2C_ADDR_7BIT << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data[0], true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

/**
 * @brief 确保电压相关 ADC 通道已开启。
 * @return `ESP_OK` 表示成功；其他错误表示寄存器读写失败。
 */
static esp_err_t axp2101_ensure_voltage_adc_channels(void)
{
    uint8_t current = 0; // 当前 ADC 通道配置。
    uint8_t desired = 0; // 期望 ADC 通道配置，需补齐 BAT/VBUS/VSYS。

    if (s_voltage_adc_ready &&
        (s_voltage_adc_mask & AXP2101_ADC_CHANNEL_VOLTAGE_MASK) ==
            AXP2101_ADC_CHANNEL_VOLTAGE_MASK)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        axp2101_read_bytes(AXP2101_REG_ADC_CHANNEL_CTRL, &current, 1), TAG,
        "read adc channel ctrl failed");
    desired = current | AXP2101_ADC_CHANNEL_VOLTAGE_MASK;
    if (desired != current)
    {
        ESP_RETURN_ON_ERROR(axp2101_write_bytes(AXP2101_REG_ADC_CHANNEL_CTRL,
                                                &desired, 1),
                            TAG, "write adc channel ctrl failed");
    }

    s_voltage_adc_mask = desired;
    s_voltage_adc_ready = true;
    return ESP_OK;
}

/**
 * @brief 解析 H5L8 格式电压寄存器值。
 * @param[in] data 两字节原始寄存器数据。
 * @return 解码后的原始数值。
 */
static uint16_t axp2101_decode_h5l8(const uint8_t *data)
{
    uint16_t raw = (((uint16_t)(data[0] & 0x1Fu)) << 8) | (uint16_t)data[1];
    return raw;
}

/**
 * @brief 解析 H6L8 格式电压寄存器值。
 * @param[in] data 两字节原始寄存器数据。
 * @return 解码后的原始数值。
 */
static uint16_t axp2101_decode_h6l8(const uint8_t *data)
{
    uint16_t raw = (((uint16_t)(data[0] & 0x3Fu)) << 8) | (uint16_t)data[1];
    return raw;
}
