#include "services/imu_service.h"

#include <math.h>
#include <stdint.h>

#include "app/board_imu.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "imu_sensor.h"

static const char *TAG = "imu_service";

static const TickType_t k_retry_delay_ticks = pdMS_TO_TICKS(5000);
static const TickType_t k_sample_period_ticks =
    pdMS_TO_TICKS(1000 / IMU_SERVICE_SAMPLE_RATE_HZ);
static const uint32_t k_sample_log_interval = IMU_SERVICE_SAMPLE_RATE_HZ * 2U;
static const float k_standard_gravity = 9.80665f;
static const float k_degrees_to_radians = 0.017453292519943295f;
/* 事件触发阈值：降低以捕获更多跌倒场景。 */
static const float k_event_acc_norm_high_mps2 = 25.0f;
static const float k_event_gyro_norm_high_radps = 5.0f;
static const float k_event_jerk_high_mps2_per_frame = 10.0f;
static const float k_event_gyro_jerk_min_mps2_per_frame = 5.0f;
static const uint32_t k_event_cooldown_frames =
    IMU_SERVICE_WINDOW_FRAME_COUNT;

typedef enum
{
    IMU_SERVICE_EVENT_TRIGGER_ACC_HIGH = 1U << 0,
    IMU_SERVICE_EVENT_TRIGGER_JERK_HIGH = 1U << 1,
    IMU_SERVICE_EVENT_TRIGGER_GYRO_JERK = 1U << 2,
} imu_service_event_trigger_flag_t;

typedef struct
{
    int64_t time_us;              /**< 样本时间戳，单位微秒。 */
    imu_sensor_sample_t physical; /**< 完整六轴物理量样本。 */
} imu_service_ring_sample_t;

typedef struct
{
    bool active;                                  /**< 已捕获事件，等待 post frames 收满。 */
    uint32_t trigger_sample_count;                /**< 事件触发帧对应的累计采样数。 */
    uint16_t trigger_ring_index;                  /**< 事件触发帧在 ring buffer 中的位置。 */
    uint32_t trigger_flags;                       /**< 触发原因 bitset。 */
    float acc_norm_mps2;                          /**< 触发帧加速度模长，单位 `m/s^2`。 */
    float gyro_norm_radps;                        /**< 触发帧角速度模长，单位 `rad/s`。 */
    float jerk_mps2_per_frame;                    /**< 触发帧加速度模长变化，单位 `m/s^2/frame`。 */
    uint32_t last_published_trigger_sample_count; /**< 最近已发布事件采样数，用于冷却去重。 */
    bool has_last_acc_norm;                       /**< 是否已有上一帧 acc_norm 可计算 jerk。 */
    float last_acc_norm_mps2;                     /**< 上一帧加速度模长，单位 `m/s^2`。 */
} imu_service_event_trigger_t;

typedef struct
{
    uint8_t accel_fs_code;  /**< CTRL2 bit[6:4]，统一配置为 ±16g。 */
    uint8_t accel_odr_code; /**< CTRL2 bit[3:0]，加速度 ODR。 */
    uint8_t gyro_fs_code;   /**< CTRL3 bit[6:4]，统一配置为 ±2048 dps。 */
    uint8_t gyro_odr_code;  /**< CTRL3 bit[3:0]，陀螺仪 ODR。 */
    bool accel_enable;      /**< true 时开启加速度计。 */
    bool gyro_enable;       /**< true 时开启陀螺仪。 */
} imu_service_profile_t;

typedef struct
{
    bool initialized;
    bool started;
    TaskHandle_t task_handle;
    portMUX_TYPE lock;
    imu_service_snapshot_t snapshot;
    imu_service_ring_sample_t *sample_ring;
    uint16_t sample_write_index;
    uint16_t sample_ring_count;
    int64_t last_sample_time_us;
    QueueHandle_t window_queue;
    uint32_t window_sequence;
    imu_service_accel_window_t *publish_window;
    imu_service_event_trigger_t event_trigger;
} imu_service_context_t;

static const imu_service_profile_t k_imu_service_profile = {
    .accel_fs_code = 3U, /* ±16g，优先避免跌落/撞击峰值饱和。 */
    .accel_odr_code = 3U,
    .gyro_fs_code = 7U, /* ±2048 dps，覆盖快速翻转和跌落旋转。 */
    .gyro_odr_code = 3U,
    .accel_enable = true,
    .gyro_enable = true,
};

static imu_service_context_t s_imu_service = {
    .initialized = false,
    .started = false,
    .task_handle = NULL,
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .snapshot = {
        .state = IMU_SERVICE_STATE_STOPPED,
        .present = false,
        .configured = false,
        .int1_isr_installed = false,
        .int1_gpio = -1,
        .int1_level = -1,
        .sampling_active = false,
        .sample_window_ready = false,
        .sample_rate_hz = IMU_SERVICE_SAMPLE_RATE_HZ,
        .window_frame_count = IMU_SERVICE_WINDOW_FRAME_COUNT,
        .last_error = ESP_OK,
    },
    .sample_ring = NULL,
    .sample_write_index = 0,
    .sample_ring_count = 0,
    .last_sample_time_us = 0,
    .window_queue = NULL,
    .window_sequence = 0,
    .publish_window = NULL,
    .event_trigger = {0},
};

static void IRAM_ATTR imu_service_int1_isr(void *arg)
{
    (void)arg;

    BaseType_t high_priority_task_woken = pdFALSE;
    TaskHandle_t task_handle = s_imu_service.task_handle;
    if (task_handle != NULL)
    {
        vTaskNotifyGiveFromISR(task_handle, &high_priority_task_woken);
    }

    if (high_priority_task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

static void imu_service_store_state(imu_service_state_t state,
                                    esp_err_t error)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.state = state;
    s_imu_service.snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static void imu_service_store_identity(const imu_sensor_info_t *identity)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.present = identity->present;
    s_imu_service.snapshot.who_am_i = identity->who_am_i;
    s_imu_service.snapshot.revision_id = identity->revision_id;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static void imu_service_store_configured(const imu_sensor_config_t *config,
                                         int64_t now_us)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.state = IMU_SERVICE_STATE_RUNNING;
    s_imu_service.snapshot.configured = true;
    s_imu_service.snapshot.accel_fs = config->accel_fs;
    s_imu_service.snapshot.accel_odr = config->accel_odr;
    s_imu_service.snapshot.gyro_fs = config->gyro_fs;
    s_imu_service.snapshot.gyro_odr = config->gyro_odr;
    s_imu_service.snapshot.accel_enabled = config->accel_enable;
    s_imu_service.snapshot.gyro_enabled = config->gyro_enable;
    s_imu_service.snapshot.configured_time_us = now_us;
    s_imu_service.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static void imu_service_store_int1_ready(gpio_num_t gpio, int level)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.int1_isr_installed = true;
    s_imu_service.snapshot.int1_gpio = (int)gpio;
    s_imu_service.snapshot.int1_level = level;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static void imu_service_store_int1_irq(gpio_num_t gpio,
                                       uint32_t irq_count,
                                       int level,
                                       int64_t now_us)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.int1_gpio = (int)gpio;
    s_imu_service.snapshot.int1_level = level;
    s_imu_service.snapshot.int1_irq_count += irq_count;
    s_imu_service.snapshot.last_int1_irq_time_us = now_us;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static int32_t imu_service_accel_axis_mg(float value_mps2)
{
    const float scaled = value_mps2 * 1000.0f / k_standard_gravity;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static int32_t imu_service_gyro_axis_mdps(float value_dps)
{
    const float scaled = value_dps * 1000.0f;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static float imu_service_accel_norm_mps2(const imu_sensor_accel_t *accel)
{
    return sqrtf((accel->x * accel->x) +
                 (accel->y * accel->y) +
                 (accel->z * accel->z));
}

static float imu_service_gyro_norm_radps(const imu_sensor_gyro_t *gyro)
{
    const float x = gyro->x * k_degrees_to_radians;
    const float y = gyro->y * k_degrees_to_radians;
    const float z = gyro->z * k_degrees_to_radians;
    return sqrtf((x * x) + (y * y) + (z * z));
}

static uint32_t imu_service_event_flags(float acc_norm_mps2,
                                        float gyro_norm_radps,
                                        float jerk_mps2_per_frame)
{
    uint32_t flags = 0;
    if (acc_norm_mps2 > k_event_acc_norm_high_mps2)
    {
        flags |= IMU_SERVICE_EVENT_TRIGGER_ACC_HIGH;
    }
    if (jerk_mps2_per_frame > k_event_jerk_high_mps2_per_frame)
    {
        flags |= IMU_SERVICE_EVENT_TRIGGER_JERK_HIGH;
    }
    if (gyro_norm_radps > k_event_gyro_norm_high_radps &&
        jerk_mps2_per_frame > k_event_gyro_jerk_min_mps2_per_frame)
    {
        flags |= IMU_SERVICE_EVENT_TRIGGER_GYRO_JERK;
    }
    return flags;
}

static bool imu_service_event_cooldown_elapsed(uint32_t sample_count)
{
    const uint32_t last_trigger =
        s_imu_service.event_trigger.last_published_trigger_sample_count;
    return last_trigger == 0U ||
           (sample_count - last_trigger) >= k_event_cooldown_frames;
}

static void imu_service_store_sampling_started(void)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.sampling_active = true;
    s_imu_service.snapshot.sample_rate_hz = IMU_SERVICE_SAMPLE_RATE_HZ;
    s_imu_service.snapshot.window_frame_count = IMU_SERVICE_WINDOW_FRAME_COUNT;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static esp_err_t imu_service_prepare_buffers(void)
{
    if (s_imu_service.sample_ring == NULL)
    {
        s_imu_service.sample_ring =
            (imu_service_ring_sample_t *)heap_caps_calloc(
                IMU_SERVICE_WINDOW_FRAME_COUNT,
                sizeof(imu_service_ring_sample_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_imu_service.sample_ring != NULL,
                            ESP_ERR_NO_MEM, TAG,
                            "sample ring psram allocation failed");
    }

    if (s_imu_service.publish_window == NULL)
    {
        s_imu_service.publish_window =
            (imu_service_accel_window_t *)heap_caps_calloc(
                1U, sizeof(imu_service_accel_window_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_imu_service.publish_window != NULL,
                            ESP_ERR_NO_MEM, TAG,
                            "publish window psram allocation failed");
    }

    return ESP_OK;
}

static void imu_service_start_event_trigger(uint32_t sample_count,
                                            uint16_t ring_index,
                                            uint32_t flags,
                                            float acc_norm_mps2,
                                            float gyro_norm_radps,
                                            float jerk_mps2_per_frame)
{
    s_imu_service.event_trigger.active = true;
    s_imu_service.event_trigger.trigger_sample_count = sample_count;
    s_imu_service.event_trigger.trigger_ring_index = ring_index;
    s_imu_service.event_trigger.trigger_flags = flags;
    s_imu_service.event_trigger.acc_norm_mps2 = acc_norm_mps2;
    s_imu_service.event_trigger.gyro_norm_radps = gyro_norm_radps;
    s_imu_service.event_trigger.jerk_mps2_per_frame = jerk_mps2_per_frame;

    ESP_LOGI(TAG,
             "事件触发表 | 触发采样=%-6u flags=0x%02x | acc_norm=%.2f_mps2 gyro_norm=%.2f_radps jerk=%.2f_mps2pf | pre=%u post=%u",
             (unsigned)sample_count,
             (unsigned)flags,
             (double)acc_norm_mps2,
             (double)gyro_norm_radps,
             (double)jerk_mps2_per_frame,
             (unsigned)IMU_SERVICE_EVENT_PRE_FRAMES,
             (unsigned)IMU_SERVICE_EVENT_POST_FRAMES);
}

static void imu_service_update_event_trigger(const imu_sensor_sample_t *sample,
                                             uint32_t sample_count,
                                             uint16_t ring_index)
{
    const float acc_norm_mps2 = imu_service_accel_norm_mps2(&sample->accel);
    const float gyro_norm_radps = imu_service_gyro_norm_radps(&sample->gyro);
    float jerk_mps2_per_frame = 0.0f;
    if (s_imu_service.event_trigger.has_last_acc_norm)
    {
        jerk_mps2_per_frame = fabsf(
            acc_norm_mps2 -
            s_imu_service.event_trigger.last_acc_norm_mps2);
    }

    const bool has_pre_window =
        s_imu_service.sample_ring_count > IMU_SERVICE_EVENT_PRE_FRAMES;
    if (!s_imu_service.event_trigger.active && has_pre_window &&
        imu_service_event_cooldown_elapsed(sample_count))
    {
        const uint32_t flags = imu_service_event_flags(
            acc_norm_mps2, gyro_norm_radps, jerk_mps2_per_frame);
        if (flags != 0U)
        {
            imu_service_start_event_trigger(sample_count,
                                            ring_index,
                                            flags,
                                            acc_norm_mps2,
                                            gyro_norm_radps,
                                            jerk_mps2_per_frame);
        }
    }

    s_imu_service.event_trigger.has_last_acc_norm = true;
    s_imu_service.event_trigger.last_acc_norm_mps2 = acc_norm_mps2;
}

static uint32_t imu_service_store_sample(const imu_sensor_sample_t *sample,
                                         int64_t now_us)
{
    int32_t interval_us = 0;
    if (s_imu_service.last_sample_time_us > 0)
    {
        interval_us = (int32_t)(now_us - s_imu_service.last_sample_time_us);
    }
    s_imu_service.last_sample_time_us = now_us;

    const uint16_t write_index = s_imu_service.sample_write_index;
    s_imu_service.sample_ring[write_index].time_us = now_us;
    s_imu_service.sample_ring[write_index].physical = *sample;
    s_imu_service.sample_write_index =
        (uint16_t)((write_index + 1U) % IMU_SERVICE_WINDOW_FRAME_COUNT);
    if (s_imu_service.sample_ring_count < IMU_SERVICE_WINDOW_FRAME_COUNT)
    {
        s_imu_service.sample_ring_count++;
    }

    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.sample_count++;
    const uint32_t sample_count = s_imu_service.snapshot.sample_count;
    s_imu_service.snapshot.last_sample_time_us = now_us;
    s_imu_service.snapshot.last_sample_interval_us = interval_us;
    s_imu_service.snapshot.sample_window_ready =
        s_imu_service.sample_ring_count >= IMU_SERVICE_WINDOW_FRAME_COUNT;
    s_imu_service.snapshot.last_error = ESP_OK;
    taskEXIT_CRITICAL(&s_imu_service.lock);
    imu_service_update_event_trigger(sample, sample_count, write_index);
    return sample_count;
}

static void imu_service_store_sample_error(esp_err_t error)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.sample_error_count++;
    s_imu_service.snapshot.last_error = error;
    taskEXIT_CRITICAL(&s_imu_service.lock);
}

static esp_err_t imu_service_validate_board_config(
    const board_imu_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "board imu config is null");
    ESP_RETURN_ON_FALSE(config->qmi_i2c_addr_7bit <= 0x7F,
                        ESP_ERR_INVALID_ARG, TAG,
                        "qmi i2c addr is not 7-bit");
    return ESP_OK;
}

static bool imu_service_build_event_window(uint32_t source_sample_count,
                                           imu_service_accel_window_t *window)
{
    if (window == NULL ||
        s_imu_service.sample_ring_count < IMU_SERVICE_WINDOW_FRAME_COUNT)
    {
        return false;
    }

    const imu_service_event_trigger_t *event = &s_imu_service.event_trigger;
    if (!event->active)
    {
        return false;
    }

    const uint32_t collected_post_frames =
        source_sample_count - event->trigger_sample_count + 1U;
    if (collected_post_frames < IMU_SERVICE_EVENT_POST_FRAMES)
    {
        return false;
    }

    const uint16_t start_index = (uint16_t)((event->trigger_ring_index + IMU_SERVICE_WINDOW_FRAME_COUNT -
                                             IMU_SERVICE_EVENT_PRE_FRAMES) %
                                            IMU_SERVICE_WINDOW_FRAME_COUNT);
    const uint16_t end_index = (uint16_t)((start_index + IMU_SERVICE_WINDOW_FRAME_COUNT - 1U) %
                                          IMU_SERVICE_WINDOW_FRAME_COUNT);

    window->sequence = ++s_imu_service.window_sequence;
    window->source_sample_count = source_sample_count;
    window->trigger_sample_count = event->trigger_sample_count;
    window->frame_count = IMU_SERVICE_WINDOW_FRAME_COUNT;
    window->sample_rate_hz = IMU_SERVICE_SAMPLE_RATE_HZ;
    window->trigger_frame_index = IMU_SERVICE_EVENT_PRE_FRAMES;
    window->trigger_flags = event->trigger_flags;
    window->start_time_us = s_imu_service.sample_ring[start_index].time_us;
    window->trigger_time_us =
        s_imu_service.sample_ring[event->trigger_ring_index].time_us;
    window->end_time_us = s_imu_service.sample_ring[end_index].time_us;
    window->trigger_acc_norm_mps2 = event->acc_norm_mps2;
    window->trigger_gyro_norm_radps = event->gyro_norm_radps;
    window->trigger_jerk_mps2_per_frame = event->jerk_mps2_per_frame;

    for (uint16_t frame = 0; frame < IMU_SERVICE_WINDOW_FRAME_COUNT; ++frame)
    {
        const uint16_t ring_index =
            (uint16_t)((start_index + frame) % IMU_SERVICE_WINDOW_FRAME_COUNT);
        window->accel[frame] =
            s_imu_service.sample_ring[ring_index].physical.accel;
        window->gyro[frame] =
            s_imu_service.sample_ring[ring_index].physical.gyro;
    }
    return true;
}

static void imu_service_publish_window_if_ready(uint32_t sample_count)
{
    if (sample_count < IMU_SERVICE_WINDOW_FRAME_COUNT ||
        !s_imu_service.event_trigger.active)
    {
        return;
    }

    taskENTER_CRITICAL(&s_imu_service.lock);
    QueueHandle_t window_queue = s_imu_service.window_queue;
    taskEXIT_CRITICAL(&s_imu_service.lock);
    if (window_queue == NULL || s_imu_service.publish_window == NULL)
    {
        return;
    }
    const uint32_t collected_post_frames =
        sample_count - s_imu_service.event_trigger.trigger_sample_count + 1U;
    if (collected_post_frames >= IMU_SERVICE_EVENT_POST_FRAMES)
    {
        if (imu_service_build_event_window(sample_count,
                                           s_imu_service.publish_window))
        {
            const BaseType_t sent =
                xQueueOverwrite(window_queue, s_imu_service.publish_window);
            if (sent == pdPASS)
            {
                ESP_LOGI(TAG,
                         "事件窗口表 | 序号=%-4u 来源采样=%-6u 触发采样=%-6u 帧数=%-3u 触发帧=%-3u flags=0x%02x | 起始_us=%-12lld 触发_us=%-12lld 结束_us=%-12lld",
                         (unsigned)s_imu_service.publish_window->sequence,
                         (unsigned)s_imu_service.publish_window->source_sample_count,
                         (unsigned)s_imu_service.publish_window->trigger_sample_count,
                         (unsigned)s_imu_service.publish_window->frame_count,
                         (unsigned)s_imu_service.publish_window->trigger_frame_index,
                         (unsigned)s_imu_service.publish_window->trigger_flags,
                         (long long)s_imu_service.publish_window->start_time_us,
                         (long long)s_imu_service.publish_window->trigger_time_us,
                         (long long)s_imu_service.publish_window->end_time_us);
                s_imu_service.event_trigger.last_published_trigger_sample_count =
                    s_imu_service.event_trigger.trigger_sample_count;
                s_imu_service.event_trigger.active = false;
            }
            else
            {
                ESP_LOGW(TAG, "事件窗口发布失败: 序号=%u",
                         (unsigned)s_imu_service.publish_window->sequence);
            }
        }
    }
}

static esp_err_t imu_service_install_int1_isr(const board_imu_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "board imu config is null");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(config->qmi_int1_gpio),
                        ESP_ERR_INVALID_ARG, TAG,
                        "qmi int1 gpio is invalid");

    const uint64_t pin_mask = 1ULL << (uint32_t)config->qmi_int1_gpio;
    const gpio_config_t int1_gpio_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int1_gpio_config), TAG,
                        "qmi int1 gpio config failed");

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "gpio isr service install failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(config->qmi_int1_gpio, imu_service_int1_isr,
                               NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "qmi int1 isr add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(gpio_intr_enable(config->qmi_int1_gpio), TAG,
                        "qmi int1 gpio interrupt enable failed");

    const int level = gpio_get_level(config->qmi_int1_gpio);
    imu_service_store_int1_ready(config->qmi_int1_gpio, level);
    ESP_LOGI(TAG, "int1_gpio_ready gpio=%d level=%d",
             (int)config->qmi_int1_gpio, level);
    return ESP_OK;
}

static esp_err_t imu_service_probe_and_configure(void)
{
    imu_service_store_state(IMU_SERVICE_STATE_PROBING, ESP_OK);
    const board_imu_config_t *board_config = board_imu_get_config();
    ESP_RETURN_ON_ERROR(imu_service_validate_board_config(board_config), TAG,
                        "board imu config invalid");

    const imu_sensor_bus_t bus_config = {
        .addr = board_config->qmi_i2c_addr_7bit,
    };
    esp_err_t ret = imu_sensor_init(&bus_config);
    if (ret != ESP_OK)
    {
        imu_service_store_state(IMU_SERVICE_STATE_ERROR, ret);
        return ret;
    }

    imu_sensor_info_t identity = {0};
    ret = imu_sensor_probe(&identity);
    if (ret != ESP_OK)
    {
        imu_service_store_state(IMU_SERVICE_STATE_ERROR, ret);
        return ret;
    }

    imu_service_store_identity(&identity);
    ESP_LOGI(TAG, "probe: present=%d who_am_i=0x%02x revision_id=0x%02x",
             identity.present,
             identity.who_am_i,
             identity.revision_id);

    if (!identity.present)
    {
        imu_service_store_state(IMU_SERVICE_STATE_ERROR, ESP_ERR_NOT_FOUND);
        return ESP_ERR_NOT_FOUND;
    }

    const imu_sensor_config_t config = {
        .accel_fs = k_imu_service_profile.accel_fs_code,
        .accel_odr = k_imu_service_profile.accel_odr_code,
        .gyro_fs = k_imu_service_profile.gyro_fs_code,
        .gyro_odr = k_imu_service_profile.gyro_odr_code,
        .accel_enable = k_imu_service_profile.accel_enable,
        .gyro_enable = k_imu_service_profile.gyro_enable,
        .int1_source = IMU_SENSOR_INT_SOURCE_DISABLED,
        .int2_source = IMU_SENSOR_INT_SOURCE_DISABLED,
    };
    ret = imu_sensor_config(&config);
    if (ret != ESP_OK)
    {
        imu_service_store_state(IMU_SERVICE_STATE_ERROR, ret);
        return ret;
    }

    ret = imu_service_install_int1_isr(board_config);
    if (ret != ESP_OK)
    {
        imu_service_store_state(IMU_SERVICE_STATE_ERROR, ret);
        return ret;
    }

    imu_service_store_configured(&config, esp_timer_get_time());
    ESP_LOGI(TAG,
             "configured: accel_fs=%u accel_odr=%u gyro_fs=%u gyro_odr=%u accel_enable=%d gyro_enable=%d int1_source=DISABLED int2_source=DISABLED",
             (unsigned)config.accel_fs,
             (unsigned)config.accel_odr,
             (unsigned)config.gyro_fs,
             (unsigned)config.gyro_odr,
             config.accel_enable ? 1 : 0,
             config.gyro_enable ? 1 : 0);
    return ESP_OK;
}

static void imu_service_run_sampling_loop(void)
{
    const board_imu_config_t *board_config = board_imu_get_config();
    const gpio_num_t int1_gpio = board_config->qmi_int1_gpio;
    TickType_t last_wake_tick = xTaskGetTickCount();

    imu_service_store_sampling_started();
    ESP_LOGI(TAG, "sampling_started: rate_hz=%u window_frames=%u",
             (unsigned)IMU_SERVICE_SAMPLE_RATE_HZ,
             (unsigned)IMU_SERVICE_WINDOW_FRAME_COUNT);
    while (1)
    {
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, 0);
        if (notified > 0)
        {
            const int level = gpio_get_level(int1_gpio);
            const int64_t now_us = esp_timer_get_time();
            imu_service_store_int1_irq(int1_gpio, notified, level, now_us);
            ESP_LOGI(TAG, "INT1中断: gpio=%d 电平=%d 本次次数=%u",
                     (int)int1_gpio, level, (unsigned)notified);
        }

        imu_sensor_sample_t sample = {0};
        const esp_err_t ret = imu_sensor_read(&sample);
        const int64_t now_us = esp_timer_get_time();
        if (ret == ESP_OK)
        {
            const uint32_t sample_count =
                imu_service_store_sample(&sample, now_us);
            taskENTER_CRITICAL(&s_imu_service.lock);
            const int32_t interval_us =
                s_imu_service.snapshot.last_sample_interval_us;
            const bool window_ready = s_imu_service.snapshot.sample_window_ready;
            taskEXIT_CRITICAL(&s_imu_service.lock);
            if (sample_count == 1U ||
                (sample_count % k_sample_log_interval) == 0U)
            {
                ESP_LOGI(TAG,
                         "采样表 | 次数=%-5u 间隔_us=%-6d 窗口=%d | 加速度mg[x y z]=%6d %6d %6d | 陀螺仪mdps[x y z]=%7d %7d %7d",
                         (unsigned)sample_count,
                         (int)interval_us,
                         window_ready ? 1 : 0,
                         (int)imu_service_accel_axis_mg(sample.accel.x),
                         (int)imu_service_accel_axis_mg(sample.accel.y),
                         (int)imu_service_accel_axis_mg(sample.accel.z),
                         (int)imu_service_gyro_axis_mdps(sample.gyro.x),
                         (int)imu_service_gyro_axis_mdps(sample.gyro.y),
                         (int)imu_service_gyro_axis_mdps(sample.gyro.z));
            }
            imu_service_publish_window_if_ready(sample_count);
        }
        else
        {
            imu_service_store_sample_error(ret);
            ESP_LOGW(TAG, "采样失败: %s", esp_err_to_name(ret));
        }

        vTaskDelayUntil(&last_wake_tick, k_sample_period_ticks);
    }
}

static void imu_service_task(void *arg)
{
    (void)arg;

    while (1)
    {
        esp_err_t ret = imu_service_probe_and_configure();
        if (ret == ESP_OK)
        {
            imu_service_run_sampling_loop();
            return;
        }

        ESP_LOGW(TAG, "configure_failed: %s", esp_err_to_name(ret));
        vTaskDelay(k_retry_delay_ticks);
    }
}

esp_err_t imu_service_init(void)
{
    if (s_imu_service.initialized)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(imu_service_prepare_buffers(), TAG,
                        "imu psram buffer allocation failed");

    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.snapshot.state = IMU_SERVICE_STATE_STOPPED;
    s_imu_service.snapshot.last_error = ESP_OK;
    s_imu_service.initialized = true;
    taskEXIT_CRITICAL(&s_imu_service.lock);
    return ESP_OK;
}

esp_err_t imu_service_start(void)
{
    esp_err_t ret = imu_service_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    taskENTER_CRITICAL(&s_imu_service.lock);
    const bool already_started = s_imu_service.started;
    taskEXIT_CRITICAL(&s_imu_service.lock);
    if (already_started)
    {
        return ESP_OK;
    }

    const BaseType_t ok = xTaskCreate(imu_service_task, "imu_service", 4096,
                                      NULL, 3, &s_imu_service.task_handle);
    if (ok != pdPASS)
    {
        s_imu_service.task_handle = NULL;
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.started = true;
    taskEXIT_CRITICAL(&s_imu_service.lock);

    ESP_LOGI(TAG, "已启动: 50Hz采样");
    return ESP_OK;
}

esp_err_t imu_service_get_snapshot(imu_service_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_imu_service.lock);
    *out = s_imu_service.snapshot;
    taskEXIT_CRITICAL(&s_imu_service.lock);
    return ESP_OK;
}

esp_err_t imu_service_set_window_queue(QueueHandle_t queue)
{
    taskENTER_CRITICAL(&s_imu_service.lock);
    s_imu_service.window_queue = queue;
    taskEXIT_CRITICAL(&s_imu_service.lock);

    ESP_LOGI(TAG, "窗口队列已%s", queue != NULL ? "注册" : "清除");
    return ESP_OK;
}

const char *imu_service_state_text(imu_service_state_t state)
{
    switch (state)
    {
    case IMU_SERVICE_STATE_STOPPED:
        return "STOPPED";
    case IMU_SERVICE_STATE_PROBING:
        return "PROBING";
    case IMU_SERVICE_STATE_RUNNING:
        return "RUNNING";
    case IMU_SERVICE_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
