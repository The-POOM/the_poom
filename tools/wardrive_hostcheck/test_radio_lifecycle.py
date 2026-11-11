"""Compile the real private adapters; exercise ownership, failures and async BLE."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

COMMON = r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_ARG,
       ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_WIFI_NOT_INIT=0x3001 };
typedef const char *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define IP_EVENT ((esp_event_base_t)2)
enum { ESP_EVENT_ANY_ID=-1, WIFI_EVENT_STA_CONNECTED=1, WIFI_EVENT_STA_DISCONNECTED,
       IP_EVENT_STA_GOT_IP, WIFI_MODE_STA, WIFI_STORAGE_RAM, WIFI_IF_STA,
       WIFI_AUTH_WPA2_PSK, WIFI_AUTH_OPEN };
typedef int wifi_mode_t;
typedef struct { int marker; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 99 }
typedef struct { struct { unsigned char ssid[32],password[64]; struct { int authmode; } threshold; } sta; } wifi_config_t;
enum { ESP_NETIF_DNS_MAX=3, ESP_NETIF_DHCP_STARTED=1 };
typedef int esp_netif_dns_type_t;
typedef int esp_netif_dhcp_status_t;
typedef struct { unsigned marker; } esp_netif_dns_info_t;
typedef struct { unsigned addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip,netmask,gw; } esp_netif_ip_info_t;
typedef struct { int identity; } esp_netif_t;
typedef struct { esp_netif_t *esp_netif; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef struct { int reason; } wifi_event_sta_disconnected_t;
enum { POOM_WIFI_CTRL_EVT_STA_CONNECTED, POOM_WIFI_CTRL_EVT_STA_DISCONNECTED, POOM_WIFI_CTRL_EVT_STA_GOT_IP };
typedef struct { int evt,reason; esp_ip4_addr_t ip,netmask,gw; } poom_wifi_ctrl_evt_info_t;
typedef void (*poom_wifi_ctrl_evt_cb_t)(const poom_wifi_ctrl_evt_info_t *,void *);
typedef void *esp_event_handler_instance_t;
typedef int esp_bt_controller_status_t;
enum { ESP_BT_CONTROLLER_STATUS_IDLE,ESP_BT_CONTROLLER_STATUS_INITED,ESP_BT_CONTROLLER_STATUS_ENABLED,
       ESP_BLUEDROID_STATUS_UNINITIALIZED,ESP_BLUEDROID_STATUS_INITIALIZED,ESP_BLUEDROID_STATUS_ENABLED,
       ESP_BT_MODE_BLE,ESP_BT_STATUS_SUCCESS=0 };
typedef int esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() 1
typedef int esp_bluedroid_config_t;
#define BT_BLUEDROID_INIT_CONFIG_DEFAULT() 1
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
typedef int esp_gap_ble_cb_event_t;
enum { ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,
       ESP_GAP_BLE_SCAN_RESULT_EVT,ESP_GAP_SEARCH_INQ_RES_EVT,BLE_SCAN_TYPE_PASSIVE,
       BLE_ADDR_TYPE_PUBLIC,BLE_SCAN_FILTER_ALLOW_ALL,BLE_SCAN_DUPLICATE_DISABLE };
typedef struct { struct { int status; } scan_param_cmpl,scan_start_cmpl;
                 struct { int search_evt; } scan_rst; } esp_ble_gap_cb_param_t;
typedef struct { int scan_type,own_addr_type,scan_filter_policy,scan_interval,scan_window,scan_duplicate; } esp_ble_scan_params_t;
'''

WIFI = r'''
static int failure, step, ctrl=ESP_BT_CONTROLLER_STATUS_IDLE;
static bool driver, external_driver, existing_netif, existing_loop, cleanup_failure;
static unsigned inits,deinits,stops,created,destroyed,registered,unregistered,loops_deleted;
static esp_netif_t original={1}, own={2};
static esp_netif_t *default_netif=&original;
static int fail(void) { return ++step==failure ? ESP_FAIL : ESP_OK; }
static int esp_bt_controller_get_status(void) { return ctrl; }
static int esp_wifi_get_mode(int *m) { (void)m; return driver||external_driver?ESP_OK:ESP_ERR_WIFI_NOT_INIT; }
static int esp_netif_init(void) { return ESP_OK; }
static int esp_event_loop_create_default(void) { return existing_loop?ESP_ERR_INVALID_STATE:ESP_OK; }
static int esp_event_loop_delete_default(void) { loops_deleted++;return ESP_OK; }
static esp_netif_t *esp_netif_get_default_netif(void) { return default_netif; }
static int esp_netif_set_default_netif(esp_netif_t *n) { default_netif=n;return ESP_OK; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { assert(!strcmp(k,"WIFI_STA_DEF"));return existing_netif?&original:NULL; }
static esp_netif_t *esp_netif_create_default_wifi_sta(void) { created++;default_netif=&own;return &own; }
static void esp_netif_destroy_default_wifi(esp_netif_t *n) { assert(n==&own);destroyed++; }
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *i) { assert(n);memset(i,0,sizeof(*i));return ESP_OK; }
static int esp_netif_set_ip_info(esp_netif_t *n,const esp_netif_ip_info_t *i) { assert(n&&i);return ESP_OK; }
static int esp_netif_get_dns_info(esp_netif_t *n,int type,esp_netif_dns_info_t *i) { assert(n&&type<3);i->marker=type;return ESP_OK; }
static int esp_netif_set_dns_info(esp_netif_t *n,int type,const esp_netif_dns_info_t *i) { assert(n&&i->marker==(unsigned)type);return ESP_OK; }
static int esp_netif_dhcpc_get_status(esp_netif_t *n,int *status) { assert(n);*status=1;return ESP_OK; }
static int esp_netif_dhcpc_stop(esp_netif_t *n) { assert(n);return ESP_OK; }
static int esp_netif_dhcpc_start(esp_netif_t *n) { assert(n);return ESP_OK; }
static int esp_wifi_init(const wifi_init_config_t *c) { assert(c->marker==99);int e=fail();if(!e){driver=true;inits++;}return e; }
static int esp_wifi_deinit(void) { assert(driver);if(cleanup_failure){cleanup_failure=false;return ESP_FAIL;}driver=false;deinits++;return ESP_OK; }
static int esp_wifi_scan_stop(void) { return ESP_OK; }
static int esp_wifi_clear_ap_list(void) { return ESP_OK; }
static int esp_wifi_disconnect(void) { return ESP_OK; }
static int esp_wifi_stop(void) { assert(driver);stops++;return ESP_OK; }
static int esp_event_handler_instance_register(esp_event_base_t b,int id,void (*cb)(void *,esp_event_base_t,int,void *),void *ctx,void **h) {
    (void)b;(void)id;(void)cb;(void)ctx;int e=fail();if(!e){registered++;*h=(void *)1;}return e;
}
static int esp_event_handler_instance_unregister(esp_event_base_t b,int id,void *h) { (void)b;(void)id;assert(h);unregistered++;return ESP_OK; }
static int esp_wifi_set_storage(int m) { assert(m==WIFI_STORAGE_RAM);return fail(); }
static int esp_wifi_set_mode(int m) { assert(m==WIFI_MODE_STA);return fail(); }
static int esp_wifi_start(void) { return fail(); }
static int esp_wifi_set_config(int m,const wifi_config_t *c) { assert(m==WIFI_IF_STA&&c->sta.ssid[0]);return ESP_OK; }
static int esp_wifi_connect(void) { return ESP_OK; }
#include "wardrive_wifi.c"
int main(void) {
    external_driver=true;assert(wd_wifi_init_sta()==ESP_ERR_INVALID_STATE&&!inits);
    assert(wd_wifi_deinit()==ESP_OK&&!deinits&&!destroyed);external_driver=false;
    ctrl=ESP_BT_CONTROLLER_STATUS_ENABLED;assert(wd_wifi_init_sta()==ESP_ERR_INVALID_STATE);ctrl=ESP_BT_CONTROLLER_STATUS_IDLE;
    for(failure=1;failure<=6;failure++) {
        step=0;assert(wd_wifi_init_sta()!=ESP_OK);assert(!driver&&!wd_wifi_owned());
        assert(created==destroyed&&registered==unregistered&&default_netif==&original);
    }
    failure=0;step=0;assert(wd_wifi_init_sta()==ESP_OK&&driver&&wd_wifi_owned());
    ip_event_got_ip_t ip={.esp_netif=&own};event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    assert(wd_wifi_sta_has_ip());
    assert(wd_wifi_sta_connect("synthetic","")==ESP_OK);
    cleanup_failure=true;assert(wd_wifi_deinit()==ESP_FAIL&&wd_wifi_owned());
    assert(wd_wifi_deinit()==ESP_OK&&!driver&&!wd_wifi_sta_has_ip());
    assert(inits==deinits&&created==destroyed&&registered==unregistered&&stops==deinits+1);
    existing_netif=existing_loop=true;step=0;
    unsigned old_created=created, old_deleted=loops_deleted;
    assert(wd_wifi_init_sta()==ESP_OK&&wd_wifi_deinit()==ESP_OK);
    assert(created==old_created&&loops_deleted==old_deleted&&default_netif==&original);
    puts("Private WiFi: external-owner protection, partial-init rollback, netif/handler/loop ownership and repeat cleanup passed");
}
'''

BLE = r'''
static int ctrl=ESP_BT_CONTROLLER_STATUS_IDLE, host=ESP_BLUEDROID_STATUS_UNINITIALIZED;
static int failure,step,result_mode;
static unsigned sems,received,disabled,deinits;
static bool wifi_driver, cleanup_failure;
static void (*gap_cb)(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t *);
static int fail(void) { return ++step==failure?ESP_FAIL:ESP_OK; }
static int esp_wifi_get_mode(int *m) { (void)m;return wifi_driver?ESP_OK:ESP_ERR_WIFI_NOT_INIT; }
static int esp_bt_controller_get_status(void) { return ctrl; }
static int esp_bluedroid_get_status(void) { return host; }
static void *xSemaphoreCreateBinary(void) { sems++;return malloc(1); }
static void vSemaphoreDelete(void *p) { assert(p);sems--;free(p); }
static int xSemaphoreGive(void *p) { assert(p);return 1; }
static int xSemaphoreTake(void *p,unsigned t) { assert(p&&t==5000);return result_mode==2?0:1; }
static int esp_bt_controller_init(const int *c) { assert(*c==1);int e=fail();if(!e)ctrl=ESP_BT_CONTROLLER_STATUS_INITED;return e; }
static int esp_bt_controller_enable(int mode) { assert(mode==ESP_BT_MODE_BLE);int e=fail();if(!e)ctrl=ESP_BT_CONTROLLER_STATUS_ENABLED;return e; }
static int esp_bt_controller_disable(void) { assert(ctrl==ESP_BT_CONTROLLER_STATUS_ENABLED);if(cleanup_failure){cleanup_failure=false;return ESP_FAIL;}disabled++;ctrl=ESP_BT_CONTROLLER_STATUS_INITED;return ESP_OK; }
static int esp_bt_controller_deinit(void) { assert(ctrl==ESP_BT_CONTROLLER_STATUS_INITED);deinits++;ctrl=ESP_BT_CONTROLLER_STATUS_IDLE;return ESP_OK; }
static int esp_bluedroid_init_with_cfg(const int *c) { assert(*c==1);int e=fail();if(!e)host=ESP_BLUEDROID_STATUS_INITIALIZED;return e; }
static int esp_bluedroid_enable(void) { int e=fail();if(!e)host=ESP_BLUEDROID_STATUS_ENABLED;return e; }
static int esp_bluedroid_disable(void) { assert(host==ESP_BLUEDROID_STATUS_ENABLED);host=ESP_BLUEDROID_STATUS_INITIALIZED;return ESP_OK; }
static int esp_bluedroid_deinit(void) { assert(host==ESP_BLUEDROID_STATUS_INITIALIZED);host=ESP_BLUEDROID_STATUS_UNINITIALIZED;return ESP_OK; }
static int esp_ble_gap_register_callback(void (*cb)(int,esp_ble_gap_cb_param_t *)) { gap_cb=cb;return fail(); }
static int esp_ble_gap_start_scanning(unsigned seconds) {
    assert(!seconds);esp_ble_gap_cb_param_t p={0};p.scan_start_cmpl.status=result_mode==1?1:ESP_BT_STATUS_SUCCESS;
    if(result_mode!=2)gap_cb(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,&p);return ESP_OK;
}
static int esp_ble_gap_set_scan_params(const esp_ble_scan_params_t *p) {
    /* Enforce IDF's actual parameter bounds, not the invalid stock helper values. */
    assert(p->scan_type==BLE_SCAN_TYPE_PASSIVE);
    assert(p->scan_interval>=0x0004&&p->scan_interval<=0x4000);
    assert(p->scan_window>=0x0004&&p->scan_window<=p->scan_interval);
    int e=fail();if(!e){esp_ble_gap_cb_param_t event={0};event.scan_param_cmpl.status=result_mode==3?1:0;gap_cb(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT,&event);}return e;
}
static int esp_ble_gap_stop_scanning(void) { return ESP_OK; }
#include "wardrive_ble.c"
static void report(const esp_ble_gap_cb_param_t *p) { assert(p);received++; }
static void clean(void) { assert(!sems&&ctrl==ESP_BT_CONTROLLER_STATUS_IDLE&&host==ESP_BLUEDROID_STATUS_UNINITIALIZED); }
int main(void) {
    ctrl=ESP_BT_CONTROLLER_STATUS_ENABLED;assert(wd_ble_start(report)==ESP_ERR_INVALID_STATE&&!sems);
    assert(wd_ble_stop()==ESP_OK&&ctrl==ESP_BT_CONTROLLER_STATUS_ENABLED);ctrl=ESP_BT_CONTROLLER_STATUS_IDLE;
    wifi_driver=true;assert(wd_ble_start(report)==ESP_ERR_INVALID_STATE);wifi_driver=false;
    for(failure=1;failure<=6;failure++){step=0;assert(wd_ble_start(report)!=ESP_OK);clean();}
    failure=0;
    for(result_mode=1;result_mode<=3;result_mode++){step=0;assert(wd_ble_start(report)!=ESP_OK);clean();}
    result_mode=0;step=0;assert(wd_ble_start(report)==ESP_OK);
    esp_ble_gap_cb_param_t p={0};p.scan_rst.search_evt=ESP_GAP_SEARCH_INQ_RES_EVT;gap_cb(ESP_GAP_BLE_SCAN_RESULT_EVT,&p);assert(received==1);
    cleanup_failure=true;assert(wd_ble_stop()==ESP_FAIL&&wd_ble_owned());
    assert(wd_ble_stop()==ESP_OK);clean();gap_cb(ESP_GAP_BLE_SCAN_RESULT_EVT,&p);assert(received==1);
    assert(wd_ble_stop()==ESP_OK);step=0;assert(wd_ble_start(report)==ESP_OK&&wd_ble_stop()==ESP_OK);clean();
    assert(disabled&&deinits);
    puts("Private BLE: external-owner protection, every init failure, async scan failure/timeout, callback gating and restart passed");
}
'''

headers = ["esp_err.h", "poom_wifi_ctrl.h", "esp_event.h", "esp_netif.h", "esp_wifi.h",
           "esp_wifi_default.h", "esp_bt.h", "esp_bt_main.h", "esp_gap_ble_api.h",
           "freertos/FreeRTOS.h", "freertos/semphr.h"]
with tempfile.TemporaryDirectory(prefix="wardrive-lifecycle-") as tmp:
    root = Path(tmp)
    (root / "common.h").write_text(COMMON)
    for header in headers:
        p = root / header
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('#include "common.h"\n')
    for name, source in [("wifi", WIFI), ("ble", BLE)]:
        p = root / f"{name}.c"
        p.write_text('#include "common.h"\n#include <stdio.h>\n' + source)
        exe = root / name
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
            "-fsanitize=address,undefined", "-g", "-I" + tmp,
            "-Imodules/poom_wardrive/include", "-Imodules/poom_wardrive",
            str(p), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
