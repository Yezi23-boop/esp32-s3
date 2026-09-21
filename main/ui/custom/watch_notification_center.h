#ifndef WATCH_NOTIFICATION_CENTER_H
#define WATCH_NOTIFICATION_CENTER_H

/**
 * @file watch_notification_center.h
 * @brief 全局气泡通知中心（Stage 4）。
 *
 * 单例 controller 挂在 lv_layer_top()，消费 memory_watch_service inbox
 * snapshot 的 generation 变化；支持 Hermes inbox 气泡和 conversation reply
 * 气泡两条独立通道。
 *
 * 约束：
 * - 所有公开 API 只能在 LVGL task 调用。
 * - controller 不执行 HTTP、不持有 service 可写指针。
 * - 气泡遵守 CO5300 安全区：x=40，w=330，y=20，h=64，圆角 32px（灵动岛胶囊）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 气泡点击后跳转目标。 */
typedef enum
{
    WATCH_NC_NAV_INBOX_DETAIL = 0, /**< 跳转到指定 notification_id 的详情。 */
    WATCH_NC_NAV_INBOX_LIST,       /**< 跳转到收件箱列表（多条合并时）。 */
    WATCH_NC_NAV_HERMES_VOICE,     /**< 跳转到 Hermes 语音/对话页。 */
} watch_nc_nav_target_t;

/**
 * @brief 气泡点击回调。
 *
 * controller 在用户点击气泡时调用；调用方根据 target 执行页面跳转。
 * notification_id 仅在 target == INBOX_DETAIL 时有效。
 */
typedef void (*watch_nc_click_cb_t)(watch_nc_nav_target_t target,
                                    const char *notification_id,
                                    void *user_data);

/**
 * @brief 气泡右滑回调（只清除气泡，不标已读，不影响未读数）。
 */
typedef void (*watch_nc_dismiss_cb_t)(void *user_data);

/** @brief notification center 初始化配置。 */
typedef struct
{
    watch_nc_click_cb_t   click_cb;
    watch_nc_dismiss_cb_t dismiss_cb;
    void                 *user_data;
} watch_nc_config_t;

/**
 * @brief 初始化全局 notification center（仅调用一次，LVGL task 内）。
 *
 * 创建 top-layer 气泡对象；不显示任何气泡。
 * @param[in] config 回调配置，不能为 NULL。
 */
void watch_nc_init(const watch_nc_config_t *config);

/**
 * @brief 定期调用（与 LVGL timer 频率相同），检查 inbox generation 变化。
 *
 * 内部会读取 memory_watch_service inbox meta（极低成本），
 * 如果有新的未 surfaced 消息且未处于暂缓状态，则触发气泡显示。
 *
 * @param[in] is_recording      是否正在录音/编码（暂缓条件）。
 * @param[in] safety_alert_active 是否有更高优先级安全 overlay（暂缓条件）。
 */
void watch_nc_poll(bool is_recording, bool safety_alert_active);

/**
 * @brief 通知 Hermes 对话回复已到达（后台 Hermes 返回时调用）。
 *
 * 若当前没有 inbox 气泡，立即显示"Hermes 回复已到达"气泡；
 * 若已有 inbox 气泡，将 reply 记为待展示槽位。
 *
 * @param[in] request_id 已完成的 Hermes 请求 ID（用于调试/关联）。
 */
void watch_nc_notify_hermes_reply(const char *request_id);

/**
 * @brief 通知收件箱列表页当前处于前台（避免重复弹气泡）。
 * @param[in] active true = 列表在前台，false = 离开列表。
 */
void watch_nc_set_inbox_list_active(bool active);

/**
 * @brief 查询全局通知气泡当前是否可见。
 *
 * 该只读接口供前台交互页面决定是否自动暂停，不推进通知状态机。
 * 必须在 LVGL task 内调用。
 *
 * @return true 表示气泡正在遮挡当前页面。
 */
bool watch_nc_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_NOTIFICATION_CENTER_H */
