#include "snifzig_capture.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_ieee802154.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

namespace esphome { namespace snifzig_capture {
static const char *const TAG = "snifzig_capture";
static SnifzigCapture *s_instance = nullptr;
static constexpr gpio_num_t RF_SWITCH_ENABLE_GPIO = GPIO_NUM_3;
static constexpr gpio_num_t RF_ANTENNA_SELECT_GPIO = GPIO_NUM_14;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 45000, WIFI_SHUTDOWN_TIMEOUT_MS = 10000;
static constexpr uint32_t UPLOAD_RETRY_DELAY_MS = 300000;
static constexpr gpio_num_t STATUS_LED_GPIO = GPIO_NUM_15, OTA_BUTTON_GPIO = GPIO_NUM_9;
static constexpr uint32_t OTA_BUTTON_HOLD_MS = 3000, OTA_MAINTENANCE_WINDOW_MS = 600000;
static constexpr uint32_t STATUS_LED_PERIOD_MS = 1000, STATUS_LED_PULSE_MS = 120;
static constexpr uint32_t REPORT_HTTP_TIMEOUT_MS = 20000;

void SnifzigCapture::setup() {
  s_instance = this;
  gpio_config_t cfg{};
  cfg.pin_bit_mask = (1ULL << RF_SWITCH_ENABLE_GPIO) | (1ULL << RF_ANTENNA_SELECT_GPIO);
  cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(gpio_set_level(RF_SWITCH_ENABLE_GPIO, 0));
  ESP_ERROR_CHECK(gpio_set_level(RF_ANTENNA_SELECT_GPIO, 0));
  cfg = {};
  cfg.pin_bit_mask = 1ULL << STATUS_LED_GPIO; cfg.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg)); ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO, 1));
  cfg = {}; cfg.pin_bit_mask = 1ULL << OTA_BUTTON_GPIO; cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE; ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(esp_ieee802154_enable());
  ESP_ERROR_CHECK(esp_ieee802154_set_channel(this->channel_));
  ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_ieee802154_set_coordinator(false));
  ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
  ESP_ERROR_CHECK(esp_ieee802154_receive());
  this->last_report_ms_ = millis();
  ESP_LOGI(TAG, "Passive Zigbee statistics started on channel %u; hourly text reports", this->channel_);
  ESP_LOGI(TAG, "Hold BOOT for 3 seconds to open 10-minute OTA window");
}

void SnifzigCapture::loop() {
  const uint32_t now = millis();
  const bool wifi_ota = this->state_ == UploadState::OTA_MAINTENANCE &&
      wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
  const bool led = wifi_ota || (this->state_ == UploadState::CAPTURING && now % STATUS_LED_PERIOD_MS < STATUS_LED_PULSE_MS);
  if (led != this->status_led_on_) { gpio_set_level(STATUS_LED_GPIO, led ? 0 : 1); this->status_led_on_ = led; }

  if (this->state_ == UploadState::CAPTURING) {
    const bool pressed = gpio_get_level(OTA_BUTTON_GPIO) == 0;
    if (pressed) {
      if (!this->ota_button_held_) { this->ota_button_held_ = true; this->ota_button_started_ms_ = now; }
      else if (now - this->ota_button_started_ms_ >= OTA_BUTTON_HOLD_MS) {
        this->ota_button_held_ = false; this->begin_ota_maintenance_(); return;
      }
    } else this->ota_button_held_ = false;
    if (now - this->last_report_ms_ >= STATS_INTERVAL_MS) this->begin_upload_();
    return;
  }
  if (this->state_ == UploadState::OTA_MAINTENANCE) {
    const bool connected = wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
    const bool expired = now - this->state_started_ms_ >= OTA_MAINTENANCE_WINDOW_MS;
    if (expired || (!connected && now - this->state_started_ms_ >= WIFI_CONNECT_TIMEOUT_MS)) {
      ESP_LOGI(TAG, "OTA window ended or Wi-Fi connection timed out"); this->end_ota_maintenance_();
    }
    return;
  }
  if (this->state_ == UploadState::OTA_WIFI_OFF) {
    if (!wifi::global_wifi_component || wifi::global_wifi_component->is_disabled()) {
      this->upload_succeeded_ = false; this->resume_capture_(); return;
    }
    if (now - this->state_started_ms_ >= WIFI_SHUTDOWN_TIMEOUT_MS) {
      wifi::global_wifi_component->disable(); this->state_started_ms_ = now;
    }
    return;
  }
  if (this->state_ == UploadState::WAITING_FOR_WIFI) {
    if (!wifi::global_wifi_component) this->finish_upload_(false);
    else if (wifi::global_wifi_component->is_connected()) this->finish_upload_(this->send_report_());
    else if (now - this->state_started_ms_ >= WIFI_CONNECT_TIMEOUT_MS) this->finish_upload_(false);
    return;
  }
  if (this->state_ == UploadState::WAITING_FOR_WIFI_OFF) {
    if (!wifi::global_wifi_component || wifi::global_wifi_component->is_disabled()) {
      if (this->upload_succeeded_) {
        this->clear_stats_(); this->report_number_++; this->resume_capture_();
      } else {
        this->retry_due_ms_ = now + UPLOAD_RETRY_DELAY_MS; this->state_ = UploadState::RETRY_WAIT;
        ESP_LOGW(TAG, "Summary retained; retry in 5 minutes");
      }
    } else if (now - this->state_started_ms_ >= WIFI_SHUTDOWN_TIMEOUT_MS) {
      wifi::global_wifi_component->disable(); this->state_started_ms_ = now;
    }
    return;
  }
  if (this->state_ == UploadState::RETRY_WAIT && static_cast<int32_t>(now - this->retry_due_ms_) >= 0)
    this->begin_upload_();
}

void SnifzigCapture::begin_ota_maintenance_() {
  if (!wifi::global_wifi_component) return;
  const esp_err_t e = esp_ieee802154_sleep();
  if (e != ESP_OK) ESP_LOGW(TAG, "802.15.4 sleep for OTA: %s", esp_err_to_name(e));
  this->upload_succeeded_ = false; this->state_started_ms_ = millis();
  this->state_ = UploadState::OTA_MAINTENANCE;
  gpio_set_level(STATUS_LED_GPIO, 1); this->status_led_on_ = false;
  wifi::global_wifi_component->enable(); ESP_LOGI(TAG, "OTA Wi-Fi enabled");
}
void SnifzigCapture::end_ota_maintenance_() {
  this->state_started_ms_ = millis(); this->state_ = UploadState::OTA_WIFI_OFF;
  if (wifi::global_wifi_component) wifi::global_wifi_component->disable();
}
void SnifzigCapture::begin_upload_() {
  if (!wifi::global_wifi_component) {
    this->retry_due_ms_ = millis() + UPLOAD_RETRY_DELAY_MS; this->state_ = UploadState::RETRY_WAIT; return;
  }
  const esp_err_t e = esp_ieee802154_sleep();
  if (e != ESP_OK) ESP_LOGW(TAG, "802.15.4 sleep before report: %s", esp_err_to_name(e));
  this->upload_succeeded_ = false; this->state_started_ms_ = millis(); this->state_ = UploadState::WAITING_FOR_WIFI;
  gpio_set_level(STATUS_LED_GPIO, 1); this->status_led_on_ = false;
  wifi::global_wifi_component->enable(); ESP_LOGI(TAG, "Hourly statistics ready; enabling Wi-Fi");
}
void SnifzigCapture::finish_upload_(bool success) {
  this->upload_succeeded_ = success;
  ESP_LOGI(TAG, success ? "ntfy accepted hourly text summary" : "ntfy failed; retaining hourly statistics");
  this->state_started_ms_ = millis(); this->state_ = UploadState::WAITING_FOR_WIFI_OFF;
  if (wifi::global_wifi_component) wifi::global_wifi_component->disable();
}
void SnifzigCapture::resume_capture_() {
  const esp_err_t e = esp_ieee802154_receive();
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "Could not resume capture: %s", esp_err_to_name(e));
    this->retry_due_ms_ = millis() + UPLOAD_RETRY_DELAY_MS; this->state_ = UploadState::RETRY_WAIT; return;
  }
  this->state_ = UploadState::CAPTURING; this->last_report_ms_ = millis();
  ESP_LOGI(TAG, "Wi-Fi off; capture resumed on channel %u", this->channel_);
}
CaptureStats SnifzigCapture::get_stats() const {
  CaptureStats s{};
  portENTER_CRITICAL(&this->mux_);
  s.frames_received=this->frames_received_; s.invalid_frames=this->invalid_frames_;
  s.rx_release_errors=this->rx_release_errors_; s.rx_restart_errors=this->rx_restart_errors_;
  for (uint8_t i=0;i<5;i++) s.frame_types[i]=this->frame_types_[i];
  for (uint8_t i=0;i<4;i++) { s.rssi_buckets[i]=this->rssi_buckets_[i]; s.lqi_buckets[i]=this->lqi_buckets_[i]; }
  s.rssi_sum=this->rssi_sum_; s.lqi_sum=this->lqi_sum_;
  s.rssi_min=this->rssi_min_; s.rssi_max=this->rssi_max_; s.lqi_min=this->lqi_min_; s.lqi_max=this->lqi_max_;
  portEXIT_CRITICAL(&this->mux_); return s;
}
void SnifzigCapture::clear_stats_() {
  portENTER_CRITICAL(&this->mux_);
  this->frames_received_=this->invalid_frames_=this->rx_release_errors_=this->rx_restart_errors_=0;
  this->rssi_sum_=0; this->lqi_sum_=0; this->rssi_min_=127; this->rssi_max_=-128; this->lqi_min_=255; this->lqi_max_=0;
  for (uint8_t i=0;i<5;i++) this->frame_types_[i]=0;
  for (uint8_t i=0;i<4;i++) this->rssi_buckets_[i]=this->lqi_buckets_[i]=0;
  portEXIT_CRITICAL(&this->mux_);
}
bool SnifzigCapture::send_report_() {
  const CaptureStats s=this->get_stats(); const uint32_t total=s.frames_received;
  const float ra=total ? (float)s.rssi_sum/total : 0, la=total ? (float)s.lqi_sum/total : 0;
  char body[768];
  const int n=snprintf(body,sizeof(body),
    "Snifzig - resumo horario Zigbee\nCanal: %u | periodo: 60 min\nTramas recebidas: %lu\n"
    "Tipos MAC: beacon %lu | dados %lu | ACK %lu | comando %lu | outros %lu\n"
    "RSSI dBm: min %d | media %.1f | max %d\nRSSI: <=-90 %lu | -89..-80 %lu | -79..-70 %lu | >=-69 %lu\n"
    "LQI: min %u | media %.1f | max %u\nLQI: <8 %lu | 8..9 %lu | 10..11 %lu | >=12 %lu\n"
    "Erros RX: frames invalidos %lu | libertacao %lu | reinicio %lu\n"
    "Medicao passiva neste local; nao confirma entrega nem rotas.",
    this->channel_,(unsigned long)total,(unsigned long)s.frame_types[0],(unsigned long)s.frame_types[1],
    (unsigned long)s.frame_types[2],(unsigned long)s.frame_types[3],(unsigned long)s.frame_types[4],
    total?s.rssi_min:0,ra,total?s.rssi_max:0,(unsigned long)s.rssi_buckets[0],(unsigned long)s.rssi_buckets[1],
    (unsigned long)s.rssi_buckets[2],(unsigned long)s.rssi_buckets[3],
    total?s.lqi_min:0,la,total?s.lqi_max:0,(unsigned long)s.lqi_buckets[0],(unsigned long)s.lqi_buckets[1],
    (unsigned long)s.lqi_buckets[2],(unsigned long)s.lqi_buckets[3],(unsigned long)s.invalid_frames,
    (unsigned long)s.rx_release_errors,(unsigned long)s.rx_restart_errors);
  if(n<=0 || (size_t)n>=sizeof(body)) return false;
  char title[72]; snprintf(title,sizeof(title),"Snifzig resumo horario #%lu",(unsigned long)this->report_number_);
  esp_http_client_config_t cfg{}; cfg.url=this->ntfy_publish_url_.c_str(); cfg.method=HTTP_METHOD_POST;
  cfg.timeout_ms=REPORT_HTTP_TIMEOUT_MS; cfg.crt_bundle_attach=esp_crt_bundle_attach;
  esp_http_client_handle_t c=esp_http_client_init(&cfg); if(!c) return false;
  esp_err_t e=esp_http_client_set_header(c,"Content-Type","text/plain; charset=utf-8");
  if(e==ESP_OK) e=esp_http_client_set_header(c,"Title",title);
  if(e==ESP_OK && !this->ntfy_authorization_.empty())
    e=esp_http_client_set_header(c,"Authorization",this->ntfy_authorization_.c_str());
  if(e==ESP_OK) e=esp_http_client_open(c,n);
  if(e==ESP_OK && esp_http_client_write(c,body,n)!=n) e=ESP_FAIL;
  if(e!=ESP_OK) { esp_http_client_close(c); esp_http_client_cleanup(c); return false; }
  esp_http_client_fetch_headers(c); const int status=esp_http_client_get_status_code(c);
  esp_http_client_close(c); esp_http_client_cleanup(c);
  if(status<200 || status>=300) { ESP_LOGW(TAG,"ntfy HTTP %d",status); return false; }
  ESP_LOGI(TAG,"Sent hourly summary for %u frames",(unsigned)total); return true;
}
void IRAM_ATTR SnifzigCapture::handle_frame(uint8_t *frame, esp_ieee802154_frame_info_t *info) {
  const bool valid=frame && info; const uint8_t len=valid?frame[0]:0;
  portENTER_CRITICAL_ISR(&this->mux_);
  if(!valid || len==0 || len>127) this->invalid_frames_++;
  else {
    this->frames_received_++; const int8_t r=info->rssi; const uint8_t q=info->lqi;
    this->rssi_sum_+=r; this->lqi_sum_+=q;
    if(r<this->rssi_min_) this->rssi_min_=r; if(r>this->rssi_max_) this->rssi_max_=r;
    if(q<this->lqi_min_) this->lqi_min_=q; if(q>this->lqi_max_) this->lqi_max_=q;
    if(r<=-90) this->rssi_buckets_[0]++; else if(r<=-80) this->rssi_buckets_[1]++;
    else if(r<=-70) this->rssi_buckets_[2]++; else this->rssi_buckets_[3]++;
    if(q<8) this->lqi_buckets_[0]++; else if(q<10) this->lqi_buckets_[1]++;
    else if(q<12) this->lqi_buckets_[2]++; else this->lqi_buckets_[3]++;
    const uint8_t t=len>=2?(frame[1]&7):4; this->frame_types_[t<=3?t:4]++;
  }
  portEXIT_CRITICAL_ISR(&this->mux_);
  if(frame && esp_ieee802154_receive_handle_done(frame)!=ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_); this->rx_release_errors_++; portEXIT_CRITICAL_ISR(&this->mux_);
  }
  if(this->state_==UploadState::CAPTURING && esp_ieee802154_receive()!=ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_); this->rx_restart_errors_++; portEXIT_CRITICAL_ISR(&this->mux_);
  }
}
static void IRAM_ATTR dispatch_received_frame(uint8_t *frame,esp_ieee802154_frame_info_t *info) {
  if(s_instance) s_instance->handle_frame(frame,info);
  else { if(frame) esp_ieee802154_receive_handle_done(frame); esp_ieee802154_receive(); }
}
} }
extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame,esp_ieee802154_frame_info_t *info) {
  esphome::snifzig_capture::dispatch_received_frame(frame,info);
}
