// SPDX-License-Identifier: MIT
#include "observer_platform.h"
#include "poom_wifi_observer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,calls,fail_at,deinits,tunes;
static bool enabled,connected,heap_fail,reject_all,deinit_fail;
static uint32_t clock_ms;
static wifi_promiscuous_cb_t callback;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static esp_err_t step(void){return ++calls==fail_at?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_get_promiscuous(bool *value){*value=enabled;return ESP_OK;}
esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *value){CHECK(value->filter_mask==3);return step();}
esp_err_t poom_wifi_ctrl_init_null(void){return step();}
esp_err_t poom_wifi_ctrl_deinit(void){++deinits;if(deinit_fail)return ESP_FAIL;enabled=false;return ESP_OK;}
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel){CHECK(channel>0);++tunes;return reject_all?ESP_FAIL:step();}
esp_err_t poom_wifi_ctrl_set_promiscuous(bool value){esp_err_t err=step();if(err==ESP_OK)enabled=value;return err;}
esp_err_t poom_wifi_ctrl_set_promiscuous_rx_cb(wifi_promiscuous_cb_t value){esp_err_t err=step();if(err==ESP_OK)callback=value;return err;}
bool poom_wifi_ctrl_sta_is_connected(void){return connected;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){CHECK(caps==3);return heap_fail?NULL:calloc(n,size);}
int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
static void reset(void){calls=fail_at=deinits=tunes=clock_ms=0;enabled=connected=heap_fail=reject_all=deinit_fail=false;callback=NULL;}
int main(void){
    poom_wifi_observer_t *s=NULL,*other=NULL;
    reset();CHECK(poom_wifi_observer_start(NULL)==ESP_ERR_INVALID_ARG);
    enabled=true;CHECK(poom_wifi_observer_start(&s)==ESP_ERR_INVALID_STATE);CHECK(deinits==0);
    enabled=false;connected=true;CHECK(poom_wifi_observer_start(&s)==ESP_ERR_INVALID_STATE);CHECK(deinits==0);
    connected=false;heap_fail=true;CHECK(poom_wifi_observer_start(&s)==ESP_ERR_NO_MEM);
    for(unsigned failure=1;failure<=4;++failure){reset();fail_at=failure;CHECK(poom_wifi_observer_start(&s)==ESP_FAIL);CHECK(!s);CHECK(deinits==1);CHECK(!enabled);}
    reset();reject_all=true;CHECK(poom_wifi_observer_start(&s)==ESP_FAIL);CHECK(!s);CHECK(deinits==1);CHECK(tunes>1);
    reset();CHECK(poom_wifi_observer_start(&s)==ESP_OK);CHECK(s && callback && enabled);
    CHECK(poom_wifi_observer_start(&other)==ESP_ERR_INVALID_STATE);CHECK(deinits==0);
    wifi_promiscuous_pkt_t packet={0};packet.rx_ctrl.sig_len=40+4;packet.rx_ctrl.channel=6;packet.rx_ctrl.rssi=-55;
    packet.payload[0]=0x80;packet.payload[16]=2;packet.payload[21]=1;packet.payload[36]=0;packet.payload[37]=2;packet.payload[38]='a';packet.payload[39]='p';
    for(unsigned i=0;i<25;++i)callback(&packet,WIFI_PKT_MGMT);
    CHECK(poom_wifi_observer_dropped(s)==15);CHECK(poom_wifi_observer_model(s)->ap_count==0);
    poom_wifi_observer_poll(s);CHECK(poom_wifi_observer_model(s)->ap_count==1);CHECK(poom_wifi_observer_model(s)->frames==10);
    CHECK(poom_wifi_observer_focus(s,6)==ESP_OK);unsigned previous=tunes;clock_ms=1000;poom_wifi_observer_poll(s);CHECK(tunes==previous);
    reject_all=true;CHECK(poom_wifi_observer_focus(s,11)==ESP_FAIL);CHECK(poom_wifi_observer_channel(s)==6);
    reject_all=false;CHECK(poom_wifi_observer_focus(s,0)==ESP_OK);poom_wifi_observer_poll(s);CHECK(tunes>previous);
    wifi_promiscuous_cb_t late=callback;poom_wifi_observer_stop(&s);CHECK(!s && !enabled && !callback);CHECK(deinits==1);
    late(&packet,WIFI_PKT_MGMT);poom_wifi_observer_stop(&s);CHECK(deinits==1);
    CHECK(poom_wifi_observer_start(&s)==ESP_OK);poom_wifi_observer_stop(&s);
    reset();CHECK(poom_wifi_observer_start(&s)==ESP_OK);fail_at=calls+1U;
    CHECK(poom_wifi_observer_stop(&s)==ESP_FAIL);CHECK(s);CHECK(poom_wifi_observer_start(&other)==ESP_ERR_INVALID_STATE);
    CHECK(poom_wifi_observer_focus(s,6)==ESP_ERR_INVALID_STATE);CHECK(poom_wifi_observer_stop(&s)==ESP_OK);CHECK(!s);
    reset();CHECK(poom_wifi_observer_start(&s)==ESP_OK);deinit_fail=true;
    CHECK(poom_wifi_observer_stop(&s)==ESP_FAIL);CHECK(s);CHECK(poom_wifi_observer_stop(&s)==ESP_FAIL);
    deinit_fail=false;CHECK(poom_wifi_observer_stop(&s)==ESP_OK);CHECK(!s);
    reset();fail_at=1;deinit_fail=true;CHECK(poom_wifi_observer_start(&s)==ESP_FAIL);CHECK(s);
    deinit_fail=false;CHECK(poom_wifi_observer_stop(&s)==ESP_OK);CHECK(!s);
    printf("Observer lifecycle: %u checks passed\n",checks);return 0;
}
