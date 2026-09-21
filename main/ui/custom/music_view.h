#ifndef MUSIC_VIEW_H
#define MUSIC_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "services/music/music_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct music_view music_view_t;

typedef void (*music_view_action_cb_t)(void *user_data);
typedef void (*music_view_source_cb_t)(uint8_t source_index, void *user_data);
typedef void (*music_view_track_cb_t)(const char *source_id,
                                      const char *track_id,
                                      void *user_data);
typedef void (*music_view_catalog_load_more_cb_t)(const char *source_id,
                                                  uint32_t offset,
                                                  void *user_data);
typedef void (*music_view_mode_cb_t)(music_service_mode_t mode, void *user_data);

typedef struct
{
    music_view_action_cb_t back_cb;
    music_view_source_cb_t source_cb;
    music_view_track_cb_t track_cb;
    music_view_action_cb_t toggle_cb;
    music_view_action_cb_t previous_cb;
    music_view_action_cb_t next_cb;
    music_view_mode_cb_t mode_cb;
    music_view_action_cb_t catalog_back_cb;
    music_view_catalog_load_more_cb_t catalog_load_more_cb;
    music_view_action_cb_t account_cb;
    void *user_data;
} music_view_config_t;

/** @brief 创建独立音乐页面；页面只负责显示和投递用户意图。 */
music_view_t *music_view_create(const music_view_config_t *config);

/** @brief 销毁音乐页面。 */
void music_view_destroy(music_view_t *view);

/** @brief 获取音乐页面 screen。 */
lv_obj_t *music_view_get_screen(const music_view_t *view);

/** @brief 将 music service 的只读快照渲染到页面。 */
void music_view_apply_snapshot(music_view_t *view,
                               const music_service_snapshot_t *snapshot);

/** @brief 显示来源加载状态或分页歌曲列表。 */
void music_view_apply_catalog(music_view_t *view,
                              const music_service_catalog_snapshot_t *catalog);

/** @brief 立即切换到来源列表加载态。 */
void music_view_show_catalog_loading(music_view_t *view, const char *source_id);

/** @brief 返回四类来源入口。 */
void music_view_show_sources(music_view_t *view);

/** @brief 显示二维码登录页加载状态。 */
void music_view_show_account_loading(music_view_t *view);

/** @brief 将账户状态与二维码位图渲染到登录页。 */
void music_view_apply_account(
    music_view_t *view, const music_service_account_snapshot_t *account,
    const uint8_t *qr_data, uint16_t qr_size, size_t qr_bytes);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_VIEW_H */
