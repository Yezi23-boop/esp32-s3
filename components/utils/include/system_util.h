#ifndef _SYSTEM_UTIL_H
#define _SYSTEM_UTIL_H

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t get_flash_size(void);
size_t get_minimum_free_heap_size(void);
const char *get_mac_address(void);
const char *generate_uuid(void);
const char *get_compile_time(const esp_app_desc_t *desc);
uint32_t sys_get_chipid(void);
void sys_get_timenow(char *buff, int len);
uint32_t sys_get_random(void);
uint32_t sys_get_random_inrange(int min, int max);

#ifdef __cplusplus
}
#endif

#endif
