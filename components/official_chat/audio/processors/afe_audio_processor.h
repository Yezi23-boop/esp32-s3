#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>

#include "audio/audio_processor.h"

namespace official_chat {

class AfeAudioProcessor : public AudioProcessor {
 public:
  AfeAudioProcessor();
  ~AfeAudioProcessor() override;

  void Initialize(AudioCodecIface *codec, int frame_duration_ms,
                  srmodel_list_t *models_list) override;
  void Feed(std::vector<int16_t> &&data) override;
  void Start() override;
  void Stop() override;
  bool IsRunning() override;
  void OnOutput(std::function<void(std::vector<int16_t> &&data)> callback) override;
  void OnVadStateChange(std::function<void(bool speaking)> callback) override;
  size_t GetFeedSize() override;
  void EnableDeviceAec(bool enable) override;

 private:
  void AudioProcessorTask();
  void EmitFallbackFrames(std::vector<int16_t> &&data);

  EventGroupHandle_t event_group_ = nullptr;
  TaskHandle_t worker_task_handle_ = nullptr;
  const esp_afe_sr_iface_t *afe_iface_ = nullptr;
  esp_afe_sr_data_t *afe_data_ = nullptr;
  AudioCodecIface *codec_ = nullptr;
  std::function<void(std::vector<int16_t> &&data)> output_callback_;
  std::function<void(bool speaking)> vad_state_change_callback_;
  std::vector<int16_t> input_buffer_;
  std::mutex input_buffer_mutex_;
  std::vector<int16_t> output_buffer_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> worker_stop_requested_{false};
  bool is_speaking_ = false;
  bool device_aec_enabled_ = false;
  int frame_samples_ = 0;
  bool owns_models_ = false;
  srmodel_list_t *owned_models_ = nullptr;
};

}  // namespace official_chat
