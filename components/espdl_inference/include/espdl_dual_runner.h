/**
 * @file espdl_dual_runner.h
 * @brief 双模型并行推理运行器（C API）。
 *
 * 同时管理 V3.2 DS-TCN-small 和 V3.3 DS-CNN-tiny 两个 ESP-DL 模型，
 * 共享同一份 Fbank 特征，分别推理后按可配置策略融合结果。
 *
 * 资源估算：
 *   - DS-TCN: ~261KB PSRAM, ~57KB flash
 *   - DS-CNN: ~137KB PSRAM, ~22KB flash
 *   - 总计: ~398KB PSRAM, ~79KB flash（在 8MB PSRAM 的 ESP32-S3 上完全可行）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdl_feature_pipeline.h"
#include "espdl_model_runner.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 双模型融合策略。 */
typedef enum {
    /** 任一模型判定 danger 即触发，召回优先。 */
    ESPDL_FUSION_ANY_DANGER = 0,
    /** 两模型均判定 danger 才触发，精度优先。 */
    ESPDL_FUSION_BOTH_DANGER = 1,
    /** 取两模型 danger 概率最大值做阈值判定。 */
    ESPDL_FUSION_MAX_PROB = 2,
} espdl_fusion_strategy_t;

/** 双模型并行推理结果。 */
typedef struct {
    espdl_model_result_t dstcn_result;  /**< V3.2 DS-TCN-small 推理结果。 */
    espdl_model_result_t dscnn_result;  /**< V3.3 DS-CNN-tiny 推理结果。 */
    int fused_label_index;              /**< 融合后最终类别索引。 */
    float fused_confidence;             /**< 融合后置信度。 */
    float fused_danger_prob;            /**< 融合后 danger 概率。 */
    bool dstcn_ok;                      /**< DS-TCN 推理是否成功。 */
    bool dscnn_ok;                      /**< DS-CNN 推理是否成功。 */
} espdl_dual_result_t;

/** 不透明双模型运行器句柄。 */
typedef struct espdl_dual_runner_t espdl_dual_runner_t;

/**
 * @brief 创建双模型运行器并加载两个模型。
 *
 * @param[out] out_runner 输出运行器句柄。
 * @param[in] dstcn_data DS-TCN-small .espdl rodata 起始地址。
 * @param[in] dscnn_data DS-CNN-tiny .espdl rodata 起始地址。
 * @param[in] strategy 融合策略。
 * @return ESP_OK 表示两个模型均加载成功。
 */
esp_err_t espdl_dual_runner_create(espdl_dual_runner_t **out_runner,
                                   const uint8_t *dstcn_data,
                                   const uint8_t *dscnn_data,
                                   espdl_fusion_strategy_t strategy);

/**
 * @brief 销毁双模型运行器。
 *
 * @param[in] runner 运行器句柄，可为 NULL。
 */
void espdl_dual_runner_destroy(espdl_dual_runner_t *runner);

/**
 * @brief 对共享 Fbank 特征执行双模型并行推理并融合结果。
 *
 * 两个模型串行执行（ESP-DL 单核模式非线程安全），共享同一份特征。
 *
 * @param[in] runner 运行器句柄。
 * @param[in] feature 固定形状 [98, 40] Fbank 特征。
 * @param[out] result 双模型推理及融合结果。
 * @return ESP_OK 表示至少一个模型推理成功。
 */
esp_err_t espdl_dual_runner_run(espdl_dual_runner_t *runner,
                                const espdl_feature_frame_t *feature,
                                espdl_dual_result_t *result);

/**
 * @brief 对两个模型分别执行官方 test vector 自检。
 *
 * @param[in] runner 运行器句柄。
 * @return ESP_OK 表示两个模型均通过。
 */
esp_err_t espdl_dual_runner_self_test(espdl_dual_runner_t *runner);

/**
 * @brief 获取单模型运行器句柄（用于外部单独调用）。
 *
 * @param[in] runner 双模型运行器。
 * @return DS-TCN 单模型运行器，NULL 表示未初始化。
 */
espdl_model_runner_t *espdl_dual_runner_get_dstcn(espdl_dual_runner_t *runner);

/**
 * @brief 获取单模型运行器句柄（用于外部单独调用）。
 *
 * @param[in] runner 双模型运行器。
 * @return DS-CNN 单模型运行器，NULL 表示未初始化。
 */
espdl_model_runner_t *espdl_dual_runner_get_dscnn(espdl_dual_runner_t *runner);

#ifdef __cplusplus
}
#endif
