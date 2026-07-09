idf_component_register(
    SRCS "mp3_player.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_codec esp_psram chmorgan__esp-audio-player spiffs
)
