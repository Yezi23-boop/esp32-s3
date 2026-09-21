/**
 * @file audio_codec.c
 * @brief 音频 codec 统一适配层实现
 * @details
 * - 控制面：通过 I2C 管理 ES8311/ES7210 寄存器
 * - 数据面：通过 I2S TX/RX 通道传输 PCM
 * - 对上层提供初始化、读写、音量、静音和增益控制接口
 */

#include <string.h>

#include "audio_codec_bus.h"
#include "audio_codec.h"
#include "audio_platform_config.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_manager.h"
#include "nvs.h"

static const char *TAG = "audio_codec";

static const audio_codec_ctrl_if_t *s_playback_ctrl_if = NULL; // ES8311 I2C 控制接口。
static const audio_codec_ctrl_if_t *s_record_ctrl_if = NULL;   // ES7210 I2C 控制接口。
static const audio_codec_gpio_if_t *s_gpio_if = NULL;          // 公共 GPIO 控制接口，负责 PA 等控制。
static const audio_codec_if_t *s_playback_codec_if = NULL;     // ES8311 codec 抽象接口。
static const audio_codec_if_t *s_record_codec_if = NULL;       // ES7210 codec 抽象接口。
static const audio_codec_data_if_t *s_data_if = NULL;          // I2S 数据接口。
static esp_codec_dev_handle_t s_playback_dev = NULL;           // 播放设备句柄。
static esp_codec_dev_handle_t s_record_dev = NULL;             // 录音设备句柄。

static const int kDefaultVolumePercent = 60;                                  // NVS 缺失或非法时使用的默认音量。
static const char *kVolumeNvsNamespace = "audio_codec";                       // 用户音量偏好的 NVS 命名空间。
static const char *kVolumeNvsKey = "volume";                                 // 0~100 音量百分比键。
static int s_current_volume = 60;                                              // 当前已应用音量，范围为 0~100。
static int s_persisted_volume = 60;                                            // 最近成功读取或写入 NVS 的音量。
static int s_input_sample_rate = AUDIO_PLATFORM_HW_SAMPLE_RATE;                // 录音采样率，单位为 Hz。
static int s_output_sample_rate = AUDIO_PLATFORM_HW_SAMPLE_RATE;               // 播放采样率，单位为 Hz。
static int s_physical_rx_slots = 4;                                            // RX 物理时隙数，当前走 TDM。
static const char *s_logical_input_format = AUDIO_PLATFORM_ADC_CHANNEL_FORMAT; // 逻辑输入格式标签。
static int s_logical_input_channels = AUDIO_PLATFORM_HW_INPUT_CHANNELS;        // 逻辑输入通道数。
static bool s_input_reference = true;                                          // 是否包含参考通道。
static int s_output_channels = AUDIO_PLATFORM_HW_OUTPUT_CHANNELS;              // 输出通道数。
static int s_bits_per_sample = AUDIO_PLATFORM_HW_BITS_PER_SAMPLE;              // 采样位宽，单位为 bit。
static const uint8_t s_tx_silence_preload[2048] = {0};                         // TX flush 使用的静音预装缓冲。

static SemaphoreHandle_t s_resource_mutex = NULL;                       // 保护 codec 生命周期引用和会话 owner。
static uint32_t s_lifecycle_ref_count = 0;                               // 持有 audio_codec_init() 的模块数量。
static audio_codec_owner_t s_input_session_owner = AUDIO_CODEC_OWNER_SYSTEM;  // 当前独占录音链路的 owner。
static audio_codec_owner_t s_output_session_owner = AUDIO_CODEC_OWNER_SYSTEM; // 当前独占播放链路的 owner。
static bool s_input_session_active = false;                              // true 表示 I2S RX/ES7210 已被某个运行时占用。
static bool s_output_session_active = false;                             // true 表示 I2S TX/ES8311 已被某个播放者占用。

/* 会话状态缓存：由资源 mutex 持有路径维护，供无锁低频策略读取，避免阻塞等待。 */
static audio_codec_session_snapshot_t s_cached_session_snapshot = {0};
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;

/* 会话变更回调：注册发生在 service 初始化阶段，运行期不变，因此无需额外加锁。 */
static audio_codec_session_change_cb_t s_session_change_cb = NULL;
static void *s_session_change_ctx = NULL;

/**
 * @brief 在资源 mutex 内刷新会话缓存并通知变更回调。
 *
 * 只在 acquire/release 成功路径调用，此时调用方持有资源 mutex；回调只允许
 * 复制状态或发送通知，不允许阻塞。
 */
static void audio_codec_publish_session_change(void)
{
    audio_codec_session_snapshot_t snapshot = {
        .input_active = s_input_session_active,
        .input_owner = s_input_session_owner,
        .output_active = s_output_session_active,
        .output_owner = s_output_session_owner,
    };
    taskENTER_CRITICAL(&s_cache_lock);
    s_cached_session_snapshot = snapshot;
    taskEXIT_CRITICAL(&s_cache_lock);

    if (s_session_change_cb != NULL)
    {
        s_session_change_cb(s_session_change_ctx);
    }
}

#define ES8311_CODEC_ADDR 0x30
#define ES7210_ADC_ADDR 0x80

static esp_err_t audio_codec_deinit_hardware_locked(void);

/**
 * @brief 从 NVS 恢复用户音量，调用方必须持有资源 mutex。
 *
 * NVS 不可用、键不存在或数值越界时继续使用 60%，避免音量偏好阻断
 * codec 基础初始化。
 */
static void audio_codec_load_volume_preference_locked(void)
{
    uint8_t stored_volume = (uint8_t)kDefaultVolumePercent;
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kVolumeNvsNamespace, NVS_READONLY, &handle);
    if (ret == ESP_OK)
    {
        ret = nvs_get_u8(handle, kVolumeNvsKey, &stored_volume);
        nvs_close(handle);
    }

    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Failed to load volume preference: %s",
                 esp_err_to_name(ret));
    }
    if (ret != ESP_OK || stored_volume > 100U)
    {
        if (ret == ESP_OK)
        {
            ESP_LOGW(TAG, "Ignore invalid stored volume: %u",
                     (unsigned int)stored_volume);
        }
        stored_volume = (uint8_t)kDefaultVolumePercent;
    }

    s_current_volume = (int)stored_volume;
    s_persisted_volume = (int)stored_volume;
    ESP_LOGI(TAG, "Speaker volume restored: %d%%", s_current_volume);
}

/**
 * @brief 返回音频 owner 的日志名称。
 *
 * @param[in] owner 音频资源申请者。
 * @return 静态字符串，便于在会话冲突时定位占用者。
 */
static const char *audio_codec_owner_name(audio_codec_owner_t owner)
{
    switch (owner)
    {
    case AUDIO_CODEC_OWNER_SYSTEM:
        return "system";
    case AUDIO_CODEC_OWNER_TRAFFIC_INFERENCE:
        return "traffic_inference";
    case AUDIO_CODEC_OWNER_ESPDL_INFERENCE:
        return "espdl_inference";
    case AUDIO_CODEC_OWNER_AUDIO_PLAYER:
        return "audio_player";
    case AUDIO_CODEC_OWNER_ALERT_PLAYER:
        return "alert_player";
    case AUDIO_CODEC_OWNER_AUDIO_RECORDER:
        return "audio_recorder";
    case AUDIO_CODEC_OWNER_OFFICIAL_CHAT:
        return "official_chat";
    case AUDIO_CODEC_OWNER_HERMES:
        return "hermes";
    case AUDIO_CODEC_OWNER_MUSIC_PLAYER:
        return "music_player";
    default:
        return "unknown";
    }
}

const char *audio_codec_owner_to_text(audio_codec_owner_t owner)
{
    return audio_codec_owner_name(owner);
}

/**
 * @brief 确保音频资源 mutex 已创建。
 *
 * 该 mutex 是 audio_codec 的 owner 边界：生命周期引用计数、录音会话和播放会话
 * 都必须在同一把锁下修改，避免 UI/运行时任务交错 start/stop 时拆掉硬件资源。
 *
 * @return `ESP_OK` 表示 mutex 可用；`ESP_ERR_NO_MEM` 表示创建失败。
 */
static esp_err_t audio_codec_ensure_resource_mutex(void)
{
    if (s_resource_mutex != NULL)
    {
        return ESP_OK;
    }

    s_resource_mutex = xSemaphoreCreateMutex();
    if (s_resource_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create audio codec resource mutex");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 按超时时间获取音频资源 mutex。
 *
 * @param[in] timeout_ms 等待时间，单位毫秒；`UINT32_MAX` 表示永久等待。
 * @return `ESP_OK` 表示已持锁。
 */
static esp_err_t audio_codec_lock_resources(uint32_t timeout_ms)
{
    esp_err_t ret = audio_codec_ensure_resource_mutex();
    if (ret != ESP_OK)
    {
        return ret;
    }

    const TickType_t ticks =
        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_resource_mutex, ticks) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief 释放音频资源 mutex。
 */
static void audio_codec_unlock_resources(void)
{
    if (s_resource_mutex != NULL)
    {
        (void)xSemaphoreGive(s_resource_mutex);
    }
}

static int audio_codec_get_tx_slot_channels(void)
{
    // TX 至少按双声道时隙工作，避免单声道配置下的 slot 对齐问题。
    return s_output_channels > 1 ? s_output_channels : 2;
}

/**
 * @brief 判断当前输入通道顺序是否已满足逻辑层要求。
 * @return true 表示当前无需重排。
 */
static bool audio_codec_channel_order_is_identity(void)
{
    return true;
}

/**
 * @brief 将采样数据重排到逻辑输入格式。
 * @param[in,out] samples 采样数据缓冲区。
 * @param[in] sample_count 采样点数。
 * @return 无返回值。
 */
static void audio_codec_reorder_to_logical_input_format(int16_t *samples,
                                                        size_t sample_count)
{
    (void)samples;
    (void)sample_count;
    /* Keep the logical MR order stable here if runtime probing later shows
     * the selected RX slots are not already emitted as microphone/reference. */
}

/**
 * @brief 计算录音切片读取大小。
 * @return 建议的读取分片大小，单位为字节。
 */
static size_t audio_codec_get_read_slice_bytes(void)
{
    int bytes_per_sample = s_bits_per_sample / 8;
    size_t frame_bytes = 0;
    size_t bytes_per_tick = 0;

    if (s_input_sample_rate <= 0 || s_logical_input_channels <= 0 ||
        bytes_per_sample <= 0)
    {
        return 1;
    }
    frame_bytes =
        (size_t)s_logical_input_channels * (size_t)bytes_per_sample;
    if (frame_bytes == 0)
    {
        return 1;
    }

    /* 保持有限等待语义，但不要把分片切得比一个 RTOS tick 还细。 */
    bytes_per_tick = (((size_t)s_input_sample_rate * frame_bytes) +
                      configTICK_RATE_HZ - 1) /
                     configTICK_RATE_HZ;
    if (bytes_per_tick < frame_bytes)
    {
        bytes_per_tick = frame_bytes;
    }
    return ((bytes_per_tick + frame_bytes - 1) / frame_bytes) * frame_bytes;
}

/**
 * @brief 计算 I2S TX flush 预装静音长度。
 * @return 建议的预装字节数。
 */
static size_t audio_codec_get_i2s_bus_preload_bytes(void)
{
    int bytes_per_sample = s_bits_per_sample / 8;
    size_t frame_bytes = 0;
    size_t bytes_per_tick = 0;

    if (s_output_sample_rate <= 0 || bytes_per_sample <= 0)
    {
        return 1;
    }

    frame_bytes =
        (size_t)audio_codec_get_tx_slot_channels() * (size_t)bytes_per_sample;
    if (frame_bytes == 0)
    {
        return 1;
    }

    bytes_per_tick =
        (((size_t)s_output_sample_rate * frame_bytes) + configTICK_RATE_HZ - 1) /
        configTICK_RATE_HZ;
    if (bytes_per_tick < frame_bytes)
    {
        bytes_per_tick = frame_bytes;
    }
    return ((bytes_per_tick + frame_bytes - 1) / frame_bytes) * frame_bytes;
}

/**
 * @brief 按切片和超时语义读取录音数据。
 * @param[in] dev 录音设备句柄。
 * @param[out] buffer 输出缓冲区。
 * @param[in] bytes 目标读取字节数。
 * @param[out] bytes_read 实际读取字节数。
 * @param[in] ticks_to_wait 最大等待 tick 数。
 * @param[in] slice_bytes 单次切片大小，单位为字节。
 * @return `ESP_OK` 表示读满；`ESP_ERR_TIMEOUT` 表示超时前仅拿到部分数据；其他错误表示底层读取失败。
 */
static esp_err_t audio_codec_read_with_timeout_slice(
    esp_codec_dev_handle_t dev, void *buffer, size_t bytes,
    size_t *bytes_read, TickType_t ticks_to_wait, size_t slice_bytes)
{
    TickType_t start_ticks = 0;
    size_t transferred = 0;

    if (bytes_read != NULL)
    {
        *bytes_read = 0;
    }
    if (dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL && bytes > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes == 0)
    {
        return ESP_OK;
    }
    if (slice_bytes == 0)
    {
        slice_bytes = 1;
    }

    if (ticks_to_wait == portMAX_DELAY)
    {
        // 无限等待模式下，直接一次性读满目标长度，避免引入额外切片开销。
        int ret = esp_codec_dev_read(dev, buffer, (int)bytes);
        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGW(TAG, "audio codec read failed: %d", ret);
            return ESP_FAIL;
        }
        if (bytes_read != NULL)
        {
            *bytes_read = bytes;
        }
        return ESP_OK;
    }

    start_ticks = xTaskGetTickCount();
    while (transferred < bytes)
    {
        size_t chunk_bytes = bytes - transferred; // 本轮计划读取字节数。
        int ret = 0;

        if ((xTaskGetTickCount() - start_ticks) >= ticks_to_wait &&
            transferred > 0)
        {
            break;
        }
        if (chunk_bytes > slice_bytes)
        {
            chunk_bytes = slice_bytes;
        }

        ret = esp_codec_dev_read(dev, (uint8_t *)buffer + transferred,
                                 (int)chunk_bytes);
        if (ret != ESP_CODEC_DEV_OK)
        {
            ESP_LOGW(TAG, "audio codec read failed: %d", ret);
            return ESP_FAIL;
        }

        transferred += chunk_bytes;
        if (transferred >= bytes)
        {
            break;
        }
        if (ticks_to_wait == 0 ||
            (xTaskGetTickCount() - start_ticks) >= ticks_to_wait)
        {
            break;
        }
    }

    if (bytes_read != NULL)
    {
        *bytes_read = transferred;
    }
    return transferred == bytes ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 初始化共享 I2C 总线。
 * @return `ESP_OK` 表示成功；其他错误表示总线初始化失败。
 */
static esp_err_t audio_i2c_init(void)
{
    return i2c_manager_init();
}

/**
 * @brief 创建复用共享 I2C 总线的 codec 控制接口。
 * @param[in] addr codec 地址。
 * @return 成功时返回控制接口对象；失败返回 NULL。
 */
static const audio_codec_ctrl_if_t *audio_codec_new_shared_i2c_ctrl(uint8_t addr)
{
    // addr 使用 codec 驱动约定的地址格式（与总线扫描显示风格可能不同）。
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_MANAGER_PORT,
        .addr = addr,
    };

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_cfg.bus_handle = i2c_manager_get_bus_handle();
    if (i2c_cfg.bus_handle == NULL)
    {
        ESP_LOGE(TAG, "I2C master bus handle is NULL");
        return NULL;
    }
#endif

    return audio_codec_new_i2c_ctrl(&i2c_cfg);
}

/**
 * @brief 初始化默认控制接口和 GPIO 接口。
 * @return `ESP_OK` 表示成功；其他错误表示接口创建失败。
 */
static esp_err_t audio_codec_init_default_interfaces(void)
{
    s_playback_ctrl_if = audio_codec_new_shared_i2c_ctrl(ES8311_CODEC_ADDR);
    if (s_playback_ctrl_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ES8311 I2C control interface");
        return ESP_FAIL;
    }

    s_record_ctrl_if = audio_codec_new_shared_i2c_ctrl(ES7210_ADC_ADDR);
    if (s_record_ctrl_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ES7210 I2C control interface");
        return ESP_FAIL;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create shared GPIO interface");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 初始化 ES8311 播放链路。
 * @return `ESP_OK` 表示成功；其他错误表示 codec 创建或打开失败。
 */
static esp_err_t audio_es8311_init(void)
{
    // 播放链路采样参数决定 DAC 端 I2S 工作格式；这里必须和总线配置保持一致。
    esp_codec_dev_hw_gain_t hw_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = s_output_sample_rate,
        .channel = s_output_channels,
        .bits_per_sample = s_bits_per_sample,
    };

    if (s_playback_ctrl_if == NULL || s_gpio_if == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_playback_codec_if = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = s_playback_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = AUDIO_PA_CTRL_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    });
    if (s_playback_codec_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }

    s_playback_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .codec_if = s_playback_codec_if,
        .data_if = s_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    });
    if (s_playback_dev == NULL ||
        esp_codec_dev_open(s_playback_dev, &sample_info) != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "Failed to init ES8311");
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(s_playback_dev, s_current_volume);
    return ESP_OK;
}

/**
 * @brief 初始化 ES7210 录音链路。
 * @return `ESP_OK` 表示成功；其他错误表示 codec 创建或打开失败。
 */
static esp_err_t audio_es7210_init(void)
{
    // 录音链路里 `channel` 表示物理时隙数，`channel_mask` 决定真正采集哪些通道。
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = s_input_sample_rate,
        .channel = s_physical_rx_slots,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .bits_per_sample = s_bits_per_sample,
        .mclk_multiple = 0,
    };

    if (s_record_ctrl_if == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_input_reference)
    {
        sample_info.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    }

    s_record_codec_if = es7210_codec_new(&(es7210_codec_cfg_t){
        .ctrl_if = s_record_ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        .mclk_src = ES7210_MCLK_FROM_PAD,
    });
    if (s_record_codec_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ES7210 codec");
        return ESP_FAIL;
    }

    s_record_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .codec_if = s_record_codec_if,
        .data_if = s_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    });
    if (s_record_dev == NULL ||
        esp_codec_dev_open(s_record_dev, &sample_info) != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "Failed to init ES7210");
        return ESP_FAIL;
    }

    esp_codec_dev_set_in_gain(s_record_dev, 36.0);
    return ESP_OK;
}

/**
 * @brief 执行真正的 codec 硬件初始化。
 *
 * 该函数只允许在持有 `s_resource_mutex` 且生命周期引用计数为 0 时调用。
 * 对外的 `audio_codec_init()` 负责幂等 retain，避免多个上层模块重复创建 I2S
 * 与 codec 对象。
 *
 * @return `ESP_OK` 表示成功；其他错误表示总线、控制接口或 codec 打开失败。
 */
static esp_err_t audio_codec_init_hardware_locked(void)
{
    esp_err_t ret;
    audio_codec_i2s_cfg_t i2s_cfg = {0};

    ret = audio_i2c_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = audio_codec_bus_init();
    if (ret != ESP_OK)
    {
        return ret;
    }

    i2s_cfg.rx_handle = audio_codec_bus_get_rx_handle(); // 录音 RX 通道。
    i2s_cfg.tx_handle = audio_codec_bus_get_tx_handle(); // 播放 TX 通道。
    i2s_cfg.port = audio_codec_bus_get_port();           // 绑定 I2S 端口。
    if (i2s_cfg.rx_handle == NULL || i2s_cfg.tx_handle == NULL)
    {
        ret = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "Audio codec bus handles are unavailable");
        goto init_failed;
    }

    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL)
    {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        goto init_failed;
    }

    ret = audio_codec_init_default_interfaces();
    if (ret != ESP_OK)
    {
        goto init_failed;
    }

    ret = audio_es8311_init();
    if (ret != ESP_OK)
    {
        goto init_failed;
    }

    ret = audio_es7210_init();
    if (ret != ESP_OK)
    {
        goto init_failed;
    }

    ESP_LOGI(TAG,
             "Audio codec ready: tx_mode=std rx_mode=tdm physical_rx_slots=%d "
             "logical_input_format=%s logical_input_channels=%d "
             "input_reference=%d in=%dHz/%dbit/%dch out=%dHz/%dbit/%dch",
             s_physical_rx_slots, s_logical_input_format,
             s_logical_input_channels, s_input_reference ? 1 : 0,
             s_input_sample_rate, s_bits_per_sample, s_logical_input_channels,
             s_output_sample_rate, s_bits_per_sample, s_output_channels);
    return ESP_OK;

init_failed:
    audio_codec_deinit_hardware_locked();
    return ret;
}

/**
 * @brief 反初始化真实 codec 硬件资源。
 *
 * 该函数只允许在持有 `s_resource_mutex` 且引用计数已经降到 0 时调用。
 * 它不会处理引用计数和会话 owner，只负责按安全顺序释放底层硬件对象。
 *
 * @return `ESP_OK` 表示成功。
 */
static esp_err_t audio_codec_deinit_hardware_locked(void)
{
    // 释放顺序遵循“先 device，再接口对象，再总线”，避免悬挂引用。
    if (s_playback_dev != NULL)
    {
        esp_codec_dev_close(s_playback_dev);
        esp_codec_dev_delete(s_playback_dev);
        s_playback_dev = NULL;
    }

    if (s_record_dev != NULL)
    {
        esp_codec_dev_close(s_record_dev);
        esp_codec_dev_delete(s_record_dev);
        s_record_dev = NULL;
    }

    if (s_playback_codec_if != NULL)
    {
        audio_codec_delete_codec_if(s_playback_codec_if);
        s_playback_codec_if = NULL;
    }

    if (s_record_codec_if != NULL)
    {
        audio_codec_delete_codec_if(s_record_codec_if);
        s_record_codec_if = NULL;
    }

    if (s_playback_ctrl_if != NULL)
    {
        audio_codec_delete_ctrl_if(s_playback_ctrl_if);
        s_playback_ctrl_if = NULL;
    }

    if (s_record_ctrl_if != NULL)
    {
        audio_codec_delete_ctrl_if(s_record_ctrl_if);
        s_record_ctrl_if = NULL;
    }

    if (s_gpio_if != NULL)
    {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }

    if (s_data_if != NULL)
    {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }

    (void)audio_codec_bus_deinit();

    ESP_LOGI(TAG, "Audio codec deinitialized");
    return ESP_OK;
}

/**
 * @brief 初始化音频 codec 子系统并持有一次生命周期引用。
 *
 * 该接口是幂等 retain：第一次调用真正打开 ES8311/ES7210/I2S，后续调用只增加
 * 引用计数。这样危险识别、语音、播放等模块可以独立 start/stop，不会互相拆掉
 * 全局音频硬件。
 *
 * @return `ESP_OK` 表示成功；其他错误表示 mutex、总线、控制接口或 codec 打开失败。
 */
esp_err_t audio_codec_init(void)
{
    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_lifecycle_ref_count > 0)
    {
        s_lifecycle_ref_count++;
        ESP_LOGD(TAG, "Audio codec retain: refs=%lu",
                 (unsigned long)s_lifecycle_ref_count);
        audio_codec_unlock_resources();
        return ESP_OK;
    }

    audio_codec_load_volume_preference_locked();
    ret = audio_codec_init_hardware_locked();
    if (ret == ESP_OK)
    {
        s_lifecycle_ref_count = 1;
        ESP_LOGI(TAG, "Audio codec retain: refs=%lu",
                 (unsigned long)s_lifecycle_ref_count);
    }
    audio_codec_unlock_resources();
    return ret;
}

/**
 * @brief 释放一次音频 codec 生命周期引用。
 *
 * 引用计数归零后才真正释放硬件；如果仍有录音或播放会话占用，返回
 * `ESP_ERR_INVALID_STATE`，避免在活跃 I2S 传输中关闭 codec。
 *
 * @return `ESP_OK` 表示释放成功；其他错误表示引用计数异常或仍有会话占用。
 */
esp_err_t audio_codec_deinit(void)
{
    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (s_lifecycle_ref_count == 0)
    {
        audio_codec_unlock_resources();
        return ESP_OK;
    }

    s_lifecycle_ref_count--;
    ESP_LOGD(TAG, "Audio codec release: refs=%lu",
             (unsigned long)s_lifecycle_ref_count);
    if (s_lifecycle_ref_count > 0)
    {
        audio_codec_unlock_resources();
        return ESP_OK;
    }

    if (s_input_session_active || s_output_session_active)
    {
        s_lifecycle_ref_count = 1;
        ESP_LOGW(TAG,
                 "Refuse to deinit audio codec while sessions are active: "
                 "input=%s(%d), output=%s(%d)",
                 audio_codec_owner_name(s_input_session_owner),
                 s_input_session_active ? 1 : 0,
                 audio_codec_owner_name(s_output_session_owner),
                 s_output_session_active ? 1 : 0);
        audio_codec_unlock_resources();
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_codec_deinit_hardware_locked();
    audio_codec_unlock_resources();
    return ret;
}

/**
 * @brief 通用申请音频独占会话。
 *
 * @param[in,out] active 会话激活标志。
 * @param[in,out] current_owner 当前 owner 存储。
 * @param[in] owner 申请者。
 * @param[in] timeout_ms 等待互斥锁的超时，单位毫秒。
 * @param[in] session_name 日志中的会话名称。
 * @return `ESP_OK` 表示获得会话。
 */
static esp_err_t audio_codec_acquire_session(bool *active,
                                             audio_codec_owner_t *current_owner,
                                             audio_codec_owner_t owner,
                                             uint32_t timeout_ms,
                                             const char *session_name)
{
    bool busy_logged = false; // 避免等待期间反复刷屏，只记录第一次 owner 冲突。
    const TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks =
        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (active == NULL || current_owner == NULL || session_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms > 0U && timeout_ms != UINT32_MAX && timeout_ticks == 0)
    {
        timeout_ticks = 1;
    }

    while (true)
    {
        uint32_t lock_timeout_ms = 0U; // 本轮等待 mutex 的时间，仍受总 timeout 约束。
        if (timeout_ms == UINT32_MAX)
        {
            lock_timeout_ms = UINT32_MAX;
        }
        else if (timeout_ms > 0U)
        {
            const TickType_t elapsed = xTaskGetTickCount() - start_ticks;
            const TickType_t remaining_ticks =
                elapsed < timeout_ticks ? timeout_ticks - elapsed : 0;
            lock_timeout_ms =
                ((uint32_t)remaining_ticks * 1000U) / configTICK_RATE_HZ + 1U;
        }

        esp_err_t ret = audio_codec_lock_resources(lock_timeout_ms);
        if (ret != ESP_OK)
        {
            return ret;
        }

        if (s_lifecycle_ref_count == 0 || s_record_dev == NULL ||
            s_playback_dev == NULL)
        {
            audio_codec_unlock_resources();
            return ESP_ERR_INVALID_STATE;
        }

        if (!*active)
        {
            *active = true;
            *current_owner = owner;
            ESP_LOGI(TAG, "%s session acquired by %s",
                     session_name, audio_codec_owner_name(owner));
            audio_codec_publish_session_change();
            audio_codec_unlock_resources();
            return ESP_OK;
        }

        if (!busy_logged)
        {
            busy_logged = true;
            ESP_LOGW(TAG, "%s session busy: owner=%s, requester=%s",
                     session_name, audio_codec_owner_name(*current_owner),
                     audio_codec_owner_name(owner));
        }
        audio_codec_unlock_resources();

        if (timeout_ms == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }

        TickType_t delay_ticks = pdMS_TO_TICKS(10U);
        if (delay_ticks == 0)
        {
            delay_ticks = 1;
        }
        if (timeout_ms != UINT32_MAX)
        {
            const TickType_t elapsed = xTaskGetTickCount() - start_ticks;
            if (elapsed >= timeout_ticks)
            {
                return ESP_ERR_TIMEOUT;
            }
            const TickType_t remaining_ticks = timeout_ticks - elapsed;
            if (delay_ticks > remaining_ticks)
            {
                delay_ticks = remaining_ticks;
            }
        }
        vTaskDelay(delay_ticks);
    }
}

/**
 * @brief 通用释放音频独占会话。
 *
 * @param[in,out] active 会话激活标志。
 * @param[in,out] current_owner 当前 owner 存储。
 * @param[in] owner 释放者。
 * @param[in] session_name 日志中的会话名称。
 * @return `ESP_OK` 表示释放成功。
 */
static esp_err_t audio_codec_release_session(bool *active,
                                             audio_codec_owner_t *current_owner,
                                             audio_codec_owner_t owner,
                                             const char *session_name)
{
    if (active == NULL || current_owner == NULL || session_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (!*active)
    {
        audio_codec_unlock_resources();
        return ESP_ERR_INVALID_STATE;
    }
    if (*current_owner != owner)
    {
        ESP_LOGW(TAG, "%s session release owner mismatch: owner=%s, requester=%s",
                 session_name, audio_codec_owner_name(*current_owner),
                 audio_codec_owner_name(owner));
        audio_codec_unlock_resources();
        return ESP_ERR_INVALID_STATE;
    }

    *active = false;
    *current_owner = AUDIO_CODEC_OWNER_SYSTEM;
    ESP_LOGI(TAG, "%s session released by %s",
             session_name, audio_codec_owner_name(owner));
    audio_codec_publish_session_change();
    audio_codec_unlock_resources();
    return ESP_OK;
}

/**
 * @brief 申请独占录音输入会话。
 *
 * @param[in] owner 申请者标识。
 * @param[in] timeout_ms 等待已有 owner 释放的超时，单位毫秒；0 表示不等待。
 * @return `ESP_OK` 表示获得会话。
 */
esp_err_t audio_codec_acquire_input(audio_codec_owner_t owner,
                                    uint32_t timeout_ms)
{
    return audio_codec_acquire_session(&s_input_session_active,
                                       &s_input_session_owner,
                                       owner, timeout_ms, "input");
}

/**
 * @brief 释放独占录音输入会话。
 *
 * @param[in] owner 释放者标识。
 * @return `ESP_OK` 表示释放成功。
 */
esp_err_t audio_codec_release_input(audio_codec_owner_t owner)
{
    return audio_codec_release_session(&s_input_session_active,
                                       &s_input_session_owner,
                                       owner, "input");
}

/**
 * @brief 申请独占播放输出会话。
 *
 * @param[in] owner 申请者标识。
 * @param[in] timeout_ms 等待已有 owner 释放的超时，单位毫秒；0 表示不等待。
 * @return `ESP_OK` 表示获得会话。
 */
esp_err_t audio_codec_acquire_output(audio_codec_owner_t owner,
                                     uint32_t timeout_ms)
{
    return audio_codec_acquire_session(&s_output_session_active,
                                       &s_output_session_owner,
                                       owner, timeout_ms, "output");
}

/**
 * @brief 释放独占播放输出会话。
 *
 * @param[in] owner 释放者标识。
 * @return `ESP_OK` 表示释放成功。
 */
esp_err_t audio_codec_release_output(audio_codec_owner_t owner)
{
    return audio_codec_release_session(&s_output_session_active,
                                       &s_output_session_owner,
                                       owner, "output");
}

esp_err_t audio_codec_get_session_snapshot(
    audio_codec_session_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }

    snapshot->input_active = s_input_session_active;
    snapshot->input_owner = s_input_session_owner;
    snapshot->output_active = s_output_session_active;
    snapshot->output_owner = s_output_session_owner;

    audio_codec_unlock_resources();
    return ESP_OK;
}

esp_err_t audio_codec_get_cached_session_snapshot(
    audio_codec_session_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_cache_lock);
    *snapshot = s_cached_session_snapshot;
    taskEXIT_CRITICAL(&s_cache_lock);
    return ESP_OK;
}

esp_err_t audio_codec_set_session_change_callback(
    audio_codec_session_change_cb_t callback, void *user_ctx)
{
    if (s_session_change_cb != NULL && callback != s_session_change_cb)
    {
        /* 覆盖既有回调会让原注册方的会话变更通知静默失效；只打日志防呆，
         * 不拒绝安装，避免破坏启动顺序。 */
        ESP_LOGW(TAG, "session change callback replaced");
    }
    s_session_change_cb = callback;
    s_session_change_ctx = user_ctx;
    return ESP_OK;
}

/**
 * @brief 从录音链路读取 PCM 数据。
 * @param[out] buffer 接收缓冲区。
 * @param[in] bytes 目标读取字节数。
 * @param[out] bytes_read 实际读取字节数，可为 NULL。
 * @param[in] ticks_to_wait 最大等待 tick 数。
 * @return `ESP_OK` 表示成功；`ESP_ERR_TIMEOUT` 表示超时前仅拿到部分数据；其他错误表示底层读取失败。
 */
esp_err_t audio_codec_read(void *buffer, size_t bytes, size_t *bytes_read,
                           TickType_t ticks_to_wait)
{
    TickType_t start_ticks = xTaskGetTickCount();
    esp_err_t ret = audio_codec_read_with_timeout_slice(
        s_record_dev, buffer, bytes, bytes_read, ticks_to_wait,
        audio_codec_get_read_slice_bytes());
    size_t actual_bytes = bytes_read != NULL ? *bytes_read : bytes; // 实际读取长度。

    if (ret == ESP_OK && buffer != NULL &&
        !audio_codec_channel_order_is_identity())
    {
        audio_codec_reorder_to_logical_input_format(
            (int16_t *)buffer, actual_bytes / sizeof(int16_t));
    }

    ESP_LOGD(TAG,
             "read target=%u actual=%u elapsed_ticks=%lu wait_ticks=%lu ret=%s",
             (unsigned int)bytes,
             (unsigned int)actual_bytes,
             (unsigned long)(xTaskGetTickCount() - start_ticks),
             (unsigned long)ticks_to_wait, esp_err_to_name(ret));
    return ret;
}

/**
 * @brief 向播放链路写入 PCM 数据。
 * @param[in] buffer PCM 数据地址。
 * @param[in] bytes 待写入字节数。
 * @return `ESP_OK` 表示成功；其他错误表示参数非法或底层写入失败。
 */
esp_err_t audio_codec_write(const void *buffer, size_t bytes)
{
    if (s_playback_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer == NULL && bytes > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (bytes == 0)
    {
        return ESP_OK;
    }

    if (esp_codec_dev_write(s_playback_dev, (void *)buffer, (int)bytes) !=
        ESP_CODEC_DEV_OK)
    {
        ESP_LOGW(TAG, "audio codec write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 通过预装静音数据刷新 TX 通道。
 * @return `ESP_OK` 表示成功；其他错误表示底层 I2S 预装失败。
 */
esp_err_t audio_codec_flush_output(void)
{
    esp_err_t ret = ESP_OK;
    i2s_chan_handle_t tx_handle = audio_codec_bus_get_tx_handle(); // TX 通道句柄。
    size_t preload_bytes = 0;                                      // 计划预装静音字节数。
    size_t bytes_loaded = 0;                                       // 实际预装成功字节数。

    if (tx_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    preload_bytes = audio_codec_get_i2s_bus_preload_bytes();
    if (preload_bytes > sizeof(s_tx_silence_preload))
    {
        preload_bytes = sizeof(s_tx_silence_preload);
    }

    ret = i2s_channel_disable(tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to disable I2S TX for flush: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_preload_data(tx_handle, s_tx_silence_preload,
                                   preload_bytes, &bytes_loaded);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to preload silence for I2S TX flush: %s",
                 esp_err_to_name(ret));
        (void)i2s_channel_enable(tx_handle);
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to enable I2S TX after flush: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    if (bytes_loaded == 0)
    {
        ESP_LOGW(TAG, "no silence bytes were preloaded during TX flush");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 设置播放音量。
 * @param[in] volume 音量百分比，范围为 0~100。
 * @return `ESP_OK` 表示成功；其他错误表示参数非法或底层设置失败。
 */
esp_err_t audio_codec_set_volume(int volume)
{
    if (volume < 0 || volume > 100)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (s_playback_dev == NULL)
    {
        audio_codec_unlock_resources();
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_codec_dev_set_out_vol(s_playback_dev, volume) != ESP_CODEC_DEV_OK)
    {
        audio_codec_unlock_resources();
        return ESP_FAIL;
    }
    s_current_volume = volume;
    audio_codec_unlock_resources();
    return ESP_OK;
}

/**
 * @brief 设置并持久化用户扬声器音量。
 * @param[in] volume 音量百分比，范围为 0~100。
 * @return `ESP_OK` 表示硬件与 NVS 均更新成功。
 */
esp_err_t audio_codec_set_volume_preference(int volume)
{
    esp_err_t ret = audio_codec_set_volume(volume);
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (s_persisted_volume == volume)
    {
        audio_codec_unlock_resources();
        return ESP_OK;
    }

    nvs_handle_t handle = 0;
    ret = nvs_open(kVolumeNvsNamespace, NVS_READWRITE, &handle);
    if (ret == ESP_OK)
    {
        ret = nvs_set_u8(handle, kVolumeNvsKey, (uint8_t)volume);
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }
    if (handle != 0)
    {
        nvs_close(handle);
    }
    if (ret == ESP_OK)
    {
        s_persisted_volume = volume;
    }
    audio_codec_unlock_resources();
    return ret;
}

/**
 * @brief 获取当前缓存音量。
 * @param[out] volume 输出参数，返回 0~100。
 * @return `ESP_OK` 表示成功；其他错误表示参数非法或播放设备未就绪。
 */
esp_err_t audio_codec_get_volume(int *volume)
{
    if (volume == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_codec_lock_resources(UINT32_MAX);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (s_playback_dev == NULL)
    {
        audio_codec_unlock_resources();
        return ESP_ERR_INVALID_STATE;
    }
    *volume = s_current_volume;
    audio_codec_unlock_resources();
    return ESP_OK;
}

/**
 * @brief 设置播放静音状态。
 * @param[in] enable true 表示静音。
 * @return `ESP_OK` 表示成功；其他错误表示播放设备未就绪或底层设置失败。
 */
esp_err_t audio_codec_set_mute(bool enable)
{
    if (s_playback_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_out_mute(s_playback_dev, enable) ==
                   ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}

/**
 * @brief 控制外部功放使能引脚。
 * @param[in] enable true 表示打开功放。
 * @return `ESP_OK` 表示成功；其他错误表示 GPIO 控制接口未就绪或底层设置失败。
 */
esp_err_t audio_codec_set_pa_enable(bool enable)
{
    if (s_gpio_if == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return s_gpio_if->set(AUDIO_PA_CTRL_GPIO, enable) == ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}

/**
 * @brief 设置录音增益。
 * @param[in] db 目标增益，单位为 dB。
 * @return `ESP_OK` 表示成功；其他错误表示录音设备未就绪或底层设置失败。
 */
esp_err_t audio_codec_set_record_gain(float db)
{
    if (s_record_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_in_gain(s_record_dev, db) == ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}
