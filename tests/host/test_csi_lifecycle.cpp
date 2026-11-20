// SPDX-License-Identifier: MIT
#include "observer_platform.h"
#include "poom_wifi_csi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned checks,calls,fail_at,deinits;
static bool enabled,connected,heap_fail,deinit_fail;
static uint32_t clock_ms;
static wifi_csi_cb_t callback;
static const uint8_t ap[6]={2,0x11,0x22,0x33,0x44,0x55};
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
static esp_err_t step(){return ++calls==fail_at?ESP_FAIL:ESP_OK;}
extern "C" {
esp_err_t esp_wifi_get_promiscuous(bool *value){*value=enabled;return ESP_OK;}
esp_err_t poom_wifi_ctrl_init_sta(){return step();}
esp_err_t poom_wifi_ctrl_deinit(){++deinits;if(deinit_fail)return ESP_FAIL;enabled=false;return ESP_OK;}
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel){CHECK(channel==6);return step();}
bool poom_wifi_ctrl_channel_is_valid(uint8_t channel){return channel>=1 && channel<=13;}
esp_err_t poom_wifi_ctrl_set_promiscuous(bool value){esp_err_t err=step();if(err==ESP_OK)enabled=value;return err;}
bool poom_wifi_ctrl_sta_is_connected(){return connected;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){CHECK(caps==3);return heap_fail?nullptr:std::calloc(n,size);}
int64_t esp_timer_get_time(){return (int64_t)clock_ms*1000;}
esp_err_t esp_wifi_set_csi_config(const wifi_csi_config_t *config){CHECK(config->lltf_bit_mode==0 && config->acquire_csi_force_lltf==1 && config->enable==1);return step();}
esp_err_t esp_wifi_set_csi_rx_cb(wifi_csi_cb_t cb,void *){esp_err_t err=step();if(err==ESP_OK)callback=cb;return err;}
esp_err_t esp_wifi_set_csi(bool){return step();}
}
static void reset(){calls=fail_at=deinits=clock_ms=0;enabled=connected=heap_fail=deinit_fail=false;callback=nullptr;}
int main(){
    poom_wifi_csi_t *s=nullptr,*other=nullptr;
    reset();CHECK(poom_wifi_csi_start(nullptr,ap,6)==ESP_ERR_INVALID_ARG);
    CHECK(poom_wifi_csi_start(&s,ap,0)==ESP_ERR_INVALID_ARG);
    enabled=true;CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_ERR_INVALID_STATE);CHECK(deinits==0);
    enabled=false;connected=true;CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_ERR_INVALID_STATE);CHECK(deinits==0);
    connected=false;heap_fail=true;CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_ERR_NO_MEM);
    for(unsigned failure=1;failure<=6;++failure){reset();fail_at=failure;CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_FAIL);CHECK(!s);CHECK(deinits==1);CHECK(!enabled);}
    reset();CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_OK);CHECK(s && callback && enabled);
    CHECK(poom_wifi_csi_start(&other,ap,6)==ESP_ERR_INVALID_STATE);
    int8_t bytes[300];for(unsigned i=0;i<sizeof(bytes);i+=4){bytes[i]=80;bytes[i+1]=0;bytes[i+2]=20;bytes[i+3]=0;}
    wifi_csi_info_t sample={};sample.rx_ctrl.channel=6;sample.rx_ctrl.rssi=-50;std::memcpy(sample.mac,ap,6);sample.buf=bytes;sample.len=sizeof(bytes);
    for(unsigned i=0;i<30;++i)callback(nullptr,&sample);
    poom_csi_status_t status={};poom_wifi_csi_poll(s,&status);CHECK(status.accepted==12);CHECK(status.dropped==18);CHECK(!status.calibrated);
    sample.mac[5]++;callback(nullptr,&sample);sample.mac[5]--;sample.rx_ctrl.channel=11;callback(nullptr,&sample);
    sample.rx_ctrl.channel=6;sample.len=8;callback(nullptr,&sample);poom_wifi_csi_poll(s,&status);CHECK(status.accepted==12);
    poom_wifi_csi_recalibrate(s);poom_wifi_csi_poll(s,&status);CHECK(status.accepted==0 && status.dropped==0 && status.calibration==0);
    wifi_csi_cb_t late=callback;poom_wifi_csi_stop(&s);CHECK(!s && !enabled && !callback);CHECK(deinits==1);late(nullptr,&sample);
    poom_wifi_csi_stop(&s);CHECK(deinits==1);CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_OK);poom_wifi_csi_stop(&s);
    reset();CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_OK);fail_at=calls+1U;
    CHECK(poom_wifi_csi_stop(&s)==ESP_FAIL);CHECK(s);CHECK(poom_wifi_csi_start(&other,ap,6)==ESP_ERR_INVALID_STATE);
    CHECK(poom_wifi_csi_stop(&s)==ESP_OK);CHECK(!s);
    reset();CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_OK);deinit_fail=true;
    CHECK(poom_wifi_csi_stop(&s)==ESP_FAIL);CHECK(s);CHECK(poom_wifi_csi_stop(&s)==ESP_FAIL);
    deinit_fail=false;CHECK(poom_wifi_csi_stop(&s)==ESP_OK);CHECK(!s);
    reset();fail_at=1;deinit_fail=true;CHECK(poom_wifi_csi_start(&s,ap,6)==ESP_FAIL);CHECK(s);
    deinit_fail=false;CHECK(poom_wifi_csi_stop(&s)==ESP_OK);CHECK(!s);
    std::printf("CSI lifecycle: %u checks passed\n",checks);return 0;
}
