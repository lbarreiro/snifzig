#include "snifzig_capture.h"
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

namespace esphome { namespace snifzig_capture {
static const char *const TAG="snifzig_capture";
static SnifzigCapture *s_instance=nullptr;
static constexpr gpio_num_t RF_SWITCH_ENABLE_GPIO=GPIO_NUM_3, RF_ANTENNA_SELECT_GPIO=GPIO_NUM_14;
static constexpr gpio_num_t STATUS_LED_GPIO=GPIO_NUM_15, OTA_BUTTON_GPIO=GPIO_NUM_9;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS=45000, WIFI_SHUTDOWN_TIMEOUT_MS=10000;
static constexpr uint32_t UPLOAD_RETRY_DELAY_MS=300000, OTA_MAINTENANCE_WINDOW_MS=600000;
static constexpr uint32_t STATUS_LED_PERIOD_MS=1000, STATUS_LED_PULSE_MS=120, REPORT_HTTP_TIMEOUT_MS=20000;

static const char *grade(uint8_t value) {
  if(value>=4) return "excelente";
  if(value==3) return "bom";
  if(value==2) return "razoavel";
  return "fraco";
}
static const char *grade_score(float score) {
  if(score>=3.5f) return "excelente";
  if(score>=2.75f) return "bom";
  if(score>=2.0f) return "razoavel";
  return "fraco";
}
static uint8_t rssi_grade(float v) {
  if(v<=-90) return 1;
  if(v<=-80) return 2;
  if(v<=-70) return 3;
  return 4;
}
static uint8_t lqi_grade(float v) {
  if(v<64) return 1;
  if(v<128) return 2;
  if(v<192) return 3;
  return 4;
}

void SnifzigCapture::setup() {
  s_instance=this;
  gpio_config_t cfg{};
  cfg.pin_bit_mask=(1ULL<<RF_SWITCH_ENABLE_GPIO)|(1ULL<<RF_ANTENNA_SELECT_GPIO);
  cfg.mode=GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(gpio_set_level(RF_SWITCH_ENABLE_GPIO,0));
  ESP_ERROR_CHECK(gpio_set_level(RF_ANTENNA_SELECT_GPIO,0));
  cfg={}; cfg.pin_bit_mask=1ULL<<STATUS_LED_GPIO; cfg.mode=GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg)); ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_GPIO,1));
  cfg={}; cfg.pin_bit_mask=1ULL<<OTA_BUTTON_GPIO; cfg.mode=GPIO_MODE_INPUT; cfg.pull_up_en=GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  this->state_=UploadState::BOOT_WAIT_WIFI;
  ESP_LOGI(TAG,"Starting Wi-Fi provisioning/connection before passive Zigbee capture");
}
void SnifzigCapture::start_capture_() {
  ESP_ERROR_CHECK(esp_ieee802154_enable());
  ESP_ERROR_CHECK(esp_ieee802154_set_channel(this->channel_));
  ESP_ERROR_CHECK(esp_ieee802154_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_ieee802154_set_coordinator(false));
  ESP_ERROR_CHECK(esp_ieee802154_set_rx_when_idle(true));
  ESP_ERROR_CHECK(esp_ieee802154_receive());
  this->state_=UploadState::CAPTURING; this->last_report_ms_=millis();
  ESP_LOGI(TAG,"Wi-Fi off; passive Zigbee capture active on channel %u",this->channel_);
}
void SnifzigCapture::loop() {
  const uint32_t now=millis();
  bool led=false;
  if(this->state_==UploadState::BOOT_WAIT_WIFI) {
    // Three quick pulses every two seconds while the fallback "Snifzig" portal awaits credentials.
    const uint32_t phase=now%2000;
    led=(phase<100)||(phase>=220&&phase<320)||(phase>=440&&phase<540);
  } else if(this->state_==UploadState::CAPTURING) {
    led=(now%STATUS_LED_PERIOD_MS)<STATUS_LED_PULSE_MS;
  } else if(this->state_==UploadState::OTA_MAINTENANCE) {
    led=wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
  }
  if(led!=this->status_led_on_) { gpio_set_level(STATUS_LED_GPIO,led?0:1); this->status_led_on_=led; }

  if(this->state_==UploadState::BOOT_WAIT_WIFI) {
    if(wifi::global_wifi_component && wifi::global_wifi_component->is_connected()) {
      // Successful STA connection means the configured or portal-provisioned credentials work.
      this->state_=UploadState::BOOT_WIFI_OFF; this->state_started_ms_=now;
      wifi::global_wifi_component->disable();
      ESP_LOGI(TAG,"Wi-Fi connected; shutting it down before starting Zigbee");
    }
    return;
  }
  if(this->state_==UploadState::BOOT_WIFI_OFF) {
    if(!wifi::global_wifi_component || wifi::global_wifi_component->is_disabled()) {
      this->start_capture_(); return;
    }
    if(now-this->state_started_ms_>=WIFI_SHUTDOWN_TIMEOUT_MS) {
      wifi::global_wifi_component->disable(); this->state_started_ms_=now;
    }
    return;
  }
  if(this->state_==UploadState::CAPTURING) {
    // A single BOOT press opens the OTA window; no long press required.
    if(gpio_get_level(OTA_BUTTON_GPIO)==0) {
      if(!this->ota_button_held_) {
        this->ota_button_held_=true; this->begin_ota_maintenance_(); return;
      }
    } else this->ota_button_held_=false;
    if(now-this->last_report_ms_>=STATS_INTERVAL_MS) this->begin_upload_();
    return;
  }
  if(this->state_==UploadState::OTA_MAINTENANCE) {
    const bool connected=wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
    const bool expired=now-this->state_started_ms_>=OTA_MAINTENANCE_WINDOW_MS;
    const bool timeout=!connected && now-this->state_started_ms_>=WIFI_CONNECT_TIMEOUT_MS;
    if(expired||timeout) { ESP_LOGI(TAG,"OTA window ended or Wi-Fi connection timed out"); this->end_ota_maintenance_(); }
    return;
  }
  if(this->state_==UploadState::OTA_WIFI_OFF) {
    if(!wifi::global_wifi_component||wifi::global_wifi_component->is_disabled()) { this->resume_capture_(); return; }
    if(now-this->state_started_ms_>=WIFI_SHUTDOWN_TIMEOUT_MS) { wifi::global_wifi_component->disable(); this->state_started_ms_=now; }
    return;
  }
  if(this->state_==UploadState::WAITING_FOR_WIFI) {
    if(!wifi::global_wifi_component) this->finish_upload_(false);
    else if(wifi::global_wifi_component->is_connected()) this->finish_upload_(this->send_report_());
    else if(now-this->state_started_ms_>=WIFI_CONNECT_TIMEOUT_MS) this->finish_upload_(false);
    return;
  }
  if(this->state_==UploadState::WAITING_FOR_WIFI_OFF) {
    if(!wifi::global_wifi_component||wifi::global_wifi_component->is_disabled()) {
      if(this->upload_succeeded_) { this->clear_stats_(); this->report_number_++; this->resume_capture_(); }
      else { this->retry_due_ms_=now+UPLOAD_RETRY_DELAY_MS; this->state_=UploadState::RETRY_WAIT; ESP_LOGW(TAG,"Report retained; retry in 5 minutes"); }
    } else if(now-this->state_started_ms_>=WIFI_SHUTDOWN_TIMEOUT_MS) { wifi::global_wifi_component->disable(); this->state_started_ms_=now; }
    return;
  }
  if(this->state_==UploadState::RETRY_WAIT && static_cast<int32_t>(now-this->retry_due_ms_)>=0) this->begin_upload_();
}
void SnifzigCapture::begin_ota_maintenance_() {
  if(!wifi::global_wifi_component) return;
  const esp_err_t e=esp_ieee802154_sleep();
  if(e!=ESP_OK) ESP_LOGW(TAG,"802.15.4 sleep for OTA: %s",esp_err_to_name(e));
  this->state_=UploadState::OTA_MAINTENANCE; this->state_started_ms_=millis();
  this->status_led_on_=false; gpio_set_level(STATUS_LED_GPIO,1);
  wifi::global_wifi_component->enable(); ESP_LOGI(TAG,"OTA Wi-Fi window opened for 10 minutes");
}
void SnifzigCapture::end_ota_maintenance_() {
  this->state_=UploadState::OTA_WIFI_OFF; this->state_started_ms_=millis();
  if(wifi::global_wifi_component) wifi::global_wifi_component->disable();
}
void SnifzigCapture::begin_upload_() {
  if(!wifi::global_wifi_component) { this->retry_due_ms_=millis()+UPLOAD_RETRY_DELAY_MS; this->state_=UploadState::RETRY_WAIT; return; }
  const esp_err_t e=esp_ieee802154_sleep();
  if(e!=ESP_OK) ESP_LOGW(TAG,"802.15.4 sleep before report: %s",esp_err_to_name(e));
  this->state_=UploadState::WAITING_FOR_WIFI; this->state_started_ms_=millis();
  this->status_led_on_=false; gpio_set_level(STATUS_LED_GPIO,1);
  wifi::global_wifi_component->enable(); ESP_LOGI(TAG,"Hourly stats ready; Wi-Fi enabled for ntfy");
}
void SnifzigCapture::finish_upload_(bool success) {
  this->upload_succeeded_=success;
  ESP_LOGI(TAG,success?"ntfy accepted hourly report":"ntfy failed; keeping statistics");
  this->state_=UploadState::WAITING_FOR_WIFI_OFF; this->state_started_ms_=millis();
  if(wifi::global_wifi_component) wifi::global_wifi_component->disable();
}
void SnifzigCapture::resume_capture_() {
  this->start_capture_();
}
CaptureStats SnifzigCapture::get_stats() const {
  CaptureStats s{};
  portENTER_CRITICAL(&this->mux_);
  s.frames_received=this->frames_received_; s.unidentified_frames=this->unidentified_frames_;
  s.invalid_frames=this->invalid_frames_; s.rx_release_errors=this->rx_release_errors_; s.rx_restart_errors=this->rx_restart_errors_;
  for(uint8_t i=0;i<5;i++) s.frame_types[i]=this->frame_types_[i];
  for(uint8_t i=0;i<4;i++) { s.rssi_buckets[i]=this->rssi_buckets_[i]; s.lqi_buckets[i]=this->lqi_buckets_[i]; }
  s.rssi_sum=this->rssi_sum_; s.lqi_sum=this->lqi_sum_; s.rssi_min=this->rssi_min_; s.rssi_max=this->rssi_max_;
  s.lqi_min=this->lqi_min_; s.lqi_max=this->lqi_max_; s.device_count=this->device_count_;
  for(uint8_t i=0;i<MAX_TRACKED_DEVICES;i++) s.devices[i]=this->devices_[i];
  s.unidentified=this->unidentified_;
  portEXIT_CRITICAL(&this->mux_); return s;
}
void SnifzigCapture::clear_stats_() {
  portENTER_CRITICAL(&this->mux_);
  this->frames_received_=this->unidentified_frames_=this->invalid_frames_=this->rx_release_errors_=this->rx_restart_errors_=0;
  this->rssi_sum_=0; this->lqi_sum_=0; this->rssi_min_=127; this->rssi_max_=-128; this->lqi_min_=255; this->lqi_max_=0;
  this->device_count_=0; memset(this->devices_,0,sizeof(this->devices_)); memset(&this->unidentified_,0,sizeof(this->unidentified_));
  memset(this->frame_types_,0,sizeof(this->frame_types_)); memset(this->rssi_buckets_,0,sizeof(this->rssi_buckets_));
  memset(this->lqi_buckets_,0,sizeof(this->lqi_buckets_));
  portEXIT_CRITICAL(&this->mux_);
}
bool SnifzigCapture::send_report_() {
  const CaptureStats s=this->get_stats();
  float rssi_group_sum=0,lqi_group_sum=0; uint8_t groups=0;
  static char body[10000]; size_t used=0;
  int n=snprintf(body,sizeof(body),
    "Snifzig - resumo horario Zigbee\nCanal: %u | periodo de captura: 60 min\n"
    "Tramas recebidas: %lu | identificadas: %lu | nao identificadas: %lu\n"
    "Tipos MAC: beacon %lu | dados %lu | ACK %lu | comando %lu | outros %lu\n",
    this->channel_,(unsigned long)s.frames_received,(unsigned long)(s.frames_received-s.unidentified_frames),
    (unsigned long)s.unidentified_frames,(unsigned long)s.frame_types[0],(unsigned long)s.frame_types[1],
    (unsigned long)s.frame_types[2],(unsigned long)s.frame_types[3],(unsigned long)s.frame_types[4]);
  if(n<0||(size_t)n>=sizeof(body)) return false; used=(size_t)n;
  n=snprintf(body+used,sizeof(body)-used,"\nPor dispositivo (ID MAC | tramas | RSSI medio | LQI medio):\n");
  if(n<0||(size_t)n>=sizeof(body)-used) return false; used+=(size_t)n;
  for(uint8_t i=0;i<s.device_count;i++) {
    const DeviceMetric &d=s.devices[i]; if(!d.frames) continue;
    const float ra=(float)d.rssi_sum/d.frames, la=(float)d.lqi_sum/d.frames;
    const uint8_t rg=rssi_grade(ra), lg=lqi_grade(la);
    rssi_group_sum+=ra; lqi_group_sum+=la; groups++;
    n=snprintf(body+used,sizeof(body)-used,"%c:%012llX | %lu | %.1f dBm (%s) | %.1f (%s)\n",
      d.address_mode==2?'S':'E',(unsigned long long)d.address,(unsigned long)d.frames,ra,grade(rg),la,grade(lg));
    if(n<0||(size_t)n>=sizeof(body)-used) return false; used+=(size_t)n;
  }
  if(s.unidentified.frames) {
    const float ra=(float)s.unidentified.rssi_sum/s.unidentified.frames;
    const float la=(float)s.unidentified.lqi_sum/s.unidentified.frames;
    const uint8_t rg=rssi_grade(ra),lg=lqi_grade(la); rssi_group_sum+=ra; lqi_group_sum+=la; groups++;
    n=snprintf(body+used,sizeof(body)-used,"Nao identificadas (grupo unico) | %lu | %.1f dBm (%s) | %.1f (%s)\n",
      (unsigned long)s.unidentified.frames,ra,grade(rg),la,grade(lg));
    if(n<0||(size_t)n>=sizeof(body)-used) return false; used+=(size_t)n;
  }
  const float rssi_global_mean=groups?rssi_group_sum/groups:0;
  const float lqi_global_mean=groups?lqi_group_sum/groups:0;
  const uint8_t rssi_global=groups?rssi_grade(rssi_global_mean):0;
  const uint8_t lqi_global=groups?lqi_grade(lqi_global_mean):0;
  const float global_score=groups?((float)rssi_global+lqi_global)/2.0f:0;
  n=snprintf(body+used,sizeof(body)-used,
    "\nDispositivos identificados: %u | grupos considerados na media: %u (cada um com peso igual)\n"
    "RSSI medio por grupo: %.1f dBm - %s | LQI medio por grupo: %.1f - %s | Global - %s\n"
    "RSSI total: min %d | media %.1f | max %d\nLQI total: min %u | media %.1f | max %u\n"
    "Erros RX: invalidos %lu | libertacao %lu | reinicio %lu\n"
    "Nota: observacao passiva local; nao confirma entrega nem rotas.",
     s.device_count,groups,rssi_global_mean,groups?grade(rssi_global):"sem dados",lqi_global_mean,groups?grade(lqi_global):"sem dados",groups?grade_score(global_score):"sem dados",
    s.frames_received?s.rssi_min:0,s.frames_received?(float)s.rssi_sum/s.frames_received:0,
    s.frames_received?s.rssi_max:0,s.frames_received?s.lqi_min:0,
    s.frames_received?(float)s.lqi_sum/s.frames_received:0,s.frames_received?s.lqi_max:0,
    (unsigned long)s.invalid_frames,(unsigned long)s.rx_release_errors,(unsigned long)s.rx_restart_errors);
  if(n<0||(size_t)n>=sizeof(body)-used) return false; used+=(size_t)n;

  char title[72]; snprintf(title,sizeof(title),"Snifzig resumo horario #%lu",(unsigned long)this->report_number_);
  esp_http_client_config_t cfg{}; cfg.url=this->ntfy_publish_url_.c_str(); cfg.method=HTTP_METHOD_POST;
  cfg.timeout_ms=REPORT_HTTP_TIMEOUT_MS; cfg.crt_bundle_attach=esp_crt_bundle_attach;
  esp_http_client_handle_t c=esp_http_client_init(&cfg); if(!c) return false;
  esp_err_t e=esp_http_client_set_header(c,"Content-Type","text/plain; charset=utf-8");
  if(e==ESP_OK) e=esp_http_client_set_header(c,"Title",title);
  if(e==ESP_OK&&!this->ntfy_authorization_.empty()) e=esp_http_client_set_header(c,"Authorization",this->ntfy_authorization_.c_str());
  if(e==ESP_OK) e=esp_http_client_open(c,(int)used);
  if(e==ESP_OK&&esp_http_client_write(c,body,(int)used)!=(int)used) e=ESP_FAIL;
  if(e!=ESP_OK) { esp_http_client_close(c); esp_http_client_cleanup(c); return false; }
  esp_http_client_fetch_headers(c); const int status=esp_http_client_get_status_code(c);
  esp_http_client_close(c); esp_http_client_cleanup(c);
  if(status<200||status>=300) { ESP_LOGW(TAG,"ntfy HTTP %d",status); return false; }
  ESP_LOGI(TAG,"Hourly report sent for %lu frames",(unsigned long)s.frames_received); return true;
}
void IRAM_ATTR SnifzigCapture::handle_frame(uint8_t *frame,esp_ieee802154_frame_info_t *info) {
  const bool valid=frame&&info; const uint8_t len=valid?frame[0]:0;
  uint8_t mode=0; uint64_t address=0; bool identified=false;
  if(valid&&len>=3) {
    const uint8_t *f=frame+1; const uint16_t fcf=(uint16_t)f[0]|((uint16_t)f[1]<<8);
    const uint8_t version=(fcf>>12)&3, dst=(fcf>>10)&3, src=(fcf>>14)&3;
    // Parse the common Zigbee 2003/2006 MAC header; uncertain 2015 layouts stay in unidentified.
    if(version<=1&&(src==2||src==3)) {
      size_t i=2; bool ok=true;
      if(i+1>len) ok=false; else i++; // sequence number
      if(ok&&dst!=0) { const size_t a=dst==2?2:8; if(i+2+a>len) ok=false; else i+=2+a; }
      if(ok&&src!=0) {
        if((dst==0)||((fcf&(1u<<6))==0)) { if(i+2>len) ok=false; else i+=2; }
        const size_t a=src==2?2:8;
        if(i+a>len) ok=false;
        else { mode=src; for(size_t j=0;j<a;j++) address|=((uint64_t)f[i+j])<<(8*j); identified=true; }
      }
    }
  }
  portENTER_CRITICAL_ISR(&this->mux_);
  if(!valid||len==0||len>127) this->invalid_frames_++;
  else {
    this->frames_received_++; const int8_t r=info->rssi; const uint8_t q=info->lqi;
    this->rssi_sum_+=r; this->lqi_sum_+=q;
    if(r<this->rssi_min_)this->rssi_min_=r; if(r>this->rssi_max_)this->rssi_max_=r;
    if(q<this->lqi_min_)this->lqi_min_=q; if(q>this->lqi_max_)this->lqi_max_=q;
    if(r<=-90)this->rssi_buckets_[0]++;else if(r<=-80)this->rssi_buckets_[1]++;else if(r<=-70)this->rssi_buckets_[2]++;else this->rssi_buckets_[3]++;
    if(q<64)this->lqi_buckets_[0]++;else if(q<128)this->lqi_buckets_[1]++;else if(q<192)this->lqi_buckets_[2]++;else this->lqi_buckets_[3]++;
    const uint8_t type=len>=2?(frame[1]&7):4; this->frame_types_[type<=3?type:4]++;
    DeviceMetric *d=nullptr;
    if(identified) {
      for(uint8_t k=0;k<this->device_count_;k++) if(this->devices_[k].address==address&&this->devices_[k].address_mode==mode){d=&this->devices_[k];break;}
      if(!d&&this->device_count_<MAX_TRACKED_DEVICES) {
        d=&this->devices_[this->device_count_++]; d->address=address; d->address_mode=mode;
      }
    }
    if(!d) { this->unidentified_frames_++; d=&this->unidentified_; }
    d->frames++; d->rssi_sum+=r; d->lqi_sum+=q;
  }
  portEXIT_CRITICAL_ISR(&this->mux_);
  if(frame&&esp_ieee802154_receive_handle_done(frame)!=ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_);this->rx_release_errors_++;portEXIT_CRITICAL_ISR(&this->mux_);
  }
  if(this->state_==UploadState::CAPTURING&&esp_ieee802154_receive()!=ESP_OK) {
    portENTER_CRITICAL_ISR(&this->mux_);this->rx_restart_errors_++;portEXIT_CRITICAL_ISR(&this->mux_);
  }
}
static void IRAM_ATTR dispatch_received_frame(uint8_t *frame,esp_ieee802154_frame_info_t *info) {
  if(s_instance)s_instance->handle_frame(frame,info);
  else { if(frame)esp_ieee802154_receive_handle_done(frame); esp_ieee802154_receive(); }
}
} }
extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame,esp_ieee802154_frame_info_t *info) {
  esphome::snifzig_capture::dispatch_received_frame(frame,info);
}
