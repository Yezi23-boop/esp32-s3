/*
 * 时钟功能模块 - 高内聚设计
 * 负责所有与时间显示相关的功能
 * -----------------------------------------------------------------------------
 * 设计原则：高内聚、低耦合
 * - 只包含时钟相关功能
 * - 对外提供清晰的接口
 * - 内部实现细节封装
 */

#include "clock_functions.h"
#include "lvgl.h"
#include "gui_guider.h"
#include <stdio.h>
#include <time.h>
// // 外部全局变量声明（在generated文件中定义）
// extern int screen_digital_clock_1_hour_value;
// extern int screen_digital_clock_1_min_value;
// extern int screen_digital_clock_1_sec_value;
/**
 * 更新数字时钟的时间显示
 *
 * @param hour 小时 (0-23)
 * @param minute 分钟 (0-59)
 * @param second 秒钟 (0-59)
 *
 * 功能说明：
 * - 验证时间参数的有效性
 * - 更新全局时间变量
 * - 刷新时钟显示控件
 * - 提供错误保护机制
 */
void update_digital_clock(int hour, int minute, int second)
{
    // 验证输入参数范围（参数校验层）
    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59)
    {
        return; // 参数超出范围，不执行更新（错误保护）
    }

    // // 更新全局时间变量（数据层）
    // screen_digital_clock_1_hour_value = hour;
    // screen_digital_clock_1_min_value = minute;
    // screen_digital_clock_1_sec_value = second;

    // // 更新时钟显示（UI层）
    // if (lv_obj_is_valid(guider_ui.screen_digital_clock_1))
    // {
    //     lv_dclock_set_text_fmt(guider_ui.screen_digital_clock_1, "%02d:%02d:%02d",
    //                            hour, minute, second);
    // }
}

/**
 * 获取当前系统日期
 *
 * @param date 存储日期的结构体指针
 */
void get_current_date(lv_calendar_date_t *date)
{
    time_t now;
    struct tm *timeinfo;
    time(&now);
    timeinfo = localtime(&now);

    date->year = timeinfo->tm_year + 1900; // tm_year是从1900年开始计算的
    date->month = timeinfo->tm_mon + 1;    // tm_mon范围是0-11
    date->day = timeinfo->tm_mday;
}

/**
 * 更新日期显示
 *
 * 功能说明：
 * - 获取当前系统日期
 * - 更新日期标签显示
 * - 提供错误保护机制
 */
void update_date_display(void)
{
    lv_calendar_date_t current_date;
    get_current_date(&current_date);

    // 格式化日期字符串（数据处理层）
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%d/%02d/%02d",
                current_date.year, current_date.month, current_date.day);

    // // 检查控件是否存在并更新日期标签（UI层）
    // if (lv_obj_is_valid(guider_ui.screen_time_datetext_1))
    // {
    //     lv_label_set_text(guider_ui.screen_time_datetext_1, buf);
    // }
}