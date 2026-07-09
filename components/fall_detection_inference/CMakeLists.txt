set(srcs
    fall_model_runner.cpp
)

idf_build_get_property(component_targets __COMPONENT_TARGETS)

if("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
else()
    message(FATAL_ERROR "fall_detection_inference: esp-dl component not found")
endif()

set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

set(embed_files
    models/esp32s3/cnn_v1_recall90_6ch_2s_with_test.espdl
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES
        esp-dl
        esp_timer
)

foreach(embed_file ${embed_files})
    target_add_aligned_binary_data(${COMPONENT_LIB} ${embed_file} BINARY)
endforeach()
