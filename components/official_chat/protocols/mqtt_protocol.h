#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <esp_timer.h>
#include <mbedtls/aes.h>

#include "protocol_config.h"
#include "protocols/protocol.h"

namespace official_chat {

constexpr EventBits_t MQTT_PROTOCOL_CONNECTED_EVENT = BIT0;
constexpr EventBits_t MQTT_PROTOCOL_SERVER_HELLO_EVENT = BIT1;
constexpr EventBits_t MQTT_PROTOCOL_DISCONNECTED_EVENT = BIT2;
constexpr EventBits_t MQTT_PROTOCOL_RECEIVE_TASK_EXIT_EVENT = BIT3;

class MqttProtocol : public Protocol {
 public:
  explicit MqttProtocol(MqttRuntimeConfig config);
  ~MqttProtocol() override;

  bool Start() override;
  bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
  bool OpenAudioChannel() override;
  void CloseAudioChannel(bool send_goodbye = true) override;
  bool IsAudioChannelOpened() const override;

 private:
  static void MqttEventHandler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
  static void ReconnectTimerCallback(void *arg);
  static void ReceiveTask(void *arg);

  bool StartMqttClient(bool report_error, bool wait_for_connection);
  void StopMqttClientLocked();
  void StopMqttClient();
  void HandleMqttEvent(esp_mqtt_event_handle_t event);
  bool SendText(const std::string &text) override;
  std::string GetHelloMessage();
  void ParseServerHello(const cJSON *root);
  bool ConnectUdpSocket();
  void DisconnectUdpSocket();
  void RunReceiveLoop();
  void ResetUdpReceiveStatistics();
  void LogUdpReceiveStatistics(const char *reason);
  void ResetUdpAudioStallTracking(bool audio_active);
  bool HasUdpAudioTimedOut(int64_t now_us) const;
  void ScheduleReconnect();
  void CancelReconnect();
  void RecordOutboundPayload(const std::string &payload);
  bool ShouldIgnoreLoopbackPayload(const std::string &payload);
  std::string DecodeHexString(const std::string &hex_string) const;

  struct UdpReceiveStatistics {
    uint32_t packet_count = 0;
    uint32_t old_count = 0;
    uint32_t gap_count = 0;
    uint32_t max_gap = 0;
    uint32_t last_expected_sequence = 0;
    uint32_t last_received_sequence = 0;
    uint32_t timeout_count = 0;
    uint32_t error_count = 0;
  };

  MqttRuntimeConfig config_;
  EventGroupHandle_t event_group_handle_ = nullptr;
  esp_mqtt_client_handle_t mqtt_client_handle_ = nullptr;
  esp_timer_handle_t reconnect_timer_ = nullptr;
  TaskHandle_t receive_task_handle_ = nullptr;
  mutable std::mutex mqtt_mutex_;
  mutable std::mutex channel_mutex_;
  mutable std::mutex outbound_mutex_;
  std::deque<std::string> recent_outbound_payloads_;
  std::string incoming_topic_;
  std::string incoming_payload_;
  std::string publish_topic_;
  std::string udp_server_;
  std::string aes_nonce_;
  int udp_port_ = 0;
  int udp_socket_ = -1;
  uint32_t local_sequence_ = 0;
  uint32_t remote_sequence_ = 0;
  UdpReceiveStatistics udp_receive_stats_;
  mbedtls_aes_context aes_ctx_;
  std::atomic<bool> alive_{true};
  std::atomic<bool> mqtt_connected_{false};
  std::atomic<bool> udp_connected_{false};
  std::atomic<bool> manual_mqtt_stop_{false};
  std::atomic<bool> closing_udp_{false};
  std::atomic<bool> tts_downlink_active_{false};
  std::atomic<bool> udp_audio_stall_detected_{false};
  std::atomic<int64_t> last_udp_audio_time_us_{0};
  int udp_gap_log_budget_ = 6;
  int udp_old_log_budget_ = 4;
};

}  // namespace official_chat
