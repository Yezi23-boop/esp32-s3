#include "imu_sensor.h"

#include "esp_check.h"
#include "qmi8658c.h"

static const char *TAG = "imu_sensor";

esp_err_t imu_sensor_init(const imu_sensor_bus_t *bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "imu bus config is null");
    ESP_RETURN_ON_FALSE(bus->addr <= 0x7F, ESP_ERR_INVALID_ARG, TAG,
                        "imu i2c addr is not 7-bit");

    const qmi8658c_bus_t qmi_bus = {
        .addr = bus->addr,
    };
    return qmi8658c_init_bus(&qmi_bus);
}

esp_err_t imu_sensor_probe(imu_sensor_info_t *info)
{
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "imu info output is null");

    qmi8658c_info_t qmi_info = {0};
    ESP_RETURN_ON_ERROR(qmi8658c_probe(&qmi_info), TAG,
                        "qmi8658c probe failed");

    info->present = qmi_info.present;
    info->who_am_i = qmi_info.who_am_i;
    info->revision_id = qmi_info.revision_id;
    return ESP_OK;
}

esp_err_t imu_sensor_config(const imu_sensor_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "imu sensor config is null");
    ESP_RETURN_ON_FALSE(config->int1_source == IMU_SENSOR_INT_SOURCE_DISABLED,
                        ESP_ERR_INVALID_ARG, TAG,
                        "imu int1 source is reserved and must be disabled");
    ESP_RETURN_ON_FALSE(config->int2_source == IMU_SENSOR_INT_SOURCE_DISABLED,
                        ESP_ERR_INVALID_ARG, TAG,
                        "imu int2 source is reserved and must be disabled");

    const qmi8658c_config_t qmi_config = {
        .accel_fs = config->accel_fs,
        .accel_odr = config->accel_odr,
        .gyro_fs = config->gyro_fs,
        .gyro_odr = config->gyro_odr,
        .accel_enable = config->accel_enable,
        .gyro_enable = config->gyro_enable,
        .int1_source = QMI8658C_INT_SOURCE_DISABLED,
        .int2_source = QMI8658C_INT_SOURCE_DISABLED,
    };
    return qmi8658c_config(&qmi_config);
}

esp_err_t imu_sensor_read(imu_sensor_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "imu sample output is null");

    qmi8658c_sample_t qmi_sample = {0};
    ESP_RETURN_ON_ERROR(qmi8658c_read(&qmi_sample), TAG,
                        "qmi8658c read failed");

    sample->accel.x = qmi_sample.accel.x;
    sample->accel.y = qmi_sample.accel.y;
    sample->accel.z = qmi_sample.accel.z;
    sample->gyro.x = qmi_sample.gyro.x;
    sample->gyro.y = qmi_sample.gyro.y;
    sample->gyro.z = qmi_sample.gyro.z;
    return ESP_OK;
}

