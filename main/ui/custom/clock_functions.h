#ifndef __CLOCK_FUNCTIONS_H_
#define __CLOCK_FUNCTIONS_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include "lvgl.h"

    /**
     * 时钟功能模块接口
     * -----------------------------------------------------------------------------
     * 设计原则：低耦合接口设计
     * - 提供清晰的功能接口
     * - 隐藏内部实现细节
     * - 支持模块化扩展
     */

    /**
     * 更新数字时钟的时间显示
     *
     * @param hour 小时 (0-23)
     * @param minute 分钟 (0-59)
     * @param second 秒钟 (0-59)
     *
     * 功能特性：
     * - 参数有效性验证
     * - 线程安全设计
     * - 错误保护机制
     */
    void update_digital_clock(int hour, int minute, int second);

    /**
     * 获取当前系统日期
     *
     * @param date 存储日期的结构体指针
     */
    void get_current_date(lv_calendar_date_t *date);

    /**
     * 更新日期显示
     *
     * 功能特性：
     * - 自动获取系统日期
     * - 更新日期标签显示
     * - 错误保护机制
     */
    void update_date_display(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_FUNCTIONS_H_ */