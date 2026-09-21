#ifndef MUSIC_CONTROLLER_H
#define MUSIC_CONTROLLER_H

#include "gui_guider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化音乐页面控制器。 */
void music_controller_init(lv_ui *ui);

/** @brief 打开独立音乐页面；不会自动播放或恢复。 */
void music_controller_open(void);

/** @brief 在 LVGL 线程刷新音乐只读快照。 */
void music_controller_poll_ui(void);

#ifdef AGENT_PREVIEW_HOST
/** @brief Host 预览专用：直接打开今日推荐滚动目录。 */
void music_controller_preview_open_catalog(void);

/** @brief Host 预览专用：打开账号二维码加载页。 */
void music_controller_preview_open_account(void);

#endif

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_CONTROLLER_H */
