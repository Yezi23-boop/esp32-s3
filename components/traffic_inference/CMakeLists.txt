set(EDGE_IMPULSE_MODEL_DIR
    "${CMAKE_CURRENT_LIST_DIR}/edge_impulse/manual_v7_1s")
set(EDGE_IMPULSE_SDK_DIR
    "${CMAKE_CURRENT_LIST_DIR}/edge_impulse/manual_v5/edge-impulse-sdk")
set(EDGE_IMPULSE_PORTING_ESPRESSIF_DIR
    "${EDGE_IMPULSE_SDK_DIR}/porting/espressif")
set(EDGE_IMPULSE_ESP_DSP_DIR
    "${EDGE_IMPULSE_PORTING_ESPRESSIF_DIR}/esp-dsp")
set(EDGE_IMPULSE_ESP_NN_DIR
    "${EDGE_IMPULSE_PORTING_ESPRESSIF_DIR}/ESP-NN")

file(GLOB_RECURSE EDGE_IMPULSE_CORE_CPP
    "${EDGE_IMPULSE_SDK_DIR}/*.cpp")
file(GLOB_RECURSE EDGE_IMPULSE_CORE_CC
    "${EDGE_IMPULSE_SDK_DIR}/*.cc")
file(GLOB_RECURSE EDGE_IMPULSE_CORE_C
    "${EDGE_IMPULSE_SDK_DIR}/*.c")

foreach(EI_SOURCE_LIST
        EDGE_IMPULSE_CORE_CPP
        EDGE_IMPULSE_CORE_CC
        EDGE_IMPULSE_CORE_C)
    list(FILTER ${EI_SOURCE_LIST} EXCLUDE REGEX ".*/cmake/.*")
    list(FILTER ${EI_SOURCE_LIST} EXCLUDE REGEX ".*/CMSIS/.*")
    list(FILTER ${EI_SOURCE_LIST} EXCLUDE REGEX ".*/porting/.*")
    list(FILTER ${EI_SOURCE_LIST} EXCLUDE REGEX ".*/tensorflow/lite/micro/kernels/.*")
    list(FILTER ${EI_SOURCE_LIST} EXCLUDE REGEX ".*/tensorflow/lite/micro/(fake_|mock_|recording_|test_).*")
endforeach()

set(EDGE_IMPULSE_ESPRESSIF_PORTING_SOURCES
    "${EDGE_IMPULSE_PORTING_ESPRESSIF_DIR}/debug_log.cpp"
    "${EDGE_IMPULSE_PORTING_ESPRESSIF_DIR}/ei_classifier_porting.cpp"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/activation_functions/esp_nn_relu_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/basic_math/esp_nn_add_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/basic_math/esp_nn_mul_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_opt.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_opt.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/fully_connected/esp_nn_fully_connected_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/pooling/esp_nn_avg_pool_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/pooling/esp_nn_max_pool_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/softmax/esp_nn_softmax_ansi.c"
    "${EDGE_IMPULSE_ESP_NN_DIR}/src/softmax/esp_nn_softmax_opt.c")

if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND EDGE_IMPULSE_ESPRESSIF_PORTING_SOURCES
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/common/esp_nn_common_functions_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/common/esp_nn_multiply_by_quantized_mult_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/common/esp_nn_multiply_by_quantized_mult_ver1_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/activation_functions/esp_nn_relu_s8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/basic_math/esp_nn_add_s8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/basic_math/esp_nn_mul_s8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_esp32s3.c"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s8_esp32s3.c"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_s16_mult8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_s8_mult8_1x1_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_s16_mult4_1x1_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_conv_s8_filter_aligned_input_padded_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s8_mult1_3x3_padded_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult1_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult1_3x3_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult1_3x3_no_pad_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult8_3x3_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult4_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/convolution/esp_nn_depthwise_conv_s16_mult8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/fully_connected/esp_nn_fully_connected_s8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/pooling/esp_nn_max_pool_s8_esp32s3.S"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/pooling/esp_nn_avg_pool_s8_esp32s3.S")
endif()

file(GLOB EDGE_IMPULSE_MODEL_SOURCES
    "${EDGE_IMPULSE_MODEL_DIR}/tflite-model/*.cpp")

set(EDGE_IMPULSE_MICRO_KERNEL_SOURCES
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/activations.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/activations_common.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/conv.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/conv_common.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/depthwise_conv.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/depthwise_conv_common.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/fully_connected.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/fully_connected_common.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/kernel_util_micro.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/micro_tensor_utils.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/pooling.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/pooling_common.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/reshape.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/scratch_buffers.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/scratch_buf_mgr.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/softmax.cc"
    "${EDGE_IMPULSE_SDK_DIR}/tensorflow/lite/micro/kernels/softmax_common.cc")

set(EDGE_IMPULSE_CMSIS_SOURCES
    "${EDGE_IMPULSE_SDK_DIR}/CMSIS/DSP/Source/StatisticsFunctions/arm_rms_f32.c")

idf_component_register(
    SRCS
        "traffic_inference_runner.cc"
        "traffic_inference_sliding_window.cc"
        "traffic_inference_postprocess.cc"
        "traffic_inference_realtime.cc"
        "traffic_audio_runtime.cc"
        "traffic_inference_demo.cc"
        "traffic_inference_porting.cc"
        ${EDGE_IMPULSE_CORE_CPP}
        ${EDGE_IMPULSE_CORE_CC}
        ${EDGE_IMPULSE_CORE_C}
        ${EDGE_IMPULSE_ESPRESSIF_PORTING_SOURCES}
        ${EDGE_IMPULSE_MODEL_SOURCES}
        ${EDGE_IMPULSE_MICRO_KERNEL_SOURCES}
        ${EDGE_IMPULSE_CMSIS_SOURCES}
    INCLUDE_DIRS
        "include"
        "${EDGE_IMPULSE_MODEL_DIR}"
        "${EDGE_IMPULSE_MODEL_DIR}/model-parameters"
        "${EDGE_IMPULSE_MODEL_DIR}/tflite-model"
        "${CMAKE_CURRENT_LIST_DIR}/edge_impulse/manual_v5"
        "${EDGE_IMPULSE_SDK_DIR}"
        "${EDGE_IMPULSE_SDK_DIR}/porting"
        "${EDGE_IMPULSE_ESP_DSP_DIR}/modules/common/include"
        "${EDGE_IMPULSE_ESP_DSP_DIR}/modules/fft/include"
        "${EDGE_IMPULSE_ESP_NN_DIR}"
        "${EDGE_IMPULSE_ESP_NN_DIR}/include"
        "${EDGE_IMPULSE_ESP_NN_DIR}/src/common"
    REQUIRES
        audio_codec
        esp_timer
        espressif__esp-dsp
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE asm=__asm__)

if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -mlongcalls
        -fno-unroll-loops
        -O2
        -Wno-unused-function
        -Wno-error=uninitialized
        -Wno-error=maybe-uninitialized)
else()
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -O2
        -Wno-unused-function
        -Wno-error=uninitialized
        -Wno-error=maybe-uninitialized)
endif()
