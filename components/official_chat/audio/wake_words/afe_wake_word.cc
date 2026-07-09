#include "audio/wake_words/afe_wake_word.h"

#include "sdkconfig.h"

#include <cstring>
#include <sstream>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>

#include "audio/audio_service.h"
#include "audio/input_format_utils.h"

namespace official_chat {

namespace {

constexpr char kTag[] = "official_afe_ww";
constexpr TickType_t kDetectionStopTimeoutTicks = pdMS_TO_TICKS(1000);
constexpr TickType_t kDetectionFetchTimeoutTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kEncodeStopTimeoutTicks = pdMS_TO_TICKS(1000);
constexpr int kWakeWordCacheMs = 2000;
constexpr int kWakeWordDetectFrameMs = 30;
constexpr size_t kWakeWordEncodeTaskStackBytes = 4096 * 6;

void LogWakeWordHeapState(const char *stage) {
  const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t spiram_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  ESP_LOGI(kTag,
           "wake word heap (%s): internal_free=%u internal_largest=%u "
           "spiram_free=%u spiram_largest=%u",
           stage != nullptr ? stage : "unknown",
           static_cast<unsigned>(internal_free),
           static_cast<unsigned>(internal_largest),
           static_cast<unsigned>(spiram_free),
           static_cast<unsigned>(spiram_largest));
}

void LogEncodeTaskStackHighWater(const char *stage) {
  const auto high_water = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(kTag, "wake word encode stack high watermark (%s): %lu",
           stage != nullptr ? stage : "unknown",
           static_cast<unsigned long>(high_water));
}

}  // namespace

AfeWakeWord::AfeWakeWord() = default;

AfeWakeWord::~AfeWakeWord() {
  detection_stop_requested_ = true;
  running_ = false;
  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->reset_buffer(afe_data_);
  }

  WaitForDetectionTaskExit();
  WaitForEncodeTaskExit();

  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->destroy(afe_data_);
    afe_data_ = nullptr;
    afe_iface_ = nullptr;
  }
  if (wake_word_encode_task_ == nullptr && wake_word_encode_task_stack_ != nullptr) {
    heap_caps_free(wake_word_encode_task_stack_);
    wake_word_encode_task_stack_ = nullptr;
  }
  if (wake_word_encode_task_ == nullptr && wake_word_encode_task_buffer_ != nullptr) {
    heap_caps_free(wake_word_encode_task_buffer_);
    wake_word_encode_task_buffer_ = nullptr;
  }
  if (owns_models_ && models_ != nullptr) {
    esp_srmodel_deinit(models_);
    models_ = nullptr;
  }
}

bool AfeWakeWord::Initialize(AudioCodecIface *codec, srmodel_list_t *models_list) {
  codec_ = codec;
  owns_models_ = false;
  input_buffer_.clear();
  wake_words_.clear();
  last_detected_wake_word_.clear();
  wakenet_model_ = nullptr;
  {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_pcm_.clear();
    wake_word_opus_.clear();
  }

  if (codec_ == nullptr) {
    ESP_LOGE(kTag, "audio codec adapter is null");
    return false;
  }
  if (initialized_) {
    return true;
  }

  models_ = models_list;
  if (models_ == nullptr) {
    models_ = esp_srmodel_init("model");
    owns_models_ = true;
  }
  if (models_ == nullptr || models_->num == -1) {
    ESP_LOGE(kTag, "failed to initialize wake word models");
    return false;
  }

  for (int i = 0; i < models_->num; ++i) {
    if (::strstr(models_->model_name[i], ESP_WN_PREFIX) != nullptr) {
      wakenet_model_ = models_->model_name[i];
      std::stringstream ss(
          esp_srmodel_get_wake_words(models_, wakenet_model_));
      std::string wake_word;
      while (std::getline(ss, wake_word, ';')) {
        wake_words_.push_back(wake_word);
      }
      break;
    }
  }
  if (wakenet_model_ == nullptr || wake_words_.empty()) {
    ESP_LOGW(kTag, "no wake word model found");
    return false;
  }

  const char *input_format = codec_->input_format();
  if (input_format == nullptr || input_format[0] == '\0') {
    ESP_LOGE(kTag, "input format is empty");
    return false;
  }

  LogWakeWordHeapState("before-afe-config");
  afe_config_t *afe_config = afe_config_init(input_format, models_,
                                             AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
  if (afe_config == nullptr) {
    ESP_LOGE(kTag, "failed to create wake word AFE config");
    return false;
  }
  afe_config->aec_init = codec_->input_reference();
  afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
  afe_config->afe_perferred_core = 1;
  afe_config->afe_perferred_priority = 1;
  afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

  afe_iface_ = esp_afe_handle_from_config(afe_config);
  if (afe_iface_ == nullptr) {
    ESP_LOGE(kTag, "failed to get wake word AFE interface");
    return false;
  }
  afe_data_ = afe_iface_->create_from_config(afe_config);
  if (afe_data_ == nullptr) {
    ESP_LOGE(kTag, "failed to create wake word AFE handle");
    afe_iface_ = nullptr;
    return false;
  }

  detection_stop_requested_ = false;
  if (detection_task_handle_ == nullptr) {
    LogWakeWordHeapState("before-detection-task");
    const BaseType_t created = xTaskCreate(
        [](void *arg) {
          auto *self = static_cast<AfeWakeWord *>(arg);
          self->AudioDetectionTask();
          self->detection_task_handle_ = nullptr;
          vTaskDelete(nullptr);
        },
        "afe_wake", 4096, this, 3, &detection_task_handle_);
    if (created != pdPASS) {
      ESP_LOGE(kTag, "failed to create wake word detection task");
      LogWakeWordHeapState("detection-task-create-failed");
      afe_iface_->destroy(afe_data_);
      afe_data_ = nullptr;
      afe_iface_ = nullptr;
      return false;
    }
  }

  initialized_ = true;
  return true;
}

void AfeWakeWord::Feed(const std::vector<int16_t> &data) {
  if (!running_ || afe_data_ == nullptr || afe_iface_ == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(input_buffer_mutex_);
  if (!running_) {
    return;
  }

  input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
  const size_t chunk_size =
      afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
  while (input_buffer_.size() >= chunk_size) {
    afe_iface_->feed(afe_data_, input_buffer_.data());
    input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
  }
}

void AfeWakeWord::OnWakeWordDetected(
    std::function<void(const std::string &wake_word)> callback) {
  wake_word_detected_callback_ = std::move(callback);
}

void AfeWakeWord::Start() { running_ = true; }

void AfeWakeWord::Stop() {
  running_ = false;
  if (afe_data_ != nullptr && afe_iface_ != nullptr) {
    afe_iface_->reset_buffer(afe_data_);
  }
  std::lock_guard<std::mutex> lock(input_buffer_mutex_);
  input_buffer_.clear();
}

size_t AfeWakeWord::GetFeedSize() {
  if (afe_data_ == nullptr || afe_iface_ == nullptr) {
    return 0;
  }
  return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::EncodeWakeWordData() {
  FinalizeFinishedEncodeTask();

  {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_opus_.clear();
    if (wake_word_pcm_.empty()) {
      PushWakeWordSentinelLocked();
      return;
    }
  }

  if (wake_word_encode_task_ != nullptr) {
    ESP_LOGW(kTag, "wake word encode task is still running");
    NotifyWakeWordEncodingFailure();
    return;
  }

  if (wake_word_encode_task_stack_ == nullptr) {
    wake_word_encode_task_stack_ = static_cast<StackType_t *>(
        heap_caps_malloc(kWakeWordEncodeTaskStackBytes, MALLOC_CAP_SPIRAM));
  }
  if (wake_word_encode_task_buffer_ == nullptr) {
    wake_word_encode_task_buffer_ = static_cast<StaticTask_t *>(
        heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
  }
  if (wake_word_encode_task_stack_ == nullptr ||
      wake_word_encode_task_buffer_ == nullptr) {
    ESP_LOGE(kTag, "failed to allocate wake word encode task resources");
    NotifyWakeWordEncodingFailure();
    return;
  }

  encode_task_finished_ = false;
  wake_word_encode_task_ = xTaskCreateStatic(
      [](void *arg) {
        auto *self = static_cast<AfeWakeWord *>(arg);
        const auto start_time = esp_timer_get_time();
        LogEncodeTaskStackHighWater("startup");

        esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
        void *encoder_handle = nullptr;
        auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t),
                                     &encoder_handle);
        if (encoder_handle == nullptr) {
          ESP_LOGE(kTag, "failed to create wake word encoder: %d", ret);
          self->NotifyWakeWordEncodingFailure();
          self->encode_task_finished_ = true;
          vTaskDelete(nullptr);
          return;
        }

        int frame_size = 0;
        int outbuf_size = 0;
        esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
        frame_size = frame_size / static_cast<int>(sizeof(int16_t));

        std::deque<std::vector<int16_t>> wake_word_pcm;
        {
          std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
          wake_word_pcm = self->wake_word_pcm_;
          self->wake_word_pcm_.clear();
        }

        int packets = 0;
        std::vector<int16_t> input_frames;
        for (const auto &pcm : wake_word_pcm) {
          input_frames.insert(input_frames.end(), pcm.begin(), pcm.end());
          while (input_frames.size() >= static_cast<size_t>(frame_size)) {
            std::vector<uint8_t> opus_buf(outbuf_size);
            esp_audio_enc_in_frame_t in = {
                .buffer = reinterpret_cast<uint8_t *>(input_frames.data()),
                .len = static_cast<uint32_t>(frame_size * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = opus_buf.data(),
                .len = static_cast<uint32_t>(outbuf_size),
                .encoded_bytes = 0,
                .pts = 0,
            };
            ret = esp_opus_enc_process(encoder_handle, &in, &out);
            if (ret == ESP_AUDIO_ERR_OK) {
              std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
              self->wake_word_opus_.emplace_back(opus_buf.data(),
                                                 opus_buf.data() + out.encoded_bytes);
              self->wake_word_cv_.notify_all();
              packets++;
            } else {
              ESP_LOGE(kTag, "failed to encode wake word audio: %d", ret);
            }

            input_frames.erase(input_frames.begin(),
                               input_frames.begin() + frame_size);
          }
        }

        esp_opus_enc_close(encoder_handle);
        const auto end_time = esp_timer_get_time();
        ESP_LOGI(kTag, "encode wake word opus %d packets in %ld ms", packets,
                 static_cast<long>((end_time - start_time) / 1000));
        LogEncodeTaskStackHighWater("complete");

        {
          std::lock_guard<std::mutex> lock(self->wake_word_mutex_);
          self->PushWakeWordSentinelLocked();
        }
        self->encode_task_finished_ = true;
        vTaskDelete(nullptr);
      },
      "encode_wake_word", kWakeWordEncodeTaskStackBytes, this, 2,
      wake_word_encode_task_stack_, wake_word_encode_task_buffer_);

  if (wake_word_encode_task_ == nullptr) {
    ESP_LOGE(kTag, "failed to create wake word encode task");
    NotifyWakeWordEncodingFailure();
    return;
  }
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t> &opus) {
  std::unique_lock<std::mutex> lock(wake_word_mutex_);
  wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
  opus = std::move(wake_word_opus_.front());
  wake_word_opus_.pop_front();
  return !opus.empty();
}

const std::string &AfeWakeWord::GetLastDetectedWakeWord() const {
  return last_detected_wake_word_;
}

void AfeWakeWord::AudioDetectionTask() {
  while (!detection_stop_requested_) {
    if (!running_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (afe_data_ == nullptr || afe_iface_ == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // 退出 AI 页时析构会等待检测任务收敛，不能让 AFE fetch 永久阻塞。
    auto *res =
        afe_iface_->fetch_with_delay(afe_data_, kDetectionFetchTimeoutTicks);
    if (detection_stop_requested_) {
      break;
    }
    if (!running_) {
      continue;
    }
    if (res == nullptr || res->ret_value == ESP_FAIL) {
      continue;
    }

    StoreWakeWordData(res->data,
                      static_cast<size_t>(res->data_size / sizeof(int16_t)));
    if (res->wakeup_state != WAKENET_DETECTED) {
      continue;
    }

    Stop();
    const int index = static_cast<int>(res->wakenet_model_index) - 1;
    if (index >= 0 && index < static_cast<int>(wake_words_.size())) {
      last_detected_wake_word_ = wake_words_[index];
    } else if (!wake_words_.empty()) {
      last_detected_wake_word_ = wake_words_.front();
    } else {
      last_detected_wake_word_.clear();
    }
    ESP_LOGI(kTag, "wake word detected: %s", last_detected_wake_word_.c_str());
    if (wake_word_detected_callback_) {
      wake_word_detected_callback_(last_detected_wake_word_);
    }
  }
}

void AfeWakeWord::FinalizeFinishedEncodeTask() {
  if (wake_word_encode_task_ == nullptr || !encode_task_finished_) {
    return;
  }
  if (eTaskGetState(wake_word_encode_task_) == eDeleted) {
    wake_word_encode_task_ = nullptr;
    encode_task_finished_ = false;
  }
}

void AfeWakeWord::NotifyWakeWordEncodingFailure() {
  std::lock_guard<std::mutex> lock(wake_word_mutex_);
  wake_word_opus_.clear();
  PushWakeWordSentinelLocked();
}

void AfeWakeWord::PushWakeWordSentinelLocked() {
  wake_word_opus_.push_back(std::vector<uint8_t>());
  wake_word_cv_.notify_all();
}

void AfeWakeWord::WaitForDetectionTaskExit() {
  if (detection_task_handle_ == nullptr) {
    return;
  }
  const TickType_t deadline = xTaskGetTickCount() + kDetectionStopTimeoutTicks;
  while (detection_task_handle_ != nullptr && xTaskGetTickCount() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (detection_task_handle_ != nullptr) {
    ESP_LOGW(kTag, "wake word detection task did not exit before destruction");
  }
}

void AfeWakeWord::WaitForEncodeTaskExit() {
  const TickType_t deadline = xTaskGetTickCount() + kEncodeStopTimeoutTicks;
  while (wake_word_encode_task_ != nullptr && xTaskGetTickCount() < deadline) {
    FinalizeFinishedEncodeTask();
    if (wake_word_encode_task_ != nullptr) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  FinalizeFinishedEncodeTask();
  if (wake_word_encode_task_ != nullptr) {
    ESP_LOGW(kTag, "wake word encode task did not exit before destruction");
  }
}

void AfeWakeWord::StoreWakeWordData(const int16_t *data, size_t samples) {
  if (data == nullptr || samples == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(wake_word_mutex_);
  wake_word_pcm_.emplace_back(data, data + samples);
  while (wake_word_pcm_.size() > kWakeWordCacheMs / kWakeWordDetectFrameMs) {
    wake_word_pcm_.pop_front();
  }
}

}  // namespace official_chat
