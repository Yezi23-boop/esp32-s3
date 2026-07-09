#include "pcf85063atl.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "i2c_manager.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif

static const char *TAG = "pcf85063atl";

#define PCF85063ATL_I2C_ADDR_7BIT 0x51

#define PCF85063ATL_REG_CONTROL_2 0x01
#define PCF85063ATL_REG_SECONDS 0x04
#define PCF85063ATL_REG_TIMER_VALUE 0x10
#define PCF85063ATL_REG_TIMER_MODE 0x11

#define PCF85063ATL_CONTROL2_AF (1u << 6)
#define PCF85063ATL_CONTROL2_TF (1u << 3)

#define PCF85063ATL_SECONDS_OS (1u << 7)

/*
 * Timer_mode:
 * - TCF[1:0] = 10b 选择 1 Hz source clock；
 * - TE = 1 启动倒计时；
 * - TIE = 1 允许 timer interrupt；
 * - TI_TP = 0 让 INT 跟随 TF，便于 GPIO39 轮询看到低电平直到清标志。
 */
#define PCF85063ATL_TIMER_MODE_TCF_1HZ (2u << 3)
#define PCF85063ATL_TIMER_MODE_TE (1u << 2)
#define PCF85063ATL_TIMER_MODE_TIE (1u << 1)

static bool s_ready = false;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static i2c_master_dev_handle_t s_dev_handle = NULL;
#endif

static esp_err_t pcf85063atl_read_bytes(uint8_t reg, uint8_t *data, size_t len);
static esp_err_t pcf85063atl_write_byte(uint8_t reg, uint8_t value);
static esp_err_t pcf85063atl_write_bytes(uint8_t reg, const uint8_t *data,
                                         size_t len);

static uint8_t pcf85063atl_bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0FU));
}

static uint8_t pcf85063atl_dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

esp_err_t pcf85063atl_init(void)
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
        .device_address = PCF85063ATL_I2C_ADDR_7BIT,
        .scl_speed_hz = I2C_MANAGER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev_handle), TAG,
        "add pcf85063atl device failed");
#endif

    s_ready = true;
    return ESP_OK;
}

esp_err_t pcf85063atl_probe(bool *present)
{
    ESP_RETURN_ON_FALSE(present != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "present is null");
    *present = false;

    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG, "init failed before probe");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared i2c bus is not ready");

    esp_err_t ret = i2c_master_probe(bus_handle, PCF85063ATL_I2C_ADDR_7BIT, 50);
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
    esp_err_t ret = pcf85063atl_read_bytes(PCF85063ATL_REG_CONTROL_2, &value, 1);
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

esp_err_t pcf85063atl_read_time(pcf85063atl_time_t *time)
{
    ESP_RETURN_ON_FALSE(time != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "time is null");
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG, "init failed before read time");

    uint8_t raw[7] = {0};
    ESP_RETURN_ON_ERROR(
        pcf85063atl_read_bytes(PCF85063ATL_REG_SECONDS, raw, sizeof(raw)), TAG,
        "read time registers failed");

    time->oscillator_stopped = (raw[0] & PCF85063ATL_SECONDS_OS) != 0;
    time->seconds = pcf85063atl_bcd_to_dec(raw[0] & 0x7FU);
    time->minutes = pcf85063atl_bcd_to_dec(raw[1] & 0x7FU);
    time->hours = pcf85063atl_bcd_to_dec(raw[2] & 0x3FU);
    time->days = pcf85063atl_bcd_to_dec(raw[3] & 0x3FU);
    time->weekdays = pcf85063atl_bcd_to_dec(raw[4] & 0x07U);
    time->months = pcf85063atl_bcd_to_dec(raw[5] & 0x1FU);
    time->years = pcf85063atl_bcd_to_dec(raw[6]);
    return ESP_OK;
}

esp_err_t pcf85063atl_set_time(const pcf85063atl_time_t *time)
{
    ESP_RETURN_ON_FALSE(time != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "time is null");
    ESP_RETURN_ON_FALSE(time->seconds <= 59, ESP_ERR_INVALID_ARG, TAG,
                        "seconds out of range");
    ESP_RETURN_ON_FALSE(time->minutes <= 59, ESP_ERR_INVALID_ARG, TAG,
                        "minutes out of range");
    ESP_RETURN_ON_FALSE(time->hours <= 23, ESP_ERR_INVALID_ARG, TAG,
                        "hours out of range");
    ESP_RETURN_ON_FALSE(time->days >= 1 && time->days <= 31,
                        ESP_ERR_INVALID_ARG, TAG, "days out of range");
    ESP_RETURN_ON_FALSE(time->weekdays <= 6, ESP_ERR_INVALID_ARG, TAG,
                        "weekdays out of range");
    ESP_RETURN_ON_FALSE(time->months >= 1 && time->months <= 12,
                        ESP_ERR_INVALID_ARG, TAG, "months out of range");
    ESP_RETURN_ON_FALSE(time->years <= 99, ESP_ERR_INVALID_ARG, TAG,
                        "years out of range");
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before set time");

    uint8_t raw[7] = {
        (uint8_t)(pcf85063atl_dec_to_bcd(time->seconds) &
                  (uint8_t)~PCF85063ATL_SECONDS_OS),
        pcf85063atl_dec_to_bcd(time->minutes),
        pcf85063atl_dec_to_bcd(time->hours),
        pcf85063atl_dec_to_bcd(time->days),
        pcf85063atl_dec_to_bcd(time->weekdays),
        pcf85063atl_dec_to_bcd(time->months),
        pcf85063atl_dec_to_bcd(time->years),
    };

    return pcf85063atl_write_bytes(PCF85063ATL_REG_SECONDS, raw, sizeof(raw));
}

esp_err_t pcf85063atl_clear_oscillator_stopped(void)
{
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before clear oscillator flag");

    uint8_t seconds_raw = 0;
    ESP_RETURN_ON_ERROR(
        pcf85063atl_read_bytes(PCF85063ATL_REG_SECONDS, &seconds_raw, 1), TAG,
        "read seconds before clear os failed");

    seconds_raw &= (uint8_t)~PCF85063ATL_SECONDS_OS;
    return pcf85063atl_write_byte(PCF85063ATL_REG_SECONDS, seconds_raw);
}

esp_err_t pcf85063atl_read_status(pcf85063atl_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "status is null");
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before read status");

    uint8_t seconds_raw = 0;
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(
        pcf85063atl_read_bytes(PCF85063ATL_REG_SECONDS, &seconds_raw, 1), TAG,
        "read seconds status failed");
    ESP_RETURN_ON_ERROR(
        pcf85063atl_read_bytes(PCF85063ATL_REG_CONTROL_2, &control2, 1), TAG,
        "read control2 status failed");

    status->oscillator_stopped =
        (seconds_raw & PCF85063ATL_SECONDS_OS) != 0;
    status->alarm_flag = (control2 & PCF85063ATL_CONTROL2_AF) != 0;
    status->timer_flag = (control2 & PCF85063ATL_CONTROL2_TF) != 0;
    status->control2 = control2;
    status->seconds_raw = seconds_raw;
    return ESP_OK;
}

esp_err_t pcf85063atl_read_control2(uint8_t *control2)
{
    ESP_RETURN_ON_FALSE(control2 != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "control2 is null");
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before control2 read");

    return pcf85063atl_read_bytes(PCF85063ATL_REG_CONTROL_2, control2, 1);
}

esp_err_t pcf85063atl_clear_interrupt_flags(void)
{
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(pcf85063atl_read_control2(&control2), TAG,
                        "read control2 before clear failed");

    control2 &= (uint8_t) ~(PCF85063ATL_CONTROL2_AF | PCF85063ATL_CONTROL2_TF);
    return pcf85063atl_write_byte(PCF85063ATL_REG_CONTROL_2, control2);
}

esp_err_t pcf85063atl_arm_countdown_timer(uint8_t seconds)
{
    ESP_RETURN_ON_FALSE(seconds > 0, ESP_ERR_INVALID_ARG, TAG,
                        "seconds must be > 0");
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before arm timer");

    /*
     * 先停 timer、装载倒计时、清旧标志，再启动 1 Hz level interrupt。
     * 这样 GPIO39 证据只对应本次 arm，不会被历史 TF/AF 污染。
     */
    ESP_RETURN_ON_ERROR(pcf85063atl_write_byte(PCF85063ATL_REG_TIMER_MODE, 0),
                        TAG, "stop timer failed");
    ESP_RETURN_ON_ERROR(
        pcf85063atl_write_byte(PCF85063ATL_REG_TIMER_VALUE, seconds), TAG,
        "write timer value failed");
    ESP_RETURN_ON_ERROR(pcf85063atl_clear_interrupt_flags(), TAG,
                        "clear interrupt flags failed");
    return pcf85063atl_write_byte(
        PCF85063ATL_REG_TIMER_MODE,
        PCF85063ATL_TIMER_MODE_TCF_1HZ | PCF85063ATL_TIMER_MODE_TE |
            PCF85063ATL_TIMER_MODE_TIE);
}

esp_err_t pcf85063atl_stop_countdown_timer(void)
{
    ESP_RETURN_ON_ERROR(pcf85063atl_init(), TAG,
                        "init failed before stop timer");
    return pcf85063atl_write_byte(PCF85063ATL_REG_TIMER_MODE, 0);
}

static esp_err_t pcf85063atl_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "pcf85063atl device not ready");
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len, 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063ATL_I2C_ADDR_7BIT << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063ATL_I2C_ADDR_7BIT << 1) | I2C_MASTER_READ,
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

static esp_err_t pcf85063atl_write_byte(uint8_t reg, uint8_t value)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "pcf85063atl device not ready");

    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(s_dev_handle, buffer, sizeof(buffer), 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063ATL_I2C_ADDR_7BIT << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

static esp_err_t pcf85063atl_write_bytes(uint8_t reg, const uint8_t *data,
                                         size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "pcf85063atl device not ready");

    uint8_t buffer[8] = {0};
    ESP_RETURN_ON_FALSE(len <= (sizeof(buffer) - 1U), ESP_ERR_INVALID_SIZE,
                        TAG, "write len too large");
    buffer[0] = reg;
    memcpy(&buffer[1], data, len);
    return i2c_master_transmit(s_dev_handle, buffer, len + 1U, 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063ATL_I2C_ADDR_7BIT << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    for (size_t index = 0; index < len; ++index)
    {
        i2c_master_write_byte(cmd, data[index], true);
    }
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}
