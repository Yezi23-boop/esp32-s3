/**
 * @file espdl_model_runner.h
 * @brief 单个 ESP-DL 模型推理封装（C API）。
 *
 * 支持从 rodata 加载 .espdl 模型，对固定窗 Fbank 特征执行
 * INT8 量化推理，输出 softmax 概率和阈值后处理结果。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espdl_feature_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** danger_binary 类别数：0=non_danger, 1=danger。 */
#define ESPDL_CLASS_COUNT 2

/** DS-TCN-small (V3.2) danger 概率阈值。 */
#define ESPDL_DSTCN_DANGER_THRESHOLD  0.35f
/** DS-CNN V59 softdistill danger 概率阈值；沿用样板验证的 T90 配置。 */
#define ESPDL_DSCNN_DANGER_THRESHOLD  0.90f

/** 单模型推理结果。 */
typedef struct {
    int label_index;                        /**< 阈值后处理后的类别索引。 */
    float confidence;                       /**< 判定类别对应的 softmax 概率。 */
    float logits[ESPDL_CLASS_COUNT];        /**< 反量化后的原始 logits。 */
    float probabilities[ESPDL_CLASS_COUNT]; /**< softmax 概率。 */
    uint64_t window_end_sample_index;       /**< 本次 16kHz 推理窗口末尾样本索引（end-exclusive）。 */
} espdl_model_result_t;

/** 不透明模型运行器句柄。 */
typedef struct espdl_model_runner_t espdl_model_runner_t;

/**
 * @brief 创建模型运行器并从 rodata 加载 .espdl 模型。
 *
 * @param[out] out_runner 输出运行器句柄。
 * @param[in] model_data rodata 中嵌入的 .espdl 二进制起始地址。
 * @param[in] model_name 模型名称，仅用于日志。
 * @return ESP_OK 表示加载成功。
 */
esp_err_t espdl_model_runner_create(espdl_model_runner_t **out_runner,
                                    const uint8_t *model_data,
                                    const char *model_name);

/**
 * @brief 销毁模型运行器并释放资源。
 *
 * @param[in] runner 运行器句柄，可为 NULL。
 */
void espdl_model_runner_destroy(espdl_model_runner_t *runner);

/**
 * @brief 对固定窗 Fbank 特征执行一次推理。
 *
 * @param[in] runner 运行器句柄。
 * @param[in] feature 固定形状 [98, 40] Fbank 特征。
 * @param[out] result 推理结果。
 * @return ESP_OK 表示推理完成。
 */
esp_err_t espdl_model_runner_run(espdl_model_runner_t *runner,
                                 const espdl_feature_frame_t *feature,
                                 espdl_model_result_t *result);

/**
 * @brief 设置单窗 danger 概率阈值。
 *
 * @param[in] runner 运行器句柄。
 * @param[in] threshold danger 概率阈值，合法范围为 0.0f 到 1.0f。
 * @return ESP_OK 表示设置成功。
 */
esp_err_t espdl_model_runner_set_threshold(espdl_model_runner_t *runner,
                                           float threshold);

/**
 * @brief 执行 ESP-DL 官方 test vector 自检。
 *
 * @param[in] runner 运行器句柄。
 * @return ESP_OK 表示通过。
 */
esp_err_t espdl_model_runner_self_test(espdl_model_runner_t *runner);

/**
 * @brief 返回 danger_binary 类别名称。
 *
 * @param[in] index 类别索引。
 * @return 类别名称；越界返回 "unknown"。
 */
const char *espdl_model_runner_label_name(int index);

#ifdef __cplusplus
}
#endif
