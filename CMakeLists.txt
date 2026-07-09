# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 抑制第三方库的编译警告
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-attributes -Wno-unused-function")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-attributes -Wno-unused-function")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai-memory-watch)

set(RESOURCE_FS_DIR ${CMAKE_SOURCE_DIR}/resources)
set(AI_FONT_ASSETS_DIR ${CMAKE_SOURCE_DIR}/assets/ai-fonts)
set(AI_FONT_ASSETS_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/build_ai_font_assets.py)
file(GLOB AI_FONT_ASSETS_INPUTS CONFIGURE_DEPENDS ${AI_FONT_ASSETS_DIR}/*)

if(CONFIG_PARTITION_TABLE_CUSTOM)
  partition_table_get_partition_info(size "--partition-name assets" "size")
  partition_table_get_partition_info(offset "--partition-name assets" "offset")

  if("${size}" AND "${offset}")
    idf_build_get_property(build_dir BUILD_DIR)
    set(ai_font_assets_image_file ${build_dir}/assets/ai-fonts.bin)

    add_custom_command(
      OUTPUT ${ai_font_assets_image_file}
      COMMENT "Pack AI font assets..."
      COMMAND python ${AI_FONT_ASSETS_SCRIPT}
              --assets-dir ${AI_FONT_ASSETS_DIR}
              --output ${ai_font_assets_image_file}
              --max-size ${size}
      DEPENDS ${AI_FONT_ASSETS_SCRIPT} ${AI_FONT_ASSETS_INPUTS}
      VERBATIM)

    add_custom_target(ai_font_assets_bin ALL DEPENDS ${ai_font_assets_image_file})
    add_dependencies(flash ai_font_assets_bin)
    esptool_py_flash_to_partition(flash "assets" "${ai_font_assets_image_file}")
  else()
    message(WARNING "Failed to find assets partition in partition table; skip AI font assets image")
  endif()
endif()

if(CONFIG_PARTITION_TABLE_CUSTOM AND EXISTS ${RESOURCE_FS_DIR})
  if(COMMAND littlefs_create_partition_image)
    littlefs_create_partition_image(resources ${RESOURCE_FS_DIR} FLASH_IN_PROJECT)
  else()
    message(WARNING "littlefs_create_partition_image is unavailable; skip resources image")
  endif()
endif()

idf_component_get_property(esp_codec_dev_lib espressif__esp_codec_dev COMPONENT_LIB)
idf_component_get_property(driver_lib driver COMPONENT_LIB)
if(esp_codec_dev_lib AND driver_lib)
  target_link_libraries(${esp_codec_dev_lib} PRIVATE ${driver_lib})
  target_include_directories(${esp_codec_dev_lib} PRIVATE $ENV{IDF_PATH}/components/driver/i2c/include)
endif()

# 将WAV文件打包到SPIFFS分区(需要传入目录,不是单个文件)
#spiffs_create_partition_image(audio ${CMAKE_SOURCE_DIR}/audio_data FLASH_IN_PROJECT)
