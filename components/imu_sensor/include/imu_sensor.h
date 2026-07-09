#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 当前 IMU 设备的总线事实。
     *
     * 该结构由板级代码提供，`imu_sensor` 只把它转换成具体芯片 driver 需要的参数。
     */
    typedef struct
    {
        uint8_t addr; /**< 7-bit I2C 地址。 */
    } imu_sensor_bus_t;

    /**
     * @brief IMU 设备识别信息。
     */
    typedef struct
    {
        bool present;        /**< true 表示当前板上的 IMU 探测成功。 */
        uint8_t who_am_i;    /**< 芯片识别寄存器原始值。 */
        uint8_t revision_id; /**< 芯片 revision 原始值，仅用于排障日志。 */
    } imu_sensor_info_t;

    /**
     * @brief 三轴加速度物理量，单位 `m/s^2`。
     */
    typedef struct
    {
        float x; /**< X 轴加速度。 */
        float y; /**< Y 轴加速度。 */
        float z; /**< Z 轴加速度。 */
    } imu_sensor_accel_t;

    /**
     * @brief 三轴角速度物理量，单位 `deg/s`。
     */
    typedef struct
    {
        float x; /**< X 轴角速度。 */
        float y; /**< Y 轴角速度。 */
        float z; /**< Z 轴角速度。 */
    } imu_sensor_gyro_t;

    /**
     * @brief 一次完整六轴物理量样本。
     */
    typedef struct
    {
        imu_sensor_accel_t accel; /**< 三轴加速度，单位 `m/s^2`。 */
        imu_sensor_gyro_t gyro;   /**< 三轴角速度，单位 `deg/s`。 */
    } imu_sensor_sample_t;

    /**
     * @brief IMU 芯片侧中断事件源。
     *
     * 第一版只允许 disabled。ESP32 GPIO ISR 是 service 层运行时资源，
     * 不通过这个枚举表达。
     */
    typedef enum
    {
        IMU_SENSOR_INT_SOURCE_DISABLED = 0, /**< 禁用芯片侧 INT 输出事件源。 */
    } imu_sensor_int_source_t;

    /**
     * @brief IMU 六轴采样模式配置。
     *
     * full-scale 和 ODR 字段保留芯片编码口径，避免 service 层引入某个芯片的寄存器类型。
     * 当前 QMI8658C 适配下，`accel_fs=3` 表示 ±16g，`gyro_fs=7` 表示 ±2048 dps。
     */
    typedef struct
    {
        uint8_t accel_fs;    /**< 加速度 full-scale 编码。 */
        uint8_t accel_odr;   /**< 加速度 ODR 编码。 */
        uint8_t gyro_fs;     /**< 陀螺仪 full-scale 编码。 */
        uint8_t gyro_odr;    /**< 陀螺仪 ODR 编码。 */
        bool accel_enable;   /**< true 时开启加速度计。 */
        bool gyro_enable;    /**< true 时开启陀螺仪。 */
        imu_sensor_int_source_t int1_source; /**< 芯片 INT1 事件源，第一版必须 disabled。 */
        imu_sensor_int_source_t int2_source; /**< 芯片 INT2 事件源，第一版必须 disabled。 */
    } imu_sensor_config_t;

    /**
     * @brief 初始化当前板上的 IMU 传感器适配层。
     *
     * @param[in] bus 当前板级总线事实，不能为空。
     * @return `ESP_OK` 表示初始化成功。
     */
    esp_err_t imu_sensor_init(const imu_sensor_bus_t *bus);

    /**
     * @brief 探测当前板上的 IMU 传感器。
     *
     * @param[out] info 输出识别信息，不能为空。
     * @return `ESP_OK` 表示探测流程完成。
     */
    esp_err_t imu_sensor_probe(imu_sensor_info_t *info);

    /**
     * @brief 配置当前 IMU 传感器的六轴采样模式。
     *
     * @param[in] config 六轴采样模式配置，不能为空。
     * @return `ESP_OK` 表示配置成功。
     */
    esp_err_t imu_sensor_config(const imu_sensor_config_t *config);

    /**
     * @brief 读取一次完整六轴物理量样本。
     *
     * @param[out] sample 输出物理六轴样本，不能为空。
     * @return `ESP_OK` 表示读取成功。
     */
    esp_err_t imu_sensor_read(imu_sensor_sample_t *sample);

#ifdef __cplusplus
}
#endif

