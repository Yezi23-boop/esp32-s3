#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "printf_esp32.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include <inttypes.h> // 添加此头文件以支持PRI宏

extern char _iram_start;
extern char _iram_end;

/**
 * @brief 计算当前 target 的静态 IRAM 链接段总容量。
 *
 * ESP32-S3 的 `iram0_0_seg` 长度由 IDF 链接脚本按 cache 大小和 bootloader
 * 预留区裁剪；旧的 `SOC_IRAM0_*_SEG_LEN` 宏在 S3 上不存在，不能因此打印
 * `total: unknown`。这里复用链接脚本中的同一组边界常量，只用于诊断显示，
 * 不改变任何真实内存布局。
 */
static size_t printf_esp32_get_static_iram_total(void)
{
    size_t total = 0;
#if defined(SOC_IRAM0_0_SEG_LEN)
    total += SOC_IRAM0_0_SEG_LEN;
#endif
#if defined(SOC_IRAM0_2_SEG_LEN)
    total += SOC_IRAM0_2_SEG_LEN;
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) && \
    defined(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE)
    if (total == 0)
    {
        const size_t sram_iram_end = 0x403CB700U;
        const size_t icache_size = 0x8000U;
        const size_t i_d_sram_offset =
            (size_t)(SOC_DIRAM_IRAM_LOW - SOC_DIRAM_DRAM_LOW);
        const size_t sram_dram_end = sram_iram_end - i_d_sram_offset;
        const size_t i_d_sram_size =
            sram_dram_end - (size_t)SOC_DIRAM_DRAM_LOW;
        total = i_d_sram_size + icache_size -
                (size_t)CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE;
    }
#endif

    return total;
}

/**
 * @brief 打印ESP32系统内存统计信息
 * @details 显示内部RAM和PSRAM的详细使用情况，包括LVGL内存池状态
 * @note 当前LVGL配置使用自定义内存管理器，LVGL内存池统计为0是正常现象
 */
void printf_esp32_memory_stats(void)
{
    // 1. 统计外部 PSRAM（SPI RAM）使用情况
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM); // PSRAM 总容量（字节）
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);   // PSRAM 当前空闲容量（字节）
    size_t psram_used = psram_total - psram_free;                     // PSRAM 已用容量（字节）

    // 2. 统计内部 RAM（片上 SRAM）使用情况
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL); // 内部 RAM 总容量（字节）
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);   // 内部 RAM 当前空闲容量（字节）
    size_t internal_used = internal_total - internal_free;                 // 内部 RAM 已用容量（字节）

    // 2.1 统计 IRAM（可执行内存）使用情况（堆 + 链接器文本段）
    size_t iram_heap_total = heap_caps_get_total_size(MALLOC_CAP_EXEC); // IRAM 堆总容量（字节）
    size_t iram_heap_free = heap_caps_get_free_size(MALLOC_CAP_EXEC);   // IRAM 堆空闲容量（字节）
    size_t iram_heap_used = iram_heap_total - iram_heap_free;           // IRAM 堆已用容量（字节）

    size_t iram_text_used = (size_t)(&_iram_end - &_iram_start);
    size_t iram_text_total = printf_esp32_get_static_iram_total();

    // 3. 以 ESP-IDF 日志格式打印统计结果（等级：INFO，标签：TAG）
    ESP_LOGI(" ", "┌─────────────────────────────"); // 日志标题
    ESP_LOGI(" ", "│      系统资源统计             ");
    ESP_LOGI(" ", "├─────────────────────────────");
    // 打印 PSRAM：已用/总容量（KB） + 使用率（保留1位小数），避免 PSRAM 不存在时除零错误
    ESP_LOGI(" ",
             "│ PSRAM: %6zu KB / %6zu KB (%.1f%%) ",
             psram_used / 1024,
             psram_total / 1024,
             psram_total > 0 ? (psram_used * 100.0f / psram_total) : 0);
    ESP_LOGI(" ",
             "│ RAM:   %6zu KB / %6zu KB (%.1f%%) ",
             internal_used / 1024,
             internal_total / 1024,
             internal_used * 100.0f / internal_total);
    if (iram_heap_total > 0)
    {
        ESP_LOGI(" ",
                 "│ IRAM:  %6zu KB / %6zu KB (%.1f%%) ",
                 iram_heap_used / 1024,
                 iram_heap_total / 1024,
                 iram_heap_total > 0 ? (iram_heap_used * 100.0f / iram_heap_total) : 0);
    }
    else
    {
        if (iram_text_total > 0)
        {
            ESP_LOGI(" ",
                     "│ IRAM:  %6zu KB / %6zu KB (%.1f%%) ",
                     iram_text_used / 1024,
                     iram_text_total / 1024,
                     iram_text_used * 100.0f / iram_text_total);
        }
        else
        {
            ESP_LOGI(" ",
                     "│ IRAM:  %6zu KB (total: unknown) ",
                     iram_text_used / 1024);
        }
    }
    ESP_LOGI(" ", "├─────────────────────────────");
    ESP_LOGI(" ", "│      CPU任务统计               ");
    ESP_LOGI(" ", "└─────────────────────────────");

    // 4. 获取并打印CPU任务运行时统计信息（在未启用运行时统计时，打印提示）
#if (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    char stats_buffer[1024];
    vTaskGetRunTimeStats(stats_buffer);
    ESP_LOGI("CPU", "任务运行时统计:\n%s", stats_buffer);
#else
    ESP_LOGW("CPU", "未启用任务运行时统计。请在 sdkconfig 中开启 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS ");
#endif
    ESP_LOGI(" ", "═══════════════════════════════"); // 日志结尾
}

/**
 * @brief 打印任务栈使用统计信息
 * @param task_handle 任务句柄
 * @param stack_size_bytes 任务栈大小（字节）
 * @param task_name 任务名称（用于日志显示）
 * @details 监控指定任务的栈使用情况，包括总大小、剩余空间、已使用最大栈和使用率
 */
void printf_esp32_task_stack_stats(TaskHandle_t task_handle, uint32_t stack_size_bytes, const char *task_name)
{
    // 1. 参数有效性检查
    if (task_handle == NULL)
    {
        ESP_LOGW("STACK", "任务句柄为空，无法获取栈统计信息");
        return;
    }

    if (task_name == NULL)
    {
        task_name = "未知任务"; // 提供默认任务名称
    }

    // 2. 获取任务栈剩余空间（以字为单位，需要转换为字节）
    // uxTaskGetStackHighWaterMark() 返回任务栈的最小剩余空间（高水位标记）
    UBaseType_t stack_remaining_words = uxTaskGetStackHighWaterMark(task_handle);
    uint32_t stack_remaining_bytes = stack_remaining_words * sizeof(StackType_t);

    // 3. 计算栈使用情况
    uint32_t stack_used_bytes = stack_size_bytes - stack_remaining_bytes;
    float stack_usage_percent = (stack_used_bytes * 100.0f) / stack_size_bytes;

    // 4. 获取任务状态信息（可选，用于更详细的调试）
    eTaskState task_state = eTaskGetState(task_handle);
    const char *state_names[] = {"运行中", "就绪", "阻塞", "暂停", "删除", "无效"};
    const char *state_name = (task_state <= eInvalid) ? state_names[task_state] : "未知";

    // 5. 以统一格式打印栈统计信息
    ESP_LOGI("STACK", "┌─────────────────────────────────────");
    ESP_LOGI("STACK", "│  📋 任务栈统计: %s", task_name);
    ESP_LOGI("STACK", "├─────────────────────────────────────");
    ESP_LOGI("STACK", "│  栈总大小:   %6" PRIu32 " 字节", stack_size_bytes);
    ESP_LOGI("STACK", "│  已使用:     %6" PRIu32 " 字节 (%.1f%%)", stack_used_bytes, stack_usage_percent);
    ESP_LOGI("STACK", "│  剩余空间:   %6" PRIu32 " 字节", stack_remaining_bytes);
    ESP_LOGI("STACK", "│  高水位标记: %6" PRIu32 " 字 (%" PRIu32 " 字节)", (uint32_t)stack_remaining_words, stack_remaining_bytes);
    ESP_LOGI("STACK", "│  任务状态:   %s", state_name);
    ESP_LOGI("STACK", "└─────────────────────────────────────");

    // 6. 栈使用率警告检查
    if (stack_usage_percent > 90.0f)
    {
        ESP_LOGW("STACK", "⚠️  警告: 任务 '%s' 栈使用率过高 (%.1f%%)，可能存在栈溢出风险！", task_name, stack_usage_percent);
    }
    else if (stack_usage_percent > 75.0f)
    {
        ESP_LOGW("STACK", "⚡ 注意: 任务 '%s' 栈使用率较高 (%.1f%%)，建议监控", task_name, stack_usage_percent);
    }
}

void printf_esp32_all_task_stack_stats(void)
{
    /* 遍历所有 FreeRTOS 任务，打印栈剩余空间（高水位）。
     * TaskStatus_t 不含栈总大小，只输出 free(bytes)；
     * 各 owner 的栈总大小在 xTaskCreate 调用处已知，可对照计算使用率。
     * 用 pvPortMalloc 临时缓冲，采样完即释放，不驻留。 */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0)
    {
        ESP_LOGW("STACK", "no tasks to report");
        return;
    }

    TaskStatus_t *task_stats = (TaskStatus_t *)pvPortMalloc(task_count * sizeof(TaskStatus_t));
    if (task_stats == NULL)
    {
        ESP_LOGE("STACK", "alloc task stats buffer failed");
        return;
    }

    /* configRUN_TIME_COUNTER_TYPE 在 v5.5 默认为 uint32_t；传 NULL 表示不采集总运行时间。 */
    UBaseType_t got = uxTaskGetSystemState(task_stats, task_count, NULL);
    if (got == 0)
    {
        ESP_LOGW("STACK", "uxTaskGetSystemState returned 0");
        vPortFree(task_stats);
        return;
    }

    /* 同时打印 internal heap 快照，便于对照栈水位判断资源压力。 */
    ESP_LOGI("STACK", "=== all task stack high-water mark (free bytes) ===");
    ESP_LOGI("STACK", "internal_free=%u largest=%u psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("STACK", "%-16s %10s %6s", "name", "free(B)", "prio");

    /* 按剩余空间升序排序，剩余最少的（最危险的）排在最前。 */
    for (UBaseType_t i = 0; i < got; i++)
    {
        for (UBaseType_t j = i + 1; j < got; j++)
        {
            if (task_stats[j].usStackHighWaterMark < task_stats[i].usStackHighWaterMark)
            {
                TaskStatus_t tmp = task_stats[i];
                task_stats[i] = task_stats[j];
                task_stats[j] = tmp;
            }
        }
    }

    for (UBaseType_t i = 0; i < got; i++)
    {
        /* usStackHighWaterMark 单位是 StackType_t words，× sizeof(StackType_t) 得字节。 */
        ESP_LOGI("STACK", "%-16s %10u %6u",
                 task_stats[i].pcTaskName,
                 (unsigned)(task_stats[i].usStackHighWaterMark * sizeof(StackType_t)),
                 (unsigned)task_stats[i].uxCurrentPriority);
    }
    ESP_LOGI("STACK", "=== end task stack stats ===");

    vPortFree(task_stats);
}
