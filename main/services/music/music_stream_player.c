#include "music_stream_player.h"

#include <string.h>

#include "audio_codec.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_stream_decoder.h"
#include "services/runtime/safety_monitor_policy.h"

static const char *TAG = "music_stream";
static const size_t kHttpReadBytes = 512U;
static const size_t kPcmBytes = 16U * 1024U;
/* 服务端契约限制每个 20 ms Opus 包不超过 1536 B。 */
static const size_t kOpusPacketMaxBytes = 1536U;
/* RAW_OPUS 解码会进入 esp_audio_codec 的较深调用栈；reader 只做 HTTP。 */
static const uint32_t kReaderTaskStackBytes = 8192U;
static const uint32_t kDecoderTaskStackBytes = 16384U;
static const EventBits_t kReaderDoneBit = BIT0;
static const EventBits_t kDecoderDoneBit = BIT1;
static const EventBits_t kAllDoneBits = kReaderDoneBit | kDecoderDoneBit;
static const uint32_t kStopNotifyBit = BIT0;
static const uint32_t kReadyNotifyBit = BIT1;
static const uint32_t kStartNotifyBit = BIT2;

typedef struct
{
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
} music_ring_t;

struct music_stream_player
{
    music_http_client_config_t config;
    char stream_id[MUSIC_SERVICE_STREAM_ID_MAX_BYTES];
    QueueHandle_t event_queue;
    EventGroupHandle_t done_group;
    StaticEventGroup_t done_group_buffer;
    SemaphoreHandle_t state_mutex;
    StaticSemaphore_t state_mutex_buffer;
    TaskHandle_t reader_task;
    TaskHandle_t decoder_task;
    uint8_t *ring_storage;
    uint8_t *pcm_buffer;
    uint8_t *opus_packet;
    music_ring_t ring;
    bool shutdown_requested;
    bool reader_eof;
    esp_err_t reader_error;
    bool playing_started;
    size_t ring_min_bytes;
    size_t ring_max_bytes;
    uint32_t underrun_count;
    uint32_t eagain_count;
    uint64_t max_http_read_us;
    uint64_t max_http_idle_us;
    uint64_t max_pcm_gap_us;
    UBaseType_t reader_stack_hwm;
    UBaseType_t decoder_stack_hwm;
};

static void music_ring_init(music_ring_t *ring, uint8_t *storage,
                            size_t capacity)
{
    ring->data = storage;
    ring->capacity = capacity;
    ring->head = 0U;
    ring->tail = 0U;
    ring->size = 0U;
}

static size_t music_ring_writable_contiguous(const music_ring_t *ring)
{
    const size_t free_bytes = ring->capacity - ring->size;
    if (free_bytes == 0U)
    {
        return 0U;
    }
    const size_t until_end = ring->capacity - ring->tail;
    return free_bytes < until_end ? free_bytes : until_end;
}

static void music_ring_produce(music_ring_t *ring, size_t bytes)
{
    ring->tail = (ring->tail + bytes) % ring->capacity;
    ring->size += bytes;
}

static void music_ring_consume(music_ring_t *ring, size_t bytes)
{
    ring->head = (ring->head + bytes) % ring->capacity;
    ring->size -= bytes;
}

/* 调用方已持有 state_mutex；支持跨 ring 尾部复制一整个 Opus 包。 */
static void music_ring_copy_locked(const music_ring_t *ring, size_t offset,
                                   uint8_t *destination, size_t bytes)
{
    const size_t start = (ring->head + offset) % ring->capacity;
    const size_t first = (ring->capacity - start) < bytes
                             ? (ring->capacity - start)
                             : bytes;
    memcpy(destination, ring->data + start, first);
    if (bytes > first)
    {
        memcpy(destination + first, ring->data, bytes - first);
    }
}

static void music_stream_player_lock(music_stream_player_t *player)
{
    configASSERT(xSemaphoreTake(player->state_mutex, portMAX_DELAY) == pdTRUE);
}

static void music_stream_player_unlock(music_stream_player_t *player)
{
    xSemaphoreGive(player->state_mutex);
}

static void music_stream_player_update_ring_metrics_locked(
    music_stream_player_t *player)
{
    if (!player->playing_started || player->reader_eof ||
        player->reader_error != ESP_OK)
    {
        return;
    }
    if (player->ring.size < player->ring_min_bytes)
    {
        player->ring_min_bytes = player->ring.size;
    }
    if (player->ring.size > player->ring_max_bytes)
    {
        player->ring_max_bytes = player->ring.size;
    }
}

static void music_stream_player_emit(music_stream_player_t *player,
                                     music_stream_player_event_type_t type,
                                     esp_err_t error)
{
    music_stream_player_lock(player);
    QueueHandle_t event_queue = player->event_queue;
    const size_t buffered_bytes = player->ring.size;
    music_stream_player_unlock(player);
    if (event_queue == NULL)
    {
        return;
    }
    const music_stream_player_event_t event = {
        .type = type,
        .error = error,
        .buffered_bytes = buffered_bytes,
    };
    (void)xQueueSend(event_queue, &event, pdMS_TO_TICKS(100));
}

static bool music_stream_player_wait_for_start(music_stream_player_t *player)
{
    while (true)
    {
        music_stream_player_lock(player);
        const bool shutdown_requested = player->shutdown_requested;
        music_stream_player_unlock(player);
        if (shutdown_requested)
        {
            return false;
        }

        uint32_t notification = 0U;
        if (xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY) ==
                pdTRUE &&
            (notification & kStartNotifyBit) != 0U)
        {
            return true;
        }
    }
}

static void music_stream_player_request_shutdown(
    music_stream_player_t *player)
{
    music_stream_player_lock(player);
    player->shutdown_requested = true;
    TaskHandle_t reader_task = player->reader_task;
    TaskHandle_t decoder_task = player->decoder_task;
    music_stream_player_unlock(player);
    if (reader_task != NULL)
    {
        xTaskNotify(reader_task, kStopNotifyBit, eSetBits);
    }
    if (decoder_task != NULL)
    {
        xTaskNotify(decoder_task, kStopNotifyBit, eSetBits);
    }
}

static void music_stream_player_cleanup_audio(bool output_acquired)
{
    if (!output_acquired)
    {
        return;
    }
    (void)audio_codec_flush_output();
    (void)audio_codec_release_output(AUDIO_CODEC_OWNER_MUSIC_PLAYER);
    const esp_err_t policy_ret =
        safety_monitor_policy_set_music_active(false, "music_stopped");
    if (policy_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "music safety policy resume failed: %s",
                 esp_err_to_name(policy_ret));
    }
}

static void music_stream_reader_task(void *arg)
{
    music_stream_player_t *player = (music_stream_player_t *)arg;
    music_http_stream_t *stream = NULL;
    int64_t last_progress_log_us = 0;
    int64_t idle_started_us = 0;
    if (!music_stream_player_wait_for_start(player))
    {
        goto finish;
    }

    esp_err_t open_ret = music_http_client_open_stream(
        &player->config, player->stream_id, &stream);
    if (open_ret != ESP_OK)
    {
        music_stream_player_lock(player);
        if (!player->shutdown_requested)
        {
            player->reader_error = open_ret;
        }
        TaskHandle_t decoder_task = player->decoder_task;
        music_stream_player_unlock(player);
        if (decoder_task != NULL)
        {
            xTaskNotify(decoder_task, kReadyNotifyBit, eSetBits);
        }
        goto finish;
    }

    while (true)
    {
        music_stream_player_lock(player);
        const bool shutdown_requested = player->shutdown_requested;
        const size_t writable =
            music_ring_writable_contiguous(&player->ring);
        uint8_t *destination = player->ring.data + player->ring.tail;
        music_stream_player_unlock(player);
        if (shutdown_requested)
        {
            break;
        }
        if (writable == 0U)
        {
            uint32_t notification = 0U;
            (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                                  portMAX_DELAY);
            continue;
        }

        const size_t read_capacity =
            writable < kHttpReadBytes ? writable : kHttpReadBytes;
        size_t received = 0U;
        const int64_t read_started_us = esp_timer_get_time();
        esp_err_t read_ret = music_http_client_read_stream(
            stream, destination, read_capacity, &received);
        const int64_t read_finished_us = esp_timer_get_time();
        const uint64_t read_duration_us =
            (uint64_t)(read_finished_us - read_started_us);
        if (read_ret == ESP_OK &&
            (received == 0U || received > read_capacity))
        {
            read_ret = received > read_capacity ? ESP_ERR_INVALID_SIZE
                                                : ESP_FAIL;
        }

        music_stream_player_lock(player);
        if (read_duration_us > player->max_http_read_us)
        {
            player->max_http_read_us = read_duration_us;
        }
        uint64_t idle_us = 0U;
        if (read_ret == ESP_ERR_HTTP_EAGAIN)
        {
            if (idle_started_us == 0)
            {
                idle_started_us = read_started_us;
            }
            idle_us = (uint64_t)(read_finished_us - idle_started_us);
            ++player->eagain_count;
        }
        else if (received > 0U && idle_started_us != 0)
        {
            idle_us = (uint64_t)(read_finished_us - idle_started_us);
            idle_started_us = 0;
        }
        if (idle_us > player->max_http_idle_us)
        {
            player->max_http_idle_us = idle_us;
        }
        const bool stopped_during_read = player->shutdown_requested;
        bool reader_finished = false;
        if (!stopped_during_read)
        {
            if (read_ret == ESP_ERR_NOT_FOUND)
            {
                player->reader_eof = true;
                reader_finished = true;
            }
            else if (read_ret == ESP_ERR_HTTP_EAGAIN)
            {
                /* 250 ms 内没有新数据只记录一次等待，保持媒体连接。 */
            }
            else if (read_ret != ESP_OK)
            {
                player->reader_error = read_ret;
                reader_finished = true;
            }
            else
            {
                // 单 producer 先在未提交区直接读，网络调用结束后才发布 size；
                // consumer 因此永远看不到仍在写入的 PSRAM 数据。
                music_ring_produce(&player->ring, received);
                music_stream_player_update_ring_metrics_locked(player);
            }
        }
        const size_t ring_bytes = player->ring.size;
        const uint32_t eagain_count = player->eagain_count;
        const uint64_t max_http_read_us = player->max_http_read_us;
        const uint64_t max_http_idle_us = player->max_http_idle_us;
        TaskHandle_t decoder_task = player->decoder_task;
        music_stream_player_unlock(player);
        const int64_t now_us = esp_timer_get_time();
        const uint64_t current_idle_us =
            idle_started_us == 0 ? 0U : (uint64_t)(now_us - idle_started_us);
        if (last_progress_log_us == 0 ||
            now_us - last_progress_log_us >= 1000LL * 1000LL)
        {
            ESP_LOGI(TAG,
                     "reader ring=%u/%uB http_read=%lluus max=%lluus "
                     "eagain=%lu idle=%lluus idle_max=%lluus",
                     (unsigned)ring_bytes, (unsigned)MUSIC_SERVICE_RING_BYTES,
                     (unsigned long long)read_duration_us,
                     (unsigned long long)max_http_read_us,
                     (unsigned long)eagain_count,
                     (unsigned long long)current_idle_us,
                     (unsigned long long)max_http_idle_us);
            last_progress_log_us = now_us;
        }
        if (decoder_task != NULL)
        {
            xTaskNotify(decoder_task, kReadyNotifyBit, eSetBits);
        }
        if (stopped_during_read || reader_finished)
        {
            break;
        }
    }

finish:
    if (stream != NULL)
    {
        music_http_client_close_stream(stream);
    }
    const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    music_stream_player_lock(player);
    player->reader_stack_hwm = stack_hwm;
    player->reader_task = NULL;
    EventGroupHandle_t done_group = player->done_group;
    music_stream_player_unlock(player);
    xEventGroupSetBits(done_group, kReaderDoneBit);
    vTaskDeleteWithCaps(NULL);
}

static void music_stream_decoder_task(void *arg)
{
    music_stream_player_t *player = (music_stream_player_t *)arg;
    music_stream_decoder_t *decoder = NULL;
    bool output_acquired = false;
    bool task_started = music_stream_player_wait_for_start(player);
    bool stop_requested = !task_started;
    bool underrun_active = false;
    int64_t last_pcm_write_us = 0;
    esp_err_t result = ESP_OK;
    if (!task_started)
    {
        goto finish;
    }

    music_stream_player_emit(player, MUSIC_STREAM_PLAYER_EVENT_BUFFERING,
                             ESP_OK);
    while (true)
    {
        music_stream_player_lock(player);
        const bool shutdown_requested = player->shutdown_requested;
        const bool reader_eof = player->reader_eof;
        const esp_err_t reader_error = player->reader_error;
        const size_t buffered_bytes = player->ring.size;
        music_stream_player_unlock(player);
        if (shutdown_requested)
        {
            stop_requested = true;
            goto finish;
        }
        if (reader_error != ESP_OK)
        {
            result = reader_error;
            goto finish;
        }
        if (buffered_bytes >= MUSIC_SERVICE_START_BUFFER_BYTES || reader_eof)
        {
            break;
        }
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
    }

    music_stream_player_lock(player);
    const bool empty_stream = player->reader_eof && player->ring.size == 0U;
    music_stream_player_unlock(player);
    if (empty_stream)
    {
        goto finish;
    }

    result = music_stream_decoder_open(&decoder);
    if (result != ESP_OK)
    {
        goto finish;
    }
    result = audio_codec_acquire_output(AUDIO_CODEC_OWNER_MUSIC_PLAYER, 0U);
    if (result != ESP_OK)
    {
        goto finish;
    }
    output_acquired = true;
    const esp_err_t policy_ret =
        safety_monitor_policy_set_music_active(true, "music_playing");
    if (policy_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "music safety policy pause failed: %s",
                 esp_err_to_name(policy_ret));
    }

    music_stream_player_lock(player);
    player->playing_started = true;
    player->ring_min_bytes = player->ring.size;
    player->ring_max_bytes = player->ring.size;
    music_stream_player_unlock(player);
    music_stream_player_emit(player, MUSIC_STREAM_PLAYER_EVENT_PLAYING, ESP_OK);

    while (true)
    {
        music_stream_player_lock(player);
        const bool shutdown_requested = player->shutdown_requested;
        const bool reader_eof = player->reader_eof;
        const esp_err_t reader_error = player->reader_error;
        const size_t buffered_bytes = player->ring.size;
        if (shutdown_requested)
        {
            music_stream_player_unlock(player);
            stop_requested = true;
            break;
        }
        if (reader_error != ESP_OK)
        {
            music_stream_player_unlock(player);
            result = reader_error;
            break;
        }
        size_t packet_bytes = 0U;
        TaskHandle_t reader_task = NULL;
        bool packet_ready = false;
        if (buffered_bytes >= 2U)
        {
            const uint8_t packet_length[2] = {
                player->ring.data[player->ring.head],
                player->ring.data[(player->ring.head + 1U) %
                                  player->ring.capacity],
            };
            packet_bytes = ((size_t)packet_length[0] << 8U) |
                           packet_length[1];
            if (packet_bytes == 0U || packet_bytes > kOpusPacketMaxBytes)
            {
                music_stream_player_unlock(player);
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (buffered_bytes >= packet_bytes + 2U)
            {
                music_ring_copy_locked(&player->ring, 2U, player->opus_packet,
                                       packet_bytes);
                music_ring_consume(&player->ring, packet_bytes + 2U);
                music_stream_player_update_ring_metrics_locked(player);
                reader_task = player->reader_task;
                packet_ready = true;
            }
        }
        music_stream_player_unlock(player);
        if (!packet_ready)
        {
            if (reader_eof)
            {
                result = buffered_bytes == 0U ? ESP_OK : ESP_ERR_INVALID_SIZE;
                break;
            }
            if (!underrun_active)
            {
                music_stream_player_lock(player);
                ++player->underrun_count;
                music_stream_player_unlock(player);
                underrun_active = true;
            }
            uint32_t notification = 0U;
            (void)xTaskNotifyWait(0U, UINT32_MAX, &notification,
                                  portMAX_DELAY);
            continue;
        }
        underrun_active = false;
        if (reader_task != NULL)
        {
            xTaskNotify(reader_task, kReadyNotifyBit, eSetBits);
        }

        size_t decoded = 0U;
        const esp_err_t decode_ret = music_stream_decoder_process(
            decoder, player->opus_packet, packet_bytes, player->pcm_buffer,
            kPcmBytes, &decoded);
        if (decoded > 0U)
        {
            const int64_t write_started_us = esp_timer_get_time();
            result = audio_codec_write(player->pcm_buffer, decoded);
            if (result != ESP_OK)
            {
                break;
            }
            if (last_pcm_write_us != 0)
            {
                const uint64_t gap_us =
                    (uint64_t)(write_started_us - last_pcm_write_us);
                music_stream_player_lock(player);
                if (gap_us > player->max_pcm_gap_us)
                {
                    player->max_pcm_gap_us = gap_us;
                }
                music_stream_player_unlock(player);
            }
            last_pcm_write_us = write_started_us;
        }
        if (decode_ret != ESP_OK)
        {
            result = decode_ret;
            break;
        }
    }

finish:
    if (decoder != NULL)
    {
        music_stream_decoder_close(decoder);
    }
    music_stream_player_cleanup_audio(output_acquired);
    music_stream_player_request_shutdown(player);
    music_stream_player_lock(player);
    const bool reader_was_created = player->reader_task != NULL ||
                                    (xEventGroupGetBits(player->done_group) &
                                     kReaderDoneBit) != 0U;
    music_stream_player_unlock(player);
    if (reader_was_created)
    {
        (void)xEventGroupWaitBits(player->done_group, kReaderDoneBit, pdFALSE,
                                  pdTRUE, portMAX_DELAY);
    }

    const UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    music_stream_player_lock(player);
    player->decoder_stack_hwm = stack_hwm;
    player->decoder_task = NULL;
    QueueHandle_t event_queue = player->event_queue;
    EventGroupHandle_t done_group = player->done_group;
    const size_t buffered_bytes = player->ring.size;
    const size_t ring_min_bytes = player->ring_min_bytes;
    const size_t ring_max_bytes = player->ring_max_bytes;
    const uint32_t underrun_count = player->underrun_count;
    const uint32_t eagain_count = player->eagain_count;
    const uint64_t max_http_read_us = player->max_http_read_us;
    const uint64_t max_http_idle_us = player->max_http_idle_us;
    const uint64_t max_pcm_gap_us = player->max_pcm_gap_us;
    const UBaseType_t reader_stack_hwm = player->reader_stack_hwm;
    music_stream_player_unlock(player);

    music_stream_player_event_type_t terminal_type =
        MUSIC_STREAM_PLAYER_EVENT_ENDED;
    if (result != ESP_OK)
    {
        terminal_type = MUSIC_STREAM_PLAYER_EVENT_ERROR;
        ESP_LOGW(TAG, "stream stopped with error: %s", esp_err_to_name(result));
    }
    else if (stop_requested)
    {
        terminal_type = MUSIC_STREAM_PLAYER_EVENT_STOPPED;
    }
    if (task_started)
    {
        ESP_LOGI(TAG,
                 "stats ring_min=%u ring_max=%u underruns=%lu "
                 "eagain=%lu http_read_max=%lluus http_idle_max=%lluus "
                 "pcm_gap_max=%lluus "
                 "reader_stack_hwm=%uB decoder_stack_hwm=%uB",
                 (unsigned)ring_min_bytes, (unsigned)ring_max_bytes,
                 (unsigned long)underrun_count,
                 (unsigned long)eagain_count,
                 (unsigned long long)max_http_read_us,
                 (unsigned long long)max_http_idle_us,
                 (unsigned long long)max_pcm_gap_us,
                 (unsigned)reader_stack_hwm, (unsigned)stack_hwm);
    }

    const music_stream_player_event_t terminal_event = {
        .type = terminal_type,
        .error = result,
        .buffered_bytes = buffered_bytes,
    };
    // done bit 先发布；service 收到终态时即可安全释放 player。此后任务只使用局部副本。
    xEventGroupSetBits(done_group, kDecoderDoneBit);
    if (task_started && event_queue != NULL)
    {
        (void)xQueueSend(event_queue, &terminal_event, pdMS_TO_TICKS(1000));
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t music_stream_player_start(
    const music_http_client_config_t *config, const char *stream_id,
    QueueHandle_t event_queue, music_stream_player_t **out_player)
{
    if (config == NULL || stream_id == NULL || stream_id[0] == '\0' ||
        event_queue == NULL || out_player == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_player = NULL;
    music_stream_player_t *player = heap_caps_calloc(
        1U, sizeof(*player), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (player == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(&player->config, config, sizeof(player->config));
    snprintf(player->stream_id, sizeof(player->stream_id), "%s", stream_id);
    player->event_queue = event_queue;
    player->reader_error = ESP_OK;
    player->ring_storage = heap_caps_malloc(
        MUSIC_SERVICE_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    player->pcm_buffer = heap_caps_malloc(kPcmBytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    player->opus_packet = heap_caps_malloc(
        kOpusPacketMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (player->ring_storage == NULL || player->pcm_buffer == NULL ||
        player->opus_packet == NULL)
    {
        ESP_LOGE(TAG,
                 "PSRAM allocation failed ring=%p pcm=%p opus=%p "
                 "free=%uB largest=%uB",
                 (void *)player->ring_storage, (void *)player->pcm_buffer,
                 (void *)player->opus_packet,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        music_stream_player_release(player);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PSRAM buffers ring=%uB pcm=%uB opus=%uB free=%uB",
             (unsigned)MUSIC_SERVICE_RING_BYTES, (unsigned)kPcmBytes,
             (unsigned)kOpusPacketMaxBytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    music_ring_init(&player->ring, player->ring_storage,
                    MUSIC_SERVICE_RING_BYTES);
    player->state_mutex =
        xSemaphoreCreateMutexStatic(&player->state_mutex_buffer);
    player->done_group = xEventGroupCreateStatic(&player->done_group_buffer);
    if (player->state_mutex == NULL || player->done_group == NULL)
    {
        music_stream_player_release(player);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreateWithCaps(
        music_stream_decoder_task, "music_decoder", kDecoderTaskStackBytes,
        player, 5,
        &player->decoder_task, MALLOC_CAP_SPIRAM);
    if (created != pdPASS)
    {
        player->decoder_task = NULL;
        music_stream_player_release(player);
        return ESP_ERR_NO_MEM;
    }
    created = xTaskCreateWithCaps(
        music_stream_reader_task, "music_reader", kReaderTaskStackBytes, player,
        4,
        &player->reader_task, MALLOC_CAP_SPIRAM);
    if (created != pdPASS)
    {
        player->reader_task = NULL;
        music_stream_player_request_shutdown(player);
        (void)xEventGroupWaitBits(player->done_group, kDecoderDoneBit, pdFALSE,
                                  pdTRUE, portMAX_DELAY);
        music_stream_player_release(player);
        return ESP_ERR_NO_MEM;
    }

    xTaskNotify(player->decoder_task, kStartNotifyBit, eSetBits);
    xTaskNotify(player->reader_task, kStartNotifyBit, eSetBits);
    *out_player = player;
    return ESP_OK;
}

esp_err_t music_stream_player_stop(music_stream_player_t *player,
                                   uint32_t timeout_ms)
{
    if (player == NULL)
    {
        return ESP_OK;
    }
    if (music_stream_player_is_stopped(player))
    {
        return ESP_OK;
    }
    music_stream_player_request_shutdown(player);
    const EventBits_t bits = xEventGroupWaitBits(
        player->done_group, kAllDoneBits, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & kAllDoneBits) == kAllDoneBits ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool music_stream_player_is_stopped(const music_stream_player_t *player)
{
    return player != NULL &&
           (xEventGroupGetBits(player->done_group) & kAllDoneBits) ==
               kAllDoneBits;
}

void music_stream_player_release(music_stream_player_t *player)
{
    if (player == NULL)
    {
        return;
    }
    if ((player->reader_task != NULL || player->decoder_task != NULL) &&
        !music_stream_player_is_stopped(player))
    {
        ESP_LOGW(TAG, "release requested before stream workers stopped");
        return;
    }
    heap_caps_free(player->ring_storage);
    heap_caps_free(player->pcm_buffer);
    heap_caps_free(player->opus_packet);
    heap_caps_free(player);
}
