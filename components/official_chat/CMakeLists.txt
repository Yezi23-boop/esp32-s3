set(OFFICIAL_CHAT_SOURCES
    "official_chat_c_api.cc"
    "official_chat_websocket_transport.cc"
    "application.cc"
    "board_metadata.cc"
    "settings.cc"
    "system_identity.cc"
    "ota.cc"
    "assets_runtime.cc"
    "device_state_machine.cc"
    "mcp_server.cc"
    "protocol_config.cc"
    "audio/audio_service.cc"
    "audio/local_audio_codec_adapter.cc"
    "audio/processors/afe_audio_processor.cc"
    "audio/processors/no_audio_processor.cc"
    "audio/wake_words/afe_wake_word.cc"
    "audio/demuxer/ogg_demuxer.cc"
    "protocols/protocol.cc"
    "protocols/mqtt_protocol.cc"
    "protocols/websocket_protocol.cc"
    "net/esp_tcp.cc"
    "net/esp_ssl.cc"
    "net/websocket_client.cc"
)

idf_component_register(
    SRCS ${OFFICIAL_CHAT_SOURCES}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "audio" "audio/processors" "audio/wake_words" "audio/demuxer" "protocols" "net"
    EMBED_TXTFILES "board_metadata/esp32-s3-touch-amoled-2.06.json"
    REQUIRES audio_codec utils json esp-tls esp_timer mbedtls esp_audio_effects esp-sr espressif__esp_audio_codec wifi_control mqtt lwip nvs_flash esp_http_client app_update efuse
)
