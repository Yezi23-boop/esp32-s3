#include <stdio.h>
#include <string.h>
#include <time.h>

#include "system_util.h"

#define TAG "system_util"

size_t get_flash_size(void)
{
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t get_minimum_free_heap_size(void)
{
    return esp_get_minimum_free_heap_size();
}

const char *get_compile_time(const esp_app_desc_t *desc)
{
    static char buff[64];
    memset(buff, 0, sizeof(buff));
    snprintf(buff, sizeof(buff), "%sT%s", desc->date, desc->time);
    return buff;
}

const char *generate_uuid(void)
{
    static char uuid_str[37];
    uint8_t uuid[16];

    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    memset(uuid_str, 0, sizeof(uuid_str));
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
    return uuid_str;
}

const char *get_mac_address(void)
{
    static char mac_str[18];
    uint8_t mac[6];

    memset(mac_str, 0, sizeof(mac_str));
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mac_str;
}

uint32_t sys_get_chipid(void)
{
    uint32_t chip_id = 0;
    uint64_t chip_mac_id = 0;

    esp_efuse_mac_get_default((uint8_t *)(&chip_mac_id));
    for (int i = 0; i < 17; i += 8) {
        chip_id |= ((chip_mac_id >> (40 - i)) & 0xff) << i;
    }
    return chip_id;
}

void sys_get_timenow(char *buff, int len)
{
    time_t t;
    struct tm *now;

    if (buff == NULL || len <= 0) {
        return;
    }

    t = time(NULL);
    now = localtime(&t);
    strftime(buff, len, "%Y%m%d_%H%M%S", now);
}

uint32_t sys_get_random(void)
{
    return esp_random();
}

uint32_t sys_get_random_inrange(int min, int max)
{
    return esp_random() % (max + 1 - min) + min;
}
