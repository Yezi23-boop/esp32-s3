#include "qmi8658c.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "i2c_manager.h"
#include "qmi8658c_regs.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif

static const char *TAG = "qmi8658c";

static const float k_standard_gravity = 9.80665f;

static bool s_ready = false; // 组件是否已挂到共享 I2C；不表示芯片身份已验证。
static uint8_t s_i2c_addr_7bit =
    QMI8658C_I2C_ADDR_7BIT; // 板级 SA0 绑法决定；默认兼容当前开发板。
static uint8_t s_accel_fs_code = 0; // 最近一次配置的加速度量程；0 为 ±2g。
static uint8_t s_gyro_fs_code = 0;  // 最近一次配置的陀螺仪量程；0 为 ±16 dps。
static bool s_sample_configured = false; // true 表示已通过 qmi8658c_config() 建立物理量换算口径。
static uint32_t s_read_count = 0; // 调试用读数计数，按 2s 节流打印采样寄存器状态。

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static i2c_master_dev_handle_t s_dev_handle = NULL; // 共享 master bus 下的 QMI8658C 设备句柄。
#endif

typedef struct
{
    uint8_t status0;
    uint32_t timestamp;
    int16_t temperature_raw;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} qmi8658c_raw_sample_t;

static esp_err_t qmi8658c_read_bytes(uint8_t reg, uint8_t *data, size_t len);
static esp_err_t qmi8658c_write_bytes(uint8_t reg, const uint8_t *data,
                                      size_t len);
static esp_err_t qmi8658c_write_byte(uint8_t reg, uint8_t value);
static esp_err_t qmi8658c_read_raw(qmi8658c_raw_sample_t *sample);
static bool qmi8658c_accel_odr_code_valid(uint8_t odr);
static bool qmi8658c_gyro_odr_code_valid(uint8_t odr);
static int32_t qmi8658c_accel_lsb_per_g(uint8_t accel_fs);
static float qmi8658c_accel_raw_to_mps2(int16_t raw, uint8_t accel_fs);
static esp_err_t qmi8658c_convert_raw_accel(
    const qmi8658c_raw_sample_t *raw,
    qmi8658c_accel_t *sample);
static int32_t qmi8658c_gyro_lsb_per_dps(uint8_t gyro_fs);
static float qmi8658c_gyro_raw_to_dps(int16_t raw, uint8_t gyro_fs);
static esp_err_t qmi8658c_convert_raw_gyro(
    const qmi8658c_raw_sample_t *raw,
    qmi8658c_gyro_t *sample);
static int16_t qmi8658c_decode_i16(const uint8_t *data);
static uint32_t qmi8658c_decode_u24(const uint8_t *data);

esp_err_t qmi8658c_init(void)
{
    const qmi8658c_bus_t config = {
        .addr = QMI8658C_I2C_ADDR_7BIT,
    };
    return qmi8658c_init_bus(&config);
}

esp_err_t qmi8658c_init_bus(const qmi8658c_bus_t *config)
{
    if (s_ready)
    {
        ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                            "bus config is null");
        ESP_RETURN_ON_FALSE(config->addr == s_i2c_addr_7bit,
                            ESP_ERR_INVALID_STATE, TAG,
                            "qmi8658c already initialized with another addr");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bus config is null");
    ESP_RETURN_ON_FALSE(config->addr <= 0x7F, ESP_ERR_INVALID_ARG,
                        TAG, "i2c addr is not 7-bit");
    s_i2c_addr_7bit = config->addr;

    ESP_RETURN_ON_ERROR(i2c_manager_init(), TAG, "i2c manager init failed");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared i2c bus is not ready");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_i2c_addr_7bit,
        .scl_speed_hz = I2C_MANAGER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev_handle), TAG,
        "add qmi8658c device failed");
#endif

    s_ready = true;
    return ESP_OK;
}

esp_err_t qmi8658c_probe(qmi8658c_info_t *identity)
{
    ESP_RETURN_ON_FALSE(identity != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "identity is null");
    memset(identity, 0, sizeof(*identity));

    ESP_RETURN_ON_ERROR(qmi8658c_init(), TAG, "init failed before probe");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_handle_t bus_handle = i2c_manager_get_bus_handle();
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "shared i2c bus is not ready");

    esp_err_t ret =
        i2c_master_probe(bus_handle, s_i2c_addr_7bit, 50);
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_FAIL)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "qmi8658c address probe failed");
#else
    uint8_t value = 0;
    esp_err_t ret = qmi8658c_read_bytes(QMI8658C_REG_WHO_AM_I, &value, 1);
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_FAIL)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "qmi8658c who_am_i probe failed");
#endif

    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_WHO_AM_I,
                                            &identity->who_am_i, 1),
                        TAG, "read who_am_i failed");
    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_REVISION_ID,
                                            &identity->revision_id, 1),
                        TAG, "read revision_id failed");

    identity->present = identity->who_am_i == QMI8658C_WHO_AM_I_EXPECTED;
    if (!identity->present)
    {
        ESP_LOGW(TAG, "unexpected WHO_AM_I=0x%02X revision=0x%02X",
                 identity->who_am_i, identity->revision_id);
    }
    return ESP_OK;
}

esp_err_t qmi8658c_config(const qmi8658c_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "config is null");
    ESP_RETURN_ON_FALSE(
        config->accel_fs <= QMI8658C_CTRL2_ACCEL_FS_MASK,
        ESP_ERR_INVALID_ARG, TAG, "accel full-scale code out of range");
    ESP_RETURN_ON_FALSE(qmi8658c_accel_lsb_per_g(config->accel_fs) > 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "accel full-scale code has no mps2 scale");
    ESP_RETURN_ON_FALSE(qmi8658c_accel_odr_code_valid(config->accel_odr),
                        ESP_ERR_INVALID_ARG, TAG,
                        "accel odr code is not supported by datasheet");
    ESP_RETURN_ON_FALSE(config->gyro_fs <= QMI8658C_CTRL3_GYRO_FS_MASK,
                        ESP_ERR_INVALID_ARG, TAG,
                        "gyro full-scale code out of range");
    ESP_RETURN_ON_FALSE(qmi8658c_gyro_lsb_per_dps(config->gyro_fs) > 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "gyro full-scale code has no dps scale");
    ESP_RETURN_ON_FALSE(config->gyro_odr <= QMI8658C_CTRL3_GYRO_ODR_MASK,
                        ESP_ERR_INVALID_ARG, TAG,
                        "gyro odr code out of range");
    ESP_RETURN_ON_FALSE(qmi8658c_gyro_odr_code_valid(config->gyro_odr),
                        ESP_ERR_INVALID_ARG, TAG,
                        "gyro odr code is not supported by datasheet");
    ESP_RETURN_ON_FALSE(config->int1_source == QMI8658C_INT_SOURCE_DISABLED,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "int1 source is reserved and must be disabled");
    ESP_RETURN_ON_FALSE(config->int2_source == QMI8658C_INT_SOURCE_DISABLED,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "int2 source is reserved and must be disabled");
    ESP_RETURN_ON_ERROR(qmi8658c_init(), TAG, "init failed before configure");

    /*
     * MCU reset 不一定会让 QMI8658C 断电复位。先关闭 accel/gyro，再写
     * 量程/ODR，避免芯片仍处在旧 enable 状态时忽略 full-scale 变更。
     */
    ESP_RETURN_ON_ERROR(qmi8658c_write_byte(QMI8658C_REG_CTRL7, 0), TAG,
                        "disable sensors before configure failed");

    /*
     * 对齐 Waveshare 官方 qmi8658_init()：CTRL1=0x60。
     * 只写 bit6 地址自增在当前板上会出现样本寄存器冻结。
     */
    ESP_RETURN_ON_ERROR(
        qmi8658c_write_byte(QMI8658C_REG_CTRL1,
                            QMI8658C_CTRL1_WAVESHARE_DEFAULT),
        TAG, "write ctrl1 failed");

    uint8_t ctrl2 =
        (uint8_t)((config->accel_fs << 4) | config->accel_odr);
    uint8_t ctrl3 = (uint8_t)((config->gyro_fs << 4) | config->gyro_odr);
    uint8_t ctrl7 = 0;
    if (config->accel_enable)
    {
        ctrl7 |= QMI8658C_CTRL7_ACCEL_ENABLE;
    }
    if (config->gyro_enable)
    {
        ctrl7 |= QMI8658C_CTRL7_GYRO_ENABLE;
    }

    ESP_RETURN_ON_ERROR(qmi8658c_write_byte(QMI8658C_REG_CTRL2, ctrl2), TAG,
                        "write ctrl2 failed");
    ESP_RETURN_ON_ERROR(qmi8658c_write_byte(QMI8658C_REG_CTRL3, ctrl3), TAG,
                        "write ctrl3 failed");
    ESP_RETURN_ON_ERROR(
        qmi8658c_write_byte(QMI8658C_REG_CTRL5,
                            QMI8658C_CTRL5_WAVESHARE_DEFAULT),
        TAG, "write ctrl5 failed");
    ESP_RETURN_ON_ERROR(qmi8658c_write_byte(QMI8658C_REG_CTRL7, ctrl7), TAG,
                        "write ctrl7 failed");
    s_accel_fs_code = config->accel_fs;
    s_gyro_fs_code = config->gyro_fs;
    s_sample_configured = true;
    s_read_count = 0;
    ESP_LOGI(TAG, "configured registers: ctrl1=0x%02X ctrl2=0x%02X ctrl3=0x%02X ctrl5=0x%02X ctrl7=0x%02X",
             QMI8658C_CTRL1_WAVESHARE_DEFAULT, ctrl2, ctrl3,
             QMI8658C_CTRL5_WAVESHARE_DEFAULT, ctrl7);
    return ESP_OK;
}

static esp_err_t qmi8658c_read_raw(qmi8658c_raw_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "sample is null");
    ESP_RETURN_ON_ERROR(qmi8658c_init(), TAG, "init failed before raw read");

    uint8_t status0 = 0;
    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_STATUS0, &status0, 1),
                        TAG, "read status0 failed");

    uint8_t timestamp_raw[3] = {0};
    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_TIMESTAMP_L,
                                            timestamp_raw,
                                            sizeof(timestamp_raw)),
                        TAG, "read timestamp failed");

    uint8_t temperature_raw[2] = {0};
    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_TEMP_L,
                                            temperature_raw,
                                            sizeof(temperature_raw)),
                        TAG, "read temperature failed");

    uint8_t raw[12] = {0}; // accel(6) + gyro(6)，从 AX_L 开始对齐 Waveshare 官方读法。
    ESP_RETURN_ON_ERROR(qmi8658c_read_bytes(QMI8658C_REG_AX_L, raw, sizeof(raw)),
                        TAG, "read sensor sample failed");

    qmi8658c_raw_sample_t out = {
        .status0 = status0,
        .timestamp = qmi8658c_decode_u24(timestamp_raw),
        .temperature_raw = qmi8658c_decode_i16(temperature_raw),
        .accel_x = qmi8658c_decode_i16(&raw[0]),
        .accel_y = qmi8658c_decode_i16(&raw[2]),
        .accel_z = qmi8658c_decode_i16(&raw[4]),
        .gyro_x = qmi8658c_decode_i16(&raw[6]),
        .gyro_y = qmi8658c_decode_i16(&raw[8]),
        .gyro_z = qmi8658c_decode_i16(&raw[10]),
    };
    *sample = out;

    ++s_read_count;
    if (s_read_count <= 5U || (s_read_count % 100U) == 0U)
    {
        ESP_LOGI(TAG,
                 "原始表 | 次数=%-5u 状态0=0x%02X 就绪=%d 时间戳=%-8u | 加速度raw[x y z]=%6d %6d %6d | 陀螺仪raw[x y z]=%6d %6d %6d | 温度raw=%d",
                 (unsigned)s_read_count,
                 out.status0,
                 (out.status0 & 0x03U) != 0U ? 1 : 0,
                 (unsigned)out.timestamp,
                 out.accel_x,
                 out.accel_y,
                 out.accel_z,
                 out.gyro_x,
                 out.gyro_y,
                 out.gyro_z,
                 out.temperature_raw);
    }
    return ESP_OK;
}

static esp_err_t qmi8658c_convert_raw_accel(
    const qmi8658c_raw_sample_t *raw,
    qmi8658c_accel_t *sample)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "raw sample is null");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "accel mps2 sample is null");

    const int32_t lsb_per_g = qmi8658c_accel_lsb_per_g(s_accel_fs_code);
    ESP_RETURN_ON_FALSE(lsb_per_g > 0, ESP_ERR_INVALID_STATE, TAG,
                        "accel full-scale code has no mps2 scale");

    sample->x = qmi8658c_accel_raw_to_mps2(raw->accel_x, s_accel_fs_code);
    sample->y = qmi8658c_accel_raw_to_mps2(raw->accel_y, s_accel_fs_code);
    sample->z = qmi8658c_accel_raw_to_mps2(raw->accel_z, s_accel_fs_code);
    return ESP_OK;
}

static esp_err_t qmi8658c_convert_raw_gyro(
    const qmi8658c_raw_sample_t *raw,
    qmi8658c_gyro_t *sample)
{
    ESP_RETURN_ON_FALSE(raw != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "raw sample is null");
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "gyro dps sample is null");

    const int32_t lsb_per_dps = qmi8658c_gyro_lsb_per_dps(s_gyro_fs_code);
    ESP_RETURN_ON_FALSE(lsb_per_dps > 0, ESP_ERR_INVALID_STATE, TAG,
                        "gyro full-scale code has no dps scale");

    sample->x = qmi8658c_gyro_raw_to_dps(raw->gyro_x, s_gyro_fs_code);
    sample->y = qmi8658c_gyro_raw_to_dps(raw->gyro_y, s_gyro_fs_code);
    sample->z = qmi8658c_gyro_raw_to_dps(raw->gyro_z, s_gyro_fs_code);
    return ESP_OK;
}

esp_err_t qmi8658c_read(qmi8658c_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "physical sample is null");
    ESP_RETURN_ON_FALSE(s_sample_configured, ESP_ERR_INVALID_STATE, TAG,
                        "qmi8658c sample range is not configured");

    qmi8658c_raw_sample_t raw = {0};
    ESP_RETURN_ON_ERROR(qmi8658c_read_raw(&raw), TAG,
                        "read raw before physical sample failed");
    ESP_RETURN_ON_ERROR(qmi8658c_convert_raw_accel(&raw, &sample->accel),
                        TAG, "convert accel mps2 failed");
    return qmi8658c_convert_raw_gyro(&raw, &sample->gyro);
}

static esp_err_t qmi8658c_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "len must be > 0");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "qmi8658c device not ready");
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len, 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr_7bit << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr_7bit << 1) | I2C_MASTER_READ,
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

static esp_err_t qmi8658c_write_byte(uint8_t reg, uint8_t value)
{
    return qmi8658c_write_bytes(reg, &value, 1);
}

static esp_err_t qmi8658c_write_bytes(uint8_t reg, const uint8_t *data,
                                      size_t len)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "write data is null");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG,
                        "write len must be > 0");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    ESP_RETURN_ON_FALSE(s_dev_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "qmi8658c device not ready");

    uint8_t buffer[17] = {0};
    ESP_RETURN_ON_FALSE(len < sizeof(buffer), ESP_ERR_INVALID_ARG, TAG,
                        "write len too large");
    buffer[0] = reg;
    memcpy(&buffer[1], data, len);
    return i2c_master_transmit(s_dev_handle, buffer, len + 1, 1000);
#else
    i2c_port_t port = i2c_manager_get_port();
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_RETURN_ON_FALSE(cmd != NULL, ESP_ERR_NO_MEM, TAG, "cmd alloc failed");

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr_7bit << 1) | I2C_MASTER_WRITE,
                          true);
    i2c_master_write_byte(cmd, reg, true);
    for (size_t i = 0; i < len; ++i)
    {
        i2c_master_write_byte(cmd, data[i], true);
    }
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
#endif
}

static bool qmi8658c_accel_odr_code_valid(uint8_t odr)
{
    /*
     * 手册 CTRL2 表中 accel ODR 0x0-0x8 为 normal mode，
     * 0xC-0xF 为 low-power mode，0x9-0xB 标为 N/A。
     */
    return odr <= 0x08 || (odr >= 0x0C && odr <= 0x0F);
}

static bool qmi8658c_gyro_odr_code_valid(uint8_t odr)
{
    // 手册 CTRL3 表中 gyro ODR 仅 0x0-0x8 有效，0x9-0xF 标为 N/A。
    return odr <= 0x08;
}

static int32_t qmi8658c_accel_lsb_per_g(uint8_t accel_fs)
{
    /*
     * CTRL2 accel full-scale code 使用常见 QMI8658C 量程表：
     * 0=±2g, 1=±4g, 2=±8g, 3=±16g。后续若启用其它编码，
     * 必须先补手册证据和对应换算，避免物理量 API 静默输出错误单位。
     */
    switch (accel_fs)
    {
    case 0:
        return 16384;
    case 1:
        return 8192;
    case 2:
        return 4096;
    case 3:
        return 2048;
    default:
        return 0;
    }
}

static float qmi8658c_accel_raw_to_mps2(int16_t raw, uint8_t accel_fs)
{
    const int32_t lsb_per_g = qmi8658c_accel_lsb_per_g(accel_fs);
    if (lsb_per_g <= 0)
    {
        return 0.0f;
    }
    return ((float)raw / (float)lsb_per_g) * k_standard_gravity;
}

static int32_t qmi8658c_gyro_lsb_per_dps(uint8_t gyro_fs)
{
    /*
     * CTRL3 gyro full-scale code：0=±16dps, 1=±32dps, ... 7=±2048dps。
     * QMI8658C 使用 16-bit 二补码输出，因此 LSB/dps 随量程每档减半。
     */
    switch (gyro_fs)
    {
    case 0:
        return 2048;
    case 1:
        return 1024;
    case 2:
        return 512;
    case 3:
        return 256;
    case 4:
        return 128;
    case 5:
        return 64;
    case 6:
        return 32;
    case 7:
        return 16;
    default:
        return 0;
    }
}

static float qmi8658c_gyro_raw_to_dps(int16_t raw, uint8_t gyro_fs)
{
    const int32_t lsb_per_dps = qmi8658c_gyro_lsb_per_dps(gyro_fs);
    if (lsb_per_dps <= 0)
    {
        return 0.0f;
    }
    return (float)raw / (float)lsb_per_dps;
}

static int16_t qmi8658c_decode_i16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[1] << 8) | data[0]);
}

static uint32_t qmi8658c_decode_u24(const uint8_t *data)
{
    return ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
}
