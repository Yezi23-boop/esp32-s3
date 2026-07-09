#ifndef NETWORK_CREDENTIALS_H
#define NETWORK_CREDENTIALS_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 最多保留的最近成功连接 Wi-Fi 条目数。 */
#define NETWORK_CREDENTIALS_MAX_NETWORKS 3

/** @brief Wi-Fi SSID 的最大有效长度，不含结尾 `\0`。 */
#define NETWORK_CREDENTIALS_MAX_SSID_LEN 32

/** @brief Wi-Fi 密码的最大有效长度，不含结尾 `\0`。 */
#define NETWORK_CREDENTIALS_MAX_PASSWORD_LEN 64

/**
 * @brief 一条最近成功连接的 Wi-Fi 记录。
 *
 * 该结构只保存最近成功连接的 Wi-Fi 凭据，不表达 BLE/AP/provisioning 语义。
 */
typedef struct
{
    char ssid[NETWORK_CREDENTIALS_MAX_SSID_LEN + 1]; /**< 最近连接成功的 SSID。 */
    char password[NETWORK_CREDENTIALS_MAX_PASSWORD_LEN + 1]; /**< 与该 SSID 对应的密码。 */
} network_credentials_entry_t;

/**
 * @brief 初始化 recent Wi-Fi 凭据组件。
 *
 * 该接口会准备 NVS 基础环境并加载已保存的 recent list，但不会触发联网或配网行为。
 *
 * @return `ESP_OK` 表示初始化成功；其他错误表示 NVS 初始化或读取失败。
 */
esp_err_t network_credentials_init(void);

/**
 * @brief 获取最近一次成功连接的 Wi-Fi 记录。
 *
 * 若当前没有任何 recent 记录，会返回 `ESP_ERR_NOT_FOUND`。
 *
 * @param[out] entry 输出最近一条记录。
 * @return `ESP_OK` 表示成功；`ESP_ERR_NOT_FOUND` 表示当前没有记录；
 *         `ESP_ERR_INVALID_ARG` 表示参数非法；其他错误表示底层加载失败。
 */
esp_err_t network_credentials_get_latest(network_credentials_entry_t *entry);

/**
 * @brief 枚举当前保存的 recent Wi-Fi 列表。
 *
 * 输出顺序总是“最近成功连接在前”。若 `entries` 为 `NULL` 或 `max_entries` 为 0，
 * 该接口只返回当前真实条目数。
 *
 * @param[out] entries 输出数组，可为 `NULL`。
 * @param[in] max_entries 调用方可接收的最大条目数。
 * @param[out] out_count 当前真实条目数。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法；
 *         其他错误表示 recent 列表初始化或读取失败。
 */
esp_err_t network_credentials_list(network_credentials_entry_t *entries,
                                   size_t max_entries,
                                   size_t *out_count);

/**
 * @brief 保存或前移一条 recent Wi-Fi 记录。
 *
 * - 若 SSID 已存在，则更新密码并前移到队首；
 * - 若 SSID 不存在，则插入到队首；
 * - 总条目数最多保留 `NETWORK_CREDENTIALS_MAX_NETWORKS` 条。
 *
 * @param[in] ssid 最近成功连接的 SSID。
 * @param[in] password 最近成功连接时使用的密码；可为 `NULL`，此时按空密码处理。
 * @return `ESP_OK` 表示成功；`ESP_ERR_INVALID_ARG` 表示参数非法；其他错误表示持久化失败。
 */
esp_err_t network_credentials_save_or_promote(const char *ssid,
                                              const char *password);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_CREDENTIALS_H */
