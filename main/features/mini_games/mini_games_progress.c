#include "features/mini_games/mini_games_progress.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "mini_games_progress";
static const char *kNvsNamespace = "mini_games";
static const char *kNvsScoresKey = "scores";
static const uint32_t kStorageVersion = 1U;
/* NVS 会短时关闭 Flash cache，因此该 worker 的 3072B 栈必须留在 internal RAM。 */
static const uint32_t kWorkerStackBytes = 3072U;

typedef struct {
    uint32_t version;
    uint32_t high_scores[MINI_GAMES_PROGRESS_COUNT];
} mini_games_progress_storage_t;

static portMUX_TYPE s_progress_lock = portMUX_INITIALIZER_UNLOCKED;
static mini_games_progress_storage_t s_progress = {
    .version = 1U,
};
static QueueHandle_t s_save_queue;
static TaskHandle_t s_worker_task;

static void mini_games_progress_merge_loaded(
    const mini_games_progress_storage_t *loaded)
{
    taskENTER_CRITICAL(&s_progress_lock);
    for (size_t i = 0; i < MINI_GAMES_PROGRESS_COUNT; ++i) {
        if (loaded->high_scores[i] > s_progress.high_scores[i]) {
            s_progress.high_scores[i] = loaded->high_scores[i];
        }
    }
    taskEXIT_CRITICAL(&s_progress_lock);
}

static mini_games_progress_storage_t mini_games_progress_snapshot(void)
{
    mini_games_progress_storage_t snapshot;
    taskENTER_CRITICAL(&s_progress_lock);
    snapshot = s_progress;
    taskEXIT_CRITICAL(&s_progress_lock);
    return snapshot;
}

static void mini_games_progress_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open scores for read failed: %s", esp_err_to_name(err));
        return;
    }

    mini_games_progress_storage_t loaded = {0};
    size_t length = sizeof(loaded);
    err = nvs_get_blob(handle, kNvsScoresKey, &loaded, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK || length != sizeof(loaded) ||
        loaded.version != kStorageVersion) {
        ESP_LOGW(TAG, "ignore invalid scores blob: err=%s len=%u version=%lu",
                 esp_err_to_name(err), (unsigned int)length,
                 (unsigned long)loaded.version);
        return;
    }

    mini_games_progress_merge_loaded(&loaded);
}

static void mini_games_progress_save(void)
{
    const mini_games_progress_storage_t snapshot =
        mini_games_progress_snapshot();
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open scores for write failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, kNvsScoresKey, &snapshot, sizeof(snapshot));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save scores failed: %s", esp_err_to_name(err));
    }
}

static void mini_games_progress_worker(void *arg)
{
    (void)arg;
    mini_games_progress_load();

    uint8_t save_request;
    for (;;) {
        if (xQueueReceive(s_save_queue, &save_request, portMAX_DELAY) == pdTRUE) {
            mini_games_progress_save();
        }
    }
}

esp_err_t mini_games_progress_start(void)
{
    if (s_worker_task != NULL) {
        return ESP_OK;
    }

    s_save_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_save_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        mini_games_progress_worker, "game_progress", kWorkerStackBytes, NULL,
        2, &s_worker_task, 0);
    if (created != pdPASS) {
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t mini_games_progress_get_high_score(
    mini_games_progress_id_t game_id)
{
    if (game_id < 0 || game_id >= MINI_GAMES_PROGRESS_COUNT) {
        return 0U;
    }

    uint32_t score;
    taskENTER_CRITICAL(&s_progress_lock);
    score = s_progress.high_scores[game_id];
    taskEXIT_CRITICAL(&s_progress_lock);
    return score;
}

void mini_games_progress_submit_high_score(
    mini_games_progress_id_t game_id, uint32_t score)
{
    if (game_id < 0 || game_id >= MINI_GAMES_PROGRESS_COUNT) {
        return;
    }

    bool updated = false;
    taskENTER_CRITICAL(&s_progress_lock);
    if (score > s_progress.high_scores[game_id]) {
        s_progress.high_scores[game_id] = score;
        updated = true;
    }
    taskEXIT_CRITICAL(&s_progress_lock);

    if (updated && s_save_queue != NULL) {
        const uint8_t save_request = 1U;
        (void)xQueueOverwrite(s_save_queue, &save_request);
    }
}
