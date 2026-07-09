# 统一系统时间核心
# - 封装 ESP-IDF SNTP、系统时间可信判断和 PCF85063ATL RTC 回写
# - 上层 service 负责启动编排，业务模块不直接调用 esp_sntp_*
idf_component_register(
    SRCS "system_time.c"
    INCLUDE_DIRS "include"
    REQUIRES pcf85063atl lwip
)
