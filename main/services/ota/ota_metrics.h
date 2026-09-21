#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** OTA 升级阶段，用于耗时统计与结果记录。 */
typedef enum
{
    OTA_METRICS_STAGE_MANIFEST = 0, /* manifest 拉取与解析 */
    OTA_METRICS_STAGE_DOWNLOAD,     /* 镜像/patch 下载与 staging 写入 */
    OTA_METRICS_STAGE_ACTIVATE,     /* 激活与重启前 */
    OTA_METRICS_STAGE_COUNT,
} ota_metrics_stage_t;

/**
 * @brief 初始化 ota_metrics（惰性打开 NVS）。
 *
 * 内部持有 NVS handle；首次持久化失败仅打日志，不阻塞 OTA 主流程。
 */
esp_err_t ota_metrics_init(void);

/**
 * @brief 记录阶段开始时间（用于耗时统计）。
 */
void ota_metrics_stage_begin(ota_metrics_stage_t stage);

/**
 * @brief 返回阶段自 begin 起的耗时（ms），未 begin 时为 0。
 */
uint32_t ota_metrics_stage_elapsed_ms(ota_metrics_stage_t stage);

/**
 * @brief 记录一次阶段结果并持久化到 NVS 环形日志。
 *
 * @param stage         结束的阶段
 * @param target_version 目标版本（manifest.version；失败时可为空串）
 * @param result        该阶段最终错误码（ESP_OK 表示成功）
 * @param duration_ms   该阶段耗时（ms）
 * @param is_delta      是否走 delta 差分路径
 * @param retry_count   delta 失败重试次数（全量路径为 0）
 */
void ota_metrics_record_result(ota_metrics_stage_t stage,
                               const char *target_version, esp_err_t result,
                               uint32_t duration_ms, bool is_delta,
                               uint8_t retry_count);

/**
 * @brief 打印 NVS 中最近 CONFIG_OTA_METRICS_LOG_COUNT 条升级结果。
 */
esp_err_t ota_metrics_dump_recent(void);

#ifdef __cplusplus
}
#endif
