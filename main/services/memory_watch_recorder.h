#ifndef MEMORY_WATCH_RECORDER_H
#define MEMORY_WATCH_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/memory_watch_ogg_opus_muxer.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MEMORY_WATCH_RECORDER_DEFAULT_MAX_DURATION_MS 30000U
#define MEMORY_WATCH_RECORDER_DEFAULT_MIN_DURATION_MS 500U
#define MEMORY_WATCH_RECORDER_OPUS_FRAME_DURATION_MS 60U
#define MEMORY_WATCH_RECORDER_DEFAULT_INPUT_TIMEOUT_MS 500U
#define MEMORY_WATCH_RECORDER_DEFAULT_READ_TIMEOUT_MS 500U

    /**
     * @brief 录音循环停止回调。
     *
     * 返回 true 表示用户已松手或上层请求停止。recorder 仍会保证至少录满
     * `min_duration_ms`，避免服务器收到过短音频。
     */
    typedef bool (*memory_watch_recorder_should_stop_cb_t)(void *user_ctx);

    /**
     * @brief 录音立即放弃回调。
     *
     * 返回 true 表示本次录音已被用户取消，应尽快释放麦克风并丢弃音频；
     * 与 `should_stop_cb` 不同，该回调不受最短录音时长保护。
     */
    typedef bool (*memory_watch_recorder_should_abort_cb_t)(void *user_ctx);

    /**
     * @brief AI Memory Watch 录音配置。
     */
    typedef struct
    {
        uint32_t ogg_serial;          /**< Ogg logical bitstream serial。 */
        uint32_t min_duration_ms;     /**< 最短录音时长，单位毫秒。 */
        uint32_t max_duration_ms;     /**< 最长录音时长，单位毫秒。 */
        uint32_t input_timeout_ms;    /**< 申请麦克风 session 超时，单位毫秒。 */
        uint32_t read_timeout_ms;     /**< 单次读取硬件 PCM 超时，单位毫秒。 */
        float record_gain_db;         /**< ES7210 录音增益；小于等于 0 时不主动设置。 */
        memory_watch_ogg_write_cb_t write_cb; /**< Ogg Opus 输出回调。 */
        void *write_user_ctx;         /**< 输出回调上下文。 */
        memory_watch_recorder_should_stop_cb_t should_stop_cb; /**< 停止回调。 */
        void *should_stop_user_ctx;   /**< 停止回调上下文。 */
        memory_watch_recorder_should_abort_cb_t should_abort_cb; /**< 立即取消回调。 */
        void *should_abort_user_ctx;  /**< 立即取消回调上下文。 */
    } memory_watch_recorder_config_t;

    /**
     * @brief AI Memory Watch 录音结果统计。
     */
    typedef struct
    {
        uint32_t duration_ms;         /**< 实际编码音频时长，单位毫秒。 */
        uint32_t opus_packets;        /**< 输出的 Opus audio packet 数。 */
        uint32_t opus_bytes;          /**< Opus packet 总字节数，不含 Ogg page header。 */
        uint32_t ogg_bytes;           /**< Ogg Opus 容器输出总字节数。 */
    } memory_watch_recorder_result_t;

    /**
     * @brief 录制一段 Ogg Opus 音频。
     *
     * 该函数会同步申请 `AUDIO_CODEC_OWNER_HERMES` input session，读取硬件 PCM，
     * 转为 16 kHz mono，编码为 Opus packet，并通过 muxer 输出 Ogg Opus 容器。
     * 调用方必须在普通 task 上下文中使用，不得在 LVGL timer/getter 中调用。
     *
     * @param[in] config 录音配置，`write_cb` 不能为空。
     * @param[out] out_result 可选输出统计。
     * @return `ESP_OK` 表示录制并输出成功。
     */
    esp_err_t memory_watch_recorder_capture_ogg_opus(
        const memory_watch_recorder_config_t *config,
        memory_watch_recorder_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_WATCH_RECORDER_H
