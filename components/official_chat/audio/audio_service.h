#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_audio_enc.h>
#include <esp_audio_types.h>
#include <esp_timer.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <model_path.h>

#include "esp_ae_rate_cvt.h"

#include "audio/audio_codec_iface.h"
#include "audio/audio_processor.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "audio/wake_word.h"
#include "protocols/protocol.h"

namespace official_chat {

constexpr int kOpusFrameDurationMs = 60;
constexpr int kMaxEncodeTasksInQueue = 2;
constexpr int kMaxPlaybackTasksInQueue = 2;
constexpr int kMaxDecodePacketsInQueue = 2400 / kOpusFrameDurationMs;
constexpr int kMaxSendPacketsInQueue = 2400 / kOpusFrameDurationMs;
constexpr int kAudioTestingMaxDurationMs = 10000;
constexpr int kMaxTimestampsInQueue = 3;
constexpr int kAudioPowerTimeoutMs = 15000;
constexpr int kAudioPowerCheckIntervalMs = 1000;
constexpr configSTACK_DEPTH_TYPE kAudioInputTaskStackBytes = 6144;
constexpr configSTACK_DEPTH_TYPE kAudioOutputTaskStackBytes = 4096;
constexpr configSTACK_DEPTH_TYPE kAudioOpusTaskStackBytes = 24576;

constexpr EventBits_t kAsEventAudioTestingRunning = BIT0;
constexpr EventBits_t kAsEventWakeWordRunning = BIT1;
constexpr EventBits_t kAsEventAudioProcessorRunning = BIT2;
constexpr EventBits_t kAsEventPlaybackNotEmpty = BIT3;
constexpr EventBits_t kAsEventInputTaskExited = BIT4;
constexpr EventBits_t kAsEventOutputTaskExited = BIT5;
constexpr EventBits_t kAsEventOpusTaskExited = BIT6;

#define AS_OPUS_GET_FRAME_DUR_ENUM(duration_ms)                          \
  ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :              \
   (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :            \
   (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :            \
   (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :            \
   (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :            \
   (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :            \
   (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :          \
   (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG()                                             \
  {                                                                      \
    .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,                            \
    .channel = ESP_AUDIO_MONO,                                           \
    .bits_per_sample = ESP_AUDIO_BIT16,                                  \
    .bitrate = ESP_OPUS_BITRATE_AUTO,                                    \
    .frame_duration =                                                     \
        (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DUR_ENUM(       \
            kOpusFrameDurationMs),                                       \
    .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,                  \
    .complexity = 0,                                                     \
    .enable_fec = false,                                                 \
    .enable_dtx = true,                                                  \
    .enable_vbr = true,                                                  \
  }

struct AudioServiceCallbacks {
  std::function<void()> on_send_queue_available;
  std::function<void(const std::string &)> on_wake_word_detected;
  std::function<void(bool)> on_vad_change;
  std::function<void()> on_audio_testing_queue_full;
};

enum AudioTaskType {
  kAudioTaskTypeEncodeToSendQueue,
  kAudioTaskTypeEncodeToTestingQueue,
  kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
  AudioTaskType type;
  std::vector<int16_t> pcm;
  uint32_t timestamp = 0;
};

struct DebugStatistics {
  uint32_t input_count = 0;
  uint32_t decode_count = 0;
  uint32_t encode_count = 0;
  uint32_t playback_count = 0;
  uint32_t decode_queue_push_count = 0;
  uint32_t decode_queue_drop_count = 0;
  uint32_t playback_queue_push_count = 0;
  uint32_t output_write_count = 0;
};

class AudioService {
 public:
  AudioService();
  ~AudioService();

  void Initialize(AudioCodecIface *codec);
  esp_err_t Start();
  void Stop();
  void EncodeWakeWord();
  std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
  const std::string &GetLastWakeWord() const;
  bool IsVoiceDetected() const { return voice_detected_; }
  bool IsIdle();
  bool HasPendingDownlinkPlayback();
  void WaitForPlaybackQueueEmpty();
  bool WaitForDownlinkPlaybackDrain(int quiet_ms, int timeout_ms);
  bool IsWakeWordRunning() const;
  bool IsAudioProcessorRunning() const;
  bool IsAfeWakeWord();

  void EnableWakeWordDetection(bool enable);
  void EnableVoiceProcessing(bool enable);
  void EnableAudioTesting(bool enable);
  void EnableDeviceAec(bool enable);

  void SetCallbacks(AudioServiceCallbacks &callbacks);

  bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet,
                               bool wait = false);
  std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
  void PlaySound(const std::string_view &sound);
  bool ReadAudioData(std::vector<int16_t> &data, int sample_rate, int samples);
  void ResetDecoder();
  void SetModelsList(srmodel_list_t *models_list);

 private:
  AudioCodecIface *codec_ = nullptr;
  AudioServiceCallbacks callbacks_;
  std::unique_ptr<AudioProcessor> audio_processor_;
  std::unique_ptr<WakeWord> wake_word_;
  void *opus_encoder_ = nullptr;
  void *opus_decoder_ = nullptr;
  std::mutex decoder_mutex_;
  std::mutex input_resampler_mutex_;
  esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
  esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;

  int encoder_sample_rate_ = 16000;
  int encoder_duration_ms_ = kOpusFrameDurationMs;
  int encoder_frame_size_ = 0;
  int encoder_outbuf_size_ = 0;
  int decoder_sample_rate_ = 0;
  int decoder_duration_ms_ = kOpusFrameDurationMs;
  int decoder_frame_size_ = 0;
  DebugStatistics debug_statistics_;
  srmodel_list_t *models_list_ = nullptr;

  EventGroupHandle_t event_group_ = nullptr;
  TaskHandle_t audio_input_task_handle_ = nullptr;
  TaskHandle_t audio_output_task_handle_ = nullptr;
  TaskHandle_t opus_codec_task_handle_ = nullptr;
  std::mutex audio_queue_mutex_;
  std::condition_variable audio_queue_cv_;
  std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
  std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
  std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
  std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
  std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
  std::deque<uint32_t> timestamp_queue_;

  bool wake_word_initialized_ = false;
  bool audio_processor_initialized_ = false;
  bool voice_detected_ = false;
  bool service_stopped_ = true;
  bool audio_input_need_warmup_ = false;
  bool started_ = false;

  esp_timer_handle_t audio_power_timer_ = nullptr;
  std::chrono::steady_clock::time_point last_input_time_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_output_time_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_decode_enqueue_time_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_playback_enqueue_time_ =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_statistics_log_time_ =
      std::chrono::steady_clock::now();
  int input_read_log_budget_ = 3;
  int encode_log_budget_ = 3;
  int send_queue_log_budget_ = 3;
  int decode_drop_log_budget_ = 5;

  void AudioInputTask();
  void AudioOutputTask();
  void OpusCodecTask();
  void EnsureAudioProcessorCreated();
  void BindAudioProcessorCallbacks();
  void BindWakeWordCallbacks();
  void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t> &&pcm);
  void SetDecodeSampleRate(int sample_rate, int frame_duration);
  void CheckAndUpdateAudioPowerState();
  void MaybeLogQueueStats();
  void StopSpeechFeatures();

  bool wake_word_is_afe_ = false;
};

}  // namespace official_chat
