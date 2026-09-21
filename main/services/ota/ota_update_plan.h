#ifndef OTA_UPDATE_PLAN_H
#define OTA_UPDATE_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define OTA_UPDATE_URL_MAX 192U
#define OTA_UPDATE_VERSION_MAX 32U
#define OTA_UPDATE_SHA256_HEX_LEN 64U
#define OTA_UPDATE_MD5_HEX_LEN 32U
#define OTA_UPDATE_AUTHORIZATION_MAX 256U

    /** OTA 计划的协议来源；执行层不依赖来源的 JSON 或任务字段。 */
    typedef enum
    {
        OTA_UPDATE_SOURCE_ONENET = 0,
        OTA_UPDATE_SOURCE_REMOTE_MANIFEST,
    } ota_update_source_t;

    /** 完整镜像和 patch 共用的摘要类型。 */
    typedef enum
    {
        OTA_UPDATE_CHECKSUM_SHA256 = 0,
        OTA_UPDATE_CHECKSUM_MD5,
    } ota_update_checksum_type_t;

    /**
     * @brief provider 生成的通用 OTA 计划。
     *
     * `authorization`、CA 指针和 provider_task_id 只存在于当前 OTA 会话，
     * 不应复制到 UI 快照、metrics 或 NVS。delta 字段只有 has_delta 为 true
     * 时有效；完整包字段始终保留，供 delta 失败后的回退使用。
     */
    typedef struct
    {
        ota_update_source_t source;
        char version[OTA_UPDATE_VERSION_MAX];
        char url[OTA_UPDATE_URL_MAX];
        size_t size;
        ota_update_checksum_type_t checksum_type;
        char sha256[OTA_UPDATE_SHA256_HEX_LEN + 1U];
        char md5[OTA_UPDATE_MD5_HEX_LEN + 1U];

        /* 当前下载会话的 TLS/Authorization 参数，仅驻留 RAM。 */
        const char *root_ca_pem;
        bool use_cert_bundle;
        char authorization[OTA_UPDATE_AUTHORIZATION_MAX];

        /* provider 的任务 ID 只用于 activate 前的状态持久化。 */
        uint32_t provider_task_id;

        /* 差分 artifact：完整包字段仍作为失败回退目标。 */
        bool has_delta;
        char baseline_version[OTA_UPDATE_VERSION_MAX];
        char patch_url[OTA_UPDATE_URL_MAX];
        size_t patch_size;
        char patch_sha256[OTA_UPDATE_SHA256_HEX_LEN + 1U];
        char target_sha256[OTA_UPDATE_SHA256_HEX_LEN + 1U];
    } ota_update_plan_t;

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_PLAN_H */
