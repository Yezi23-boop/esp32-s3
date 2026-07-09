idf_component_register(
    SRCS
        "src/captive_portal_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip freertos log
)
