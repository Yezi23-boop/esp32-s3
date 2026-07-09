## espdl_inference 组件 CMakeLists.txt
## 基于 ESP-DL 的单模型推理组件，当前 active 模型为 V59 softdistill T90。

set(srcs
    espdl_model_runner.cpp
    espdl_feature_pipeline.cpp
    espdl_audio_runtime.cpp
)

idf_build_get_property(component_targets __COMPONENT_TARGETS)

# 检测 esp-dl 组件是否可用，获取其路径用于 include esp-dl 的 cmake 工具函数
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
else()
    message(FATAL_ERROR "espdl_inference: 未找到 esp-dl 组件，请检查 main/idf_component.yml 是否声明了 espressif/esp-dl 依赖。")
endif()

set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

# 只嵌入 active 模型，避免多模型同时占用 flash/PSRAM。
set(embed_files
    models/edge_mix_teacher_dscnn_medium_v59_v54_anchor_softdistill_t90_20260608.espdl
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES
        esp-dl
        audio_codec
        esp_timer
)

foreach(embed_file ${embed_files})
    target_add_aligned_binary_data(${COMPONENT_LIB} ${embed_file} BINARY)
endforeach()
