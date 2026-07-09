/**
 * @file espdl_model_runner.cpp
 * @brief 单个 ESP-DL 模型推理封装实现。
 *
 * 从 rodata 加载 .espdl 模型，对固定窗 Fbank 特征执行 INT8 量化推理，
 * 输出 softmax 概率和阈值后处理结果。
 */

#include "espdl_model_runner.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "espdl_runner";

namespace {

/** 输入元素总数：98 帧 × 40 bins = 3920。 */
constexpr size_t kInputElementCount = ESPDL_FEATURE_FRAME_COUNT * ESPDL_FEATURE_BIN_COUNT;

/** 模型输入/输出形状常量。 */
static const std::vector<int> kExpectedInputShapeNhwc = {
    1, ESPDL_FEATURE_FRAME_COUNT, ESPDL_FEATURE_BIN_COUNT, 1};
static const std::vector<int> kExpectedInputShapeNchw = {
    1, 1, ESPDL_FEATURE_FRAME_COUNT, ESPDL_FEATURE_BIN_COUNT};
static const std::vector<int> kExpectedOutputShape = {1, ESPDL_CLASS_COUNT};

static const char *const kLabels[ESPDL_CLASS_COUNT] = {
    "non_danger",
    "danger",
};

/**
 * @brief 判断模型输入 shape 是否属于当前支持的固定窗布局。
 */
bool is_supported_input_shape(const std::vector<int> &shape)
{
    return shape == kExpectedInputShapeNhwc || shape == kExpectedInputShapeNchw;
}

/**
 * @brief 根据模型声明的输入布局返回运行时应使用的 Tensor shape。
 */
std::vector<int> select_runtime_input_shape(const std::vector<int> &model_input_shape)
{
    if (model_input_shape == kExpectedInputShapeNhwc) {
        return kExpectedInputShapeNhwc;
    }
    if (model_input_shape == kExpectedInputShapeNchw) {
        return kExpectedInputShapeNchw;
    }
    return {};
}

/**
 * @brief 按模型输入 exponent 把 float Fbank 量化为 INT8。
 */
void quantize_feature_to_int8(const float *values, int input_exponent,
                              int8_t *quantized)
{
    const float inv_scale = DL_RESCALE(input_exponent);
    for (size_t i = 0; i < kInputElementCount; ++i) {
        quantized[i] = dl::quantize<int8_t>(values[i], inv_scale);
    }
}

/**
 * @brief 从 ESP-DL 输出 Tensor 读取 logits 并反量化。
 */
esp_err_t dequantize_output_logits(dl::TensorBase *output_tensor, float *logits)
{
    if (output_tensor == nullptr || logits == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (output_tensor->get_shape() != kExpectedOutputShape ||
        output_tensor->get_size() != ESPDL_CLASS_COUNT) {
        ESP_LOGE(TAG, "模型输出形状不匹配: shape=%s, size=%d",
                 dl::vector_to_string(output_tensor->get_shape()).c_str(),
                 output_tensor->get_size());
        return ESP_ERR_INVALID_SIZE;
    }

    const int output_exponent = output_tensor->get_exponent();
    const float output_scale = DL_SCALE(output_exponent);

    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
        const int8_t *raw = static_cast<const int8_t *>(output_tensor->data);
        for (int i = 0; i < ESPDL_CLASS_COUNT; ++i) {
            logits[i] = dl::dequantize<int8_t, float>(raw[i], output_scale);
        }
        return ESP_OK;
    }
    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT16) {
        const int16_t *raw = static_cast<const int16_t *>(output_tensor->data);
        for (int i = 0; i < ESPDL_CLASS_COUNT; ++i) {
            logits[i] = dl::dequantize<int16_t, float>(raw[i], output_scale);
        }
        return ESP_OK;
    }
    if (output_tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        const float *raw = static_cast<const float *>(output_tensor->data);
        for (int i = 0; i < ESPDL_CLASS_COUNT; ++i) {
            logits[i] = raw[i];
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "不支持的输出 dtype: %s", output_tensor->get_dtype_string());
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 将 logits 转成 softmax 概率，按阈值给出最终决策。
 */
void fill_binary_threshold_result(const float *logits, float threshold,
                                  espdl_model_result_t *result)
{
    float max_logit = logits[0];
    for (int i = 1; i < ESPDL_CLASS_COUNT; ++i) {
        max_logit = std::max(max_logit, logits[i]);
    }

    float sum = 0.0f;
    for (int i = 0; i < ESPDL_CLASS_COUNT; ++i) {
        result->logits[i] = logits[i];
        result->probabilities[i] = std::exp(logits[i] - max_logit);
        sum += result->probabilities[i];
    }
    for (int i = 0; i < ESPDL_CLASS_COUNT; ++i) {
        result->probabilities[i] = sum > 0.0f ? result->probabilities[i] / sum : 0.0f;
    }

    const float danger_prob = result->probabilities[1];
    result->label_index = danger_prob >= threshold ? 1 : 0;
    result->confidence = result->probabilities[result->label_index];
}

/**
 * @brief 打印 ESP-DL 模型内存占用。
 */
void log_model_memory_info(dl::Model *model, const char *model_name)
{
    if (model == nullptr) {
        return;
    }

    const auto memory_info = model->get_memory_info();
    ESP_LOGI(TAG, "[%s] ESP-DL memory:", model_name);

    const char *const rows[] = {
        "fbs_model", "variable", "parameter",
        "parameter_copy", "others", "total",
    };
    for (const char *row : rows) {
        const auto item = memory_info.find(row);
        if (item == memory_info.end()) {
            continue;
        }
        const dl::mem_info_t &info = item->second;
        ESP_LOGI(TAG, "  %-14s internal=%lu B, psram=%lu B, flash=%lu B",
                 row,
                 static_cast<unsigned long>(info.internal),
                 static_cast<unsigned long>(info.psram),
                 static_cast<unsigned long>(info.flash));
    }
}

/**
 * @brief 返回当前模型架构对应的默认 danger 阈值。
 */
float default_threshold_for(const char *model_name)
{
    if (model_name != nullptr && std::strstr(model_name, "dscnn") != nullptr) {
        return ESPDL_DSCNN_DANGER_THRESHOLD;
    }
    return ESPDL_DSTCN_DANGER_THRESHOLD;
}

}  // namespace

/** 运行器内部结构体。 */
struct espdl_model_runner_t {
    dl::Model *model;
    std::vector<int8_t> input_quantized;
    const char *name;
    std::atomic<float> threshold;
};

extern "C" {

esp_err_t espdl_model_runner_create(espdl_model_runner_t **out_runner,
                                    const uint8_t *model_data,
                                    const char *model_name)
{
    if (out_runner == nullptr || model_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto *runner = new (std::nothrow) espdl_model_runner_t();
    if (runner == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    runner->model = nullptr;
    runner->name = model_name != nullptr ? model_name : "unnamed";
    runner->threshold.store(default_threshold_for(runner->name));

    runner->model = new dl::Model((const char *)model_data,
                                  fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (runner->model == nullptr) {
        delete runner;
        return ESP_ERR_NO_MEM;
    }

    if (runner->model->get_inputs().empty() || runner->model->get_outputs().empty()) {
        ESP_LOGE(TAG, "[%s] 模型输入或输出为空", runner->name);
        delete runner->model;
        delete runner;
        return ESP_ERR_INVALID_RESPONSE;
    }

    const auto input_shape = runner->model->get_inputs().begin()->second->get_shape();
    const auto output_shape = runner->model->get_outputs().begin()->second->get_shape();
    if (!is_supported_input_shape(input_shape) || output_shape != kExpectedOutputShape) {
        ESP_LOGE(TAG, "[%s] 形状不匹配: input=%s, output=%s",
                 runner->name,
                 dl::vector_to_string(input_shape).c_str(),
                 dl::vector_to_string(output_shape).c_str());
        delete runner->model;
        delete runner;
        return ESP_ERR_INVALID_SIZE;
    }

    runner->input_quantized.resize(kInputElementCount);
    ESP_LOGI(TAG,
             "[%s] 已加载: input=%s exp=%d, output=%s exp=%d, threshold=%.2f",
             runner->name,
             runner->model->get_inputs().begin()->second->get_dtype_string(),
             runner->model->get_inputs().begin()->second->get_exponent(),
             runner->model->get_outputs().begin()->second->get_dtype_string(),
             runner->model->get_outputs().begin()->second->get_exponent(),
             runner->threshold.load());
    log_model_memory_info(runner->model, runner->name);

    *out_runner = runner;
    return ESP_OK;
}

void espdl_model_runner_destroy(espdl_model_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    delete runner->model;
    runner->model = nullptr;
    delete runner;
}

esp_err_t espdl_model_runner_run(espdl_model_runner_t *runner,
                                 const espdl_feature_frame_t *feature,
                                 espdl_model_result_t *result)
{
    if (runner == nullptr || runner->model == nullptr ||
        feature == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (feature->frames != ESPDL_FEATURE_FRAME_COUNT ||
        feature->bins != ESPDL_FEATURE_BIN_COUNT ||
        feature->values == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const auto input_iter = runner->model->get_inputs().begin();
    const std::string input_name = input_iter->first;
    dl::TensorBase *model_input = input_iter->second;
    const std::vector<int> runtime_shape = select_runtime_input_shape(
        model_input->get_shape());
    if (runtime_shape.empty()) {
        ESP_LOGE(TAG, "[%s] 不支持的输入 shape: %s",
                 runner->name,
                 dl::vector_to_string(model_input->get_shape()).c_str());
        return ESP_ERR_INVALID_SIZE;
    }

    std::unique_ptr<dl::TensorBase> input_tensor;
    if (model_input->get_dtype() == dl::DATA_TYPE_INT8) {
        quantize_feature_to_int8(feature->values, model_input->get_exponent(),
                                 runner->input_quantized.data());
        input_tensor = std::make_unique<dl::TensorBase>(
            runtime_shape, runner->input_quantized.data(),
            model_input->get_exponent(), dl::DATA_TYPE_INT8, false);
    } else if (model_input->get_dtype() == dl::DATA_TYPE_FLOAT) {
        input_tensor = std::make_unique<dl::TensorBase>(
            runtime_shape, const_cast<float *>(feature->values),
            0, dl::DATA_TYPE_FLOAT, false);
    } else {
        ESP_LOGE(TAG, "[%s] 不支持的输入 dtype: %s",
                 runner->name, model_input->get_dtype_string());
        return ESP_ERR_NOT_SUPPORTED;
    }

    std::map<std::string, dl::TensorBase *> input_map = {
        {input_name, input_tensor.get()}};
    runner->model->run(input_map, dl::RUNTIME_MODE_SINGLE_CORE);

    float logits[ESPDL_CLASS_COUNT] = {0.0f};
    const esp_err_t ret = dequantize_output_logits(
        runner->model->get_outputs().begin()->second, logits);
    if (ret != ESP_OK) {
        return ret;
    }

    fill_binary_threshold_result(logits, runner->threshold.load(), result);

    ESP_LOGD(TAG,
             "[%s] decision=%s(%d), conf=%.4f, danger=%.4f",
             runner->name,
             espdl_model_runner_label_name(result->label_index),
             result->label_index,
             result->confidence,
             result->probabilities[1]);
    return ESP_OK;
}

esp_err_t espdl_model_runner_set_threshold(espdl_model_runner_t *runner,
                                           float threshold)
{
    if (runner == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (threshold < 0.0f || threshold > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    runner->threshold.store(threshold);
    ESP_LOGI(TAG, "[%s] danger threshold set to %.2f", runner->name, threshold);
    return ESP_OK;
}

esp_err_t espdl_model_runner_self_test(espdl_model_runner_t *runner)
{
    if (runner == nullptr || runner->model == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "[%s] 开始 ESP-DL test vector 自检", runner->name);
    const esp_err_t ret = runner->model->test();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%s] 自检失败: %s", runner->name, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[%s] 自检通过", runner->name);
    return ESP_OK;
}

const char *espdl_model_runner_label_name(int index)
{
    if (index < 0 || index >= ESPDL_CLASS_COUNT) {
        return "unknown";
    }
    return kLabels[index];
}

}  // extern "C"
