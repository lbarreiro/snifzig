#pragma once
#include <stdint.h>
#include <string>
#include "esp_attr.h"
#include "esphome/core/component.h"
#include "esp_ieee802154_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
namespace esphome { namespace snifzig_capture {
static constexpr uint32_t STATS_INTERVAL_MS=3600000;
static constexpr uint8_t MAX_TRACKED_DEVICES=64;
struct DeviceMetric {
  uint64_t address;
  uint8_t address_mode;
  uint32_t frames;
  int32_t rssi_sum;
  uint32_t lqi_sum;
};
struct CaptureStats {
  uint32_t frames_received, unidentified_frames, invalid_frames, rx_release_errors, rx_restart_errors;
  uint32_t frame_types[5];
  uint32_t rssi_buckets[4], lqi_buckets[4];
  int32_t rssi_sum; uint32_t lqi_sum;
  int8_t rssi_min, rssi_max; uint8_t lqi_min, lqi_max;
  uint8_t device_count;
  DeviceMetric devices[MAX_TRACKED_DEVICES];
  DeviceMetric unidentified;
};
class SnifzigCapture : public Component {
 public:
  void set_channel(uint8_t channel){this->channel_=channel;}
  void set_ntfy_publish_url(const char *url){this->ntfy_publish_url_=url;}
  void set_ntfy_authorization(const char *authorization){this->ntfy_authorization_=authorization;}
  void setup() override; void loop() override;
  float get_setup_priority() const override{return setup_priority::DATA;}
  CaptureStats get_stats() const;
  void IRAM_ATTR handle_frame(uint8_t *frame,esp_ieee802154_frame_info_t *frame_info);
 private:
  enum class UploadState:uint8_t { BOOT_WAIT_WIFI, CAPTURING, WAITING_FOR_WIFI, WAITING_FOR_WIFI_OFF, RETRY_WAIT, BOOT_WIFI_OFF, OTA_MAINTENANCE, OTA_WIFI_OFF };
  bool send_report_(); void clear_stats_(); void begin_upload_(); void begin_ota_maintenance_();
  void end_ota_maintenance_(); void finish_upload_(bool success); void resume_capture_(); void start_capture_();
  uint8_t channel_{11}; uint32_t last_report_ms_{0},state_started_ms_{0},retry_due_ms_{0},report_number_{1};
  UploadState state_{UploadState::BOOT_WAIT_WIFI}; bool upload_succeeded_{false},status_led_on_{false},ota_button_held_{false};
  std::string ntfy_publish_url_,ntfy_authorization_;
  mutable portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
  uint32_t frames_received_{0},unidentified_frames_{0},invalid_frames_{0},rx_release_errors_{0},rx_restart_errors_{0};
  uint32_t frame_types_[5]{},rssi_buckets_[4]{},lqi_buckets_[4]{};
  int32_t rssi_sum_{0}; uint32_t lqi_sum_{0}; int8_t rssi_min_{127},rssi_max_{-128}; uint8_t lqi_min_{255},lqi_max_{0};
  uint8_t device_count_{0}; DeviceMetric devices_[MAX_TRACKED_DEVICES]{}; DeviceMetric unidentified_{};
};
} }
