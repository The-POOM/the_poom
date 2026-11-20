// SPDX-License-Identifier: MIT
#include "poom_wifi_csi.h"
#include "CsiMotionModel.h"
#include "poom_wifi_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <cstring>
#include <cstdlib>
#include <new>

static constexpr size_t kSamples = 12, kBytes = 256;
struct csi_sample_t {
    uint32_t time_ms;
    uint16_t length;
    int8_t rssi;
    bool first_invalid;
    int8_t bytes[kBytes];
};
struct poom_wifi_csi {
    CsiMotionModel model;
    csi_sample_t queue[kSamples], scratch;
    uint8_t target[6], channel, head, tail, count;
    uint32_t accepted, dropped;
    bool radio_ready, closing;
};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static poom_wifi_csi_t *s_receiver;
static bool s_claimed;
static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time()/1000); }
static void increment(uint32_t &value) { if(value != UINT32_MAX) ++value; }

#if CONFIG_IDF_TARGET_ESP32C5 && CONFIG_ESP_WIFI_CSI_ENABLED
static void receive(void *, wifi_csi_info_t *info) {
    if(!info || !info->buf || info->len < 16U || info->rx_ctrl.rx_state) return;
    const uint32_t now=now_ms();
    portENTER_CRITICAL(&s_lock);
    poom_wifi_csi_t *s=s_receiver;
    if(s && std::memcmp(info->mac,s->target,6U)==0 && info->rx_ctrl.channel==s->channel) {
        if(s->count == kSamples) increment(s->dropped);
        else {
            csi_sample_t &item=s->queue[s->head];
            item.length=info->len<kBytes?info->len:kBytes;
            item.length &= ~3U; /* complete complex tones only */
            item.first_invalid=info->first_word_invalid;item.rssi=info->rx_ctrl.rssi;item.time_ms=now;
            std::memcpy(item.bytes,info->buf,item.length);
            s->head=(s->head+1U)%kSamples;++s->count;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}
#endif
esp_err_t poom_wifi_csi_start(poom_wifi_csi_t **out,const uint8_t bssid[6],uint8_t channel) {
    if(!out || *out || !bssid || (bssid[0]&1U) || !poom_wifi_ctrl_channel_is_valid(channel)) return ESP_ERR_INVALID_ARG;
#if !CONFIG_IDF_TARGET_ESP32C5 || !CONFIG_ESP_WIFI_CSI_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    static const uint8_t zero[6]={}; if(std::memcmp(bssid,zero,6U)==0) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);const bool busy=s_claimed;if(!busy)s_claimed=true;portEXIT_CRITICAL(&s_lock);
    if(busy)return ESP_ERR_INVALID_STATE;
    bool promisc=false;(void)esp_wifi_get_promiscuous(&promisc);
    if(promisc || poom_wifi_ctrl_sta_is_connected()) {
        portENTER_CRITICAL(&s_lock);s_claimed=false;portEXIT_CRITICAL(&s_lock);return ESP_ERR_INVALID_STATE;
    }
    void *memory=heap_caps_calloc(1U,sizeof(poom_wifi_csi_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory){portENTER_CRITICAL(&s_lock);s_claimed=false;portEXIT_CRITICAL(&s_lock);return ESP_ERR_NO_MEM;}
    auto *s=new(memory) poom_wifi_csi_t{};*out=s;std::memcpy(s->target,bssid,6);s->channel=channel;
    s->model.reset(now_ms());
    s->radio_ready=true; /* Controller init can fail after starting the driver. */
    esp_err_t error=poom_wifi_ctrl_init_sta();
    if(error!=ESP_OK){poom_wifi_csi_stop(out);return error;}
    error=poom_wifi_ctrl_set_channel(channel);
    if(error==ESP_OK)error=poom_wifi_ctrl_set_promiscuous(true);
    wifi_csi_config_t config={};
    config.enable=1;config.acquire_csi_legacy=1;config.acquire_csi_force_lltf=1;
    config.acquire_csi_ht20=1;config.acquire_csi_ht40=1;config.acquire_csi_vht=1;
    config.acquire_csi_su=1;config.acquire_csi_mu=1;config.acquire_csi_dcm=1;config.acquire_csi_beamformed=1;
    config.acquire_csi_he_stbc_mode=ESP_CSI_ACQUIRE_STBC_HELTF1;
    config.lltf_bit_mode=0; /* IDF 6.1 C5: explicitly request signed 12-bit L-LTF. */
    if(error==ESP_OK)error=esp_wifi_set_csi_config(&config);
    if(error==ESP_OK)error=esp_wifi_set_csi_rx_cb(receive,nullptr);
    portENTER_CRITICAL(&s_lock);s_receiver=s;portEXIT_CRITICAL(&s_lock);
    if(error==ESP_OK)error=esp_wifi_set_csi(true);
    if(error!=ESP_OK)poom_wifi_csi_stop(out);
    return error;
#endif
}
void poom_wifi_csi_recalibrate(poom_wifi_csi_t *s) {
    if(!s || s->closing)return;
    portENTER_CRITICAL(&s_lock);s->head=s->tail=s->count=0;s->accepted=s->dropped=0;portEXIT_CRITICAL(&s_lock);
    s->model.reset(now_ms());
}
void poom_wifi_csi_poll(poom_wifi_csi_t *s,poom_csi_status_t *status) {
    if(!s || !status || s->closing)return;
    for(size_t i=0;i<kSamples;++i) {
        portENTER_CRITICAL(&s_lock);const bool ready=s->count!=0U;
        if(ready){s->scratch=s->queue[s->tail];s->tail=(s->tail+1U)%kSamples;--s->count;}
        portEXIT_CRITICAL(&s_lock);if(!ready)break;
        const auto &item=s->scratch;
        const bool valid=s->model.addSample(item.bytes,item.length,item.rssi,item.time_ms,item.first_invalid,CsiMotionModel::SampleEncoding::SIGNED_12_LE);
        portENTER_CRITICAL(&s_lock);if(valid)increment(s->accepted);else increment(s->dropped);portEXIT_CRITICAL(&s_lock);
    }
    portENTER_CRITICAL(&s_lock);const uint32_t accepted=s->accepted,dropped=s->dropped;portEXIT_CRITICAL(&s_lock);
    const uint32_t now=now_ms();s->model.tick(now,accepted,dropped);
    status->state=static_cast<poom_csi_state_t>(s->model.state());status->calibration=s->model.calibrationPercent(now);
    status->score=s->model.motionScore();status->reliability=s->model.reliability();status->amplitude=s->model.amplitudeScore();
    status->phase=s->model.phaseScore();status->correlation=s->model.correlationScore();status->channel=s->channel;
    status->samples_per_second=s->model.samplesPerSecond();status->rssi=s->model.rssi();status->accepted=accepted;
    status->dropped=dropped;status->calibrated=s->model.calibrated();
    for(size_t i=0;i<64U;++i)status->history[i]=s->model.history()[(s->model.historyHead()+CsiMotionModel::kHistorySize-64U+i)%CsiMotionModel::kHistorySize];
}
static bool stopped(esp_err_t error) {
    return error==ESP_OK || error==ESP_ERR_WIFI_NOT_INIT || error==ESP_ERR_WIFI_NOT_STARTED;
}
esp_err_t poom_wifi_csi_stop(poom_wifi_csi_t **session) {
    if(!session || !*session)return ESP_OK;
    auto *s=*session;
    s->closing=true;
    portENTER_CRITICAL(&s_lock);if(s_receiver==s)s_receiver=nullptr;portEXIT_CRITICAL(&s_lock);
    if(s->radio_ready) {
#if CONFIG_ESP_WIFI_CSI_ENABLED
        const esp_err_t disable=esp_wifi_set_csi(false),detach=esp_wifi_set_csi_rx_cb(nullptr,nullptr);
#endif
        const esp_err_t promisc=poom_wifi_ctrl_set_promiscuous(false),deinit=poom_wifi_ctrl_deinit();
#if CONFIG_ESP_WIFI_CSI_ENABLED
        if(!stopped(disable))return disable;
        if(!stopped(detach))return detach;
#endif
        if(!stopped(promisc))return promisc;
        if(!stopped(deinit))return deinit;
    }
    s->~poom_wifi_csi();std::free(s);*session=nullptr;
    portENTER_CRITICAL(&s_lock);s_claimed=false;portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
