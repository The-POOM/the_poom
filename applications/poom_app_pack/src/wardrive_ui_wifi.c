// SPDX-License-Identifier: MIT
#include "wardrive_ui_wifi.h"

#include "wardrive_wifi.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "poom_secrets_store.h"
#include "nvs.h"

bool wd_ui_wifi_password_valid(const char *password) {
    if(!password) return true;
    size_t n=strnlen(password,65);
    if(!n||(n>=8&&n<=63)) return true;
    if(n!=64) return false;
    for(size_t i=0;i<n;i++) if(!isxdigit((unsigned char)password[i])) return false;
    return true;
}
esp_err_t wd_ui_wifi_init_transfer(void) {
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    /* Small sequential file transfers need headroom for verified TLS, not
     * the default ten permanently allocated 1.6 KiB receive buffers. */
    config.static_rx_buf_num=2; config.dynamic_rx_buf_num=8;
    if(config.dynamic_tx_buf_num>8) config.dynamic_tx_buf_num=8;
    config.rx_ba_win=2; config.mgmt_sbuf_num=6;
    config.rx_mgmt_buf_num=2; /* bounded static management RX cache */
    return wd_wifi_init_sta_with_config(&config);
}
bool wd_ui_wifi_matches(const char *ssid,const char *password) {
    if(!ssid||!ssid[0]||strnlen(ssid,33)>32||!wd_ui_wifi_password_valid(password)) return false;
    wifi_ap_record_t ap={0}; wifi_config_t config={0};
    bool ok=esp_wifi_sta_get_ap_info(&ap)==ESP_OK&&esp_wifi_get_config(WIFI_IF_STA,&config)==ESP_OK;
    size_t n=strlen(ssid),p=password?strlen(password):0;
    ok=ok&&strnlen((const char *)ap.ssid,32)==n&&!memcmp(ap.ssid,ssid,n)&&
        strnlen((const char *)config.sta.ssid,32)==n&&!memcmp(config.sta.ssid,ssid,n)&&
        strnlen((const char *)config.sta.password,64)==p&&(!p||!memcmp(config.sta.password,password,p));
    volatile unsigned char *bytes=(volatile unsigned char *)&config;
    for(size_t i=0;i<sizeof(config);i++) bytes[i]=0;
    return ok;
}
esp_err_t wd_ui_wifi_connect(const char *ssid,const char *password) {
    if(!ssid||!ssid[0]||strnlen(ssid,33)>32||!wd_ui_wifi_password_valid(password)) return ESP_ERR_INVALID_ARG;
    size_t name_len=strlen(ssid),pass_len=password?strlen(password):0;
    if(name_len<32&&pass_len&&pass_len<64) return wd_wifi_sta_connect(ssid,password);
    /* Reuse manager lifecycle/events/RAM storage. Its legacy string copier
     * reserves a NUL byte and its default threshold excludes open networks.
     * Handle only those edge cases locally without changing shared callers. */
    esp_err_t status=wd_wifi_init_sta(); if(status!=ESP_OK) return status;
    wifi_config_t config={0}; memcpy(config.sta.ssid,ssid,name_len);
    if(pass_len) memcpy(config.sta.password,password,pass_len);
    config.sta.threshold.authmode=pass_len?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    status=esp_wifi_set_config(WIFI_IF_STA,&config);
    volatile unsigned char *bytes=(volatile unsigned char *)&config;
    for(size_t i=0;i<sizeof(config);i++) bytes[i]=0;
    if(status!=ESP_OK) return status;
    (void)esp_wifi_disconnect(); return esp_wifi_connect();
}

void wd_ui_profiles_close(wd_ui_profiles_t *p) {
    if(p->items) {
        volatile unsigned char *bytes=(volatile unsigned char *)p->items;
        for(size_t i=0;i<POOM_WIFI_PROFILE_MAX*sizeof(*p->items);i++) bytes[i]=0;
        free(p->items);
    }
    p->items=NULL; p->count=0;
}
bool wd_ui_profiles_open(wd_ui_profiles_t *p) {
    wd_ui_profiles_close(p);
    p->items=calloc(POOM_WIFI_PROFILE_MAX,sizeof(*p->items));
    if(!p->items) return false;
    poom_wifi_profile_store_load(p->items,POOM_WIFI_PROFILE_MAX,&p->count);
    if(p->count>POOM_WIFI_PROFILE_MAX) { wd_ui_profiles_close(p); return false; }
    return true;
}
bool wd_ui_profile_pick(const wd_ui_profiles_t *p,unsigned index,wd_credentials_t *out) {
    if(!p->items||index>=p->count||!p->items[index].ssid[0]) return false;
    snprintf(out->ssid,sizeof(out->ssid),"%.32s",p->items[index].ssid);
    snprintf(out->password,sizeof(out->password),"%.63s",p->items[index].pass);
    out->auto_upload=false; /* selecting a network does not authorise uploads */
    return true;
}
esp_err_t wd_ui_profile_save(const wd_credentials_t *credentials) {
    if(!credentials->ssid[0]||!wd_ui_wifi_password_valid(credentials->password)) return ESP_ERR_INVALID_ARG;
    /* Preserve the shared profile wire format: it has 63 password characters. */
    if(strlen(credentials->password)>=POOM_WIFI_PROFILE_PASS_CAP) return ESP_ERR_NOT_SUPPORTED;
    uint32_t expected=0;
    esp_err_t status=poom_secrets_get_u32("wifi_prof_n",&expected);
    if(status!=ESP_OK&&status!=ESP_ERR_NVS_NOT_FOUND) return status;
    if(expected>POOM_WIFI_PROFILE_MAX) return ESP_ERR_INVALID_STATE;
    wd_ui_profiles_t profiles={0};
    if(!wd_ui_profiles_open(&profiles)) return ESP_ERR_NO_MEM;
    if(profiles.count!=expected) status=ESP_ERR_INVALID_STATE;
    else status=poom_wifi_profile_store_add_or_update(profiles.items,&profiles.count,POOM_WIFI_PROFILE_MAX,credentials->ssid,credentials->password);
    wd_ui_profiles_close(&profiles); return status;
}
#define WD_UI_NETWORK_MAX 20U
static wifi_ap_record_t *networks;
static uint16_t network_count;
void wd_ui_networks_clear(void) { free(networks); networks=NULL; network_count=0; }
unsigned wd_ui_network_count(void) { return network_count; }
bool wd_ui_network_get(unsigned index,wd_ui_network_t *network) {
    if(index>=wd_ui_network_count()) return false;
    wifi_ap_record_t *ap=&networks[index];
    memset(network,0,sizeof(*network));
    memcpy(network->ssid,ap->ssid,strnlen((const char *)ap->ssid,32));
    network->rssi=ap->rssi;
    network->open=ap->authmode==WIFI_AUTH_OPEN;
    /* Enterprise/WEP provisioning is outside this password-only chooser. */
    network->personal=ap->authmode==WIFI_AUTH_WPA2_PSK||ap->authmode==WIFI_AUTH_WPA_WPA2_PSK||
        ap->authmode==WIFI_AUTH_WPA3_PSK||ap->authmode==WIFI_AUTH_WPA2_WPA3_PSK;
    return true;
}
bool wd_ui_wifi_scan(wd_ui_job_t *job) {
    wd_ui_disconnect(); wd_ui_networks_clear();
    esp_err_t status=wd_wifi_init_sta();
#if CONFIG_IDF_TARGET_ESP32C5
    if(status==ESP_OK) status=esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    wifi_scan_config_t config={0}; config.scan_type=WIFI_SCAN_TYPE_PASSIVE;
    config.show_hidden=true; config.scan_time.passive=200;
    if(status==ESP_OK&&!atomic_load(&job->cancel)) {
        networks=calloc(WD_UI_NETWORK_MAX,sizeof(*networks));
        if(!networks) status=ESP_ERR_NO_MEM;
        else {
            status=esp_wifi_scan_start(&config,true);
            if(status==ESP_OK) {
                network_count=WD_UI_NETWORK_MAX;
                status=esp_wifi_scan_get_ap_records(&network_count,networks);
                if(network_count>WD_UI_NETWORK_MAX) { network_count=0; status=ESP_FAIL; }
            }
            (void)esp_wifi_clear_ap_list();
        }
    }
    bool ok=status==ESP_OK&&!atomic_load(&job->cancel);
    if(!ok) { wd_ui_networks_clear(); strcpy(job->message,atomic_load(&job->cancel)?"Scan cancelled":"WiFi scan failed"); }
    wd_ui_disconnect(); return ok;
}
