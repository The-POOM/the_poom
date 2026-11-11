"""Execute production transfer launch/connect functions with offline service mocks."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

def function(source, name):
    m = re.search(r'(?:static )?(?:bool|void) ' + name + r'\([^;{}]*\)\s*\{', source)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[m.start():end]

jobs = Path('applications/poom_app_pack/src/wardrive_ui_jobs.c').read_text()
harness = r'''
#include "wardrive_ui_internal.h"
#include "wardrive_debug.h"
#include "wardrive_ui_wifi.h"
#include "poom_gps_capture.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#define ESP_FAIL -1
#define pdMS_TO_TICKS(x) (x)
#define IPSTR "%u"
#define IP2STR(p) (*(p))
enum { POOM_WIFI_CTRL_EVT_STA_CONNECTED,POOM_WIFI_CTRL_EVT_STA_DISCONNECTED,POOM_WIFI_CTRL_EVT_STA_GOT_IP };
typedef struct { int evt,reason; } poom_wifi_ctrl_evt_info_t;
static atomic_int sta_event,sta_reason;
static atomic_bool sta_associated;
static bool station_owned,matches;
bool wd_ui_wifi_matches(const char *s,const char *p) { (void)s;(void)p;return matches; }
static void (*connection_event)(const poom_wifi_ctrl_evt_info_t *,void *);
typedef struct { unsigned ip; } esp_netif_ip_info_t;
typedef int esp_netif_t;
#define MALLOC_CAP_8BIT 1
static unsigned esp_get_free_heap_size(void) { return 40000; }
static unsigned heap_caps_get_largest_free_block(int cap) { assert(cap==1);return 30000; }
static int stage;
static unsigned delays,init_calls,sync_calls;
static int64_t esp_timer_get_time(void) { return 0; }
int64_t wd_clock_now(const wd_clock_t *c,uint64_t ms,int64_t system) { (void)c;(void)ms;return system; }
bool wd_clock_scope_begin(wd_clock_scope_t *s,int64_t utc) { (void)s;return utc>=1704067200; }
static bool driver_owned;
static bool wd_wifi_owned(void) { return driver_owned; }
static time_t wall=1704067200;
static wd_ui_job_t *active;
static time_t test_time(time_t *p) { (void)p; return wall; }
#define time test_time
void wd_transfer_stop(void) {}
static unsigned unregister_calls,disconnect_calls,deinit_calls;
static esp_err_t wd_wifi_unregister_cb(void) { unregister_calls++; return ESP_OK; }
static esp_err_t wd_wifi_sta_disconnect(void) { disconnect_calls++; return ESP_OK; }
static esp_err_t wd_wifi_deinit(void) { driver_owned=false;deinit_calls++; return ESP_OK; }
esp_err_t wd_ui_wifi_init_transfer(void) { init_calls++;driver_owned=stage!=1; return stage==1?ESP_FAIL:ESP_OK; }
static int wd_wifi_register_cb(void (*cb)(const poom_wifi_ctrl_evt_info_t *,void *),void *p) { connection_event=cb;(void)p; return stage==2?ESP_FAIL:ESP_OK; }
esp_err_t wd_ui_wifi_connect(const char *s,const char *p) { assert(!strcmp(s,"synthetic"));(void)p;if(stage==9||stage==10){poom_wifi_ctrl_evt_info_t e={.evt=stage==9?POOM_WIFI_CTRL_EVT_STA_DISCONNECTED:POOM_WIFI_CTRL_EVT_STA_CONNECTED,.reason=stage==9?202:0};connection_event(&e,NULL);}return stage==3?ESP_FAIL:ESP_OK; }
static bool wd_wifi_sta_has_ip(void) { return stage!=4&&stage!=8&&stage!=9&&stage!=10; }
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *s) { assert(!strcmp(s,"WIFI_STA_DEF"));static int netif;return stage==7?NULL:&netif; }
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *ip) { assert(n);ip->ip=1234;return stage==5?ESP_FAIL:ESP_OK; }

static void vTaskDelay(unsigned t) { assert(t==100);delays++;if(stage==8&&delays==2)atomic_store(&active->cancel,true); }
'''
harness += '\n'.join(function(jobs,n) for n in ('link_event','wd_ui_disconnect','connect','keep_link'))
harness += r'''
int main(void) {
    wd_ui_job_t j={.job=WD_JOB_TRANSFER};active=&j;
    assert(!connect(&j)&&!strcmp(j.message,"SET TRUSTED WIFI")&&!init_calls);
    strcpy(j.credentials.ssid,"synthetic");
    const char *errors[]={"","WIFI INIT FAILED","WIFI CALLBACK FAILED","WIFI CONNECT FAILED","WIFI / DHCP TIMEOUT","WIFI IP READ FAILED","WAIT GPS DATE/TIME","WIFI IP READ FAILED","OPERATION CANCELLED","WiFi disconnect 202","DHCP timeout"};
    for(stage=1;stage<=10;stage++) {
        delays=0;atomic_store(&j.cancel,false);j.message[0]=0;
        j.job=stage==6?WD_JOB_UPLOAD:WD_JOB_TRANSFER;wall=0;
        assert(!connect(&j));assert(!strcmp(j.message,errors[stage]));
        if(stage==4)assert(delays==200);
    }
    stage=0;delays=0;sync_calls=0;atomic_store(&j.cancel,false);j.job=WD_JOB_TRANSFER;
    assert(connect(&j)&&!strcmp(j.ip,"1234")&&!sync_calls); /* offline download needs no time service */
    j.job=WD_JOB_WIFI_JOIN;assert(connect(&j)&&!sync_calls);
    matches=true;unsigned before_init=init_calls;assert(connect(&j)&&init_calls==before_init&&keep_link(&j));
    atomic_store(&j.cancel,true);assert(!keep_link(&j));atomic_store(&j.cancel,false);
    matches=false;assert(!keep_link(&j));assert(connect(&j)&&init_calls==before_init+1);
    wd_ui_disconnect();assert(!station_owned); /* profile validation is offline too */
    assert(unregister_calls&&disconnect_calls&&deinit_calls);
    assert(unregister_calls==disconnect_calls&&disconnect_calls==deinit_calls);
    puts("Actual WiFi connect: service errors, DHCP timeout, cancel, link reuse and offline success passed (UI prerequisites: test_ui_flow)");
}
'''
with tempfile.TemporaryDirectory(prefix='wardrive-connect-') as tmp:
    source, exe = Path(tmp)/'connect.c', Path(tmp)/'connect'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + [
        '-std=c11','-Wall','-Wextra','-Werror',
        '-Iapplications/poom_app_pack/src','-Iapplications/poom_app_pack/include','-Imodules/poom_gps/include',
        '-Imodules/poom_wardrive/include','-Itools/wardrive_hostcheck/stubs',
        str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
