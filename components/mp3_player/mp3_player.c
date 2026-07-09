#include "mp3_player.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "audio_player.h"
#include "audio_codec.h"
#include "audio_platform_config.h"

static const char *TAG = "mp3_player";
static bool s_output_session_acquired = false;

static esp_err_t mp3_player_acquire_output_session(const char *reason)
{
    if (s_output_session_acquired)
    {
        return ESP_OK;
    }

    esp_err_t ret = audio_codec_acquire_output(AUDIO_CODEC_OWNER_AUDIO_PLAYER, 0U);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "resource_acquire_denied: resource=audio_output owner=audio_player reason=%s ret=%s",
                 reason != NULL ? reason : "unknown", esp_err_to_name(ret));
        return ret;
    }

    s_output_session_acquired = true;
    return ESP_OK;
}

static void mp3_player_release_output_session(const char *reason)
{
    if (!s_output_session_acquired)
    {
        return;
    }

    esp_err_t ret = audio_codec_release_output(AUDIO_CODEC_OWNER_AUDIO_PLAYER);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "resource_release failed: resource=audio_output owner=audio_player reason=%s ret=%s",
                 reason != NULL ? reason : "unknown", esp_err_to_name(ret));
        return;
    }

    s_output_session_acquired = false;
    ESP_LOGI(TAG, "resource_release: resource=audio_output owner=audio_player reason=%s",
             reason != NULL ? reason : "unknown");
}

/**
 * @brief 音频播放器事件回调。
 * @param[in] ctx 播放器事件上下文。
 * @return 无返回值。
 */
static void audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    switch (ctx->audio_event)
    {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        ESP_LOGI(TAG, "播放器状态: 空闲");
        mp3_player_release_output_session("idle");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "播放器状态: 正在播放");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
        ESP_LOGI(TAG, "播放器状态: 切换到下一首");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "播放器状态: 暂停");
        mp3_player_release_output_session("pause");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_SHUTDOWN:
        ESP_LOGI(TAG, "播放器状态: 关闭");
        mp3_player_release_output_session("shutdown");
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE:
        ESP_LOGE(TAG, "错误: 未知文件类型");
        mp3_player_release_output_session("unknown_file_type");
        break;
    default:
        ESP_LOGW(TAG, "未知事件: %d", ctx->audio_event);
        break;
    }
}

/**
 * @brief 静音控制回调。
 * @param[in] setting 目标静音状态。
 * @return 底层 `audio_codec_set_mute()` 返回值。
 */
static esp_err_t audio_mute_callback(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bool mute = (setting == AUDIO_PLAYER_MUTE);
    ESP_LOGI(TAG, "静音设置: %s", mute ? "开启" : "关闭");
    return audio_codec_set_mute(mute);
}

/**
 * @brief 音频数据写入回调。
 * @param[in] audio_buffer 待输出音频数据。
 * @param[in] len 数据长度，单位为字节。
 * @param[out] bytes_written 实际写入字节数。
 * @param[in] timeout_ms 超时参数，当前实现未使用。
 * @return `ESP_OK` 表示写入成功；其他错误表示底层 codec 写入失败。
 */
static esp_err_t audio_write_callback(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if (audio_codec_write(audio_buffer, len) == ESP_OK)
    {
        *bytes_written = len;
        return ESP_OK;
    }
    *bytes_written = 0;
    return ESP_FAIL;
}

/**
 * @brief I2S 时钟重配置回调。
 * @param[in] rate 请求采样率，单位为 Hz。
 * @param[in] bits_cfg 请求位宽配置。
 * @param[in] ch 请求声道模式。
 * @return 当前实现始终返回 `ESP_OK`。
 *
 * @note 当前硬件输出链路固定使用 `AUDIO_PLATFORM_HW_SAMPLE_RATE`，这里只做日志提示而不真正切换采样率。
 */
static esp_err_t audio_clk_reconfig_callback(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    ESP_LOGI(TAG, "重配置I2S时钟: %lu Hz, %lu bits, %s",
             rate, bits_cfg,
             ch == I2S_SLOT_MODE_MONO ? "单声道" : "立体声");
    if (rate != AUDIO_PLATFORM_HW_SAMPLE_RATE)
    {
        ESP_LOGW(TAG, "当前音频输出固定为 %d Hz，输入请求 %lu Hz",
                 AUDIO_PLATFORM_HW_SAMPLE_RATE, rate);
    }

    return ESP_OK;
}

/**
 * @brief 初始化 MP3 播放器。
 * @return `ESP_OK` 表示成功；其他错误表示底层 `audio_player` 创建或回调注册失败。
 */
esp_err_t mp3_player_init(void)
{
    ESP_LOGI(TAG, "初始化MP3播放器");

    audio_player_config_t config = {
        .mute_fn = audio_mute_callback,
        .write_fn = audio_write_callback,
        .clk_set_fn = audio_clk_reconfig_callback,
        .priority = 5,
        .coreID = 0
    };

    esp_err_t ret = audio_player_new(config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建audio_player失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = audio_player_callback_register(audio_player_callback, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "注册回调失败: %s", esp_err_to_name(ret));
        audio_player_delete();
        return ret;
    }

    ESP_LOGI(TAG, "MP3播放器初始化成功");
    return ESP_OK;
}

/**
 * @brief 播放指定音频文件。
 * @param[in] file_path 文件路径。
 * @return `ESP_OK` 表示播放已启动；其他错误表示路径非法、打开文件失败或底层播放失败。
 */
esp_err_t mp3_player_play_file(const char *file_path)
{
    if (file_path == NULL)
    {
        ESP_LOGE(TAG, "文件路径为空");
        return ESP_ERR_INVALID_ARG;
    }

    const char *file_ext = strrchr(file_path, '.');
    const char *format_name = "未知";

    if (file_ext != NULL)
    {
        if (strcasecmp(file_ext, ".mp3") == 0)
        {
            format_name = "MP3";
        }
        else if (strcasecmp(file_ext, ".wav") == 0)
        {
            format_name = "WAV";
        }
    }

    ESP_LOGI(TAG, "准备播放文件: %s (格式: %s)", file_path, format_name);

    esp_err_t ret = mp3_player_acquire_output_session("play_file");
    if (ret != ESP_OK)
    {
        return ret;
    }

    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "无法打开文件: %s", file_path);
        mp3_player_release_output_session("open_failed");
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    ESP_LOGI(TAG, "文件大小: %ld 字节 (%.2f MB)", file_size, file_size / 1024.0 / 1024.0);

    /* `audio_player_play()` 会接管文件句柄生命周期；
     * 只有启动失败时，调用方才需要自己关闭 `fp`。 */
    ret = audio_player_play(fp);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(ret));
        fclose(fp);
        mp3_player_release_output_session("play_failed");
        return ret;
    }

    ESP_LOGI(TAG, "开始播放 %s 文件", format_name);
    return ESP_OK;
}

/**
 * @brief 暂停播放。
 * @return 底层 `audio_player_pause()` 返回值。
 */
esp_err_t mp3_player_pause(void)
{
    ESP_LOGI(TAG, "暂停播放");
    esp_err_t ret = audio_player_pause();
    if (ret == ESP_OK)
    {
        mp3_player_release_output_session("pause");
    }
    return ret;
}

/**
 * @brief 恢复播放。
 * @return 底层 `audio_player_resume()` 返回值。
 */
esp_err_t mp3_player_resume(void)
{
    ESP_LOGI(TAG, "恢复播放");
    esp_err_t ret = mp3_player_acquire_output_session("resume");
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = audio_player_resume();
    if (ret != ESP_OK)
    {
        mp3_player_release_output_session("resume_failed");
    }
    return ret;
}

/**
 * @brief 停止播放。
 * @return 底层 `audio_player_stop()` 返回值。
 */
esp_err_t mp3_player_stop(void)
{
    ESP_LOGI(TAG, "停止播放");
    esp_err_t ret = audio_player_stop();
    if (ret == ESP_OK)
    {
        mp3_player_release_output_session("stop");
    }
    return ret;
}

/**
 * @brief 反初始化播放器。
 * @return 底层 `audio_player_delete()` 返回值。
 */
esp_err_t mp3_player_deinit(void)
{
    ESP_LOGI(TAG, "反初始化MP3播放器");
    esp_err_t ret = audio_player_delete();
    mp3_player_release_output_session("deinit");
    return ret;
}

/**
 * @brief 获取当前播放器状态。
 * @return 当前 `audio_player` 状态枚举。
 */
audio_player_state_t mp3_player_get_state(void)
{
    return audio_player_get_state();
}
