// SPDX-License-Identifier: MIT
#include "wardrive_ui_wifi.h"
#include "poom_wifi_scanner.h"
#include <assert.h>
#include <string.h>
#include "nvs.h"
static poom_wifi_scanner_ap_records_t records;
static unsigned loads,disconnects,scan_calls;
static int failure;
static bool corrupt_count;
static wd_ui_job_t *active;
static wifi_config_t configured;
static wifi_init_config_t transfer_profile;
esp_err_t wd_wifi_init_sta_with_config(const wifi_init_config_t *c) { transfer_profile=*c;return ESP_OK; }
static bool live_ap=true;
esp_err_t esp_wifi_get_config(int mode,wifi_config_t *out) { assert(mode==WIFI_IF_STA);*out=configured;return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) { memset(out,0,sizeof(*out));memcpy(out->ssid,configured.sta.ssid,32);return live_ap?ESP_OK:ESP_FAIL; }
static unsigned standard_connects,exact_connects;
static unsigned saves;
static uint32_t expected_profiles=2;
static esp_err_t metadata_status=ESP_OK,save_status=ESP_OK;
esp_err_t poom_secrets_get_u32(const char *key,uint32_t *out) { assert(!strcmp(key,"wifi_prof_n"));*out=expected_profiles;return metadata_status; }
esp_err_t poom_wifi_profile_store_add_or_update(poom_wifi_profile_t *p,uint16_t *count,uint16_t cap,const char *ssid,const char *password) {
    assert(p&&(*count==2||!*count)&&cap==12&&!strcmp(ssid,"saved-second")&&!strcmp(password,"synthetic-second"));
    if(save_status==ESP_OK) saves++;
    return save_status;
}
esp_err_t wd_wifi_sta_connect(const char *ssid,const char *password) { assert(strlen(ssid)<32&&strlen(password)<64);standard_connects++;return ESP_OK; }
esp_err_t esp_wifi_set_config(int mode,const wifi_config_t *config) { assert(mode==WIFI_IF_STA);configured=*config;return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { return ESP_OK; }
esp_err_t esp_wifi_connect(void) { exact_connects++;return ESP_OK; }
void poom_wifi_profile_store_load(poom_wifi_profile_t *out,uint16_t cap,uint16_t *count) {
    assert(cap==12); loads++;
    if(metadata_status==ESP_ERR_NVS_NOT_FOUND) { *count=0; return; }
    strcpy(out[0].ssid,"saved-first"); strcpy(out[0].pass,"synthetic-first");
    strcpy(out[1].ssid,"saved-second"); strcpy(out[1].pass,"synthetic-second");
    *count=corrupt_count?13:2;
}
void wd_ui_disconnect(void) { disconnects++; }
esp_err_t wd_wifi_init_sta(void) { return failure==1?ESP_FAIL:ESP_OK; }
esp_err_t esp_wifi_set_band_mode(int mode) { assert(mode==WIFI_BAND_MODE_AUTO); return failure==2?ESP_FAIL:ESP_OK; }
esp_err_t poom_wifi_scanner_clear_ap_records(void) { memset(&records,0,sizeof(records)); return ESP_OK; }
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void) { return &records; }
wifi_ap_record_t *poom_wifi_scanner_get_ap_record(unsigned index) { return index<records.count&&index<20?&records.records[index]:NULL; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config,bool blocking) {
    assert(blocking);
    assert(config->scan_type==WIFI_SCAN_TYPE_PASSIVE&&config->show_hidden&&config->scan_time.passive==200);
    scan_calls++; records.count=2;
    strcpy((char *)records.records[0].ssid,"selected-network");records.records[0].authmode=WIFI_AUTH_WPA2_PSK;records.records[0].rssi=-60;
    records.records[1].authmode=WIFI_AUTH_OPEN;
    if(failure==4)atomic_store(&active->cancel,true);
    return failure==3?ESP_FAIL:ESP_OK;
}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *out) {
    if(*count>records.count) *count=records.count;
    memcpy(out,records.records,*count*sizeof(*out));return ESP_OK;
}
esp_err_t esp_wifi_clear_ap_list(void) { return ESP_OK; }
int main(void) {
    assert(wd_ui_wifi_init_transfer()==ESP_OK&&transfer_profile.static_rx_buf_num==2&&transfer_profile.dynamic_rx_buf_num==8&&transfer_profile.dynamic_tx_buf_num==8&&transfer_profile.rx_ba_win==2&&transfer_profile.mgmt_sbuf_num==6&&transfer_profile.rx_mgmt_buf_num==2);
    assert(transfer_profile.magic==12345&&transfer_profile.feature_caps==123);
    wd_ui_profiles_t profiles={0}; assert(wd_ui_profiles_open(&profiles)&&profiles.count==2&&loads==1);
    wd_credentials_t credentials={.auto_upload=true};strcpy(credentials.api_token,"synthetic-api");
    assert(!wd_ui_profile_pick(&profiles,2,&credentials));
    assert(wd_ui_profile_pick(&profiles,1,&credentials));
    assert(!strcmp(credentials.ssid,"saved-second")&&!strcmp(credentials.password,"synthetic-second"));
    assert(!strcmp(credentials.api_token,"synthetic-api")&&!credentials.auto_upload);
    assert(!saves&&wd_ui_profile_save(&credentials)==ESP_OK&&saves==1);
    metadata_status=ESP_FAIL;assert(wd_ui_profile_save(&credentials)==ESP_FAIL&&saves==1);metadata_status=ESP_OK;
    expected_profiles=3;assert(wd_ui_profile_save(&credentials)==ESP_ERR_INVALID_STATE&&saves==1);expected_profiles=2;
    save_status=ESP_ERR_NO_MEM;assert(wd_ui_profile_save(&credentials)==ESP_ERR_NO_MEM&&saves==1);save_status=ESP_OK;
    char original[65];strcpy(original,credentials.password);memset(credentials.password,'A',64);credentials.password[64]=0;
    assert(wd_ui_profile_save(&credentials)==ESP_ERR_NOT_SUPPORTED&&saves==1);strcpy(credentials.password,original);
    metadata_status=ESP_ERR_NVS_NOT_FOUND;expected_profiles=0;
    assert(wd_ui_profile_save(&credentials)==ESP_OK&&saves==2);
    metadata_status=ESP_OK;expected_profiles=2;
    assert(wd_ui_wifi_connect(credentials.ssid,credentials.password)==ESP_OK&&standard_connects==1);
    char full_ssid[33],psk[65];memset(full_ssid,'S',32);full_ssid[32]=0;memset(psk,'A',64);psk[64]=0;
    assert(wd_ui_wifi_connect(full_ssid,psk)==ESP_OK&&exact_connects==1);
    assert(!memcmp(configured.sta.ssid,full_ssid,32)&&!memcmp(configured.sta.password,psk,64));
    assert(configured.sta.threshold.authmode==WIFI_AUTH_WPA2_PSK);
    assert(wd_ui_wifi_matches(full_ssid,psk));psk[0]='B';assert(!wd_ui_wifi_matches(full_ssid,psk));psk[0]='A';
    assert(!wd_ui_wifi_matches("different-network",psk));
    live_ap=false;assert(!wd_ui_wifi_matches(full_ssid,psk));live_ap=true;
    assert(wd_ui_wifi_connect("open-network","")==ESP_OK&&configured.sta.threshold.authmode==WIFI_AUTH_OPEN);
    assert(wd_ui_wifi_matches("open-network","")&&!wd_ui_wifi_matches("open-network","synthetic-password"));
    assert(wd_ui_wifi_connect("test","short")==ESP_ERR_INVALID_ARG);psk[0]='Z';assert(!wd_ui_wifi_password_valid(psk));
    wd_ui_profiles_close(&profiles);assert(!profiles.items&&!profiles.count);
    corrupt_count=true;assert(!wd_ui_profiles_open(&profiles)&&!profiles.items);corrupt_count=false;
    wd_ui_job_t job={0};active=&job;
    assert(wd_ui_wifi_scan(&job)&&disconnects==2&&scan_calls==1&&wd_ui_network_count()==2);
    wd_ui_network_t network;assert(wd_ui_network_get(0,&network)&&network.personal&&network.rssi==-60&&!strcmp(network.ssid,"selected-network"));
    assert(wd_ui_network_get(1,&network)&&network.open&&!network.personal&&!network.ssid[0]);
    assert(!wd_ui_network_get(2,&network));wd_ui_networks_clear();assert(!wd_ui_network_count());
    for(failure=1;failure<=4;failure++) {
        atomic_store(&job.cancel,false);job.message[0]=0;
        assert(!wd_ui_wifi_scan(&job)&&!wd_ui_network_count()&&job.message[0]);
    }
    failure=0;atomic_store(&job.cancel,true);unsigned before=scan_calls;
    assert(!wd_ui_wifi_scan(&job)&&scan_calls==before&&!strcmp(job.message,"Scan cancelled"));
    wd_ui_networks_clear();
    puts("Shared-profile picker: read-only reuse, bounded cache, credential selection, passive dual-band scan and failure/cancel cleanup passed");
}
