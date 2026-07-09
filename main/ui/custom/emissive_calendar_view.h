#ifndef EMISSIVE_CALENDAR_VIEW_H_
#define EMISSIVE_CALENDAR_VIEW_H_

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 显示“流光悬浮静界”日历弹层。
 *
 * @param anchor 触发弹层的日期控件，用于定位所属 screen。
 * @param date_text 当前日期文本，格式为 `YYYY/MM/DD`。
 * @param date_label 选中日期后需要回写的日期 label，可为空。
 * @return lv_obj_t* 弹层根对象；创建失败时返回 NULL。
 *
 * 该弹层只在 LVGL UI 线程内创建和销毁，不跨任务共享状态，因此不需要
 * FreeRTOS 锁；调用方仍需保证它运行在 LVGL owner 线程。
 */
lv_obj_t *emissive_calendar_view_show(lv_obj_t *anchor,
                                      const char *date_text,
                                      lv_obj_t *date_label);

#ifdef __cplusplus
}
#endif

#endif  // EMISSIVE_CALENDAR_VIEW_H_
