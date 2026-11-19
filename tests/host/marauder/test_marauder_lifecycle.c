#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "poom_wifi_marauder.h"
#include "mock_idf.h"

static LONG tx_count, init_count, cleanup_count, generated_count, destroyed_count;
static int fail_init, fail_channel, fail_tx, fail_task, fail_crypto;
static uint8_t captured[256];
static size_t captured_length;
static uint8_t history[64][256];
static size_t history_count;
static unsigned checks;
#define CHECK(expression) do { ++checks; assert(expression); } while (0)

esp_err_t esp_wifi_80211_tx(int interface_index, const void *frame, int length, bool system_sequence)
{
    assert(interface_index == WIFI_IF_STA && !system_sequence && length > 0 && length <= 256);
    memcpy(captured, frame, (size_t)length); captured_length = (size_t)length;
    if(history_count < 64) memcpy(history[history_count++], frame, (size_t)length);
    InterlockedIncrement(&tx_count);
    return fail_tx ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}
esp_err_t esp_wifi_set_ps(int mode) { (void)mode; return ESP_OK; }
esp_err_t poom_wifi_ctrl_init_sta(void) {InterlockedIncrement(&init_count); return fail_init ? ESP_FAIL : ESP_OK;}
esp_err_t poom_wifi_ctrl_deinit(void) {InterlockedIncrement(&cleanup_count); return ESP_OK;}
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel) {(void)channel; return fail_channel ? ESP_ERR_INVALID_ARG : ESP_OK;}
esp_err_t poom_wifi_ctrl_set_promiscuous(bool enabled) {(void)enabled; return ESP_OK;}
bool poom_wifi_ctrl_channel_is_valid(uint8_t channel) {return (channel >= 1 && channel <= 13) || channel == 36 || channel == 40 || channel == 44;}
void esp_fill_random(void *buffer, size_t length) {memset(buffer, 0xa5, length);}
int64_t esp_timer_get_time(void) {return (int64_t)GetTickCount64() * 1000;}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return CreateMutex(NULL, FALSE, NULL);}
int xSemaphoreTake(SemaphoreHandle_t mutex, DWORD timeout) {return WaitForSingleObject(mutex, timeout) == WAIT_OBJECT_0;}
void xSemaphoreGive(SemaphoreHandle_t mutex) {assert(ReleaseMutex(mutex));}
void vSemaphoreDelete(SemaphoreHandle_t mutex) {CloseHandle(mutex);}
typedef struct {void (*fn)(void *); void *arg;} thread_args;
static DWORD WINAPI thread_main(LPVOID argument) {thread_args value=*(thread_args *)argument; free(argument); value.fn(value.arg); return 0;}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    if (fail_task) return 0;
    thread_args *value = malloc(sizeof(*value)); assert(value); value->fn=fn; value->arg=arg;
    HANDLE thread=CreateThread(NULL,0,thread_main,value,0,NULL); assert(thread); CloseHandle(thread); return pdPASS;
}
void vTaskDelay(unsigned milliseconds) {Sleep(milliseconds);}
void vTaskDelete(void *task) {(void)task; ExitThread(0);}
void psa_set_key_type(psa_key_attributes_t *a, unsigned value) {a->type=value;}
void psa_set_key_bits(psa_key_attributes_t *a, unsigned value) {a->bits=value;}
void psa_set_key_usage_flags(psa_key_attributes_t *a, unsigned value) {a->usage=value;}
void psa_reset_key_attributes(psa_key_attributes_t *a) {memset(a,0,sizeof(*a));}
psa_status_t psa_crypto_init(void) {return PSA_SUCCESS;}
psa_status_t psa_generate_key(const psa_key_attributes_t *a, psa_key_id_t *key)
{
    assert(a->bits == 256 && a->usage == PSA_KEY_USAGE_EXPORT);
    if(fail_crypto) return PSA_ERROR_NOT_SUPPORTED;
    InterlockedIncrement(&generated_count); *key=1; return PSA_SUCCESS;
}
psa_status_t psa_export_key(psa_key_id_t key, uint8_t *out, size_t cap, size_t *length)
{assert(key==1 && cap>=32); memset(out,0x12,32); *length=32; return PSA_SUCCESS;}
psa_status_t psa_export_public_key(psa_key_id_t key, uint8_t *out, size_t cap, size_t *length)
{assert(key==1 && cap>=65); memset(out,0x34,65); out[0]=4; *length=65; return PSA_SUCCESS;}
psa_status_t psa_destroy_key(psa_key_id_t key) {assert(key==1); InterlockedIncrement(&destroyed_count); return PSA_SUCCESS;}

static poom_marauder_config_t config_for(poom_marauder_mode_t mode)
{
    poom_marauder_config_t c={0}; c.mode=mode; c.ap.primary=6; c.ap.authmode=WIFI_AUTH_WPA2_PSK;
    const uint8_t ap[6]={2,3,4,5,6,7}, client[6]={2,8,9,10,11,12};
    memcpy(c.ap.bssid,ap,6); memcpy(c.ap.ssid,"lab",4); memcpy(c.clients[0],client,6);
    c.client_count=1; c.duration_ms=200; c.interval_ms=100; return c;
}
static poom_marauder_status_t wait_stopped(void)
{
    poom_marauder_status_t status;
    ULONGLONG deadline=GetTickCount64()+2000;
    do {poom_wifi_marauder_get_status(&status); if(!status.running) return status; Sleep(5);} while(GetTickCount64()<deadline);
    assert(!"worker did not stop"); return status;
}
static void reset_mocks(void)
{
    poom_wifi_marauder_stop(); tx_count=init_count=cleanup_count=generated_count=destroyed_count=0;
    fail_init=fail_channel=fail_tx=fail_task=fail_crypto=0; captured_length=0; history_count=0;
}

int main(void)
{
    reset_mocks(); CHECK(cleanup_count==0);
    poom_marauder_config_t c=config_for(POOM_MARAUDER_BAD_MSG);
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK);
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_STATE);
    Sleep(40); poom_wifi_marauder_stop();
    poom_marauder_status_t s=wait_stopped();
    CHECK(s.sent==1 && s.attempted==1 && s.errors==0 && !s.running);
    CHECK(cleanup_count==1 && init_count==1 && captured_length==153);
    LONG stopped_tx=tx_count; Sleep(130); CHECK(tx_count==stopped_tx);
    poom_wifi_marauder_stop(); CHECK(cleanup_count==1);

    reset_mocks(); c=config_for(POOM_MARAUDER_ASSOC_SLEEP); c.duration_ms=150;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent==2 && s.errors==0 && cleanup_count==1 && s.elapsed_ms>=150);
    CHECK(captured[0]==0 && captured[1]==0x10);

    reset_mocks(); c=config_for(POOM_MARAUDER_QUIET); fail_tx=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.attempted==1 && s.sent==0 && s.errors==1 && s.last_error==ESP_ERR_NOT_SUPPORTED);
    CHECK(tx_count==1 && cleanup_count==1);

    reset_mocks(); fail_init=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_FAIL); s=wait_stopped();
    CHECK(s.attempted==0 && s.errors==1 && cleanup_count==1);
    reset_mocks(); fail_channel=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG); s=wait_stopped();
    CHECK(s.attempted==0 && s.errors==1 && cleanup_count==1);
    reset_mocks(); fail_task=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_NO_MEM); s=wait_stopped();
    CHECK(s.attempted==0 && s.errors==1 && cleanup_count==1);

    reset_mocks(); c=config_for(POOM_MARAUDER_SAE_COMMIT);
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_NOT_SUPPORTED && init_count==0);
    c.ap.authmode=WIFI_AUTH_WPA3_PSK; fail_crypto=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.attempted==0 && s.errors==1 && s.last_error==ESP_ERR_NOT_SUPPORTED && cleanup_count==1);
    reset_mocks(); c.duration_ms=70;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent==1 && generated_count==1 && destroyed_count==1 && captured_length==128);
    CHECK(captured[0]==0xb0 && captured[30]==19 && (captured[10]&3)==2);

    reset_mocks(); c=config_for(POOM_MARAUDER_BAD_MSG); c.client_count=0;
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.client_count=1; c.interval_ms=99; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.interval_ms=100; c.duration_ms=60001; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.duration_ms=200; c.client_count=2; memcpy(c.clients[1],c.clients[0],6);
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c=config_for(POOM_MARAUDER_CSA); c.csa_channel=c.ap.primary;
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.csa_channel=36; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.csa_channel=1; c.duration_ms=60;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped(); CHECK(s.sent==1 && cleanup_count==1);
    reset_mocks(); c=config_for(POOM_MARAUDER_PROBE_REQUEST); c.duration_ms=60;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent==1 && captured[0]==0x40 && (captured[10]&3)==2 && captured[4]==255);
    c.ap.ssid[0]=0; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    reset_mocks(); c=config_for(POOM_MARAUDER_AP_CLONE); c.duration_ms=60;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent==1 && captured[0]==0x80 && captured[34]==1 && captured[35]==0);
    CHECK(memcmp(captured+10,c.ap.bssid,6)!=0 && (captured[10]&3)==2);
    CHECK(memcmp(captured+38,c.ap.ssid,strlen((const char*)c.ap.ssid))==0);
    reset_mocks(); c=config_for(POOM_MARAUDER_BEACON_LIST); c.client_count=0;
    CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.beacon_count=33; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.beacon_count=2; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_SIZE);
    memcpy(c.beacon_ssids[0],"first",6); memcpy(c.beacon_ssids[1],"second",7);
    c.interval_ms=9; CHECK(poom_wifi_marauder_start(&c)==ESP_ERR_INVALID_ARG);
    c.interval_ms=10; c.duration_ms=65;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent>=3 && s.errors==0 && cleanup_count==1);
    CHECK(history[0][37]==5 && memcmp(history[0]+38,"first",5)==0);
    CHECK(history[1][37]==6 && memcmp(history[1]+38,"second",6)==0);
    CHECK(memcmp(history[0]+10,history[1]+10,6)!=0);
    CHECK(history[0][22]==0 && history[1][22]==0 && history[2][22]==16);
    reset_mocks(); c.random_beacon_cycles=true; c.beacon_count=32; c.duration_ms=400;
    for(unsigned i=0;i<32;++i) memcpy(c.beacon_ssids[i],"placeholder",12);
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent>=3 && s.errors==0);
    CHECK(memcmp(c.beacon_ssids[0],"placeholder",12)==0);
    for(size_t i=0;i<history_count && i<32;++i) {
        CHECK(history[i][37]>=9 && history[i][37]<=15 && (history[i][10]&3)==2);
        for(size_t j=0;j<i;++j) {
            CHECK(memcmp(history[i]+10,history[j]+10,6)!=0);
            CHECK(memcmp(history[i]+38,history[j]+38,history[i][37])!=0);
        }
    }
    reset_mocks(); c.random_beacon_cycles=false; fail_tx=1;
    CHECK(poom_wifi_marauder_start(&c)==ESP_OK); s=wait_stopped();
    CHECK(s.sent==0 && s.errors==1 && cleanup_count==1);
    printf("Marauder mocked lifecycle: %u checks passed; no device/radio I/O.\n",checks);
    return 0;
}
