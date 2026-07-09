#include "traffic_inference.h"

#include "assets/traffic_sample_background.h"
#include "assets/traffic_sample_horn.h"
#include "assets/traffic_sample_siren.h"
#include "esp_err.h"
#include "esp_log.h"
#include "traffic_inference_realtime.h"

static const char *TAG = "traffic_inference";

namespace {

const traffic_inference_demo_sample_t kDemoSamples[] = {
    {
        .samples = kTrafficSampleHorn,
        .sample_count = kTrafficSampleHornCount,
        .sample_rate_hz = kTrafficSampleHornRateHz,
        .expected_label = kTrafficSampleHornExpectedLabel,
        .source_file = kTrafficSampleHornSourceFile,
    },
    {
        .samples = kTrafficSampleSiren,
        .sample_count = kTrafficSampleSirenCount,
        .sample_rate_hz = kTrafficSampleSirenRateHz,
        .expected_label = kTrafficSampleSirenExpectedLabel,
        .source_file = kTrafficSampleSirenSourceFile,
    },
    {
        .samples = kTrafficSampleBackground,
        .sample_count = kTrafficSampleBackgroundCount,
        .sample_rate_hz = kTrafficSampleBackgroundRateHz,
        .expected_label = kTrafficSampleBackgroundExpectedLabel,
        .source_file = kTrafficSampleBackgroundSourceFile,
    },
};

constexpr size_t kDemoSampleCount = sizeof(kDemoSamples) / sizeof(kDemoSamples[0]);

}  // namespace

esp_err_t traffic_inference_run_demo(void) {
  esp_err_t final_status = ESP_OK;

  ESP_LOGI(TAG, "starting traffic inference confidence demo");
  for (size_t idx = 0; idx < kDemoSampleCount; ++idx) {
    esp_err_t ret = traffic_inference_run_single_sample(&kDemoSamples[idx]);
    if (ret != ESP_OK && final_status == ESP_OK) {
      final_status = ret;
    }
  }
  return final_status;
}

esp_err_t traffic_inference_run_sliding_window_demo(void) {
  const traffic_inference_realtime_config_t config = {
      .input_chunk_frames = 0U,
      .read_timeout_ms = 250U,
      .max_read_iterations = 0U,
  };

  ESP_LOGI(TAG, "starting realtime sliding-window traffic inference demo");
  return traffic_inference_run_realtime_sliding_window_demo(&config);
}
