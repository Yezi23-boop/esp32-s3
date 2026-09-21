#include "music_controller.h"

#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "services/music/music_service.h"
#include "music_view.h"

static const char *TAG = "music_controller";
static lv_ui *s_ui = NULL;
static music_view_t *s_view = NULL;
static lv_obj_t *s_back_screen = NULL;
static bool s_account_page = false;
static uint8_t *s_qr_copy = NULL;
static music_service_catalog_snapshot_t *s_catalog_copy = NULL;
#ifdef AGENT_PREVIEW_HOST
static bool s_preview_catalog_pending = false;
#endif

static const char *const kSourceIds[4] = {
    "today", "liked", "playlists", "recent",
};

static void music_controller_back(void *user_data)
{
    (void)user_data;
    if (s_view == NULL)
    {
        return;
    }
    if (s_account_page)
    {
        (void)music_service_cancel_qr_login();
        music_view_show_sources(s_view);
        s_account_page = false;
        return;
    }
    lv_obj_t *screen = s_back_screen != NULL ? s_back_screen :
                                               (s_ui != NULL ? s_ui->screen_main : NULL);
    if (screen != NULL)
    {
        lv_screen_load(screen);
    }
    music_view_destroy(s_view);
    s_view = NULL;
    s_back_screen = NULL;
}

static void music_controller_start_source(uint8_t source_index,
                                          void *user_data)
{
    (void)user_data;
    if (source_index >= 4U)
    {
        return;
    }
    s_account_page = false;
    (void)music_service_load_source(kSourceIds[source_index]);
    music_view_show_catalog_loading(s_view, kSourceIds[source_index]);
}

static void music_controller_start_track(const char *source_id,
                                         const char *track_id,
                                         void *user_data)
{
    (void)user_data;
    if (source_id == NULL || source_id[0] == '\0' || track_id == NULL ||
        track_id[0] == '\0')
    {
        return;
    }
    (void)music_service_start(source_id, track_id);
}

static void music_controller_catalog_back(void *user_data)
{
    (void)user_data;
    s_account_page = false;
    music_view_show_sources(s_view);
}

static void music_controller_account(void *user_data)
{
    (void)user_data;
    s_account_page = true;
    music_view_show_account_loading(s_view);
    (void)music_service_start_qr_login();
}

static void music_controller_catalog_load_more(const char *source_id,
                                               uint32_t offset,
                                               void *user_data)
{
    (void)user_data;
    if (source_id != NULL && source_id[0] != '\0')
    {
        (void)music_service_load_source_page(source_id, offset);
    }
}

static void music_controller_toggle(void *user_data)
{
    (void)user_data;
    (void)music_service_toggle_playback();
}

static void music_controller_previous(void *user_data)
{
    (void)user_data;
    (void)music_service_previous();
}

static void music_controller_next(void *user_data)
{
    (void)user_data;
    (void)music_service_next();
}

static void music_controller_mode(music_service_mode_t ignored_mode,
                                   void *user_data)
{
    (void)ignored_mode;
    (void)user_data;
    music_service_snapshot_t snapshot = {0};
    if (music_service_get_snapshot(&snapshot) != ESP_OK)
    {
        return;
    }
    music_service_mode_t next = MUSIC_SERVICE_MODE_REPEAT_ALL;
    if (snapshot.mode == MUSIC_SERVICE_MODE_REPEAT_ALL)
    {
        next = MUSIC_SERVICE_MODE_REPEAT_ONE;
    }
    else if (snapshot.mode == MUSIC_SERVICE_MODE_REPEAT_ONE)
    {
        next = MUSIC_SERVICE_MODE_SHUFFLE;
    }
    else if (snapshot.mode == MUSIC_SERVICE_MODE_SHUFFLE)
    {
        next = MUSIC_SERVICE_MODE_ORDER;
    }
    else if (snapshot.mode == MUSIC_SERVICE_MODE_ORDER)
    {
        next = MUSIC_SERVICE_MODE_SMART;
    }
    (void)music_service_set_mode(next);
}

static void music_controller_ensure_view(void)
{
    if (s_view != NULL)
    {
        return;
    }
    const music_view_config_t config = {
        .back_cb = music_controller_back,
        .source_cb = music_controller_start_source,
        .track_cb = music_controller_start_track,
        .toggle_cb = music_controller_toggle,
        .previous_cb = music_controller_previous,
        .next_cb = music_controller_next,
        .mode_cb = music_controller_mode,
        .catalog_back_cb = music_controller_catalog_back,
        .catalog_load_more_cb = music_controller_catalog_load_more,
        .account_cb = music_controller_account,
        .user_data = NULL,
    };
    s_view = music_view_create(&config);
    if (s_view == NULL)
    {
        ESP_LOGE(TAG, "music view create failed");
    }
    else
    {
        music_view_show_sources(s_view);
    }
}

void music_controller_init(lv_ui *ui)
{
    s_ui = ui;
    if (s_qr_copy == NULL)
    {
        s_qr_copy = heap_caps_calloc(1U, MUSIC_SERVICE_QR_MAX_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_catalog_copy == NULL)
    {
        s_catalog_copy = heap_caps_calloc(1U, sizeof(*s_catalog_copy),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

void music_controller_open(void)
{
    s_back_screen = lv_screen_active();
    music_controller_ensure_view();
    if (s_view == NULL)
    {
        return;
    }
    music_controller_poll_ui();
    lv_screen_load_anim(music_view_get_screen(s_view), LV_SCR_LOAD_ANIM_MOVE_LEFT,
                        250, 0, false);
}

#ifdef AGENT_PREVIEW_HOST
void music_controller_preview_open_catalog(void)
{
    s_preview_catalog_pending = true;
    music_controller_open();
}

void music_controller_preview_open_account(void)
{
    music_controller_open();
    if (s_view != NULL)
    {
        music_view_show_account_loading(s_view);
    }
}

#endif

void music_controller_poll_ui(void)
{
    if (s_view == NULL || lv_screen_active() != music_view_get_screen(s_view))
    {
        return;
    }
#ifdef AGENT_PREVIEW_HOST
    if (s_preview_catalog_pending)
    {
        s_preview_catalog_pending = false;
        music_controller_start_source(0U, NULL);
    }
#endif
    music_service_snapshot_t snapshot = {0};
    if (music_service_get_snapshot(&snapshot) == ESP_OK)
    {
        music_view_apply_snapshot(s_view, &snapshot);
    }
    if (s_catalog_copy != NULL &&
        music_service_get_catalog(s_catalog_copy) == ESP_OK)
    {
        music_view_apply_catalog(s_view, s_catalog_copy);
    }
    if (s_account_page)
    {
        music_service_account_snapshot_t account = {0};
        if (music_service_get_account(&account) == ESP_OK)
        {
            uint16_t qr_size = 0U;
            size_t qr_bytes = 0U;
            const esp_err_t qr_ret =
                s_qr_copy != NULL
                    ? music_service_copy_qr(s_qr_copy, MUSIC_SERVICE_QR_MAX_BYTES,
                                            &qr_size, &qr_bytes)
                    : ESP_ERR_NO_MEM;
            music_view_apply_account(
                s_view, &account,
                qr_ret == ESP_OK ? s_qr_copy : NULL, qr_size,
                qr_ret == ESP_OK ? qr_bytes : 0U);
        }
    }
}
