#ifndef _PRINTF_ESP32_H_
#define _PRINTF_ESP32_H_
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 统计接口
/**
 * @brief 打印ESP32系统内存统计信息
 * @details 显示内部RAM和PSRAM的详细使用情况，包括LVGL内存池状态
 */
void printf_esp32_memory_stats(void);
/**
 * @brief 打印指定任务的栈使用情况
 * @param task_handle 任务句柄
 * @param stack_size_bytes 任务栈大小（字节）
 * @param task_name 任务名称
 * @details 监控指定任务的栈使用情况，包括总大小、剩余空间、已使用最大栈和使用率
 */
void printf_esp32_task_stack_stats(TaskHandle_t task_handle, uint32_t stack_size_bytes, const char *task_name);

/**
 * @brief 遍历并打印当前所有 FreeRTOS 任务的栈高水位。
 * @details 依赖 sdkconfig 的 CONFIG_FREERTOS_USE_TRACE_FACILITY。
 *          TaskStatus_t 不含栈总大小，因此只输出剩余空间（bytes），
 *          配合各 owner 在 xTaskCreate 时已知传入的栈大小即可计算使用率。
 *          用于资源实测阶段一次性采样，不作为长期运行能力。
 */
void printf_esp32_all_task_stack_stats(void);

#endif