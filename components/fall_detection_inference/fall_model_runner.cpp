#include "fall_model_runner.h"

#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

static const char *TAG = "fall_model_runner";

static const char *const kLabels[FALL_MODEL_CLASS_COUNT] = {
    "ADL",
    "FALL",
};

static const std::vector<int> kExpectedInputShape = {1, FALL_MODEL_INPUT_ELEMENTS};

bool is_supported_output_dtype(dl::dtype_t dtype)
{
    return dtype == dl::DATA_TYPE_INT8 ||
           dtype == dl::DATA_TYPE_INT16 ||
           dtype == dl::DATA_TYPE_FLOAT;
}

bool is_supported_output_shape(dl::TensorBase *tensor)
{
    return tensor != nullptr && tensor->get_size() == FALL_MODEL_CLASS_COUNT;
}

void log_model_memory_info(dl::Model *model, const char *model_name)
{
    if (model == nullptr) {
        return;
    }

    const auto memory_info = model->get_memory_info();
    ESP_LOGI(TAG, "[%s] ESP-DL memory usage after model build:", model_name);

    const char *const rows[] = {
        "fbs_model",
        "variable",
        "parameter",
        "parameter_copy",
        "others",
        "total",
    };

    for (const char *row : rows) {
        const auto item = memory_info.find(row);
        if (item == memory_info.end()) {
            continue;
        }

        const dl::mem_info_t &info = item->second;
        ESP_LOGI(TAG,
                 "  %-14s internal=%lu B, psram=%lu B, flash=%lu B",
                 row,
                 static_cast<unsigned long>(info.internal),
                 static_cast<unsigned long>(info.psram),
                 static_cast<unsigned long>(info.flash));
    }
}

esp_err_t read_output_probabilities(dl::TensorBase *output_tensor,
                                    float probabilities[FALL_MODEL_CLASS_COUNT])
{
    if (output_tensor == nullptr || probabilities == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_supported_output_shape(output_tensor)) {
        ESP_LOGE(TAG,
                 "unexpected output shape: shape=%s, size=%d",
                 dl::vector_to_string(output_tensor->get_shape()).c_str(),
                 output_tensor->get_size());
        return ESP_ERR_INVALID_SIZE;
    }

    const int output_exponent = output_tensor->get_exponent();
    const float output_scale = DL_SCALE(output_exponent);
    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT8) {
        const int8_t *raw = static_cast<const int8_t *>(output_tensor->data);
        for (size_t i = 0; i < FALL_MODEL_CLASS_COUNT; ++i) {
            probabilities[i] = dl::dequantize<int8_t, float>(raw[i], output_scale);
        }
        return ESP_OK;
    }
    if (output_tensor->get_dtype() == dl::DATA_TYPE_INT16) {
        const int16_t *raw = static_cast<const int16_t *>(output_tensor->data);
        for (size_t i = 0; i < FALL_MODEL_CLASS_COUNT; ++i) {
            probabilities[i] = dl::dequantize<int16_t, float>(raw[i], output_scale);
        }
        return ESP_OK;
    }
    if (output_tensor->get_dtype() == dl::DATA_TYPE_FLOAT) {
        const float *raw = static_cast<const float *>(output_tensor->data);
        for (size_t i = 0; i < FALL_MODEL_CLASS_COUNT; ++i) {
            probabilities[i] = raw[i];
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "unsupported output dtype: %s", output_tensor->get_dtype_string());
    return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace

struct fall_model_runner_t {
    dl::Model *model;
    const char *name;
};

extern "C" {

esp_err_t fall_model_runner_create(fall_model_runner_t **out_runner,
                                   const uint8_t *model_data,
                                   const char *model_name)
{
    if (out_runner == nullptr || model_data == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_runner = nullptr;

    auto *runner = new (std::nothrow) fall_model_runner_t();
    if (runner == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    runner->model = nullptr;
    runner->name = model_name != nullptr ? model_name : "unnamed";

    runner->model = new (std::nothrow) dl::Model(
        reinterpret_cast<const char *>(model_data), fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (runner->model == nullptr) {
        delete runner;
        return ESP_ERR_NO_MEM;
    }
    if (runner->model->get_inputs().empty() || runner->model->get_outputs().empty()) {
        ESP_LOGE(TAG, "[%s] model has no input or output tensors", runner->name);
        fall_model_runner_destroy(runner);
        return ESP_ERR_INVALID_RESPONSE;
    }

    dl::TensorBase *model_input = runner->model->get_inputs().begin()->second;
    dl::TensorBase *model_output = runner->model->get_outputs().begin()->second;
    if (model_input->get_shape() != kExpectedInputShape ||
        !is_supported_output_shape(model_output)) {
        ESP_LOGE(TAG,
                 "[%s] tensor contract mismatch: input=%s, output=%s",
                 runner->name,
                 dl::vector_to_string(model_input->get_shape()).c_str(),
                 dl::vector_to_string(model_output->get_shape()).c_str());
        fall_model_runner_destroy(runner);
        return ESP_ERR_INVALID_SIZE;
    }
    if (model_input->get_dtype() != dl::DATA_TYPE_FLOAT ||
        !is_supported_output_dtype(model_output->get_dtype())) {
        ESP_LOGE(TAG,
                 "[%s] unsupported dtype: input=%s, output=%s",
                 runner->name,
                 model_input->get_dtype_string(),
                 model_output->get_dtype_string());
        fall_model_runner_destroy(runner);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG,
             "[%s] model loaded: input=%s exp=%d shape=%s, output=%s exp=%d shape=%s, threshold=%.2f",
             runner->name,
             model_input->get_dtype_string(),
             model_input->get_exponent(),
             dl::vector_to_string(model_input->get_shape()).c_str(),
             model_output->get_dtype_string(),
             model_output->get_exponent(),
             dl::vector_to_string(model_output->get_shape()).c_str(),
             static_cast<double>(FALL_MODEL_THRESHOLD_DEFAULT));
    log_model_memory_info(runner->model, runner->name);

    *out_runner = runner;
    return ESP_OK;
}

void fall_model_runner_destroy(fall_model_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    delete runner->model;
    runner->model = nullptr;
    delete runner;
}

esp_err_t fall_model_runner_self_test(fall_model_runner_t *runner)
{
    if (runner == nullptr || runner->model == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "[%s] running ESP-DL embedded test vector", runner->name);
    const esp_err_t ret = runner->model->test();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%s] ESP-DL test vector failed: %s",
                 runner->name, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[%s] ESP-DL test vector passed", runner->name);
    return ESP_OK;
}

esp_err_t fall_model_runner_run(fall_model_runner_t *runner,
                                const float input[FALL_MODEL_INPUT_ELEMENTS],
                                fall_model_result_t *result)
{
    if (runner == nullptr || runner->model == nullptr ||
        input == nullptr || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = {};
    result->label_index = -1;

    const auto input_iter = runner->model->get_inputs().begin();
    const std::string input_name = input_iter->first;
    dl::TensorBase *model_input = input_iter->second;
    if (model_input->get_dtype() != dl::DATA_TYPE_FLOAT ||
        model_input->get_shape() != kExpectedInputShape) {
        ESP_LOGE(TAG, "[%s] runtime input contract changed unexpectedly", runner->name);
        return ESP_ERR_INVALID_STATE;
    }

    auto input_tensor = std::make_unique<dl::TensorBase>(
        kExpectedInputShape, const_cast<float *>(input), 0, dl::DATA_TYPE_FLOAT, false);
    std::map<std::string, dl::TensorBase *> input_map = {{input_name, input_tensor.get()}};

    const int64_t start_us = esp_timer_get_time();
    runner->model->run(input_map, dl::RUNTIME_MODE_SINGLE_CORE);
    result->infer_us = esp_timer_get_time() - start_us;

    float probabilities[FALL_MODEL_CLASS_COUNT] = {0.0f, 0.0f};
    const esp_err_t ret = read_output_probabilities(
        runner->model->get_outputs().begin()->second, probabilities);
    if (ret != ESP_OK) {
        return ret;
    }

    result->adl_prob = probabilities[FALL_MODEL_LABEL_ADL];
    result->fall_prob = probabilities[FALL_MODEL_LABEL_FALL];
    const bool is_fall = result->fall_prob >= FALL_MODEL_THRESHOLD_DEFAULT;
    result->label_index = is_fall ? FALL_MODEL_LABEL_FALL : FALL_MODEL_LABEL_ADL;
    result->confidence = is_fall ? result->fall_prob : result->adl_prob;

    ESP_LOGD(TAG,
             "[%s] decision=%s confidence=%.4f adl_prob=%.4f fall_prob=%.4f threshold=%.2f infer_us=%lld",
             runner->name,
             fall_model_label_name(result->label_index),
             static_cast<double>(result->confidence),
             static_cast<double>(result->adl_prob),
             static_cast<double>(result->fall_prob),
             static_cast<double>(FALL_MODEL_THRESHOLD_DEFAULT),
             static_cast<long long>(result->infer_us));
    return ESP_OK;
}

const char *fall_model_label_name(int index)
{
    if (index < 0 || index >= static_cast<int>(FALL_MODEL_CLASS_COUNT)) {
        return "unknown";
    }
    return kLabels[index];
}

}  // extern "C"
