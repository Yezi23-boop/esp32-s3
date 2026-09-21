#include "services/power/power_policy.h"

#include <string.h>

#include "audio_codec.h"
#include "esp_log.h"

/*
 * 音频事实跨层桥接：
 * - audio_codec 是 AUDIO_ACTIVE 事实和 input/output session 的唯一 owner；
 *   组件本身不反向依赖 power_policy，也不注册省电策略。
 * - 本文件只做两件事：读取 audio owner 暴露的非阻塞 cached session snapshot，
 *   并把 input/output active 映射为 POWER_POLICY_SLEEP_BLOCKER_AUDIO_ACTIVE。
 * - 它不包含播放、录音、告警优先级或 codec 生命周期逻辑。
 */

static const char *TAG = "power_audio_bridge";
static bool s_registered = false;

/**
 * @brief 会话变更回调：请求 power_policy 尽快重算。
 *
 * 该回调由 audio_codec 在持有资源 mutex 的会话变更路径上调用，只发送
 * FreeRTOS task notification（加速触发器），不做任何音频或硬件操作；最终
 * 事实由 get_facts 在 policy task 内直接读取 audio_codec 非阻塞缓存。
 *
 * @param[in] user_ctx 未使用。
 */
static void power_policy_audio_bridge_refresh(void *user_ctx)
{
    (void)user_ctx;
    (void)power_policy_notify(POWER_POLICY_NOTIFY_AUDIO);
}

/**
 * @brief AUDIO_ACTIVE 事实 provider：直接复制 audio_codec 非阻塞缓存。
 */
static esp_err_t power_policy_audio_facts(power_policy_provider_facts_t *facts,
                                          void *context)
{
    (void)context;
    if (facts == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_codec_session_snapshot_t snapshot = {0};
    if (audio_codec_get_cached_session_snapshot(&snapshot) != ESP_OK)
    {
        /* fail-closed：读不到音频事实时不允许放行睡眠。 */
        return ESP_FAIL;
    }

    memset(facts, 0, sizeof(*facts));
    facts->running = snapshot.input_active || snapshot.output_active;
    if (facts->running)
    {
        facts->sleep_blockers |= POWER_POLICY_SLEEP_BLOCKER_AUDIO_ACTIVE;
    }
    return ESP_OK;
}

esp_err_t power_policy_audio_bridge_register(void)
{
    if (s_registered)
    {
        return ESP_OK;
    }

    const esp_err_t callback_ret = audio_codec_set_session_change_callback(
        power_policy_audio_bridge_refresh, NULL);
    if (callback_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "audio session change callback install failed: %s",
                 esp_err_to_name(callback_ret));
        return callback_ret;
    }

    const power_policy_participant_config_t participant = {
        .id = POWER_POLICY_PROVIDER_AUDIO,
        .name = "audio",
        .capabilities = POWER_POLICY_PARTICIPANT_FACTS_ONLY,
        .get_facts = power_policy_audio_facts,
        .on_budget_changed = NULL,
        .context = NULL,
    };
    const esp_err_t ret = power_policy_register_participant(&participant);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "audio participant register failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* 注册后立即触发一次重算，保证首次预算能聚合当前音频会话状态。 */
    power_policy_audio_bridge_refresh(NULL);
    s_registered = true;
    ESP_LOGI(TAG, "audio bridge registered");
    return ESP_OK;
}
