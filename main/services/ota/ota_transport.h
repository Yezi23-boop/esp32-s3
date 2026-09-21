#ifndef OTA_TRANSPORT_H
#define OTA_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/ota/ota_update_plan.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define OTA_TRANSPORT_URL_MAX OTA_UPDATE_URL_MAX
#define OTA_TRANSPORT_VERSION_MAX OTA_UPDATE_VERSION_MAX
#define OTA_TRANSPORT_SHA256_HEX_LEN OTA_UPDATE_SHA256_HEX_LEN
#define OTA_TRANSPORT_MD5_HEX_LEN OTA_UPDATE_MD5_HEX_LEN
#define OTA_TRANSPORT_AUTHORIZATION_MAX OTA_UPDATE_AUTHORIZATION_MAX
#define OTA_TRANSPORT_MANIFEST_BUFFER_MAX 2048U

    /**
     * manifest 请求配置；CA/host/current_version 指针必须在请求完成前保持
     * 有效，推荐传入编译期常量或长期配置存储，不传临时栈字符串。
     */
    typedef struct
    {
        const char *manifest_url;
        const char *root_ca_pem;
        bool use_cert_bundle;
        const char *allowed_host;
        const char *current_version;
    } ota_transport_manifest_request_t;

    /** 下载过程回调；回调在 OTA service task 上下文执行。 */
    typedef void (*ota_transport_progress_cb_t)(size_t received,
                                                size_t total,
                                                void *user_ctx);
    typedef bool (*ota_transport_cancel_cb_t)(void *user_ctx);

    /** 开发期下载故障注入点；默认必须使用 NONE。 */
    typedef enum
    {
        OTA_TRANSPORT_FAULT_NONE = 0,
        OTA_TRANSPORT_FAULT_ABORT_AT_20_PERCENT,
        OTA_TRANSPORT_FAULT_ABORT_AT_50_PERCENT,
        OTA_TRANSPORT_FAULT_ABORT_AT_90_PERCENT,
    } ota_transport_fault_mode_t;

    /** HTTPS OTA 下载配置。 */
    typedef struct
    {
        const char *root_ca_pem;
        bool use_cert_bundle;
        const char *authorization;
        ota_transport_progress_cb_t progress_cb;
        ota_transport_cancel_cb_t cancel_cb;
        void *user_ctx;
        ota_transport_fault_mode_t fault_mode;
    } ota_transport_download_config_t;

    /**
     * @brief 拉取并校验独立 manifest，不写 Flash。
     */
    esp_err_t ota_transport_fetch_manifest(
        const ota_transport_manifest_request_t *request,
        ota_update_plan_t *out_plan);

    /** @brief 比较最多三段数字版本；candidate 必须严格高于 current。 */
    bool ota_transport_version_is_newer(const char *current,
                                        const char *candidate);

    /**
     * @brief 下载并校验镜像到备用 OTA 槽，但不改变启动选择。
     *
     * 成功后 transport 保留 OTA handle，调用方必须继续调用 activate 或 abort。
     * 在 activate 前 `otadata` 不会更新。
     */
    esp_err_t ota_transport_download_to_staging(
        const ota_update_plan_t *manifest,
        const ota_transport_download_config_t *config);

    /**
     * @brief 下载差分 patch 并流式应用到备用 OTA 槽（差分升级）。
     *
     * 要求 manifest->has_delta 为 true。与全量路径相同：成功后 transport 保留
     * delta staged 状态，调用方必须继续调用 activate 或 abort；activate 前
     * `otadata` 不会更新。内部完成 patch 完整性校验（patch_sha256）与应用后
     * 目标分区哈希校验（target_sha256），任一失败返回错误且不切换启动槽。
     */
    esp_err_t ota_transport_download_delta_to_staging(
        const ota_update_plan_t *manifest,
        const ota_transport_download_config_t *config);

    /**
     * @brief 提交已完成的备用槽，使其成为下一次启动的 OTA 槽。
     * @return `ESP_OK` 表示启动选择已更新；调用方随后必须立即重启。
     */
    esp_err_t ota_transport_activate_staging(void);

    /**
     * @brief 丢弃尚未激活的下载会话，保持当前启动槽不变。
     */
    esp_err_t ota_transport_abort_staging(void);

    /** @brief 查询 transport 是否持有已下载但未激活的备用槽会话。 */
    bool ota_transport_has_staged_image(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_TRANSPORT_H */
