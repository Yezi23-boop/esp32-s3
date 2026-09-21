#include "ai_ui_controller.h"

#include <stdint.h>

#include "ai_chat_view.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gui_guider.h"
#include "services/network/network_service.h"
#include "services/official_chat_service.h"

static const char *TAG = "ai_ui";

static lv_ui *s_ui = NULL;
static ai_chat_view_t *s_view = NULL;
static lv_timer_t *s_status_timer = NULL;
static lv_timer_t *s_destroy_timer = NULL;
static ai_chat_view_t *s_pending_destroy_view = NULL;
static bool s_foreground_requested = false;
static uint32_t s_preconnect_last_request_ms = 0;
static bool s_exit_requested = false;
static const uint32_t kPreconnectRetryDelayMs = 15000;

static void ai_ui_refresh_status(void);
static void ai_ui_complete_exit_to_main(void);

static void ai_ui_flush_pending_destroy(void) {
    if (s_destroy_timer != NULL) {
        lv_timer_delete(s_destroy_timer);
        s_destroy_timer = NULL;
    }

    if (s_pending_destroy_view != NULL) {
        ai_chat_view_destroy(s_pending_destroy_view);
        s_pending_destroy_view = NULL;
    }
}

static void ai_ui_destroy_screen_cb(lv_timer_t *timer) {
    if (timer != NULL) {
        lv_timer_delete(timer);
    }

    s_destroy_timer = NULL;

    if (s_pending_destroy_view != NULL) {
        ai_chat_view_destroy(s_pending_destroy_view);
        s_pending_destroy_view = NULL;
    }
}

static void ai_ui_destroy_screen(void) {
    if (s_view == NULL) {
        return;
    }

    ai_chat_view_t *view_to_destroy = s_view;
    s_view = NULL;
    s_foreground_requested = false;
    s_exit_requested = false;

    if (s_status_timer != NULL) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }

    ai_ui_flush_pending_destroy();

    s_pending_destroy_view = view_to_destroy;
    s_destroy_timer = lv_timer_create(ai_ui_destroy_screen_cb, 350, NULL);
}

static void ai_ui_back_event(void *user_data) {
    (void)user_data;

    if (s_ui == NULL || s_ui->screen_main == NULL) {
        ESP_LOGW(TAG, "screen_main not ready when leaving ai page");
        return;
    }

    if (s_exit_requested || official_chat_service_is_shutdown_pending()) {
        ESP_LOGI(TAG, "ai exit already in progress");
        ai_ui_refresh_status();
        return;
    }

    s_exit_requested = true;
    s_foreground_requested = false;
    s_preconnect_last_request_ms = 0;
    official_chat_service_leave_foreground();
    ESP_LOGI(TAG, "ai exit requested");
    ai_ui_refresh_status();
}

static void ai_ui_voice_press_event(void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "voice button pressed: start listening");
    official_chat_service_start_listening();
}

static void ai_ui_voice_release_event(void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "voice button released: stop listening");
    official_chat_service_stop_listening();
}

static void ai_ui_refresh_status(void) {
    if (s_view == NULL) {
        return;
    }

    network_service_state_t network_state = network_service_get_state();
    official_chat_service_snapshot_t chat_snapshot = {0};
    if (official_chat_service_get_snapshot(&chat_snapshot) != ESP_OK) {
        chat_snapshot.state = official_chat_service_get_state();
        chat_snapshot.audio_channel_ready = false;
    }
    official_chat_service_state_t service_state = chat_snapshot.state;
    const bool shutdown_pending =
        s_exit_requested || official_chat_service_is_shutdown_pending();

    if (shutdown_pending) {
        ai_chat_view_set_top_status(
            s_view, ai_chat_view_network_badge_text(network_state),
            ai_chat_view_network_badge_color(network_state), "正在退出 AI",
            ai_chat_view_service_title(service_state));
        ai_chat_view_set_secondary_action(s_view, "正在退出", true, false);
        ai_chat_view_set_voice_button_visible(s_view, false);
        ai_chat_view_reload_messages(s_view, "正在安全退出 AI，请稍候。");
        return;
    }

    if (network_state == NETWORK_SERVICE_STATE_SERVICE_READY) {
        if (!s_foreground_requested) {
            official_chat_service_enter_foreground();
            s_foreground_requested = true;
            s_preconnect_last_request_ms = 0;
            ESP_LOGI(TAG, "official_chat foreground requested");
        }

        if (service_state == OFFICIAL_CHAT_SERVICE_STATE_IDLE &&
            !chat_snapshot.audio_channel_ready &&
            (s_preconnect_last_request_ms == 0 ||
             lv_tick_elaps(s_preconnect_last_request_ms) >=
                 kPreconnectRetryDelayMs)) {
            const esp_err_t ret = official_chat_service_prepare_audio_channel();
            if (ret == ESP_OK) {
                s_preconnect_last_request_ms = lv_tick_get();
                ESP_LOGI(TAG, "official_chat audio channel preconnect requested");
            } else {
                ESP_LOGW(TAG, "official_chat audio channel preconnect failed: %s",
                         esp_err_to_name(ret));
            }
        }

        const bool waiting_preconnect =
            service_state == OFFICIAL_CHAT_SERVICE_STATE_IDLE &&
            !chat_snapshot.audio_channel_ready;
        const char *status_text = waiting_preconnect
                                      ? "正在连接"
                                      : ai_chat_view_service_title(service_state);
        const char *hint_text = waiting_preconnect
                                    ? "正在预连接 WebSocket，请稍候。"
                                    : ai_chat_view_service_hint(service_state);
        ai_chat_view_set_top_status(
            s_view, ai_chat_view_network_badge_text(network_state),
            ai_chat_view_network_badge_color(network_state), status_text,
            hint_text);
        ai_chat_view_set_secondary_action(s_view, "返回主页", true, true);
        const bool voice_visible =
            ((service_state == OFFICIAL_CHAT_SERVICE_STATE_IDLE &&
              chat_snapshot.audio_channel_ready) ||
             service_state == OFFICIAL_CHAT_SERVICE_STATE_LISTENING);
        ai_chat_view_set_voice_button_visible(s_view, voice_visible);
    } else {
        s_foreground_requested = false;
        s_preconnect_last_request_ms = 0;
        ai_chat_view_set_top_status(
            s_view, ai_chat_view_network_badge_text(network_state),
            ai_chat_view_network_badge_color(network_state),
            ai_chat_view_network_title(network_state),
            ai_chat_view_network_hint(network_state));
        ai_chat_view_set_secondary_action(s_view, "返回主页", true, true);
        ai_chat_view_set_voice_button_visible(s_view, false);
    }

    ai_chat_view_reload_messages(s_view, "还没有真实消息，先说一句试试。");
}

static void ai_ui_complete_exit_to_main(void) {
    if (s_ui == NULL || s_ui->screen_main == NULL) {
        ESP_LOGW(TAG, "screen_main not ready when completing ai exit");
        return;
    }

    ai_ui_destroy_screen();
    lv_screen_load_anim(s_ui->screen_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0,
                        true);
}

static void ai_ui_status_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_view == NULL || lv_screen_active() != ai_chat_view_get_screen(s_view)) {
        return;
    }

    if (official_chat_service_is_shutdown_pending() &&
        official_chat_service_get_state() ==
            OFFICIAL_CHAT_SERVICE_STATE_STOPPED) {
        ai_ui_complete_exit_to_main();
        return;
    }

    if (s_exit_requested &&
        official_chat_service_get_state() ==
            OFFICIAL_CHAT_SERVICE_STATE_STOPPED) {
        ai_ui_complete_exit_to_main();
        return;
    }

    ai_ui_refresh_status();
}

void ai_ui_controller_init(lv_ui *ui) {
    s_ui = ui;
    s_exit_requested = false;
    s_preconnect_last_request_ms = 0;
}

static void ai_ui_ensure_screen_created(void) {
    ai_ui_flush_pending_destroy();

    if (s_view != NULL) {
        return;
    }

    static const ai_chat_view_config_t kConfig = {
        .title_text = "小智",
        .badge_text = "准备中",
        .badge_color_hex = 0x64748b,
        .status_text = "正在准备",
        .hint_text = "",
        .secondary_action_text = "返回主页",
        .secondary_action_cb = ai_ui_back_event,
        .voice_press_cb = ai_ui_voice_press_event,
        .voice_release_cb = ai_ui_voice_release_event,
        .user_data = NULL,
    };

    s_view = ai_chat_view_create(&kConfig);
    if (s_view == NULL) {
        ESP_LOGE(TAG, "ai_chat_view_create failed");
        return;
    }

    if (s_status_timer == NULL) {
        s_status_timer = lv_timer_create(ai_ui_status_timer_cb, 200, NULL);
    }

    ai_ui_refresh_status();
}

void ai_ui_open(void) {
    ai_ui_ensure_screen_created();
    if (s_view == NULL) {
        return;
    }

    s_exit_requested = false;
    ai_ui_refresh_status();
    lv_screen_load_anim(ai_chat_view_get_screen(s_view),
                        LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
