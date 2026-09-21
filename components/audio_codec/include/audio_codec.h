#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * 音频硬件引脚映射：
 * - I2C：codec 控制面
 * - I2S：音频数据面
 * - PA：外部功放使能控制
 */
#define AUDIO_I2C_SDA_GPIO (15)    // I2C SDA。
#define AUDIO_I2C_SCL_GPIO (14)    // I2C SCL。
#define AUDIO_I2S_ASDOUT_GPIO (40) // I2S TX 数据输出，到 DAC。
#define AUDIO_I2S_LRCK_GPIO (45)   // I2S LRCK/WS。
#define AUDIO_I2S_MCLK_GPIO (16)   // I2S MCLK。
#define AUDIO_I2S_SCLK_GPIO (41)   // I2S BCLK/SCLK。
#define AUDIO_I2S_DSDIN_GPIO (42)  // I2S RX 数据输入，来自 ADC。
#define AUDIO_PA_CTRL_GPIO (46)    // 外部功放使能引脚。

    /**
     * @brief 音频硬件资源使用者标识。
     *
     * 该枚举只描述“谁正在申请硬件会话”，不表达产品策略。底层用它打印
     * 冲突日志，方便定位录音/播放资源被哪个运行时占用。
     */
    typedef enum
    {
        AUDIO_CODEC_OWNER_SYSTEM = 0,            /**< 板级初始化或通用系统持有者。 */
        AUDIO_CODEC_OWNER_TRAFFIC_INFERENCE,    /**< 旧 Edge Impulse 交通声推理运行时。 */
        AUDIO_CODEC_OWNER_ESPDL_INFERENCE,      /**< ESP-DL 危险声音推理运行时。 */
        AUDIO_CODEC_OWNER_AUDIO_PLAYER,         /**< 本地提示音或 MP3 播放链路。 */
        AUDIO_CODEC_OWNER_ALERT_PLAYER,         /**< P0 危险提醒播放链路。 */
        AUDIO_CODEC_OWNER_AUDIO_RECORDER,       /**< 前台本地录音链路。 */
        AUDIO_CODEC_OWNER_OFFICIAL_CHAT,        /**< 官方聊天/语音链路。 */
        AUDIO_CODEC_OWNER_HERMES,               /**< AI Memory Watch / Hermes 语音链路。 */
        AUDIO_CODEC_OWNER_MUSIC_PLAYER,         /**< 在线音乐流播放链路。 */
    } audio_codec_owner_t;

    /** 音频独占会话只读快照。 */
    typedef struct
    {
        bool input_active;               /**< true 表示录音输入链路当前被占用。 */
        audio_codec_owner_t input_owner; /**< 当前录音输入 owner。 */
        bool output_active;              /**< true 表示播放输出链路当前被占用。 */
        audio_codec_owner_t output_owner; /**< 当前播放输出 owner。 */
    } audio_codec_session_snapshot_t;

    /**
     * @brief 返回音频 owner 的稳定日志名称。
     *
     * 该接口只用于日志和诊断展示，不表达资源策略。策略 owner 应读取
     * `audio_codec_get_session_snapshot()` 后自行判断优先级。
     *
     * @param[in] owner 音频资源申请者。
     * @return 静态字符串。
     */
    const char *audio_codec_owner_to_text(audio_codec_owner_t owner);

    /**
     * @brief 获取当前音频独占会话快照。
     *
     * 该接口不初始化或释放 codec 硬件，只读取 `audio_codec` 持有的 owner 状态。
     * 后台策略层可用它解释“麦克风被谁占用”，但仍不能直接抢占硬件。
     *
     * @param[out] snapshot 输出快照。
     * @return `ESP_OK` 表示读取成功；`ESP_ERR_INVALID_ARG` 表示输出参数为空。
     */
    esp_err_t audio_codec_get_session_snapshot(
        audio_codec_session_snapshot_t *snapshot);

    /**
     * @brief 获取会话状态的非阻塞缓存快照。
     *
     * 与 `audio_codec_get_session_snapshot()` 不同，本接口不获取资源 mutex，
     * 只读取由会话变更路径维护的缓存副本，适合低频策略任务在无锁上下文读取。
     *
     * @param[out] snapshot 输出快照。
     * @return `ESP_OK` 表示读取成功；`ESP_ERR_INVALID_ARG` 表示输出参数为空。
     */
    esp_err_t audio_codec_get_cached_session_snapshot(
        audio_codec_session_snapshot_t *snapshot);

    /** 会话变更回调；真实动作由注册方在自己的上下文执行，回调内不允许阻塞。 */
    typedef void (*audio_codec_session_change_cb_t)(void *user_ctx);

    /**
     * @brief 安装会话变更通知回调。
     *
     * 该回调在会话 acquire/release 成功的资源 mutex 持有路径内被调用，只允许
     * 复制状态或向其他任务发送通知，不允许执行音频/硬件/网络等阻塞操作。
     * 重复安装会覆盖之前的回调，返回 `ESP_OK`。
     *
     * @param[in] callback 回调函数，可为 NULL 表示卸载。
     * @param[in] user_ctx 回调上下文。
     * @return `ESP_OK` 表示安装成功。
     */
    esp_err_t audio_codec_set_session_change_callback(
        audio_codec_session_change_cb_t callback, void *user_ctx);

    /**
     * @brief 初始化音频 codec 子系统。
     *
     * 初始化流程会依次准备 I2C、I2S 总线、ES8311 播放链路和 ES7210 录音链路。
     * 该接口是幂等 retain：重复调用只增加生命周期引用计数，不会重复创建硬件对象。
     *
     * @return `ESP_OK` 表示成功；其他错误表示总线、控制接口或 codec 打开失败。
     */
    esp_err_t audio_codec_init(void);

    /**
     * @brief 释放一次音频 codec 生命周期引用。
     *
     * 只有引用计数归零且没有录音/播放会话占用时，才会真正关闭 codec 和 I2S。
     * 这样后台运行时 stop 不会误拆掉开机阶段或其他音频模块仍在使用的硬件。
     *
     * @return `ESP_OK` 表示成功。
     */
    esp_err_t audio_codec_deinit(void);

    /**
     * @brief 申请独占录音输入会话。
     *
     * 实时推理、语音对话等读取麦克风前必须先申请 input 会话，避免多个任务
     * 同时读取同一条 I2S RX/ES7210 链路。该接口不会初始化硬件，调用者仍需
     * 先通过 `audio_codec_init()` 持有生命周期引用。
     *
     * @param[in] owner 申请者标识，用于冲突日志和释放校验。
     * @param[in] timeout_ms 等待已有 owner 释放的超时，单位毫秒；0 表示不等待。
     * @return `ESP_OK` 表示获得会话；`ESP_ERR_TIMEOUT` 表示等待超时；
     *         `ESP_ERR_INVALID_STATE` 表示 codec 尚未初始化。
     */
    esp_err_t audio_codec_acquire_input(audio_codec_owner_t owner,
                                        uint32_t timeout_ms);

    /**
     * @brief 释放独占录音输入会话。
     *
     * @param[in] owner 释放者标识，必须与成功 acquire 的 owner 一致。
     * @return `ESP_OK` 表示释放成功；其他错误表示 owner 不匹配或当前没有会话。
     */
    esp_err_t audio_codec_release_input(audio_codec_owner_t owner);

    /**
     * @brief 申请独占播放输出会话。
     *
     * 该接口用于后续把提示音、MP3、聊天播放等输出路径也收敛到同一资源 owner。
     * 当前保持轻量实现，避免改变既有播放业务行为。
     *
     * @param[in] owner 申请者标识。
     * @param[in] timeout_ms 等待已有 owner 释放的超时，单位毫秒；0 表示不等待。
     * @return `ESP_OK` 表示获得会话；`ESP_ERR_TIMEOUT` 表示等待超时；
     *         `ESP_ERR_INVALID_STATE` 表示 codec 尚未初始化。
     */
    esp_err_t audio_codec_acquire_output(audio_codec_owner_t owner,
                                         uint32_t timeout_ms);

    /**
     * @brief 释放独占播放输出会话。
     *
     * @param[in] owner 释放者标识，必须与成功 acquire 的 owner 一致。
     * @return `ESP_OK` 表示释放成功；其他错误表示 owner 不匹配或当前没有会话。
     */
    esp_err_t audio_codec_release_output(audio_codec_owner_t owner);

    /**
     * @brief 从录音链路读取 PCM 数据。
     * @param[out] buffer 接收缓冲区地址。
     * @param[in] bytes 期望读取字节数。
     * @param[out] bytes_read 返回实际读取字节数，可为 NULL。
     * @param[in] ticks_to_wait 最大等待 tick 数，`portMAX_DELAY` 表示无限等待。
     * @return `ESP_OK` 表示按要求读满；`ESP_ERR_TIMEOUT` 表示超时前只拿到部分数据；其他错误表示底层读取失败。
     */
    esp_err_t audio_codec_read(void *buffer, size_t bytes, size_t *bytes_read,
                               TickType_t ticks_to_wait);

    /**
     * @brief 向播放链路写入 PCM 数据。
     * @param[in] buffer PCM 数据地址。
     * @param[in] bytes 待写入字节数。
     * @return `ESP_OK` 表示成功；其他错误表示参数非法或底层写入失败。
     */
    esp_err_t audio_codec_write(const void *buffer, size_t bytes);

    /**
     * @brief 通过预装静音数据刷新 TX 通道，减少尾音残留。
     * @return `ESP_OK` 表示成功；其他错误表示底层 I2S 预装失败。
     */
    esp_err_t audio_codec_flush_output(void);

    /**
     * @brief 设置播放音量。
     * @param[in] volume 音量百分比，范围为 0~100。
     * @return `ESP_OK` 表示成功；其他错误表示参数非法或底层设置失败。
     */
    esp_err_t audio_codec_set_volume(int volume);

    /**
     * @brief 设置并持久化用户扬声器音量。
     *
     * 音量立即应用到播放 codec，并在值发生变化时写入 NVS；下次
     * `audio_codec_init()` 会自动恢复该值。
     *
     * @param[in] volume 音量百分比，范围为 0~100。
     * @return `ESP_OK` 表示硬件和 NVS 均更新成功；其他错误表示设置或保存失败。
     */
    esp_err_t audio_codec_set_volume_preference(int volume);

    /**
     * @brief 获取当前缓存音量。
     * @param[out] volume 输出参数，返回 0~100。
     * @return `ESP_OK` 表示成功；其他错误表示参数非法或播放设备未就绪。
     */
    esp_err_t audio_codec_get_volume(int *volume);

    /**
     * @brief 设置播放静音状态。
     * @param[in] enable true 表示静音，false 表示取消静音。
     * @return `ESP_OK` 表示成功；其他错误表示播放设备未就绪或底层设置失败。
     */
    esp_err_t audio_codec_set_mute(bool enable);

    /**
     * @brief 控制外部功放使能引脚。
     * @param[in] enable true 表示打开功放，false 表示关闭功放。
     * @return `ESP_OK` 表示成功；其他错误表示 GPIO 控制接口未就绪或底层设置失败。
     */
    esp_err_t audio_codec_set_pa_enable(bool enable);

    /**
     * @brief 设置录音增益。
     * @param[in] db 目标增益，单位为 dB。
     * @return `ESP_OK` 表示成功；其他错误表示录音设备未就绪或底层设置失败。
     */
    esp_err_t audio_codec_set_record_gain(float db);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_CODEC_H
