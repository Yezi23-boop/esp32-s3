#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/wake_word.h"

namespace official_chat {

class AfeWakeWord : public WakeWord {
 public:
  AfeWakeWord();
  ~AfeWakeWord() override;

  bool Initialize(AudioCodecIface *codec, srmodel_list_t *models_list) override;
  void Feed(const std::vector<int16_t> &data) override;
  void OnWakeWordDetected(
      std::function<void(const std::string &wake_word)> callback) override;
  void Start() override;
  void Stop() override;
  size_t GetFeedSize() override;
  void EncodeWakeWordData() override;
  bool GetWakeWordOpus(std::vector<uint8_t> &opus) override;
  const std::string &GetLastDetectedWakeWord() const override;

 private:
  void AudioDetectionTask();
  void FinalizeFinishedEncodeTask();
  void NotifyWakeWordEncodingFailure();
  void PushWakeWordSentinelLocked();
  void WaitForDetectionTaskExit();
  void WaitForEncodeTaskExit();
  void StoreWakeWordData(const int16_t *data, size_t samples);

  srmodel_list_t *models_ = nullptr;
  const esp_afe_sr_iface_t *afe_iface_ = nullptr;
  esp_afe_sr_data_t *afe_data_ = nullptr;
  char *wakenet_model_ = nullptr;
  std::vector<std::string> wake_words_;
  std::function<void(const std::string &wake_word)> wake_word_detected_callback_;
  AudioCodecIface *codec_ = nullptr;
  std::string last_detected_wake_word_;
  std::vector<int16_t> input_buffer_;
  std::mutex input_buffer_mutex_;
  std::deque<std::vector<int16_t>> wake_word_pcm_;
  std::deque<std::vector<uint8_t>> wake_word_opus_;
  std::mutex wake_word_mutex_;
  std::condition_variable wake_word_cv_;
  TaskHandle_t detection_task_handle_ = nullptr;
  TaskHandle_t wake_word_encode_task_ = nullptr;
  StaticTask_t *wake_word_encode_task_buffer_ = nullptr;
  StackType_t *wake_word_encode_task_stack_ = nullptr;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> detection_stop_requested_{false};
  std::atomic<bool> encode_task_finished_{false};
  bool owns_models_ = false;
};

}  // namespace official_chat
