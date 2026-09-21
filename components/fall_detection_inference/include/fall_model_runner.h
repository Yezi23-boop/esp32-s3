#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FALL_MODEL_FRAME_COUNT 100U
#define FALL_MODEL_CHANNEL_COUNT 6U
#define FALL_MODEL_INPUT_ELEMENTS 600U
#define FALL_MODEL_CLASS_COUNT 2U
#define FALL_MODEL_LABEL_ADL 0
#define FALL_MODEL_LABEL_FALL 1
#define FALL_MODEL_THRESHOLD_DEFAULT 0.60f

    typedef struct fall_model_runner_t fall_model_runner_t;

    /**
     * @brief Fall model inference result.
     *
     * The model output order is fixed to `[ADL, FALL]`. The ESP-DL asset already
     * contains Softmax, so these fields are probabilities read from the output
     * tensor, not logits passed through another Softmax on firmware.
     */
    typedef struct
    {
        int label_index;  /**< `0` is ADL, `1` is FALL. */
        float confidence; /**< Probability of the selected label. */
        float adl_prob;   /**< ADL probability. */
        float fall_prob;  /**< FALL probability. */
        int64_t infer_us; /**< Single-window inference time, in microseconds. */
    } fall_model_result_t;

    /**
     * @brief Load the ESP-DL fall detection model from flash rodata.
     *
     * @param[out] out_runner Created runner handle.
     * @param[in] model_data Embedded `.espdl` binary start address.
     * @param[in] model_name Stable model name for logs.
     * @return `ESP_OK` on success.
     */
    esp_err_t fall_model_runner_create(fall_model_runner_t **out_runner,
                                       const uint8_t *model_data,
                                       const char *model_name);

    /**
     * @brief Destroy a fall model runner.
     */
    void fall_model_runner_destroy(fall_model_runner_t *runner);

    /**
     * @brief Run ESP-DL embedded test vectors from the `.espdl` asset.
     */
    esp_err_t fall_model_runner_self_test(fall_model_runner_t *runner);

    /**
     * @brief Run one 2-second IMU fall-detection window.
     *
     * @param[in] runner Model runner.
     * @param[in] input Flattened float input, shape `[1,600]` (100帧×6ch)。
     * @param[out] result Inference result.
     * @return `ESP_OK` on success.
     */
    esp_err_t fall_model_runner_run(fall_model_runner_t *runner,
                                    const float input[FALL_MODEL_INPUT_ELEMENTS],
                                    fall_model_result_t *result);

    /**
     * @brief Return the stable label text for a model output index.
     */
    const char *fall_model_label_name(int index);

#ifdef __cplusplus
}
#endif
