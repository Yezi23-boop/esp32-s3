#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "imu_sensor.h"

#define IMU_SERVICE_SAMPLE_RATE_HZ 50U
#define IMU_SERVICE_EVENT_PRE_FRAMES 50U
#define IMU_SERVICE_EVENT_POST_FRAMES 250U
#define IMU_SERVICE_WINDOW_FRAME_COUNT \
    (IMU_SERVICE_EVENT_PRE_FRAMES + IMU_SERVICE_EVENT_POST_FRAMES)

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief IMU 配置服务状态。
     */
    typedef enum
    {
        IMU_SERVICE_STATE_STOPPED = 0, /**< 服务尚未启动。 */
        IMU_SERVICE_STATE_PROBING,     /**< 正在探测当前板上的 IMU。 */
        IMU_SERVICE_STATE_RUNNING,     /**< IMU 已完成统一配置，GPIO21 ISR 已安装。 */
        IMU_SERVICE_STATE_ERROR,       /**< 最近一次探测或配置失败。 */
        IMU_SERVICE_STATE_STOPPING,    /**< 正在由 owner task 释放运行时资源。 */
    } imu_service_state_t;

    /**
     * @brief IMU 服务只读快照。
     *
     * getter 只复制 service task 已缓存的事实，不访问 I2C、不推进状态机。
     */
    typedef struct
    {
        imu_service_state_t state; /**< 当前服务状态。 */
        bool present;              /**< 当前板上的 IMU 是否通过 `WHO_AM_I` 探测。 */
        bool configured;           /**< 是否已完成 `imu_sensor_config()` 统一配置。 */
        uint8_t who_am_i;          /**< 最近一次 `WHO_AM_I`。 */
        uint8_t revision_id;       /**< 最近一次 `REVISION_ID`。 */
        uint8_t accel_fs;          /**< 当前配置的加速度 full-scale 编码。 */
        uint8_t accel_odr;         /**< 当前配置的加速度 ODR 编码。 */
        uint8_t gyro_fs;           /**< 当前配置的陀螺仪 full-scale 编码。 */
        uint8_t gyro_odr;          /**< 当前配置的陀螺仪 ODR 编码。 */
        bool accel_enabled;        /**< true 表示已请求开启加速度计。 */
        bool gyro_enabled;         /**< true 表示已请求开启陀螺仪。 */
        bool int1_isr_installed;   /**< true 表示 service 已安装 ESP32 GPIO 侧 INT1 ISR。 */
        int int1_gpio;             /**< 当前板级 QMI_INT1 对应的 ESP32 GPIO 编号。 */
        int int1_level;            /**< 最近一次记录到的 INT1 GPIO 电平。 */
        uint32_t int1_irq_count;   /**< service task 已处理的 INT1 GPIO 中断次数。 */
        int64_t last_int1_irq_time_us; /**< 最近一次 INT1 GPIO 中断处理时间戳，单位微秒。 */
        bool sampling_active;      /**< true 表示 50Hz 周期采样循环已经运行。 */
        bool sample_window_ready;  /**< true 表示 6 秒 / 300 帧事件窗口缓冲已经填满过。 */
        uint16_t sample_rate_hz;   /**< 当前 service 采样频率，单位 Hz。 */
        uint16_t window_frame_count; /**< 当前 service 窗口帧数。 */
        uint32_t sample_count;     /**< 成功读取并写入环形缓冲的样本数。 */
        uint32_t sample_error_count; /**< 周期采样读取失败次数。 */
        int64_t last_sample_time_us; /**< 最近一次样本时间戳，单位微秒。 */
        int32_t last_sample_interval_us; /**< 最近两次成功样本间隔，单位微秒。 */
        int64_t configured_time_us; /**< 最近一次配置完成时间戳，单位为微秒。 */
        esp_err_t last_error;      /**< 最近一次错误码；正常为 `ESP_OK`。 */
    } imu_service_snapshot_t;

    /**
     * @brief IMU service 发布给上层算法的 6 秒事件窗口副本。
     *
     * 窗口固定为 50Hz / 300 帧 / 6 秒，其中事件点位于第
     * `IMU_SERVICE_EVENT_PRE_FRAMES` 帧，用于同时承载 2 秒模型子窗口和
     * 事件后 post-check。`accel` 和 `gyro` 是当前板级右手系
     * 物理轴语义下的物理量：`+X` 朝手表顶部、`+Y` 朝手表右侧、`+Z`
     * 朝表背/向下。模型消费方负责自己的坐标契约，例如 Fall V1 输入层
     * 对 Z 轴取反，并将 gyro 从 `deg/s` 转换为 `rad/s`。
     */
    typedef struct
    {
        uint32_t sequence; /**< service 生成的窗口序号。 */
        uint32_t source_sample_count; /**< 生成窗口时累计成功采样数。 */
        uint32_t trigger_sample_count; /**< 触发事件对应的累计采样数。 */
        uint16_t frame_count;         /**< 固定为 `IMU_SERVICE_WINDOW_FRAME_COUNT`。 */
        uint16_t sample_rate_hz;      /**< 固定为 `IMU_SERVICE_SAMPLE_RATE_HZ`。 */
        uint16_t trigger_frame_index; /**< 事件点在窗口内的帧索引。 */
        uint32_t trigger_flags;       /**< 触发原因 bitset，仅用于日志和排障。 */
        int64_t start_time_us;        /**< 窗口第一帧时间戳，单位微秒。 */
        int64_t trigger_time_us;      /**< 事件触发帧时间戳，单位微秒。 */
        int64_t end_time_us;          /**< 窗口最后一帧时间戳，单位微秒。 */
        float trigger_acc_norm_mps2;  /**< 事件触发帧加速度模长，单位 `m/s^2`。 */
        float trigger_gyro_norm_radps; /**< 事件触发帧角速度模长，单位 `rad/s`。 */
        float trigger_jerk_mps2_per_frame; /**< 事件触发帧加速度模长变化，单位 `m/s^2/frame`。 */
        imu_sensor_accel_t accel[IMU_SERVICE_WINDOW_FRAME_COUNT]; /**< 逐帧加速度。 */
        imu_sensor_gyro_t gyro[IMU_SERVICE_WINDOW_FRAME_COUNT];   /**< 逐帧角速度，单位 `deg/s`。 */
    } imu_service_accel_window_t;

    /**
     * @brief 初始化 IMU 服务内部状态。
     *
     * @return `ESP_OK` 表示初始化完成。
     */
    esp_err_t imu_service_init(void);

    /**
     * @brief 启动 IMU 配置服务。
     *
     * 服务在后台 task 中完成 IMU 探测、统一最大量程配置、ESP32 GPIO21 ISR 安装，
     * 并启动 50Hz 周期采样。第一版不启用芯片侧 WoM/INT 事件源。
     *
     * @return `ESP_OK` 表示 task 已启动或之前已启动；其他错误表示初始化或创建 task 失败。
     */
    esp_err_t imu_service_start(void);

    /**
     * @brief 请求停止并销毁 IMU 运行时服务。
     *
     * 调用会立即停止向 fall service 投递窗口；任务、GPIO ISR 和 PSRAM
     * 缓冲由 IMU owner task 在安全退出点释放。销毁进行中再次 start 会返回
     * `ESP_ERR_INVALID_STATE`，待快照回到 `STOPPED` 后即可重新启动。
     *
     * @return `ESP_OK` 表示已停止或已发出停止请求。
     */
    esp_err_t imu_service_destroy(void);

    /**
     * @brief 复制 IMU 服务快照。
     *
     * @param[out] out 输出快照，不能为空。
     * @return `ESP_OK` 表示复制成功；`ESP_ERR_INVALID_ARG` 表示参数为空。
     */
    esp_err_t imu_service_get_snapshot(imu_service_snapshot_t *out);

    /**
     * @brief 注册 IMU 完整窗口输出队列。
     *
     * 队列 item 必须是 `imu_service_accel_window_t`，推荐长度为 1。
     * `imu_service` 只在实时事件触发并收满 6 秒窗口后使用 `xQueueOverwrite()`
     * 发布最新窗口，避免模型推理慢时阻塞 50Hz 采样 task。传入 `NULL`
     * 表示取消注册。
     *
     * @param[in] queue 接收窗口副本的 FreeRTOS queue。
     * @return `ESP_OK` 表示注册成功。
     */
    esp_err_t imu_service_set_window_queue(QueueHandle_t queue);

    /**
     * @brief 将 IMU 服务状态转换为稳定日志文本。
     *
     * @param[in] state 服务状态。
     * @return 静态字符串，不需要调用方释放。
     */
    const char *imu_service_state_text(imu_service_state_t state);

#ifdef __cplusplus
}
#endif
