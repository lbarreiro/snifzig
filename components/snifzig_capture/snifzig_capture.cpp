#include "snifzig_capture.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_ieee802154.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace snifzig_capture {

static const char *const TAG = "snifzig_capture";
static SnifzigCapture *s_instance = nullptr;

// XIAO ESP32-C6 RF switch. GPIO3 LOW enables antenna selection;
// GPIO14 LOW selects the onboard ceramic antenna.
static constexpr gpio_num_t RF_SWITCH_ENABLE_GPIO = GPIO_NUM_3;
static constexpr gpio_num_t RF_ANTENNA_SELECT_GPIO = GPIO_NUM_14;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 45000;
static constexpr uint32_t WIFI_SHUTDOWN_TIMEOUT_MS = 10000;
static constexpr uint32_t UPLOAD_RETRY_DELAY_MS = 300000;
static constexpr gpio_num_t STATUS_LED_GPIO = GPIO_NUM_15;
static constexpr gpio_num_t OTA_BUTTON_GPIO = GPIO_NUM_9;
static constexpr uint32_t OTA_BUTTON_HOLD_MS = 3000;
static constexpr uint32_t OTA_MAINTENANCE_WINDOW_MS = 600000;
static constexpr uint32_t STATUS_LED_PERIOD_MS = 1000;
static constexpr uint32_t STATUS_LED_PULSE_MS = 120;
static constexpr uint32_t CSV_HTTP_TIMEOUT_MS = 20000;

void SnifzigCapture::setup() {
  s_instance = this;

  gpio_config_t antenna_config{};
  antenna_config.pin_bit_mask =
      (1ULL << RF_SWITCH_ENABLE_GPIO) | (1ULL << RF_ANTENNA_SELECT_GPIO);
  antenna_config.mode = GPIO_MODE_OUTPUT;
  antenna_config.pull_up_en = GPIO_PULLUP_DISABLE;
  antenna_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  antenna_config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&antenna_config));
  ESP_ERROR_CHECK(gpio_set_level(RF_SWITCH_ENABLE_GPIO, 0));
  ESP_ERROR_CHECK(gpio_set_level(RF_ANTENNA_SELECT_GPIO, 0));

  gpio_config_t status_led_config{};
  status_led_config.pin_bit_mask = 1ULL << STATUS_LED_GPIO;
  status_led_config.mode = GPIO_MODE_OUTPUT;
  status_led_config.pull_up_en = GPIO_PULLUP_DISABLE;
  status_led_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  status_led_config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&status_led_config));
  // The XIAO ESP32-C6 user LED is active-low; start with it off.
  ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO, 1));

  gpio_config_t ota_button_config{};
  ota_button_config.pin_bit_mask = 1ULL << OTA_BUTTON_GPIO;
  ota_button_config.mode = GPIO_MODE_INPUT;
  ota_button_config.pull_up_en = GPIO_PULLUP_ENABLE;
  ota_button_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ota_button_config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&ota_button_config));

  ESP_ERROR_CHECK(esp_ieee802154_enable());
  ESP_ERROR_CHECK(esp_ieee802154_set_channel(this->channel_));
  ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_ieee802154_set_coordinator(false));
  ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
  ESP_ERROR_CHECK(esp_ieee802154_receive());

  ESP_LOGI(TAG, "Passive capture started on Zigbee channel %u", this->channel_);
  ESP_LOGI(TAG, "Wi-Fi stays off during capture; hold BOOT for 3 seconds for OTA maintenance");
  ESP_LOGI(TAG, "RAM ring capacity: %u frames", CAPTURE_RING_CAPACITY);
}

void SnifzigCapture::loop() {
  const uint32_t now = millis();

  // Short heartbeat pulse only while the 802.15.4 receiver is capturing.
  const bool wifi_connected_for_ota = this->state_ == UploadState::OTA_MAINTENANCE &&
      wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
  const bool led_should_be_on = wifi_connected_for_ota ||
      (this->state_ == UploadState::CAPTURING &&
       (now % STATUS_LED_PERIOD_MS) < STATUS_LED_PULSE_MS);
  if (led_should_be_on != this->status_led_on_) {
    gpio_set_level(STATUS_LED_GPIO, led_should_be_on ? 0 : 1);
    this->status_led_on_ = led_should_be_on;
  }

  if (this->state_ == UploadState::CAPTURING) {
    const bool ota_button_pressed = gpio_get_level(OTA_BUTTON_GPIO) == 0;
    if (ota_button_pressed) {
      if (!this->ota_button_held_) {
        this->ota_button_held_ = true;
        this->ota_button_started_ms_ = now;
      } else if (now - this->ota_button_started_ms_ >= OTA_BUTTON_HOLD_MS) {
        this->ota_button_held_ = false;
        this->begin_ota_maintenance_();
        return;
      }
    } else {
      this->ota_button_held_ = false;
    }

    // Let a deliberate long press enter OTA mode even if the capture buffer fills.
    if (!ota_button_pressed && this->get_stats().records_in_buffer >= CAPTURE_RING_CAPACITY) {
      this->begin_upload_();
    }

    if (now - this->last_report_ms_ >= 30000) {
      this->last_report_ms_ = now;
      const CaptureStats stats = this->get_stats();
      ESP_LOGI(TAG,
               "frames=%u buffered=%u/%u overwritten=%u invalid=%u release_errors=%u restart_errors=%u",
               static_cast<unsigned>(stats.frames_received),
               static_cast<unsigned>(stats.records_in_buffer),
               static_cast<unsigned>(CAPTURE_RING_CAPACITY),
               static_cast<unsigned>(stats.records_overwritten),
               static_cast<unsigned>(stats.invalid_frames),
               static_cast<unsigned>(stats.rx_release_errors),
               static_cast<unsigned>(stats.rx_restart_errors));
    }
    return;
  }

  if (this->state_ == UploadState::OTA_MAINTENANCE) {
    const bool connected = wifi::global_wifi_component != nullptr &&
                           wifi::global_wifi_component->is_connected();
    const bool window_expired = now - this->state_started_ms_ >= OTA_MAINTENANCE_WINDOW_MS;
    const bool connection_timed_out = !connected &&
        now - this->state_started_ms_ >= WIFI_CONNECT_TIMEOUT_MS;
    if (window_expired || connection_timed_out) {
      ESP_LOGI(TAG, window_expired ? "OTA maintenance window ended" :
               "Wi-Fi connection timed out during OTA maintenance");
      this->end_ota_maintenance_();
    }
    return;
  }

  if (this->state_ == UploadState::OTA_WIFI_OFF) {
    const bool wifi_is_off = wifi::global_wifi_component == nullptr ||
                              wifi::global_wifi_component->is_disabled();
    if (wifi_is_off) {
      this->upload_succeeded_ = false;
      this->resume_capture_();
      return;
    }
    if (now - this->state_started_ms_ >= WIFI_SHUTDOWN_TIMEOUT_MS) {
      if (wifi::global_wifi_component != nullptr) {
        wifi::global_wifi_component->disable();
      }
      this->state_started_ms_ = now;
    }
    return;
  }

  if (this->state_ == UploadState::WAITING_FOR_WIFI) {
    if (wifi::global_wifi_component == nullptr) {
      ESP_LOGE(TAG, "ESPHome Wi-Fi component is unavailable; retaining batch");
      this->finish_upload_(false);
      return;
    }

    if (wifi::global_wifi_component->is_connected()) {
      const bool success = this->send_csv_();
      this->finish_upload_(success);
      return;
    }

    if (now - this->state_started_ms_ >= WIFI_CONNECT_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Wi-Fi connection timed out; retaining batch for retry");
      this->finish_upload_(false);
    }
    return;
  }

  if (this->state_ == UploadState::WAITING_FOR_WIFI_OFF) {
    const bool wifi_is_off =
        wifi::global_wifi_component == nullptr || wifi::global_wifi_component->is_disabled();

    if (wifi_is_off) {
      if (this->upload_succeeded_) {
        this->resume_capture_();
      } else {
        this->retry_due_ms_ = now + UPLOAD_RETRY_DELAY_MS;
        this->state_ = UploadState::RETRY_WAIT;
        ESP_LOGI(TAG, "Pending batch retained; next ntfy attempt in 5 minutes");
      }
      return;
    }

    if (now - this->state_started_ms_ >= WIFI_SHUTDOWN_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Wi-Fi has not reported disabled yet; requesting shutdown again");
      if (wifi::global_wifi_component != nullptr) {
        wifi::global_wifi_component->disable();
      }
      this->state_started_ms_ = now;
    }
    return;
  }

  if (this->state_ == UploadState::RETRY_WAIT &&
      static_cast<int32_t>(now - this->retry_due_ms_) >= 0) {
    this->begin_upload_();
  }
}

void SnifzigCapture::begin_ota_maintenance_() {
  if (wifi::global_wifi_component == nullptr) {
    ESP_LOGE(TAG, "Cannot start OTA maintenance: ESPHome Wi-Fi component is unavailable");
    return;
  }

  const esp_err_t sleep_result = esp_ieee802154_sleep();
  if (sleep_result != ESP_OK) {
    ESP_LOGW(TAG, "Could not put 802.15.4 radio to sleep for OTA: %s",
             esp_err_to_name(sleep_result));
  }

  this->upload_succeeded_ = false;
  this->state_started_ms_ = millis();
  this->state_ = UploadState::OTA_MAINTENANCE;
  gpio_set_level(STATUS_LED_GPIO, 1);
  this->status_led_on_ = false;
  wifi::global_wifi_component->enable();
  ESP_LOGI(TAG, "OTA maintenance started for 10 minutes; Wi-Fi enabled");
}

void SnifzigCapture::end_ota_maintenance_() {
  this->state_started_ms_ = millis();
  this->state_ = UploadState::OTA_WIFI_OFF;
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->disable();
  }
}

void SnifzigCapture::begin_upload_() {
  if (wifi::global_wifi_component == nullptr) {
    ESP_LOGE(TAG, "Cannot upload: ESPHome Wi-Fi component is unavailable");
    this->upload_succeeded_ = false;
    this->retry_due_ms_ = millis() + UPLOAD_RETRY_DELAY_MS;
    this->state_ = UploadState::RETRY_WAIT;
    return;
  }

  // The RX callback has stopped re-arming the 802.15.4 radio when the ring fills.
  // Put the radio fully to sleep before enabling Wi-Fi on the shared RF hardware.
  const esp_err_t sleep_result = esp_ieee802154_sleep();
  if (sleep_result != ESP_OK) {
    ESP_LOGW(TAG, "Could not put 802.15.4 radio to sleep: %s", esp_err_to_name(sleep_result));
  }

  this->upload_succeeded_ = false;
  this->state_started_ms_ = millis();
  this->state_ = UploadState::WAITING_FOR_WIFI;
  gpio_set_level(STATUS_LED_GPIO, 1);
  this->status_led_on_ = false;
  wifi::global_wifi_component->enable();
  ESP_LOGI(TAG, "Capture batch ready; enabling Wi-Fi for ntfy upload");
}

void SnifzigCapture::finish_upload_(bool success) {
  this->upload_succeeded_ = success;
  if (success) {
    ESP_LOGI(TAG, "ntfy accepted batch %u", static_cast<unsigned>(this->batch_number_));
  } else {
    ESP_LOGW(TAG, "ntfy upload failed; keeping batch in RAM");
  }

  this->state_started_ms_ = millis();
  this->state_ = UploadState::WAITING_FOR_WIFI_OFF;
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->disable();
  }
}

void SnifzigCapture::resume_capture_() {
  if (this->upload_succeeded_) {
    portENTER_CRITICAL(&this->mux_);
    this->head_ = 0;
    this->count_ = 0;
    portEXIT_CRITICAL(&this->mux_);
    this->batch_number_++;
  }

  const esp_err_t receive_result = esp_ieee802154_receive();
  if (receive_result != ESP_OK) {
    ESP_LOGE(TAG, "Could not resume channel %u capture: %s",
             this->channel_, esp_err_to_name(receive_result));
    this->retry_due_ms_ = millis() + UPLOAD_RETRY_DELAY_MS;
    this->state_ = UploadState::RETRY_WAIT;
    return;
  }

  this->state_ = UploadState::CAPTURING;
  this->last_report_ms_ = millis();
  ESP_LOGI(TAG, "Wi-Fi is off; Zigbee capture resumed on channel %u", this->channel_);
}

bool SnifzigCapture::build_csv_(size_t *csv_size) {
  if (csv_size == nullptr) {
    return false;
  }

  size_t used = 0;
  int written = snprintf(this->csv_buffer_, sizeof(this->csv_buffer_),
                         "sequence,sfd_timestamp,channel,rssi_dbm,lqi,length,frame_hex\n");
  if (written < 0 || static_cast<size_t>(written) >= sizeof(this->csv_buffer_)) {
    return false;
  }
  used = static_cast<size_t>(written);

  static const char HEX[] = "0123456789ABCDEF";
  const CaptureStats stats = this->get_stats();

  for (uint16_t index = 0; index < stats.records_in_buffer; index++) {
    CaptureRecord record{};
    if (!this->copy_record(index, &record)) {
      return false;
    }

    written = snprintf(this->csv_buffer_ + used, sizeof(this->csv_buffer_) - used,
                       "%" PRIu32 ",%" PRIu64 ",%u,%d,%u,%u,",
                       record.sequence,
                       record.sfd_timestamp,
                       record.channel,
                       record.rssi_dbm,
                       record.lqi,
                       record.length);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(this->csv_buffer_) - used) {
      return false;
    }
    used += static_cast<size_t>(written);

    const size_t hex_size = static_cast<size_t>(record.length) * 2;
    if (hex_size + 1 >= sizeof(this->csv_buffer_) - used) {
      return false;
    }
    for (uint8_t byte_index = 0; byte_index < record.length; byte_index++) {
      const uint8_t value = record.bytes[byte_index];
      this->csv_buffer_[used++] = HEX[value >> 4];
      this->csv_buffer_[used++] = HEX[value & 0x0F];
    }
    this->csv_buffer_[used++] = '\n';
  }

  *csv_size = used;
  return true;
}

bool SnifzigCapture::send_csv_() {
  size_t csv_size = 0;
  if (!this->build_csv_(&csv_size) || csv_size == 0) {
    ESP_LOGE(TAG, "Could not serialize capture batch");
    return false;
  }

  char filename[40];
  char title[80];
  snprintf(filename, sizeof(filename), "snifzig_%06" PRIu32 ".csv", this->batch_number_);
  snprintf(title, sizeof(title), "Snifzig Zigbee capture, channel %u", this->channel_);

  esp_http_client_config_t config{};
  config.url = this->ntfy_publish_url_.c_str();
  config.method = HTTP_METHOD_PUT;
  config.timeout_ms = CSV_HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "Could not create ntfy HTTP client");
    return false;
  }

  esp_err_t result = esp_http_client_set_header(client, "Content-Type", "text/csv");
  if (result == ESP_OK) {
    result = esp_http_client_set_header(client, "X-Filename", filename);
  }
  if (result == ESP_OK) {
    result = esp_http_client_set_header(client, "Title", title);
  }
  if (result == ESP_OK && !this->ntfy_authorization_.empty()) {
    result = esp_http_client_set_header(client, "Authorization",
                                        this->ntfy_authorization_.c_str());
  }
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Could not configure ntfy request headers: %s", esp_err_to_name(result));
    esp_http_client_cleanup(client);
    return false;
  }

  result = esp_http_client_open(client, static_cast<int>(csv_size));
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Could not open ntfy upload: %s", esp_err_to_name(result));
    esp_http_client_cleanup(client);
    return false;
  }

  size_t offset = 0;
  while (offset < csv_size) {
    const int chunk = esp_http_client_write(
        client,
        this->csv_buffer_ + offset,
        static_cast<int>(csv_size - offset));
    if (chunk <= 0) {
      ESP_LOGE(TAG, "ntfy upload stopped after %u/%u bytes",
               static_cast<unsigned>(offset),
               static_cast<unsigned>(csv_size));
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    offset += static_cast<size_t>(chunk);
  }

  esp_http_client_fetch_headers(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (status_code < 200 || status_code >= 300) {
    ESP_LOGW(TAG, "ntfy returned HTTP %d; retaining capture batch", status_code);
    return false;
  }

  ESP_LOGI(TAG, "Uploaded %u frames (%u bytes) as %s",
           static_cast<unsigned>(this->get_stats().records_in_buffer),
           static_cast<unsigned>(csv_size),
           filename);
  return true;
}

bool SnifzigCapture::copy_record(uint16_t index, CaptureRecord *out) const {
  if (out == nullptr) {
    return false;
  }

  bool copied = false;
  portENTER_CRITICAL(&this->mux_);
  if (index < this->count_) {
    const uint16_t oldest =
        (this->head_ + CAPTURE_RING_CAPACITY - this->count_) % CAPTURE_RING_CAPACITY;
    const uint16_t slot = (oldest + index) % CAPTURE_RING_CAPACITY;
    *out = this->ring_[slot];
    copied = true;
  }
  portEXIT_CRITICAL(&this->mux_);
  return copied;
}

CaptureStats SnifzigCapture::get_stats() const {
  CaptureStats stats{};
  portENTER_CRITICAL(&this->mux_);
  stats.frames_received = this->frames_received_;
  stats.records_overwritten = this->records_overwritten_;
  stats.invalid_frames = this->invalid_frames_;
  stats.rx_release_errors = this->rx_release_errors_;
  stats.rx_restart_errors = this->rx_restart_errors_;
  stats.records_in_buffer = this->count_;
  portEXIT_CRITICAL(&this->mux_);
  return stats;
}

void IRAM_ATTR SnifzigCapture::handle_frame(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *frame_info) {
  const bool valid = frame != nullptr && frame_info != nullptr;
  const uint8_t length = valid ? frame[0] : 0;
  bool rearm_receiver = true;

  if (!valid || length > MAX_MAC_FRAME_LENGTH) {
    portENTER_CRITICAL_ISR(&this->mux_);
    this->invalid_frames_++;
    portEXIT_CRITICAL_ISR(&this->mux_);
  } else {
    portENTER_CRITICAL_ISR(&this->mux_);

    CaptureRecord &record = this->ring_[this->head_];
    record.sequence = this->frames_received_;
    record.sfd_timestamp = frame_info->timestamp;
    record.rssi_dbm = frame_info->rssi;
    record.lqi = frame_info->lqi;
    record.channel = frame_info->channel;
    record.length = length;

    // frame[0] is the length. Keep the MAC header and payload bytes as
    // delivered by the radio; RSSI/LQI are separate receive metadata.
    for (uint8_t i = 0; i < length; i++) {
      record.bytes[i] = frame[i + 1];
    }

    this->head_ = (this->head_ + 1) % CAPTURE_RING_CAPACITY;
    if (this->count_ < CAPTURE_RING_CAPACITY) {
      this->count_++;
    } else {
      this->records_overwritten_++;
    }
    this->frames_received_++;
    rearm_receiver = this->count_ < CAPTURE_RING_CAPACITY;

    portEXIT_CRITICAL_ISR(&this->mux_);
  }

  if (frame != nullptr && esp_ieee802154_receive_handle_done(frame) != ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_);
    this->rx_release_errors_++;
    portEXIT_CRITICAL_ISR(&this->mux_);
  }
  if (rearm_receiver && esp_ieee802154_receive() != ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_);
    this->rx_restart_errors_++;
    portEXIT_CRITICAL_ISR(&this->mux_);
  }
}

static void IRAM_ATTR dispatch_received_frame(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *frame_info) {
  if (s_instance != nullptr) {
    s_instance->handle_frame(frame, frame_info);
  } else {
    if (frame != nullptr) {
      esp_ieee802154_receive_handle_done(frame);
    }
    esp_ieee802154_receive();
  }
}

}  // namespace snifzig_capture
}  // namespace esphome

extern "C" void IRAM_ATTR esp_ieee802154_receive_done(
    uint8_t *frame,
    esp_ieee802154_frame_info_t *frame_info) {
  esphome::snifzig_capture::dispatch_received_frame(frame, frame_info);
}
