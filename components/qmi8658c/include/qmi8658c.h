#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief QMI8658C 芯片识别结果。
     *
     * 第一阶段只用该结构证明板级 I2C 通信与 `WHO_AM_I` 成立。
     * `revision_id` 只用于日志和排障，不作为硬件版本的固定断言。
     */
    typedef struct
    {
        bool present;        /**< true 表示 `0x6B` 地址应答且 `WHO_AM_I` 读取成功。 */
        uint8_t who_am_i;    /**< `WHO_AM_I` 原始值，期望为 `0x05`。 */
        uint8_t revision_id; /**< `REVISION_ID` 原始值，手册版本间可能存在差异。 */
    } qmi8658c_info_t;

    /**
     * @brief QMI8658C 总线挂载配置。
     *
     * 地址由板级 `SA0` 绑法决定，因此允许上层在初始化前传入当前板地址。
     */
    typedef struct
    {
        uint8_t addr; /**< 7-bit I2C 地址，当前板默认为 `0x6B`。 */
    } qmi8658c_bus_t;

    /**
     * @brief QMI8658C 三轴加速度物理量样本。
     *
     * `x/y/z` 已按当前 CTRL2 加速度量程从 raw LSB 换算为 `m/s^2`。
     * 该结构不表达板级安装方向；轴向映射仍由 `board_imu` / 上层 service 负责。
     */
    typedef struct
    {
        float x; /**< X 轴加速度。 */
        float y; /**< Y 轴加速度。 */
        float z; /**< Z 轴加速度。 */
    } qmi8658c_accel_t;

    /**
     * @brief QMI8658C 三轴角速度物理量样本。
     *
     * `x/y/z` 已按当前 CTRL3 陀螺仪量程从 raw LSB 换算为 `deg/s`。
     * 该结构只表达芯片寄存器坐标系，不表达板级安装方向。
     */
    typedef struct
    {
        float x; /**< X 轴角速度。 */
        float y; /**< Y 轴角速度。 */
        float z; /**< Z 轴角速度。 */
    } qmi8658c_gyro_t;

    /**
     * @brief QMI8658C 一次物理六轴样本。
     *
     * 该结构是 driver 对 board/service 层的采样输出契约。寄存器 raw 值只在
     * driver 内部用于换算，不作为跨层接口暴露，避免上层重复维护量程表。
     */
    typedef struct
    {
        qmi8658c_accel_t accel; /**< 三轴加速度，单位 `m/s^2`。 */
        qmi8658c_gyro_t gyro;   /**< 三轴角速度，单位 `deg/s`。 */
    } qmi8658c_sample_t;

    /**
     * @brief QMI8658C 芯片侧中断输出源。
     *
     * 第一版只预留字段，不启用芯片 INT1/INT2 事件输出。ESP32 GPIO ISR 属于
     * board/service 层资源，不由 QMI8658C driver 安装。
     */
    typedef enum
    {
        QMI8658C_INT_SOURCE_DISABLED = 0, /**< 禁用芯片侧 INT 输出事件源。 */
    } qmi8658c_int_source_t;

    /**
     * @brief QMI8658C 六轴采样模式配置。
     *
     * `accel_fs/accel_odr/gyro_fs/gyro_odr` 直接对应 CTRL2/CTRL3 的字段值，
     * 调用方必须传入手册允许的编码。第一版不启用 FIFO、AttitudeEngine 或磁力计。
     */
    typedef struct
    {
        uint8_t accel_fs;    /**< CTRL2 bit[6:4]，加速度 full-scale 编码。 */
        uint8_t accel_odr;   /**< CTRL2 bit[3:0]，加速度 ODR 编码。 */
        uint8_t gyro_fs;     /**< CTRL3 bit[6:4]，陀螺仪 full-scale 编码。 */
        uint8_t gyro_odr;    /**< CTRL3 bit[3:0]，陀螺仪 ODR 编码。 */
        bool accel_enable;   /**< true 时在 CTRL7 置位 aEN。 */
        bool gyro_enable;    /**< true 时在 CTRL7 置位 gEN。 */
        qmi8658c_int_source_t int1_source; /**< 预留 INT1 芯片侧事件源；第一版必须为 disabled。 */
        qmi8658c_int_source_t int2_source; /**< 预留 INT2 芯片侧事件源；第一版必须为 disabled。 */
    } qmi8658c_config_t;

    /**
     * @brief 初始化 QMI8658C 驱动并绑定共享 I2C 设备句柄。
     *
     * 该函数复用 `i2c_manager` 已定义的 `GPIO14/GPIO15` 共享总线，
     * 不会创建新的 I2C bus，也不会开始连续采样。
     *
     * @return `ESP_OK` 表示成功或之前已初始化；其他错误表示共享 I2C 不可用或设备句柄创建失败。
     *
     * @note 仅允许在任务上下文调用；内部 I2C 初始化和设备挂载可能阻塞。
     */
    esp_err_t qmi8658c_init(void);

    /**
     * @brief 按板级地址初始化 QMI8658C 驱动。
     *
     * 若驱动已经初始化，传入相同地址会直接返回成功；传入不同地址会返回
     * `ESP_ERR_INVALID_STATE`，避免运行中切换 I2C 设备句柄。
     *
     * @param[in] bus 总线挂载配置，不能为空，`addr` 必须为 7-bit I2C 地址。
     * @return `ESP_OK` 表示初始化成功或已按相同地址初始化。
     */
    esp_err_t qmi8658c_init_bus(const qmi8658c_bus_t *bus);

    /**
     * @brief 探测 QMI8658C 并读取芯片识别寄存器。
     *
     * 该接口先探测当前板级地址 `0x6B`，再读取 `WHO_AM_I` 与 `REVISION_ID`。
     * 若地址无应答，函数返回 `ESP_OK` 且 `info->present=false`，便于板级日志继续运行。
     *
     * @param[out] info 输出芯片识别结果，不能为空。
     * @return `ESP_OK` 表示探测流程完成；其他错误表示共享 I2C 或寄存器读取出现异常。
     *
     * @note 该接口只做一次性探测，不配置采样模式，不应在 UI 高频路径调用。
     */
    esp_err_t qmi8658c_probe(qmi8658c_info_t *info);

    /**
     * @brief 配置 QMI8658C 的加速度和陀螺仪采样模式。
     *
     * 第一阶段只配置基础六轴路径：CTRL1/CTRL5 采用当前板已验证的 Waveshare 口径，
     * CTRL2/CTRL3 配置量程和 ODR，CTRL7 只开启 accel/gyro。保持 FIFO、AttitudeEngine、
     * 磁力计和中断事件源关闭。
     * `int1_source/int2_source` 是后续芯片侧 INT 事件源预留字段，当前必须传 disabled。
     *
     * @param[in] config 六轴采样模式配置，不能为空。
     * @return `ESP_OK` 表示配置成功；`ESP_ERR_INVALID_ARG` 表示字段编码越界；
     *         其他错误表示 I2C 写入失败。
     *
     * @note 该接口会改变芯片工作模式，但不会创建采样任务。
     */
    esp_err_t qmi8658c_config(const qmi8658c_config_t *config);

    /**
     * @brief 读取一次完整物理六轴样本。
     *
     * 该接口内部从 `TEMP_L` 连续读取到 `GZ_H`，保证同一帧加速度和陀螺仪
     * 来自同一次 I2C 事务；对外只返回 `m/s^2` 与 `deg/s`。
     *
     * @param[out] sample 输出物理六轴样本，不能为空。
     * @return `ESP_OK` 表示读取并换算成功；`ESP_ERR_INVALID_STATE` 表示尚未调用
     *         `qmi8658c_config()` 完成量程配置；其他错误表示 I2C 访问失败。
     *
     * @note 该接口不做板级轴向映射、滤波或姿态语义判断。
     */
    esp_err_t qmi8658c_read(qmi8658c_sample_t *sample);

#ifdef __cplusplus
}
#endif
