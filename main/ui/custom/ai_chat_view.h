#ifndef AI_CHAT_VIEW_H
#define AI_CHAT_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "services/network/network_service.h"
#include "services/official_chat_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ai_chat_view ai_chat_view_t;

typedef struct {
    const char *title_text;
    const char *badge_text;
    uint32_t badge_color_hex;
    const char *status_text;
    const char *hint_text;
    const char *secondary_action_text;
    void (*secondary_action_cb)(void *user_data);
    void (*voice_press_cb)(void *user_data);
    void (*voice_release_cb)(void *user_data);
    void *user_data;
} ai_chat_view_config_t;

ai_chat_view_t *ai_chat_view_create(const ai_chat_view_config_t *config);
void ai_chat_view_destroy(ai_chat_view_t *view);
lv_obj_t *ai_chat_view_get_screen(const ai_chat_view_t *view);

void ai_chat_view_set_top_status(ai_chat_view_t *view, const char *badge_text,
                                 lv_color_t badge_color,
                                 const char *status_text,
                                 const char *hint_text);
void ai_chat_view_set_secondary_action(ai_chat_view_t *view,
                                       const char *action_text, bool visible,
                                       bool enabled);
void ai_chat_view_reload_messages(ai_chat_view_t *view,
                                  const char *empty_placeholder_text);
void ai_chat_view_scroll_to_bottom(ai_chat_view_t *view);

void ai_chat_view_set_voice_button_visible(ai_chat_view_t *view, bool visible);

const char *ai_chat_view_network_badge_text(network_service_state_t state);
lv_color_t ai_chat_view_network_badge_color(network_service_state_t state);
const char *ai_chat_view_network_title(network_service_state_t state);
const char *ai_chat_view_network_hint(network_service_state_t state);
const char *ai_chat_view_service_title(official_chat_service_state_t state);
const char *ai_chat_view_service_hint(official_chat_service_state_t state);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_VIEW_H
