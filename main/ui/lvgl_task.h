#ifndef LVGL_TASK_H
#define LVGL_TASK_H

#include "gui_guider.h" // guider_ui的定义在这里

/*
 * 文件作用：
 * 1. 对外暴露 LVGL 前台主任务入口；
 * 2. 暴露 GUI Guider 生成的全局 UI 对象，供控制器和事件绑定模块共享；
 * 3. 该头文件只声明入口，不负责任何具体刷新或事件实现。
 *
 * LVGL UI 主任务入口：
 * - 负责初始化显示驱动、界面对象和 UI 控制器；
 * - 持续驱动 `lv_timer_handler()`；
 * - 协调刷新策略、告警展示和录音按钮等前台交互逻辑。
 */

/* GUI Guider 生成的全局 UI 树根对象，生命周期与 LVGL 主任务一致。 */
extern lv_ui guider_ui;

/*
 * FreeRTOS 任务入口，由系统启动阶段创建。
 * 调用方无需直接操作内部循环，任务会在自身上下文里完成 UI 初始化和周期调度。
 */
void lvgl_task(void *pvParameter);

#endif // LVGL_TASK_H
