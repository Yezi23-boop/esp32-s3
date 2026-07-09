/**
 * @file espdl_audio_runtime.h
 * @brief ESP-DL 单模型实时音频推理运行时（C API）。
 *
 * 镜像 traffic_audio_runtime 的生命周期管理，但使用 ESP-DL active 单模型推理：
 *   ES7210 ADC (24kHz, 2ch TDM) → 提取主麦克风 → 重采样到 16kHz
 *   → Fbank 特征提取 → V3.4 T90 sharp 单模型 → 回调通知上层
 *
 * 使用方法：
 *   1. espdl_audio_runtime_start() 启动后台 FreeRTOS 任务
 *   2. 任务持续读取麦克风、推理、通过回调上报结果
 *   3. espdl_audio_runtime_stop() 请求停止并等待任务退出
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdl_model_runner.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 运行时生命周期状态。 */
typedef enum {
    ESPDL_AUDIO_RUNTIME_STATE_IDLE = 0,
    ESPDL_AUDIO_RUNTIME_STATE_STARTING,
    ESPDL_AUDIO_RUNTIME_STATE_RUNNING,
    ESPDL_AUDIO_RUNTIME_STATE_STOPPING,
    ESPDL_AUDIO_RUNTIME_STATE_FAILED,
} espdl_audio_runtime_state_t;

/** 运行时配置。 */
typedef struct {
    size_t input_chunk_frames;   /**< 每次读取的硬件帧数，0 使用默认值。 */
    uint32_t read_timeout_ms;    /**< 音频读取超时，单位毫秒。 */
    uint32_t task_stack_size;    /**< FreeRTOS 任务栈大小，单位字节。 */
    UBaseType_t task_priority;   /**< FreeRTOS 任务优先级。 */
} espdl_audio_runtime_config_t;

/**
 * @brief 实时推理结果回调。
 *
 * 每次单模型推理完成后调用，由运行时任务上下文触发。
 * 回调内应尽快完成处理，避免阻塞推理循环。
 *
 * @param[in] result 单模型推理结果。
 * @param[in] user_data 用户数据指针。
 */
typedef void (*espdl_audio_runtime_result_callback_t)(
    const espdl_model_result_t *result,
    void *user_data);

/**
 * @brief PCM tap 连续音频块元数据。
 *
 * 包含当前 16kHz 单声道 PCM chunk 的绝对样本索引信息，用于上层按推理
 * 结果的窗口索引对齐切片；runtime 不知道 recorder 或 SD 卡。
 */
typedef struct {
    uint64_t absolute_sample_index;  /**< chunk 首样本的绝对索引（16kHz 单声道）。 */
    uint32_t window_samples;         /**< chunk 样本数；沿用字段名以保持 API 兼容。 */
    uint32_t stride_samples;         /**< PCM chunk 无滑窗步进，当前固定为 0。 */
} espdl_audio_pcm_window_meta_t;

/**
 * @brief PCM tap 回调。
 *
 * 在每次 24kHz→16kHz 重采样后调用，提供连续 PCM chunk 和绝对样本索引。
 * 回调内应尽快完成处理，避免阻塞推理循环。
 *
 * @param[in] pcm_data 连续 PCM 数据（int16_t 格式，16kHz 单声道）。
 * @param[in] samples PCM 数据样本数。
 * @param[in] meta 窗口元数据（绝对样本索引等）。
 * @param[in] user_data 用户数据指针。
 */
typedef void (*espdl_audio_pcm_tap_callback_t)(
    const int16_t *pcm_data,
    size_t samples,
    const espdl_audio_pcm_window_meta_t *meta,
    void *user_data);

/**
 * @brief 启动 ESP-DL 实时音频推理运行时。
 *
 * @param[in] config 运行时配置，NULL 使用默认值。
 * @return ESP_OK 表示已启动。
 */
esp_err_t espdl_audio_runtime_start(
    const espdl_audio_runtime_config_t *config);

/**
 * @brief 停止运行时。
 *
 * @param[in] timeout_ms 等待超时，0 使用默认 2000ms。
 * @return ESP_OK 表示已停止。
 */
esp_err_t espdl_audio_runtime_stop(uint32_t timeout_ms);

/**
 * @brief 查询运行时是否正在运行。
 */
bool espdl_audio_runtime_is_running(void);

/**
 * @brief 获取当前运行时状态。
 */
espdl_audio_runtime_state_t espdl_audio_runtime_get_state(void);

/**
 * @brief 设置 ESP-DL 单模型 danger 概率阈值。
 *
 * 可在运行前或运行中调用。运行前调用会保存为下次启动的阈值；运行中调用会立即
 * 下发到当前模型 runner。
 *
 * @param[in] threshold danger 概率阈值，合法范围为 0.0f 到 1.0f。
 * @return ESP_OK 表示设置成功。
 */
esp_err_t espdl_audio_runtime_set_danger_threshold(float threshold);

/**
 * @brief 注册推理结果回调。
 *
 * @param[in] callback 回调函数，NULL 注销。
 * @param[in] user_data 用户数据。
 * @return ESP_OK 表示注册成功。
 */
esp_err_t espdl_audio_runtime_set_result_callback(
    espdl_audio_runtime_result_callback_t callback,
    void *user_data);

/**
 * @brief 注册 PCM tap 回调。
 *
 * 在重采样后发布连续 PCM chunk。上层如需保存事件样本，应结合
 * `espdl_model_result_t.window_end_sample_index` 自己完成切片。
 *
 * @param[in] callback 回调函数，NULL 注销。
 * @param[in] user_data 用户数据。
 * @return ESP_OK 表示注册成功。
 */
esp_err_t espdl_audio_runtime_set_pcm_tap_callback(
    espdl_audio_pcm_tap_callback_t callback,
    void *user_data);

#ifdef __cplusplus
}
#endif
