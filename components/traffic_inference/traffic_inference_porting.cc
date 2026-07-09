#include "esp_log.h"

static const char *TAG = "traffic_inference";

extern "C" void traffic_inference_porting_log_ready(void) {
  ESP_LOGD(TAG, "traffic inference porting scaffold ready");
}
