file(GLOB app_srcs

    ${CMAKE_CURRENT_LIST_DIR}/app/*.c

)



file(GLOB_RECURSE service_srcs

    ${CMAKE_CURRENT_LIST_DIR}/services/*.c
    ${CMAKE_CURRENT_LIST_DIR}/services/*.cc

)

file(GLOB_RECURSE feature_srcs

    ${CMAKE_CURRENT_LIST_DIR}/features/*.c

)



file(GLOB ui_runtime_srcs

    ${CMAKE_CURRENT_LIST_DIR}/ui/*.c
    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/*.c

)



file(GLOB_RECURSE ui_generated_srcs

    ${CMAKE_CURRENT_LIST_DIR}/ui/generated/*.c

)

file(GLOB_RECURSE ui_custom_font_srcs

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/fonts/*.c

)



set(srcs

    ${app_srcs}

    ${service_srcs}

    ${feature_srcs}

    ${ui_runtime_srcs}

    ${ui_generated_srcs}

    ${ui_custom_font_srcs}

)



set(include_dirs

    .

    ui

    ui/custom

    ui/generated

)

idf_component_register(

    SRCS ${srcs}

    INCLUDE_DIRS ${include_dirs}

    REQUIRES

        lvgl_port

        z_print_esp32

        system_time

        sd_card

        audio_codec

        mp3_player

        espdl_inference
        fall_detection_inference

        network_manager

        wifi_control

        ap_portal_adapter

        official_chat

        axp2101

        ds2413

        espressif__onewire_bus

        pcf85063atl

        imu_sensor

        78__xiaozhi-fonts

        chmorgan__esp-audio-player

        espressif__esp_audio_codec

        nvs_flash

        esp_wifi

        esp_event

        esp_http_client

        esp_netif

        esp_system

        esp-tls

        esp_https_ota
        esp_delta_ota

        mbedtls

        json

        driver

        freertos

        spiffs

        joltwallet__littlefs

)

target_compile_options(${COMPONENT_LIB} PRIVATE -Os)

