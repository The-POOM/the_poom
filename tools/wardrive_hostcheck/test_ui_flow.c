// SPDX-License-Identifier: MIT
/* Compile the production Wardrive UI (menu_wardrive.c) on the host with
 * stubbed hardware services and drive real press/release/click sequences.
 * Every frame goes through a bounded OLED mock that rejects off-screen or
 * overlapping text and pixels drawn over text. No radio, WiFi or WiGLE. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_err.h"
#include "nvs.h"
static time_t wall=1794117600; /* 2026-11-13 06:00:00 UTC */
static time_t fake_time(time_t *out) { if(out) *out=wall; return wall; }
#define time fake_time
static int64_t now_us;

/* ---- OLED mock --------------------------------------------------------- */
#include "Arduboy2.h"
extern const uint8_t poom_arduboy_font5x7[];
static bool text_px[64][128],ink[64][128];
typedef struct { int x,y; char s[22]; } print_t;
static print_t prints[64]; static unsigned print_count;
static unsigned rect_count,fill_width,frames;
static int cursor_x,cursor_y;
static char events[8]; static unsigned event_count;
static int highlights[8]; static unsigned highlight_count;
void poom_arduboy_clear(void) { memset(text_px,0,sizeof(text_px)); memset(ink,0,sizeof(ink)); print_count=0; rect_count=0; fill_width=0; highlight_count=0; event_count=0; events[0]=0; }
void poom_arduboy_set_text_size(uint8_t s) { assert(s==1); }
void poom_arduboy_set_cursor(int16_t x,int16_t y) { cursor_x=x; cursor_y=y; }
void poom_arduboy_print(const char *s) {
    size_t n=strlen(s); assert(n>0&&n<=21&&cursor_x>=0&&cursor_y>=0&&cursor_y+8<=64&&cursor_x+(int)n*6<=128);
    for(int y=cursor_y;y<cursor_y+8;y++) for(int x=cursor_x;x<cursor_x+(int)n*6;x++) { assert(!text_px[y][x]&&!ink[y][x]); text_px[y][x]=true; }
    assert(print_count<64); prints[print_count].x=cursor_x; prints[print_count].y=cursor_y; snprintf(prints[print_count++].s,22,"%s",s);
    for(size_t i=0;i<n;i++) { const uint8_t *g=poom_arduboy_font5x7+((unsigned char)s[i]-32)*5;
        for(int c=0;c<5;c++) for(int r=0;r<7;r++) if(g[c]&(1U<<r)) ink[cursor_y+r][cursor_x+(int)i*6+c]=true; }
}
void poom_arduboy_draw_pixel(int16_t x,int16_t y,uint8_t color) {
    assert(color==WHITE&&x>=0&&x<128&&y>=0&&y<64&&!text_px[y][x]&&!ink[y][x]); ink[y][x]=true;
}
void poom_arduboy_draw_line(int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint8_t color) {
    int n=x1-x0; assert(color==WHITE&&n==2);
    for(int i=0;i<=n;i++) { int x=x0+i,y=y0+(y1-y0)*i/n; assert(x>=0&&x<128&&y>=0&&y<64&&!text_px[y][x]); ink[y][x]=true; }
}
static void area(int16_t x,int16_t y,int16_t w,int16_t h,bool outline) {
    assert(w>0&&h>0&&x>=0&&y>=0&&x+w<=128&&y+h<=64);
    for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) {
        if(outline&&j!=y&&j!=y+h-1&&i!=x&&i!=x+w-1) continue;
        assert(!text_px[j][i]&&!ink[j][i]); ink[j][i]=true;
    }
}
void poom_arduboy_draw_rect(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t c) { assert(c==WHITE); area(x,y,w,h,true); rect_count++; }

void poom_arduboy_fill_rect(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t c) {
    if(c==INVERT) { /* selection bar: full width, one 9-pixel text row, inverted in place */
        assert(x==0&&w==128&&h==9&&y>=0&&y+h<=64&&highlight_count<8); highlights[highlight_count++]=y+1;
        for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) ink[j][i]=!ink[j][i];
        return;
    }
    assert(c==WHITE); area(x,y,w,h,false); fill_width=(unsigned)w;
}
static bool hl(int y) { for(unsigned i=0;i<highlight_count;i++) if(highlights[i]==y) return true; return false; }
static void event(char e) { if(event_count<7) { events[event_count++]=e; events[event_count]=0; } }
void poom_arduboy_display(void) { frames++; event('F'); }
void poom_arduboy_set_display_enabled(bool e) { event(e?'1':'0'); }
static const print_t *find(const char *s) { for(unsigned i=0;i<print_count;i++) if(!strcmp(prints[i].s,s)) return &prints[i]; return NULL; }
static const print_t *find_at(int x,int y) { for(unsigned i=0;i<print_count;i++) if(prints[i].x==x&&prints[i].y==y) return &prints[i]; return NULL; }
static bool contains(const char *needle) { for(unsigned i=0;i<print_count;i++) if(strstr(prints[i].s,needle)) return true; return false; }
static bool at(int x,int y,const char *s) { const print_t *p=find_at(x,y); return p&&!strcmp(p->s,s); }
/* Optional review images: WARDRIVE_PREVIEW_DIR=dir writes <name>.pbm frames. */
static void save_preview(const char *name) {
    const char *directory=getenv("WARDRIVE_PREVIEW_DIR"); if(!directory) return;
    char path[256]; assert(snprintf(path,sizeof(path),"%s/%s.pbm",directory,name)<(int)sizeof(path));
    FILE *file=fopen(path,"w"); assert(file); fputs("P1\n128 64\n",file);
    for(unsigned y=0;y<64;y++) { for(unsigned x=0;x<128;x++) fprintf(file,"%u ",ink[y][x]?1:0); fputc('\n',file); }
    fclose(file);
}
static bool any_ink(int x0,int y0,int x1,int y1) { for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) if(ink[y][x]) return true; return false; }

/* ---- Platform stubs ---------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
QueueHandle_t xQueueCreate(unsigned n,size_t s) { (void)n;(void)s; return NULL; }
void xQueueReset(QueueHandle_t q) { (void)q; }
int xQueueSend(QueueHandle_t q,const void *v,unsigned t) { (void)q;(void)v;(void)t; return 0; }
int xQueueReceive(QueueHandle_t q,void *v,unsigned t) { (void)q;(void)v;(void)t; return 0; }
void vQueueDelete(QueueHandle_t q) { (void)q; }
void vTaskDelay(unsigned t) { (void)t; }
void vTaskDelete(void *t) { (void)t; }
unsigned uxTaskGetStackHighWaterMark(void *t) { (void)t; return 0; }
int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,void *h) { (void)f;(void)n;(void)s;(void)a;(void)p;(void)h; return 0; }
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_random(void) { return 0x1234abcd; }
void esp_fill_random(void *p,size_t n) { memset(p,0x5a,n); }
size_t heap_caps_get_largest_free_block(uint32_t c) { (void)c; return 0; }
#include "esp_heap_caps.h"
static size_t ui_bytes; /* size of Wardrive's state block while open */
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps) { assert(caps&MALLOC_CAP_INTERNAL); ui_bytes=n*size; return calloc(n,size); }
void heap_caps_free(void *p) { free(p); }
uint32_t esp_get_free_heap_size(void) { return 0; }
bool sd_card_is_mounted(void) { return true; }
esp_err_t sd_card_mount(void) { return ESP_OK; }
esp_err_t sd_card_unmount(void) { return ESP_OK; }
#include "poom_sbus.h"
bool poom_sbus_subscribe_cb(const char *t,poom_sbus_handler_t h,void *u) { (void)t;(void)h;(void)u; return true; }
bool poom_sbus_unsubscribe_cb(const char *t,poom_sbus_handler_t h,void *u) { (void)t;(void)h;(void)u; return true; }
bool poom_sbus_publish(const char *t,const void *d,size_t n,uint32_t f) { (void)t;(void)d;(void)n;(void)f; return true; }

/* In-memory poom_nvs. */
typedef struct { char key[16]; unsigned char value[160]; size_t length; bool used; } nvs_item_t;
static nvs_item_t nvs[8]; static unsigned settings_writes;
static nvs_item_t *nvs_find(const char *key,bool create) {
    for(unsigned i=0;i<8;i++) if(nvs[i].used&&!strcmp(nvs[i].key,key)) return &nvs[i];
    if(create) for(unsigned i=0;i<8;i++) if(!nvs[i].used) { nvs[i].used=true; snprintf(nvs[i].key,16,"%s",key); return &nvs[i]; }
    return NULL;
}
esp_err_t poom_secrets_init(void) { return ESP_OK; }
esp_err_t poom_secrets_set_blob(const char *k,const void *d,size_t n) { assert(n<=160); nvs_item_t *e=nvs_find(k,true); memcpy(e->value,d,n); e->length=n; if(!strcmp(k,"wd_settings")) settings_writes++; return ESP_OK; }
esp_err_t poom_secrets_get_blob(const char *k,void *d,size_t *n) { nvs_item_t *e=nvs_find(k,false); if(!e) return ESP_ERR_NVS_NOT_FOUND; if(*n<e->length) return ESP_ERR_NVS_INVALID_LENGTH; memcpy(d,e->value,e->length); *n=e->length; return ESP_OK; }
esp_err_t poom_secrets_set_str(const char *k,const char *v) { return poom_secrets_set_blob(k,v,strlen(v)+1); }
esp_err_t poom_secrets_get_str(const char *k,char *v,size_t *n) { return poom_secrets_get_blob(k,v,n); }
esp_err_t poom_secrets_erase_key(const char *k) { nvs_item_t *e=nvs_find(k,false); if(!e) return ESP_ERR_NVS_NOT_FOUND; memset(e,0,sizeof(*e)); return ESP_OK; }

/* GPS service: snapshots come from the real parser; no receiver I/O. */
#include "poom_gps.h"
void poom_gps_service_init(poom_gps_service_t *s,const poom_gps_adapter_t *a) { (void)a; memset(s,0,sizeof(*s)); }
void poom_gps_service_poll(poom_gps_service_t *s,uint64_t n) { (void)s;(void)n; }
void poom_gps_service_close(poom_gps_service_t *s) { (void)s; }
const poom_gps_adapter_t *poom_gps_xa1110(void) { return NULL; }

/* Recording and radio. */
#include "poom_wardrive.h"
static bool radio_started,radio_stopped,radio_paused,radio_done;
static unsigned checkpoints;
bool wd_session_start(wd_session_t *s,const char *root,const char *id,const wd_settings_t *st) { (void)root;(void)id; memset(s,0,sizeof(*s)); s->settings=*st; s->active=true; return true; }
bool wd_session_gps(wd_session_t *s,const poom_gps_fix_t *f,uint64_t n) { (void)s;(void)f;(void)n; return true; }
bool wd_session_observe(wd_session_t *s,const wd_observation_t *o) { (void)s;(void)o; return true; }
void wd_session_time(wd_session_t *s,int64_t utc,uint64_t now) { s->clock_utc=utc;s->clock_ms=now; }
bool wd_session_checkpoint(wd_session_t *s,const char *state) { (void)s;(void)state; checkpoints++; return true; }
bool wd_session_stop(wd_session_t *s) { s->active=false; return true; }
void wd_radio_set_gps(const poom_gps_fix_t *f) { (void)f; }
void wd_radio_release(void) {}
bool wd_radio_quiescent(void) { return true; }
bool wd_radio_start(const wd_settings_t *s) { (void)s; radio_started=true; radio_stopped=false; return true; }
void wd_radio_pause(bool p) { radio_paused=p; }
void wd_radio_stop(void) { radio_stopped=true; }
bool wd_radio_finished(void) { return radio_done; }
bool wd_radio_next(wd_observation_t *o) { (void)o; return false; }
uint32_t wd_radio_dropped(void) { return 3; }
int wd_radio_error(void) { return 0; }
unsigned wd_radio_stack_free(void) { return 0; }

/* Upload state: in-memory per session directory. */
#include "wardrive_connect.h"
typedef struct { char directory[160]; wd_upload_status_t status; bool used; } upload_item_t;
static upload_item_t uploads[16];
static upload_item_t *upload_find(const char *d,bool create) {
    for(unsigned i=0;i<16;i++) if(uploads[i].used&&!strcmp(uploads[i].directory,d)) return &uploads[i];
    if(create) for(unsigned i=0;i<16;i++) if(!uploads[i].used) { uploads[i].used=true; snprintf(uploads[i].directory,160,"%s",d); return &uploads[i]; }
    return NULL;
}
static unsigned status_reads;
bool wd_upload_status_read(const char *d,wd_upload_status_t *s) { status_reads++; upload_item_t *u=upload_find(d,false); *s=u?u->status:(wd_upload_status_t){0}; return true; }
bool wd_upload_status_write(const char *d,const wd_upload_status_t *s) { upload_find(d,true)->status=*s; return true; }
bool wd_transfer_expired(void) { return false; }

/* Jobs and WiFi: recorded; tests complete jobs explicitly. */
#include "wardrive_ui_wifi.h"
static wd_job_t last_job; static unsigned job_starts,disconnects; static bool link_up;
bool wd_ui_job_start(wd_ui_job_t *j) { assert(!atomic_load(&j->busy)); atomic_store(&j->busy,true); last_job=j->job; job_starts++; return true; }
void wd_ui_job_reap(void) {}
void wd_ui_disconnect(void) { disconnects++; link_up=false; }
bool wd_ui_link_up(void) { return link_up; }
bool wd_ui_wifi_password_valid(const char *p) { size_t n=strlen(p); return n>=8&&n<=64; }
static poom_wifi_profile_t profile={.ssid="saved-network",.pass="synthetic"};
bool wd_ui_profiles_open(wd_ui_profiles_t *p) { p->items=&profile; p->count=1; return true; }
void wd_ui_profiles_close(wd_ui_profiles_t *p) { p->items=NULL; p->count=0; }
bool wd_ui_profile_pick(const wd_ui_profiles_t *p,unsigned i,wd_credentials_t *c) { assert(i<p->count); strcpy(c->ssid,p->items[i].ssid); strcpy(c->password,p->items[i].pass); return true; }
static esp_err_t profile_status=ESP_OK; static unsigned profile_saves,network_total; static bool network_open;
esp_err_t wd_ui_profile_save(const wd_credentials_t *c) { assert(c->ssid[0]); if(profile_status==ESP_OK) profile_saves++; return profile_status; }
void wd_ui_networks_clear(void) {}
unsigned wd_ui_network_count(void) { return network_total; }
bool wd_ui_network_get(unsigned i,wd_ui_network_t *n) {
    if(i>=network_total) return false;
    memset(n,0,sizeof(*n)); strcpy(n->ssid,"new-network"); n->rssi=-60; n->personal=i==0; n->open=network_open; return true;
}
/* Keyboard: the test fills `edit`, A accepts. */
#include "poom_ui_keyboard.h"
static const char *keyboard_label;
void poom_ui_keyboard_init(poom_ui_keyboard_t *k,char *t,size_t c,const char *l) { (void)k;(void)t;(void)c; keyboard_label=l; }
void poom_ui_keyboard_draw(const poom_ui_keyboard_t *k) { (void)k; }
poom_ui_keyboard_action_t poom_ui_keyboard_handle_button(poom_ui_keyboard_t *k,uint8_t b) { (void)k; return b==0?POOM_UI_KEYBOARD_ACTION_ACCEPT:POOM_UI_KEYBOARD_ACTION_NONE; }

#include "menu_wardrive.c"

/* ---- Drivers ------------------------------------------------------------ */
static poom_gps_fix_t fix;
static uint64_t ms(void) { return (uint64_t)(now_us/1000); }
static void advance(uint64_t delta) { now_us+=(int64_t)delta*1000; if(wd_hold_poll(&ui->hold,ui->screen,ms())) hold_fired(ms()); }
static void frame(void) { draw(ms(),fix); }
static void ev(uint8_t button,uint8_t event) { input_event((wd_key_event_t){button,event},fix,ms()); frame(); }
static void tap(uint8_t button) { ev(button,BUTTON_PRESS_DOWN); advance(80); ev(button,BUTTON_PRESS_UP); ev(button,BUTTON_SINGLE_CLICK); }
/* Long press as iot_button reports it: no SINGLE_CLICK after a long hold. */
static void hold_for(uint8_t button,uint64_t duration) {
    ev(button,BUTTON_PRESS_DOWN);
    for(uint64_t t=0;t<duration;t+=40) { advance(40); frame(); }
    ev(button,BUTTON_PRESS_UP);
    if(duration<1500) ev(button,BUTTON_SINGLE_CLICK);
}
static void finish_job(bool ok,const char *text) {
    ui->job.success=ok; snprintf(ui->job.message,sizeof(ui->job.message),"%s",text); atomic_store(&ui->job.done,true); complete_job(); frame();
}
static void mkfile(const char *path,const char *text) { FILE *f=fopen(path,"w"); assert(f&&fputs(text,f)>=0&&!fclose(f)); }
static void make_session(const char *id,const char *utc) {
    char p[256]; snprintf(p,sizeof(p),WD_ROOT "/sessions/%s",id); assert(!mkdir(p,0700));
    snprintf(p,sizeof(p),WD_ROOT "/sessions/%s/session.json",id);
    char json[160]; snprintf(json,sizeof(json),"{\"start_utc\":\"%s\",\"state\":\"Completed\",\"observations\":5}",utc); mkfile(p,json);
}
static wd_upload_status_t state_of(const char *id) { char p[160]; session_path(p,sizeof(p),id); wd_upload_status_t s; wd_upload_status_read(p,&s); return s; }
static void set_state(const char *id,wd_upload_status_t s) { char p[160]; session_path(p,sizeof(p),id); wd_upload_status_write(p,&s); }
static void goto_main_item(wd_menu_item_t wanted) {
    wd_menu_item_t items[8]; unsigned n=wd_main_menu(ui->session.active,items);
    for(unsigned i=0;i<n;i++) if(items[i]==wanted) { ui->selection=i; ui->screen=WD_SCREEN_MAIN; frame(); return; }
    assert(0);
}

int main(void) {
    now_us=10*1000000LL;
    assert(!ui&&ui_alloc()&&ui&&ui->presented_screen==UINT_MAX&&ui->wifi_return==WD_SCREEN_WIFI_SETTINGS);
    printf("Wardrive UI state block: %zu bytes, allocated only while open\n",ui_bytes);
    wd_settings_default(&ui->settings); timezone_load(); wd_credentials_load(&ui->credentials);
    assert(!mkdir(WD_ROOT "/sessions",0700)&&!mkdir(WD_ROOT "/config",0700));
    ui->screen=WD_SCREEN_MAIN; frame();
    /* Main menu (idle): spec order, pointer column, 20-column labels fit. */
    assert(at(0,0,"WARDRIVE")&&hl(12)&&at(2,12,"Start Wardrive")&&at(2,22,"Check GPS Status")&&at(2,42,"Bulk WiGLE Upload")); save_preview("01-main-idle");
    for(unsigned i=0;i<6;i++) tap(BUTTON_DOWN);
    assert(at(2,42,"Additional Settings")&&hl(42)&&at(2,32,"WiFi Upload Settings"));
    /* Scrolling back up moves the cursor first; the window stays put. */
    tap(BUTTON_UP); assert(ui->selection==5&&hl(32)&&at(2,42,"Additional Settings")&&at(2,12,"Bulk WiGLE Upload"));
    tap(BUTTON_UP); tap(BUTTON_UP); assert(ui->selection==3&&hl(12)&&at(2,12,"Bulk WiGLE Upload"));
    tap(BUTTON_UP); assert(ui->selection==2&&hl(12)&&at(2,12,"Saved Sessions")&&at(2,42,"WiFi Upload Settings"));
    tap(BUTTON_UP); tap(BUTTON_UP); assert(ui->selection==0&&at(2,12,"Start Wardrive"));
    tap(BUTTON_UP); assert(ui->selection==6&&hl(42)&&at(2,42,"Additional Settings")); /* wrap to the end */
    tap(BUTTON_DOWN); assert(ui->selection==0&&hl(12)&&at(2,12,"Start Wardrive")); /* and back to the top */
    tap(BUTTON_DOWN); tap(BUTTON_UP); assert(ui->selection==0);

    /* Start: live dashboard layout (WiFi + BLE). */
    ui->settings.mode=WD_COMBINED; tap(BUTTON_A); assert(ui->screen==WD_SCREEN_LIVE&&ui->session.active&&radio_started);
    assert(at(0,0,"ACTIVE SESSION")&&at(98,0,"06:00")&&find("WIFI: 0")&&find("GPS:")&&at(40,33,"00:00:00")&&find("0m"));
    /* GPS clock is visible even when the platform wall clock is unset. */
    time_t saved_wall=wall;wall=0;frame();assert(at(98,0,"--:--"));
    poom_gps_fix_t gps_clock={.connected=true,.fields=GPS_DATE|GPS_TIME,
        .year=2026,.month=11,.day=13,.hour=7,.minute=8,.second=9};
    assert(wd_clock_update_gps(&ui->clock,&gps_clock,ms()));frame();
    assert(at(98,0,"07:08")&&wall==0);
    memset(&ui->clock,0,sizeof(ui->clock));wall=saved_wall;frame();
    assert(find("WIFI: 0")->x<64&&find("WIFI: 0")->y==17&&any_ink(64,17,127,24)); /* Bluetooth glyph + count on the right */
    assert(at(0,56,"HOLD A:Pause   B:BACK")&&!contains("O:")&&!contains("SD:")&&!contains("D:")&&!contains("WiFi + BLE")); save_preview("02-live");
    assert(any_ink(25,33,30,40)); /* GPS state mark: offline 'X' */
    assert(at(25,33,"X"));
    fix.connected=true; frame(); assert(!find_at(25,33)&&any_ink(25,39,29,39)); /* searching dots */
    fix.fix=true; ui->had_fix=true; frame(); assert(!find_at(25,33)&&any_ink(25,33,29,40)); /* tick */
    fix.fix=false; frame(); assert(at(25,33,"X")); /* fix lost */
    fix=(poom_gps_fix_t){0};
    ui->session.wifi_count=4294967295U; ui->session.ble_count=999999; ui->distance.metres=9999000; frame(); /* widest values */
    assert(find("WIFI: 4.2G")&&find("999k")&&find("9999km")); save_preview("03-live-widest");
    ui->settings.mode=WD_WIFI; frame(); assert(find("WIFI: 4.2G")->x==(128-60)/2&&!find("999k")&&!any_ink(0,17,find("WIFI: 4.2G")->x-1,24)); save_preview("04-live-wifi-only");
    ui->settings.mode=WD_BLE; frame(); assert(!contains("WIFI")&&find("999k")&&find("999k")->x>48&&find("999k")->x<80); save_preview("05-live-ble-only");
    ui->settings.mode=WD_COMBINED; ui->session.wifi_count=0; ui->session.ble_count=0; ui->distance.metres=1800; frame(); assert(find("1.8km"));
    tap(BUTTON_A); assert(!ui->session.paused); /* a normal tap does nothing */

    /* Elapsed timer counts active time only. */
    advance(24*60*1000+18000); frame(); assert(at(40,33,"00:24:18"));
    /* Interrupted 2 s hold: bar shown, then reset; nothing changes. */
    unsigned before=checkpoints;
    ev(BUTTON_A,BUTTON_PRESS_DOWN); advance(1000); frame(); assert(rect_count==1&&fill_width==62&&at(0,56,"Keep holding: Pause")); save_preview("06-pause-hold");
    advance(960); frame(); ev(BUTTON_A,BUTTON_PRESS_UP); frame(); assert(!ui->session.paused&&checkpoints==before&&!rect_count);
    /* Completed hold: pauses exactly once while still held. */
    ev(BUTTON_A,BUTTON_PRESS_DOWN); advance(1999); frame(); assert(!ui->session.paused);
    advance(1); assert(ui->session.paused&&radio_paused&&checkpoints==before+1);
    for(unsigned i=0;i<100;i++) advance(40);
    assert(ui->session.paused&&checkpoints==before+1);
    ev(BUTTON_A,BUTTON_PRESS_UP); ev(BUTTON_A,BUTTON_SINGLE_CLICK);
    assert(ui->session.paused&&at(0,0,"SESSION PAUSED")&&at(0,56,"HOLD A:Resume  B:BACK")); save_preview("07-paused");
    char frozen[22]; snprintf(frozen,sizeof(frozen),"%s",find_at(40,33)->s);
    advance(600000); frame(); assert(at(40,33,frozen)); /* frozen while paused */
    hold_for(BUTTON_A,2040); assert(!ui->session.paused&&!radio_paused);
    advance(1000); frame(); assert(strcmp(find_at(40,33)->s,frozen)>0&&!strncmp(find_at(40,33)->s,"00:24:2",7));

    /* Short B: back to main, recording continues; main shows Resume/Stop. */
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN&&ui->session.active&&!radio_stopped);
    assert(at(2,12,"Resume Session")&&at(2,22,"Stop Session")&&at(2,32,"Check GPS Status")&&at(0,56,"A:SELECT B:EXIT+STOP")); save_preview("08-main-recording");
    goto_main_item(WD_MENU_SAVED); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_MAIN&&!strcmp(ui->message,"STOP SESSION FIRST"));
    goto_main_item(WD_MENU_STOP); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_STOP&&at(0,0,"STOP SESSION?")); save_preview("09-stop-confirm");
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN&&ui->session.active&&!ui->stopping);
    goto_main_item(WD_MENU_START); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_LIVE);
    /* Carousel: shared header with clock; GPS page indicator at the bottom. */
    tap(BUTTON_RIGHT); assert(ui->screen==WD_SCREEN_GPS&&at(0,0,"GPS STATUS")&&at(98,0,"06:00")&&at(110,56,"1/3")&&!contains("FIX ACQUIRED")); save_preview("10-gps-live");
    tap(BUTTON_DOWN); assert(at(110,56,"2/3")&&find("SPEED --")&&find("FIX AGE --"));
    tap(BUTTON_DOWN); assert(at(110,56,"3/3")&&find("SparkFun XA1110")&&find("FIX TYPE --")&&find("I2C NO RESPONSE"));
    tap(BUTTON_RIGHT); assert(ui->screen==WD_SCREEN_STATS&&at(0,0,"SESSION STATS")&&at(98,0,"06:00")&&find("OBSERVATIONS 0")&&find("QUEUE LOST 3")); save_preview("11-stats");
    tap(BUTTON_RIGHT); assert(ui->screen==WD_SCREEN_LIVE);
    /* 3 s B hold from any live page opens Stop confirmation; no Back. */
    tap(BUTTON_RIGHT); hold_for(BUTTON_B,1500); assert(ui->screen==WD_SCREEN_GPS&&ui->session.active); /* released early */
    hold_for(BUTTON_B,3000); assert(ui->screen==WD_SCREEN_STOP&&ui->session.active&&ui->suppress_click[BUTTON_B]); /* cleared by the next press */
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_GPS&&ui->session.active); /* cancel returns to the page */
    tap(BUTTON_LEFT); assert(ui->screen==WD_SCREEN_LIVE);
    ev(BUTTON_B,BUTTON_PRESS_DOWN); advance(1500); frame(); assert(rect_count==1&&at(0,56,"Keep holding: Stop")); save_preview("12-stop-hold");
    advance(1500); assert(ui->screen==WD_SCREEN_STOP);
    ev(BUTTON_B,BUTTON_SINGLE_CLICK); assert(ui->screen==WD_SCREEN_STOP); /* a stray click after the hold is ignored */
    ev(BUTTON_B,BUTTON_PRESS_UP);
    tap(BUTTON_A); assert(ui->stopping&&radio_stopped&&ui->screen==WD_SCREEN_LIVE&&at(0,56,"STOPPING..."));
    ui->session.active=false; ui->stopping=false; radio_done=false; ui->screen=WD_SCREEN_MAIN; ui->selection=0; frame();

    /* Standalone GPS: page headings, no clock, indicator at the bottom. */
    goto_main_item(WD_MENU_GPS); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_GPS&&at(0,0,"GPS POSITION")&&!find("06:00")&&at(110,56,"1/3"));
    assert(at(0,56,"A:RAW R:SD B:BACK")&&any_ink(122,23,126,25));
    tap(BUTTON_UP); assert(at(0,0,"GPS DIAGNOSTICS")); tap(BUTTON_UP); assert(at(0,0,"GPS MOVEMENT"));
    fix=(poom_gps_fix_t){.connected=true,.fix=true,.fields=GPS_POSITION|GPS_SATELLITES|GPS_SPEED|GPS_ALTITUDE|GPS_HDOP|GPS_TYPE,
        .latitude=-33.86,.longitude=151.2,.satellites=12,.speed_kmh=12.3,.altitude_m=15.4,.hdop=0.8,.fix_type=3,.position_ms=ms()-1000};
    frame(); assert(find("SPEED 12.3km/h")&&find("ALT 15.4m")&&find("HDOP 0.8")&&find("FIX AGE 1s")); save_preview("13-gps-movement");
    tap(BUTTON_DOWN); assert(find("FIX TYPE 3D")); save_preview("14-gps-diagnostics");
    tap(BUTTON_DOWN); assert(find("LAT -33.860000")&&find("SAT 12")&&find("FIX")&&!find_at(24,38)); save_preview("15-gps-position");
    hold_for(BUTTON_B,3000); assert(ui->screen==WD_SCREEN_GPS); /* no recording: no stop hold */
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN); fix=(poom_gps_fix_t){0};

    /* Saved Sessions: DD.MM.YY rows and status marks. */
    make_session("a-pending","2026-11-08 06:56:01"); make_session("b-submitted","2026-11-08 06:56:02");
    make_session("c-failed","2026-11-08 06:56:03"); make_session("d-review","2026-11-08 06:56:04");
    make_session("e-confirmed","2026-11-09 07:00:00");
    set_state("b-submitted",(wd_upload_status_t){.state=WD_SUBMITTED}); set_state("c-failed",(wd_upload_status_t){.state=WD_FAILED,.attempts=1});
    set_state("d-review",(wd_upload_status_t){.state=WD_NEEDS_REVIEW}); set_state("e-confirmed",(wd_upload_status_t){.state=WD_CONFIRMED});
    goto_main_item(WD_MENU_SAVED); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_SAVED&&at(0,0,"SAVED SESSIONS"));
    assert(at(2,12,"09.11.26 07:00:00")&&at(2,22,"08.11.26 06:56:04")&&at(2,42,"08.11.26 06:56:02"));
    assert(!find_at(120,12)&&any_ink(120,12,125,19)); /* Confirmed -> tick */
    assert(at(120,22,"X")&&at(120,32,"X")&&!find_at(120,42)&&any_ink(120,42,125,49)&&at(0,56,"1/5 A:OPEN B:BACK")); save_preview("16-saved-sessions");
    for(unsigned i=0;i<4;i++) tap(BUTTON_DOWN);
    assert(at(2,42,"08.11.26 06:56:01")&&at(120,42,"-")&&!strcmp(ui->selected_id,"a-pending"));

    /* Selected session: CSV gate. */
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_SESSION&&at(0,11,"08.11.26 06:56 UTC"));
    assert(at(2,22,"Create WiGLE CSV")&&at(2,32,"Additional Options")&&!contains("Upload Session")&&at(0,56,"Upload: Pending")); save_preview("17-session-no-csv");
    tap(BUTTON_A); assert(last_job==WD_JOB_EXPORT&&ui->screen==WD_SCREEN_WORKING&&at(0,20,"Creating WiGLE CSV..."));
    mkfile(WD_ROOT "/sessions/a-pending/wigle.csv","WigleWifi-1.6\n"); mkfile(WD_ROOT "/sessions/a-pending/export.json","{\"exported\":4}");
    ui->job.processed=4; ui->job.failed=1; finish_job(true,"DONE");
    assert(ui->screen==WD_SCREEN_FILE_RESULT&&at(0,0,"CSV ready")&&find("CSV rows: 4")&&at(0,56,"A:Upload B:Back"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SESSION&&at(2,22,"Regenerate WiGLE CSV")&&at(2,32,"Upload Session")&&at(2,42,"Additional Options")); save_preview("18-session-csv");
    tap(BUTTON_DOWN); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_UPLOAD_CHOICE&&at(2,14,"Transfer over WiFi")&&at(2,24,"Upload to WiGLE")); save_preview("19-upload-session");
    /* Transfer: CSV only uses the existing CSV; raw bundles a TAR first. */
    unsigned starts=job_starts;
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_TRANSFER_FORMAT&&at(2,14,"WiGLE CSV only")&&at(2,24,"Raw data + WiGLE CSV")); save_preview("20-transfer-format");
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_TRANSFER&&at(0,0,"Transfer over WiFi")&&job_starts==starts); save_preview("21-transfer");
    tap(BUTTON_B); tap(BUTTON_DOWN); tap(BUTTON_A); assert(last_job==WD_JOB_ARCHIVE&&job_starts==starts+1);
    finish_job(true,"DONE"); assert(ui->screen==WD_SCREEN_ARCHIVE_RESULT&&at(0,0,"Download ready"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_TRANSFER); tap(BUTTON_B); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_UPLOAD_CHOICE);

    /* Upload to WiGLE: WiFi chooser when disconnected, API shortcut when incomplete. */
    tap(BUTTON_DOWN); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_CHOOSE&&ui->wifi_purpose==WD_JOB_WIFI_JOIN&&ui->wifi_return==WD_SCREEN_UPLOAD_MENU);
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_PROFILES); tap(BUTTON_A); assert(last_job==WD_JOB_WIFI_JOIN&&!strcmp(ui->credentials.ssid,"saved-network"));
    link_up=true; finish_job(true,"DONE"); assert(ui->screen==WD_SCREEN_UPLOAD_MENU&&at(0,0,"UPLOAD TO WIGLE"));
    assert(at(2,14,"Upload Run")&&at(2,24,"WiFi: saved-network")&&at(2,34,"Configure WiGLE API")&&at(0,56,"API keys needed")); save_preview("22-upload-menu-no-keys");
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_UPLOAD_MENU&&!strcmp(ui->message,"Set WiGLE API keys"));
    tap(BUTTON_UP); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_API&&ui->api_return==WD_SCREEN_UPLOAD_MENU);
    assert(at(2,12,"API Name: <Blank>")&&at(2,22,"API Token: <Blank>")&&at(2,32,"Import SD Setup")&&at(2,42,"Clear API Keys")&&!contains("Delete Setup"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_KEYBOARD&&!ui->edit[0]&&!strcmp(keyboard_label,"WiGLE API Name"));
    strcpy(ui->edit,"synthetic-name"); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_API&&!strcmp(ui->message,"API name saved"));
    tap(BUTTON_DOWN); tap(BUTTON_A); strcpy(ui->edit,"synthetic-secret-token"); tap(BUTTON_A);
    assert(at(2,12,"API Name: ********")&&at(2,22,"API Token: ********")&&!contains("synthetic")); save_preview("23-api-settings");
    char stored[160]; size_t stored_len=sizeof(stored); assert(poom_secrets_get_str("wd_api_token",stored,&stored_len)==ESP_OK&&!strcmp(stored,"synthetic-secret-token"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_KEYBOARD&&!ui->edit[0]); tap(BUTTON_A); assert(!strcmp(ui->message,"UNCHANGED")&&!strcmp(ui->credentials.api_token,"synthetic-secret-token"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_UPLOAD_MENU&&at(2,14,"Upload Run")&&at(2,24,"WiFi: saved-network")&&!find_at(2,34)&&at(0,56,"WiFi connected")); save_preview("24-upload-menu");
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_CONSENT&&at(0,0,"Upload to WiGLE?")); tap(BUTTON_A);
    assert(last_job==WD_JOB_UPLOAD&&ui->screen==WD_SCREEN_PROGRESS&&at(0,0,"UPLOADING TO WIGLE"));
    atomic_store(&ui->job.stage,WD_STAGE_CONNECTING); frame(); assert(find("Connecting...")&&!contains("%")&&rect_count==1&&fill_width==20);
    atomic_store(&ui->job.stage,WD_STAGE_SENDING); atomic_store(&ui->job.sent,450); atomic_store(&ui->job.total,1000); frame();
    assert(find("Sending CSV")&&find("45%")&&fill_width==124*450/1000); save_preview("25-upload-progress");
    atomic_store(&ui->job.stage,WD_STAGE_AWAITING); atomic_store(&ui->job.sent,1000); frame(); assert(find("Awaiting response...")&&find("100%"));
    ui->job.processed=0; ui->job.failed=1; set_state("a-pending",(wd_upload_status_t){.state=WD_FAILED,.attempts=1,.may_have_sent=true});
    finish_job(false,"Upload timed out"); assert(ui->screen==WD_SCREEN_UPLOAD_RESULT&&at(0,12,"Upload timed out")&&find("Failed: 1")); save_preview("26-upload-result");
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SESSION&&at(0,56,"Upload: Failed"));
    /* Normal retry path for a Failed run: no CSV regeneration or reset. */
    tap(BUTTON_DOWN); tap(BUTTON_A); tap(BUTTON_DOWN); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_UPLOAD_MENU); /* link reused */
    tap(BUTTON_A); tap(BUTTON_A); assert(last_job==WD_JOB_UPLOAD);
    ui->job.processed=1; ui->job.failed=0; set_state("a-pending",(wd_upload_status_t){.state=WD_SUBMITTED,.attempts=2});
    finish_job(true,"SUBMITTED TO WiGLE"); tap(BUTTON_B); assert(at(0,56,"Upload: Submitted"));

    /* Additional Options: scrollable notices; A only after the full text. */
    ui->session_action=2; frame(); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_OPTIONS&&at(2,14,"Recover Session Data")&&at(2,24,"Mark as Submitted")&&at(2,34,"Reset Upload Status")); save_preview("27-options");
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_NOTICE&&at(0,0,"RECOVER SESSION DATA")&&at(0,11,"Repairs a session")&&at(0,56,"DOWN:More  B:Back")); save_preview("28-recover-notice");
    starts=job_starts; tap(BUTTON_A); assert(job_starts==starts&&ui->screen==WD_SCREEN_NOTICE);
    unsigned scrolls=0; while(!notice_at_end()) { tap(BUTTON_DOWN); scrolls++; assert(scrolls<30); }
    assert(scrolls>3&&find("Would you like to")&&at(0,56,"A:Yes  B:Back")); save_preview("29-recover-notice-end");
    tap(BUTTON_A); assert(last_job==WD_JOB_RECOVER&&job_starts==starts+1);
    assert(!unlink(WD_ROOT "/sessions/a-pending/wigle.csv")&&!unlink(WD_ROOT "/sessions/a-pending/export.json")); /* job invalidates */
    finish_job(true,"DONE"); assert(at(0,0,"Recovery report")); tap(BUTTON_B);
    assert(ui->screen==WD_SCREEN_SESSION&&at(2,22,"Create WiGLE CSV")&&!contains("Upload Session")&&at(0,56,"Upload: Submitted"));
    wd_upload_status_t other=state_of("c-failed");
    tap(BUTTON_DOWN); tap(BUTTON_A); tap(BUTTON_DOWN); tap(BUTTON_DOWN); tap(BUTTON_A); assert(at(0,0,"RESET UPLOAD STATUS"));
    while(!notice_at_end()) tap(BUTTON_DOWN);
    assert(contains("duplicate")&&at(0,56,"A:Yes  B:No")); tap(BUTTON_A);
    assert(ui->screen==WD_SCREEN_SESSION&&state_of("a-pending").state==WD_PENDING&&!state_of("a-pending").attempts&&!strcmp(ui->message,"Reset to Pending"));
    assert(!memcmp(&other,&(wd_upload_status_t){.state=WD_FAILED,.attempts=1},sizeof(other))&&state_of("c-failed").state==WD_FAILED); /* others untouched */
    tap(BUTTON_DOWN); tap(BUTTON_A); tap(BUTTON_DOWN); tap(BUTTON_A); assert(at(0,0,"MARK AS SUBMITTED"));
    while(!notice_at_end()) tap(BUTTON_DOWN);
    tap(BUTTON_A); assert(state_of("a-pending").state==WD_SUBMITTED&&state_of("a-pending").manual&&state_of("c-failed").state==WD_FAILED);
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SAVED); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN);

    /* Bulk: explanation, menu, 3 s hold. */
    goto_main_item(WD_MENU_BULK); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_NOTICE&&at(0,0,"BULK WIGLE UPLOAD")&&at(0,11,"Creating any"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_NOTICE); /* not read to the end yet */
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN); tap(BUTTON_A);
    while(!notice_at_end()) tap(BUTTON_DOWN);
    assert(find("Are you sure you wish")&&at(0,56,"A:Yes  B:No")); tap(BUTTON_A);
    assert(ui->screen==WD_SCREEN_BULK_MENU&&at(2,14,"Upload All Pending")&&at(2,24,"WiFi: saved-network")&&!find_at(2,34));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_BULK_HOLD&&find("Hold A for 3 seconds")); save_preview("30-bulk-hold");
    starts=job_starts; tap(BUTTON_A); hold_for(BUTTON_A,2960); assert(job_starts==starts&&ui->screen==WD_SCREEN_BULK_HOLD);
    ev(BUTTON_A,BUTTON_PRESS_DOWN); advance(1500); frame(); assert(rect_count==1&&fill_width==62); save_preview("31-bulk-hold-progress");
    advance(1500); assert(last_job==WD_JOB_ALL&&job_starts==starts+1&&ui->screen==WD_SCREEN_PROGRESS);
    for(unsigned i=0;i<50;i++) advance(40);
    assert(job_starts==starts+1); ev(BUTTON_A,BUTTON_PRESS_UP);
    frame(); assert(at(0,0,"BULK WIGLE UPLOAD")&&find("Checking sessions..."));
    atomic_store(&ui->job.count,5); atomic_store(&ui->job.index,2); atomic_store(&ui->job.stage,WD_STAGE_SENDING); atomic_store(&ui->job.sent,500); atomic_store(&ui->job.total,1000);
    atomic_store(&ui->job.submitted,1); frame(); assert(find("Run 2 of 5")&&find("Sending CSV 50%")&&find("OK 1  Failed 0")&&fill_width==124*300/1000); save_preview("32-bulk-progress");
    ui->job.processed=3; ui->job.failed=1; ui->job.skipped=1; finish_job(false,"Upload timed out");
    assert(ui->screen==WD_SCREEN_UPLOAD_RESULT&&find("Submitted: 3")&&find("Failed: 1")&&find("Skipped: 1")); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN);

    /* API settings: setup file deletion needs a continuous 3 s A hold. */
    mkfile(WD_SETUP_FILE,"{}");
    goto_main_item(WD_MENU_API); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_API&&at(0,0,"WIGLE API SETTINGS")&&at(2,42,"Clear API Keys"));
    for(unsigned i=0;i<4;i++) tap(BUTTON_DOWN);
    assert(at(2,42,"Delete Setup File")); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_DELETE_SETUP&&at(0,56,"HOLD A:Delete B:Keep"));
    struct stat st; tap(BUTTON_A); assert(!stat(WD_SETUP_FILE,&st));
    hold_for(BUTTON_A,2000); assert(!stat(WD_SETUP_FILE,&st)&&ui->screen==WD_SCREEN_DELETE_SETUP);
    ev(BUTTON_A,BUTTON_PRESS_DOWN); advance(1500); frame(); assert(any_ink(112,28,124,28)); /* ring partly filled */ save_preview("33-delete-setup-hold");
    advance(1500); assert(stat(WD_SETUP_FILE,&st)&&ui->screen==WD_SCREEN_API&&!strcmp(ui->message,"SETUP FILE DELETED"));
    ev(BUTTON_A,BUTTON_PRESS_UP);
    assert(ui->credentials.api_token[0]&&nvs_find("wd_api_token",false)); /* stored keys survive */
    wd_credentials_t reloaded={0}; assert(wd_credentials_load(&reloaded)&&!strcmp(reloaded.api_name,"synthetic-name")); wd_credentials_clear(&reloaded);
    ui->api_row=3; frame(); tap(BUTTON_A); assert(!ui->credentials.api_name[0]&&!nvs_find("wd_api_name",false)&&!nvs_find("wd_api_token",false)&&at(2,12,"API Name: <Blank>"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN);

    /* WiFi Upload Settings reuse the chooser and show the live link. */
    goto_main_item(WD_MENU_WIFI); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_SETTINGS&&at(0,0,"WIFI UPLOAD SETTINGS")&&find("Connected")&&at(2,44,"Disconnect")); save_preview("34-wifi-settings");
    tap(BUTTON_DOWN); unsigned drops=disconnects; tap(BUTTON_A); assert(disconnects==drops+1&&!link_up&&find("Not connected")&&!find_at(2,44));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_CHOOSE&&ui->wifi_return==WD_SCREEN_WIFI_SETTINGS); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_WIFI_SETTINGS);
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN);

    /* Additional Settings: six rows, L/R adjust, save only after confirm. */
    goto_main_item(WD_MENU_SETTINGS); tap(BUTTON_A);
    assert(ui->screen==WD_SCREEN_SETTINGS&&at(0,0,"ADDITIONAL SETTINGS")&&at(2,12,"Scan Type WiFi + BLE")&&at(0,56,"L/R:ADJUST B:EXIT")&&!contains("U/D")); save_preview("35-additional-settings");
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN); /* unchanged: no prompt */
    wd_settings_t original=ui->settings; unsigned writes=settings_writes;
    tap(BUTTON_A); tap(BUTTON_RIGHT); assert(ui->settings.mode==WD_WIFI&&at(2,12,"Scan Type WiFi"));
    tap(BUTTON_DOWN); tap(BUTTON_LEFT); tap(BUTTON_DOWN); tap(BUTTON_DOWN); tap(BUTTON_RIGHT);
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SAVE_CHANGES&&at(0,0,"SAVE CHANGES?")&&at(0,24,"A: Yes")&&at(0,36,"B: No")); save_preview("36-save-changes");
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN&&!memcmp(&ui->settings,&original,sizeof(ui->settings))&&settings_writes==writes);
    tap(BUTTON_A); for(unsigned i=0;i<5;i++) tap(BUTTON_DOWN);
    assert(ui->setting_row==5&&at(0,56,"A:OPEN     B:EXIT")); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_TIMEZONE);
    wd_timezone_config_t zone=ui->time_zone.config; tap(BUTTON_RIGHT); assert(memcmp(&zone,&ui->time_zone.config,sizeof(zone)));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SETTINGS); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SAVE_CHANGES);
    tap(BUTTON_B); assert(!memcmp(&zone,&ui->time_zone.config,sizeof(zone))&&settings_writes==writes); /* nested change discarded */
    tap(BUTTON_A); ui->setting_row=3; frame(); tap(BUTTON_RIGHT); int rssi=ui->settings.rssi_threshold;
    tap(BUTTON_B); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_MAIN&&settings_writes==writes+1&&!strcmp(ui->message,"SETTINGS SAVED"));
    wd_settings_t saved; size_t length=sizeof(saved); assert(poom_secrets_get_blob("wd_settings",&saved,&length)==ESP_OK&&saved.rssi_threshold==rssi);
    /* OLED transitions blank only on page changes; steady frames don't. */
    ui->screen=WD_SCREEN_MAIN; ui->presented_screen=UINT_MAX; frame(); assert(!strcmp(events,"0F1"));
    frame(); assert(!strcmp(events,"F"));
    ui->screen=WD_SCREEN_GPS; ui->gps_page=0; frame(); assert(!strcmp(events,"0F1"));
    ui->gps_page=1; frame(); assert(!strcmp(events,"0F1")); frame(); assert(!strcmp(events,"F"));
    ui->screen=WD_SCREEN_MAIN; ui->message[0]=0; frame();

    /* Time zone persistence, defaults for corrupt data, clocks. */
    poom_secrets_erase_key("wd_timezone"); timezone_load(); assert(ui->time_zone.config.automatic&&ui->time_zone.zone==WD_TZ_UTC);
    ui->time_zone.config.automatic=0; ui->time_zone.config.manual_zone=WD_TZ_SYDNEY; assert(timezone_save());
    memset(&ui->time_zone,0,sizeof(ui->time_zone)); timezone_load(); assert(!ui->time_zone.config.automatic&&ui->time_zone.zone==WD_TZ_SYDNEY);
    poom_secrets_set_blob("wd_timezone","x",1); timezone_load(); assert(ui->time_zone.config.automatic&&ui->time_zone.zone==WD_TZ_UTC);
    wd_timezone_config_t bad={.version=2,.automatic=0,.manual_zone=WD_TZ_SYDNEY}; poom_secrets_set_blob("wd_timezone",&bad,sizeof(bad));
    timezone_load(); assert(ui->time_zone.config.automatic);
    ui->time_zone.config.automatic=0; ui->time_zone.config.manual_zone=WD_TZ_SYDNEY; wd_timezone_init(&ui->time_zone,&ui->time_zone.config);
    ui->screen=WD_SCREEN_TIMEZONE; ui->timezone_row=1; frame(); assert(at(0,0,"TIME ZONE")&&at(0,42,"17:00:00 AEDT MAN"));
    ui->screen=WD_SCREEN_STATS; frame(); assert(at(98,0,"17:00"));
    wall=0; frame(); assert(at(98,0,"--:--")); ui->screen=WD_SCREEN_TIMEZONE; frame(); assert(at(0,42,"TIME WAIT GPS/NET"));
    wall=1794117600; ui->screen=WD_SCREEN_MAIN;

    /* Saved Sessions viewport: 8 runs, moving highlight, edges, re-entry. */
    make_session("f-extra","2026-11-07 01:00:00"); make_session("g-extra","2026-11-07 01:00:01"); make_session("h-extra","2026-11-07 01:00:02");
    static const char *const order[]={"e-confirmed","d-review","c-failed","b-submitted","a-pending","h-extra","g-extra","f-extra"};
    goto_main_item(WD_MENU_SAVED); tap(BUTTON_A); assert(ui->saved_count==8&&!strcmp(ui->selected_id,order[0]));
    tap(BUTTON_UP); assert(ui->saved_row==0&&!strcmp(ui->selected_id,order[0]));
    for(unsigned i=1;i<8;i++) {
        unsigned reads=status_reads;
        tap(BUTTON_DOWN); assert(ui->saved_index==i&&ui->saved_row==(i<4?i:3)&&!strcmp(ui->selected_id,order[i]));
        assert(status_reads==reads+(i<4?0:1)); /* no SD reads inside the window; one for a new row */
        assert(at(2,12+10*ui->saved_row,ui->rows[ui->saved_row])&&!strcmp(ui->rows[ui->saved_row],ui->saved_page.entries[ui->saved_row].label));
        tap(BUTTON_A); assert(ui->screen==WD_SCREEN_SESSION&&!strcmp(ui->selected_id,order[i]));
        tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SAVED&&!strcmp(ui->selected_id,order[i])&&ui->saved_index==i);
    }
    tap(BUTTON_DOWN); assert(ui->saved_index==7);
    for(unsigned i=7;i>0;i--) { tap(BUTTON_UP); assert(ui->saved_index==i-1&&!strcmp(ui->selected_id,order[i-1])); }
    assert(ui->saved_row==0&&!strcmp(ui->saved_page.entries[0].id,order[0])&&at(2,12,ui->saved_page.entries[0].label));
    /* Marks follow their rows through shifts. */
    for(unsigned i=0;i<4;i++) assert(ui->row_status[i]==status_code(ui->saved_page.entries[i].id));

    /* WiFi chooser for a transfer: profiles, scan, passwords, hidden SSID, saving. */
    tap(BUTTON_A); ui->session_action=0; ui->credentials.ssid[0]=0; ui->credentials.password[0]=0; link_up=false;
    ui->screen=WD_SCREEN_TRANSFER; frame(); assert(at(0,56,"A:Choose WiFi B:Back"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_CHOOSE&&ui->wifi_purpose==WD_JOB_TRANSFER&&at(2,14,"Saved WiFi profiles"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_TRANSFER);
    starts=job_starts; launch(WD_JOB_TRANSFER); assert(ui->screen==WD_SCREEN_TRANSFER&&job_starts==starts);
    tap(BUTTON_A); tap(BUTTON_DOWN); tap(BUTTON_A); assert(last_job==WD_JOB_WIFI_SCAN&&at(0,20,"Scanning networks..."));
    network_total=2; finish_job(true,"DONE"); assert(ui->screen==WD_SCREEN_WIFI_NETWORKS&&contains("new-network"));
    tap(BUTTON_DOWN); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_NETWORKS&&!strcmp(ui->message,"Use WPA2/3 password"));
    tap(BUTTON_UP); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_KEYBOARD&&ui->edit_field==16&&!strcmp(keyboard_label,"new-network"));
    starts=job_starts; strcpy(ui->edit,"short"); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_KEYBOARD&&job_starts==starts&&!strcmp(keyboard_label,"8-63 chars or 64 hex"));
    strcpy(ui->edit,"synthetic-password"); tap(BUTTON_A); assert(last_job==WD_JOB_TRANSFER&&ui->screen==WD_SCREEN_WORKING&&at(0,20,"Joining WiFi..."));
    assert(!strcmp(ui->job.credentials.ssid,"new-network")&&!strcmp(ui->job.credentials.password,"synthetic-password")&&!ui->edit[0]&&!ui->wifi_ssid[0]);
    finish_job(false,"WIFI / DHCP TIMEOUT"); assert(ui->screen==WD_SCREEN_WIFI_ERROR&&at(0,12,"WIFI / DHCP TIMEOUT"));
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_WIFI_CHOOSE); tap(BUTTON_DOWN); tap(BUTTON_DOWN); tap(BUTTON_A);
    assert(ui->screen==WD_SCREEN_KEYBOARD&&ui->edit_field==17); tap(BUTTON_A); assert(ui->edit_field==17&&!strcmp(keyboard_label,"Enter network name"));
    strcpy(ui->edit,"hidden-network"); tap(BUTTON_A); assert(ui->edit_field==16&&!strcmp(ui->wifi_ssid,"hidden-network"));
    strcpy(ui->edit,"cancel-me"); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_WIFI_CHOOSE&&!ui->edit[0]&&!ui->wifi_ssid[0]&&!strcmp(ui->credentials.ssid,"new-network"));
    network_open=true; tap(BUTTON_UP); tap(BUTTON_UP); tap(BUTTON_DOWN); tap(BUTTON_A); finish_job(true,"DONE"); tap(BUTTON_A);
    assert(last_job==WD_JOB_TRANSFER&&!ui->job.credentials.password[0]); network_open=false;
    strcpy(ui->job.ip,"192.168.100.100"); strcpy(ui->job.token,"0123456789"); finish_job(true,"DONE");
    assert(ui->screen==WD_SCREEN_WIFI_SAVE&&at(0,0,"Save WiFi profile?")&&!profile_saves); tap(BUTTON_A);
    assert(profile_saves==1&&ui->screen==WD_SCREEN_TRANSFER_READY&&at(0,33,"0123456789")&&at(0,43,"10 min from start")); save_preview("37-transfer-ready");
    ui->offer_wifi_save=true; launch(WD_JOB_TRANSFER); finish_job(true,"DONE"); profile_status=ESP_ERR_NO_MEM; tap(BUTTON_A);
    assert(ui->screen==WD_SCREEN_WIFI_SAVE&&!strcmp(ui->message,"No space for profile")&&profile_saves==1); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_TRANSFER_READY);
    drops=disconnects; tap(BUTTON_B); assert(ui->screen==WD_SCREEN_SESSION&&disconnects==drops+1&&!ui->transfer_on&&!ui->job.token[0]);
    profile_status=ESP_OK;
    ui->session.active=true; launch(WD_JOB_EXPORT); assert(!strcmp(ui->message,"STOP SESSION FIRST")); ui->session.active=false;
    ui->gps_capture.file=tmpfile(); assert(ui->gps_capture.file); launch(WD_JOB_EXPORT); assert(!strcmp(ui->message,"STOP GPS CAPTURE")); fclose(ui->gps_capture.file); ui->gps_capture.file=NULL;
    tap(BUTTON_B); tap(BUTTON_B); assert(ui->screen==WD_SCREEN_MAIN);

    /* SD setup import: no read before A, keys persisted, then delete prompt. */
    goto_main_item(WD_MENU_API); tap(BUTTON_A); assert(at(2,12,"API Name: <Blank>"));
    tap(BUTTON_DOWN); tap(BUTTON_DOWN); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_IMPORT&&at(0,0,"Import SD setup")&&at(0,56,"A:Yes B:No"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_API&&!ui->credentials.api_name[0]);
    tap(BUTTON_A); tap(BUTTON_A); assert(ui->screen==WD_SCREEN_IMPORT&&!strcmp(ui->message,"Invalid SD setup file")); /* file missing */
    mkfile(WD_SETUP_FILE,"{\"wigle\":{\"api_name\":\"imported-name\",\"api_token\":\"imported-token\",\"auto_upload\":true}}");
    tap(BUTTON_A); assert(ui->screen==WD_SCREEN_DELETE_SETUP&&at(0,0,"SD setup imported")&&at(0,12,"Credentials imported")&&!ui->credentials.auto_upload);
    stored_len=sizeof(stored); assert(poom_secrets_get_str("wd_api_name",stored,&stored_len)==ESP_OK&&!strcmp(stored,"imported-name"));
    tap(BUTTON_B); assert(ui->screen==WD_SCREEN_API&&api_rows()==5&&at(2,12,"API Name: ********")); tap(BUTTON_B);

    /* A job finishing while Wardrive exits releases WiFi and returns to main. */
    ui->exiting=true; launch(WD_JOB_EXPORT); drops=disconnects; finish_job(true,"DONE"); assert(ui->screen==WD_SCREEN_MAIN&&disconnects==drops+1); ui->exiting=false;
    /* Closing Wardrive returns the whole state block (wiped) to other apps. */
    strcpy(ui->credentials.api_token,"synthetic-secret"); ui_free(); assert(!ui);
    assert(ui_alloc()&&!ui->credentials.api_token[0]&&ui->stop_return==WD_SCREEN_MAIN&&!ui->clock.utc); ui_free();
    printf("Production Wardrive UI: menus, live dashboard/holds/timer, GPS pages, saved sessions/CSV gate, transfer, single/bulk upload, notices, API keys, WiFi and settings flows passed (%u frames)\n",frames);
}
