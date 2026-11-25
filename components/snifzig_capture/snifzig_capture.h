#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "esp_attr.h"
#include "esphome/core/component.h"
#include "esp_ieee802154_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace esphome {
namespace snifzig_capture {

static constexpr uint16_t CAPTURE_RING_CAPACITY = 128;
static constexpr uint8_t MAX_MAC_FRAME_LENGTH = 127;
static constexpr size_t MAX_CSV_BUFFER_BYTES =
    CAPTURE_RING_CAPACITY * (MAX_MAC_FRAME_LENGTH * 2 + 96) + 128;

struct CaptureRecord {
  uint32_t sequence;
  uint64_t sfd_timestamp;
  int8_t rssi_dbm;
  uint8_t lqi;
  uint8_t channel;
  uint8_t length;
  uint8_t bytes[MAX_MAC_FRAME_LENGTH];
};

struct CaptureStats {
  uint32_t frames_received;
  uint32_t records_overwritten;
  uint32_t invalid_frames;
  uint32_t rx_release_errors;
  uint32_t rx_restart_errors;
  uint16_t records_in_buffer;
};

class SnifzigCapture : public Component {
 public:
  void set_channel(uint8_t channel) { this->channel_ = channel; }
  void set_ntfy_publish_url(const char *url) { this->ntfy_publish_url_ = url; }
  void set_ntfy_authorization(const char *authorization) { this->ntfy_authorization_ = authorization; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  bool copy_record(uint16_t index, CaptureRecord *out) const;
  CaptureStats get_stats() const;
  void IRAM_ATTR handle_frame(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info);

 private:
  enum class UploadState : uint8_t {
    CAPTURING,
    WAITING_FOR_WIFI,
    WAITING_FOR_WIFI_OFF,
    RETRY_WAIT,
  };

  bool build_csv_(size_t *csv_size);
  bool send_csv_();
  void begin_upload_();
  void finish_upload_(bool success);
  void resume_capture_();

  uint8_t channel_{11};
  uint32_t last_report_ms_{0};
  uint32_t state_started_ms_{0};
  uint32_t retry_due_ms_{0};
  uint32_t batch_number_{0};
  UploadState state_{UploadState::CAPTURING};
  bool upload_succeeded_{false};
  bool status_led_on_{false};
  std::string ntfy_publish_url_;
  std::string ntfy_authorization_;
  CaptureRecord ring_[CAPTURE_RING_CAPACITY]{};
  char csv_buffer_[MAX_CSV_BUFFER_BYTES]{};
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint16_t head_{0};
  uint16_t count_{0};
  uint32_t frames_received_{0};
  uint32_t records_overwritten_{0};
  uint32_t invalid_frames_{0};
  uint32_t rx_release_errors_{0};
  uint32_t rx_restart_errors_{0};
};

}  // namespace snifzig_capture
}
