set(app_srcs

    ${CMAKE_CURRENT_LIST_DIR}/app/app_main.c

    ${CMAKE_CURRENT_LIST_DIR}/app/hardware_init.c

    ${CMAKE_CURRENT_LIST_DIR}/app/board_button.c

    ${CMAKE_CURRENT_LIST_DIR}/app/board_ds2413_motor.c

    ${CMAKE_CURRENT_LIST_DIR}/app/board_power.c

    ${CMAKE_CURRENT_LIST_DIR}/app/board_imu.c

    ${CMAKE_CURRENT_LIST_DIR}/app/resource_fs.c

)



set(service_srcs

    ${CMAKE_CURRENT_LIST_DIR}/services/network_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/background_https_gate.c

    ${CMAKE_CURRENT_LIST_DIR}/services/foreground_runtime_gate.c

    ${CMAKE_CURRENT_LIST_DIR}/services/official_chat_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/memory_watch_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/memory_watch_ogg_opus_muxer.c

    ${CMAKE_CURRENT_LIST_DIR}/services/memory_watch_recorder.c

    ${CMAKE_CURRENT_LIST_DIR}/services/audio_mic_test_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/memory_watch_voice_client.c
    ${CMAKE_CURRENT_LIST_DIR}/services/watch_endpoint_service.c
    ${CMAKE_CURRENT_LIST_DIR}/services/memory_watch_ws_client.cc

    ${CMAKE_CURRENT_LIST_DIR}/services/power_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/power_policy.c

    ${CMAKE_CURRENT_LIST_DIR}/services/sleep_coordinator.c

    ${CMAKE_CURRENT_LIST_DIR}/services/wakeup_evidence_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/system_time_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/imu_service.c
    ${CMAKE_CURRENT_LIST_DIR}/services/fall_detection_service.c

    ${CMAKE_CURRENT_LIST_DIR}/services/startup_readiness.c

    ${CMAKE_CURRENT_LIST_DIR}/services/safety_monitor_session.c

    ${CMAKE_CURRENT_LIST_DIR}/services/background_service_manager.c

)



set(feature_srcs

    ${CMAKE_CURRENT_LIST_DIR}/features/alerts/app_alert_manager.c

    ${CMAKE_CURRENT_LIST_DIR}/features/alerts/audio_alert_player.c

    ${CMAKE_CURRENT_LIST_DIR}/features/alerts/haptic_alert_player.c

    ${CMAKE_CURRENT_LIST_DIR}/features/alerts/display_alert_adapter.c

    ${CMAKE_CURRENT_LIST_DIR}/features/audio/audio_app.c

    ${CMAKE_CURRENT_LIST_DIR}/features/danger_detection/danger_detection_service.c

    ${CMAKE_CURRENT_LIST_DIR}/features/danger_detection/danger_sample_recorder.c

    ${CMAKE_CURRENT_LIST_DIR}/features/mini_games/mini_game_2048.c

    ${CMAKE_CURRENT_LIST_DIR}/features/mini_games/mini_game_flappy.c

    ${CMAKE_CURRENT_LIST_DIR}/features/mini_games/mini_game_dino.c

    ${CMAKE_CURRENT_LIST_DIR}/features/weather/hptts.c

    ${CMAKE_CURRENT_LIST_DIR}/features/weather/time_weather.c

)



set(ui_runtime_srcs

    ${CMAKE_CURRENT_LIST_DIR}/ui/lvgl_task.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/ui_refresh_policy.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/ai_chat_view.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/ai_ui_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/cbin_font_bridge.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/clock_functions.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/custom.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/emissive_calendar_view.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/danger_detection_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/danger_detection_view.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/main_dropdown_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/memory_watch_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/watch_notification_center.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/memory_watch_view.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/mini_games_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/wifi_management_controller.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/scroll_functions.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/ui_font_assets.c

    ${CMAKE_CURRENT_LIST_DIR}/ui/custom/mini_game_sprites.c

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

set(BUILTIN_TEXT_FONT font_puhui_common_20_4 CACHE STRING "内置到固件的 xiaozhi cbin 文本字体")
set_property(CACHE BUILTIN_TEXT_FONT PROPERTY STRINGS
    font_puhui_common_14_1
    font_puhui_common_16_4
    font_puhui_common_20_4
    font_puhui_common_30_4
    font_noto_qwen_14_1
    font_noto_qwen_16_4
    font_noto_qwen_20_4
    font_noto_qwen_30_4
)
string(REGEX REPLACE "\\.bin$" "" BUILTIN_TEXT_FONT_ID "${BUILTIN_TEXT_FONT}")
set(BUILTIN_TEXT_FONT_FILE
    ${CMAKE_CURRENT_LIST_DIR}/../managed_components/78__xiaozhi-fonts/cbin/${BUILTIN_TEXT_FONT_ID}.bin
)
if(NOT EXISTS ${BUILTIN_TEXT_FONT_FILE})
    message(FATAL_ERROR "BUILTIN_TEXT_FONT not found: ${BUILTIN_TEXT_FONT_FILE}")
endif()
message(STATUS "Builtin text font: ${BUILTIN_TEXT_FONT_ID}.bin")



idf_component_register(

    SRCS ${srcs}

    INCLUDE_DIRS ${include_dirs}

    EMBED_FILES ${BUILTIN_TEXT_FONT_FILE}

    REQUIRES

        lvgl_port

        z_print_esp32

        system_time

        sd_card

        audio_codec

        mp3_player

        traffic_inference

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

        json

        driver

        freertos

        spiffs

        joltwallet__littlefs

)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "UI_FONT_ASSETS_BUILTIN_TEXT_NAME=\"${BUILTIN_TEXT_FONT_ID}.bin\""
    "UI_FONT_ASSETS_BUILTIN_TEXT_START_SYMBOL=\"_binary_${BUILTIN_TEXT_FONT_ID}_bin_start\""
    "UI_FONT_ASSETS_BUILTIN_TEXT_END_SYMBOL=\"_binary_${BUILTIN_TEXT_FONT_ID}_bin_end\""
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Os)

