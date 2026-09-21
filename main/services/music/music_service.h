#ifndef MUSIC_SERVICE_H
#define MUSIC_SERVICE_H

#include <stdbool.h>

#include "esp_err.h"
#include "music_protocol.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 初始化在线音乐 owner task；不会自动联网或开始播放。 */
    esp_err_t music_service_init(void);

    /** @brief 从服务器来源与曲目创建新队列并立即播放。 */
    esp_err_t music_service_start(const char *source_id,
                                  const char *track_id);

    /** @brief 让服务端按来源选择首曲并立即播放。 */
    esp_err_t music_service_start_source(const char *source_id);

    /** @brief 异步读取来源第一页歌曲，不开始播放。 */
    esp_err_t music_service_load_source(const char *source_id);

    /** @brief 异步读取来源的下一页歌曲，不开始播放。 */
    esp_err_t music_service_load_source_page(const char *source_id,
                                             uint32_t offset);

    /** @brief 短按控制播放/暂停。 */
    esp_err_t music_service_toggle_playback(void);

    /** @brief 用户主动暂停；暂停后不自动恢复。 */
    esp_err_t music_service_pause(void);

    /** @brief 用户主动继续播放。 */
    esp_err_t music_service_resume(void);

    /** @brief 切换上一首。 */
    esp_err_t music_service_previous(void);

    /** @brief 切换下一首。 */
    esp_err_t music_service_next(void);

    /** @brief 设置服务器侧选曲模式。 */
    esp_err_t music_service_set_mode(music_service_mode_t mode);

    /**
     * @brief Hermes 页面进入时暂停音乐并释放扬声器输出。
     *
     * 页面离开后不自动恢复，必须由用户再次短按音乐控制。
     */
    esp_err_t music_service_pause_for_hermes_page(void);

    /** @brief 长按销毁音乐会话和本地播放 worker。 */
    esp_err_t music_service_destroy(void);

    /** @brief 复制音乐状态快照；不触发网络或状态推进。 */
    esp_err_t music_service_get_snapshot(music_service_snapshot_t *out_snapshot);

    /** @brief 复制最近一次来源分页快照；不触发网络请求。 */
    esp_err_t music_service_get_catalog(
        music_service_catalog_snapshot_t *out_catalog);

    /** @brief 在音乐页前台创建二维码登录会话。 */
    esp_err_t music_service_start_qr_login(void);

    /** @brief 离开二维码页时停止后续轮询。 */
    esp_err_t music_service_cancel_qr_login(void);

    /** @brief 复制账户登录状态；不触发网络请求。 */
    esp_err_t music_service_get_account(
        music_service_account_snapshot_t *out_account);

    /** @brief 复制当前二维码模块位图到调用方缓冲区。 */
    esp_err_t music_service_copy_qr(uint8_t *out_data, size_t capacity,
                                    uint16_t *out_size, size_t *out_bytes);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SERVICE_H */
