idf_component_register(
    SRCS "printf_esp32.c"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_timer heap
)
