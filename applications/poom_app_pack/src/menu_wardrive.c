// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
#include "menu_wardrive.h"
#include "poom_gps_capture.h"
#include <errno.h>
#include "wardrive_ui_nav.h"
#include "wardrive_ui_internal.h"
#include "wardrive_ui_logic.h"
#include "wardrive_ui_wifi.h"
#include "poom_ui_keyboard.h"
#include "poom_secrets_store.h"
#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <stdlib.h>
#include "poom_wardrive.h"
#include "wardrive_distance.h"
#include "wardrive_sessions.h"
#include "wardrive_timezone.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "sd_card.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
/* Raw press/release and short clicks; long holds are timed here so every
 * hold has its own duration, progress feedback and exactly-once firing. */
typedef struct { uint8_t button, event; } wd_key_event_t;
static QueueHandle_t buttons;
static portMUX_TYPE button_lock=portMUX_INITIALIZER_UNLOCKED;
static atomic_bool visible;
/* BEGIN UI STATE (host tests copy this block verbatim) */
typedef enum { WD_NOTICE_RECOVER,WD_NOTICE_MARK,WD_NOTICE_RESET,WD_NOTICE_BULK } wd_notice_t;
typedef enum { WD_ACTION_CSV,WD_ACTION_UPLOAD,WD_ACTION_OPTIONS } wd_session_action_t;
typedef enum { WD_UPLOAD_ROW_RUN,WD_UPLOAD_ROW_WIFI,WD_UPLOAD_ROW_API } wd_upload_row_t;
#define WD_SETUP_FILE WD_ROOT "/config/wardrive-setup.json"
/* All Wardrive UI state lives in one block allocated while the app is open
 * (internal RAM) and freed on exit, so other apps get this RAM back. */
typedef struct {
    poom_gps_service_t gps;
    poom_gps_capture_t gps_capture;
    wd_session_t session;
    wd_settings_t settings,settings_before;
    wd_timezone_state_t time_zone;
    wd_clock_t clock;
    wd_timezone_config_t timezone_before;
    unsigned timezone_row;
    unsigned screen,selection,gps_page,setting_row;
    unsigned presented_screen,presented_gps_page;
    bool exiting,stopping,mounted_here,raw,had_fix;
    uint64_t started,last_raw,last_gps_log,last_checkpoint;
    wd_elapsed_t elapsed;
    wd_hold_t hold;
    bool suppress_click[6];
    wd_credentials_t credentials;
    wd_ui_job_t job;
    poom_ui_keyboard_t keyboard;
    char edit[129],selected_id[49],rows[4][49],row_status[4];
    unsigned saved_index,saved_count,saved_row,session_action,edit_field;
    char selected_status[22],selected_label[22];
    bool selected_csv,transfer_on,offer_wifi_save,setup_imported,setup_present;
    wd_ui_profiles_t wifi_profiles;
    unsigned wifi_selection,wifi_keyboard_return,wifi_return;
    unsigned upload_row,api_row,option_row,choice_row,edit_return_screen;
    unsigned api_return,stop_return,notice_return;
    unsigned notice,notice_top;
    wd_job_t wifi_purpose;
    char wifi_ssid[33];
    wd_distance_t distance;
    char message[64];
    wd_session_page_t saved_page;
    wd_session_cache_t saved_cache;
    unsigned notice_count;
    uint8_t menu_tops[WD_SCREEN_COUNT];
} wd_ui_state_t;
static wd_ui_state_t *ui;
static bool ui_alloc(void);
static void ui_free(void);
/* END UI STATE */
static void launch(wd_job_t type);
static void saved_refresh(int direction);
static void line(unsigned y,const char *s) {
    char text[22];
    snprintf(text,sizeof(text),"%.*s",21,s);
    for(size_t i=0;text[i];i++) if((unsigned char)text[i]<32||(unsigned char)text[i]>126) text[i]='?';
    if(!text[0]) return;
    poom_arduboy_set_cursor(0,(int16_t)y); poom_arduboy_print(text);
}
static void text_at(int16_t x,unsigned y,const char *s) {
    char text[22]; size_t room=(size_t)((128-x)/6);
    snprintf(text,sizeof(text),"%.*s",(int)(room<21?room:21),s);
    for(size_t i=0;text[i];i++) if((unsigned char)text[i]<32||(unsigned char)text[i]>126) text[i]='?';
    if(!text[0]) return;
    poom_arduboy_set_cursor(x,(int16_t)y); poom_arduboy_print(text);
}
/* Selection bar as in the main POOM menu: a full-width 9-pixel row inverted
 * after its contents are drawn (black text on white). */
static void row_highlight(unsigned y) { poom_arduboy_fill_rect(0,(int16_t)(y-1),128,9,INVERT); }
/* Menu row: label from x=2 (up to 20 columns), highlighted when selected. */
static void menu_row(unsigned y,bool selected,const char *label) {
    text_at(2,y,label);
    if(selected) row_highlight(y);
}
extern const uint8_t poom_arduboy_font5x7[];
static void compact_line(unsigned y,const char *text) {
    /* Reuse the existing mixed-case font with proportional column spacing.
     * The shared 3x5 technical font uppercases letters; keep this UI local. */
    unsigned x=0;
    for(unsigned i=0;text[i]&&i<64;i++) {
        unsigned char c=(unsigned char)text[i];
        if(c==' ') { x+=3; continue; }
        if(c<32||c>126) c='?';
        const uint8_t *glyph=poom_arduboy_font5x7+(c-32)*5;
        unsigned first=0,last=5;
        while(first<5&&!glyph[first]) first++;
        while(last>first&&!glyph[last-1]) last--;
        unsigned width=last-first;
        if(x+width>128) break;
        for(unsigned col=first;col<last;col++) for(unsigned row=0;row<7;row++)
            if(glyph[col]&(1U<<row)) poom_arduboy_draw_pixel((int16_t)(x+col-first),(int16_t)(y+row),WHITE);
        x+=width+1;
    }
}
static void glyph(int16_t x,unsigned y,const uint8_t columns[5]) {
    for(unsigned col=0;col<5;col++) for(unsigned row=0;row<8;row++)
        if(columns[col]&(1U<<row)) poom_arduboy_draw_pixel((int16_t)(x+col),(int16_t)(y+row),WHITE);
}
/* Upload status: '-' Pending, tick Submitted, 'X' Failed, '?' unreadable. */
static void status_mark(int16_t x,unsigned y,char status) {
    if(status=='v') glyph(x,y,wd_glyph_check);
    else { char text[2]={status,0}; text_at(x,y,text); }
}
static void gps_mark(int16_t x,unsigned y,wd_gps_icon_t icon) {
    if(icon==WD_GPS_ICON_FIX) glyph(x,y,wd_glyph_check);
    else if(icon==WD_GPS_ICON_NONE) text_at(x,y,"X");
    else for(unsigned i=0;i<3;i++) poom_arduboy_draw_pixel((int16_t)(x+i*2),(int16_t)(y+6),WHITE);
}
/* Filled bar from real progress (0..1000), or a moving block when unknown. */
static void progress_bar(unsigned y,int permille,uint64_t now) {
    poom_arduboy_draw_rect(0,(int16_t)y,128,6,WHITE);
    if(permille<0) { int16_t x=(int16_t)(2+((now/40)*4)%104); poom_arduboy_fill_rect(x,(int16_t)(y+2),20,2,WHITE); return; }
    int16_t width=(int16_t)(124*(permille>1000?1000:permille)/1000);
    if(width>0) poom_arduboy_fill_rect(2,(int16_t)(y+2),width,2,WHITE);
}
/* Hold ring: dotted outline that fills clockwise with the held time. */
static void hold_ring(int16_t cx,int16_t cy,unsigned permille) {
    unsigned filled=permille*wd_ring_point_count/1000U;
    for(unsigned i=0;i<wd_ring_point_count;i++)
        if(i<filled||i%4==0) poom_arduboy_draw_pixel((int16_t)(cx+wd_ring_points[i].x),(int16_t)(cy+wd_ring_points[i].y),WHITE);
}
static void timezone_load(void) {
    wd_timezone_config_t config; size_t length=sizeof(config);
    if(poom_secrets_get_blob("wd_timezone",&config,&length)!=ESP_OK||length!=sizeof(config)||!wd_timezone_config_valid(&config)) wd_timezone_default(&config);
    wd_timezone_init(&ui->time_zone,&config);
}
static bool timezone_save(void) {
    return poom_secrets_set_blob("wd_timezone",&ui->time_zone.config,sizeof(ui->time_zone.config))==ESP_OK;
}
static int64_t app_utc(void) {
    return wd_clock_now(&ui->clock,(uint64_t)esp_timer_get_time()/1000,(int64_t)time(NULL));
}
static void local_clock(unsigned y) {
    wd_local_time_t local; char text[32];
    static const char *const sources[]={"FB","GPS","MAN","LAST"};
    if(wd_timezone_local(app_utc(),ui->time_zone.zone,&local)&&wd_clock_valid(app_utc())) {
        char clock[9]; strftime(clock,sizeof(clock),"%H:%M:%S",&local.calendar);
        snprintf(text,sizeof(text),"%s %.5s %s",clock,local.abbreviation,sources[ui->time_zone.source]);
    } else snprintf(text,sizeof(text),"TIME WAIT GPS/NET");
    line(y,text);
}
/* GPS pages keep the last column for the page chevrons. */
static void gline(unsigned y,const char *s) {
    char text[21]; snprintf(text,sizeof(text),"%.20s",s); line(y,text);
}
static void gps_chevrons(void) {
    poom_arduboy_draw_line(122,25,124,23,WHITE);
    poom_arduboy_draw_line(124,23,126,25,WHITE);
    poom_arduboy_draw_line(122,43,124,45,WHITE);
    poom_arduboy_draw_line(124,45,126,43,WHITE);
}
/* Shared live-session header: left heading, right local HH:MM. */
static void live_header(const char *title) {
    char clock[6]; wd_clock_text(app_utc(),ui->time_zone.zone,clock);
    text_at(0,0,title); text_at(98,0,clock);
}
static bool api_ready(void) { return ui->credentials.api_name[0]&&ui->credentials.api_token[0]; }
static void session_path(char *out,size_t capacity,const char *id) { snprintf(out,capacity,WD_ROOT "/sessions/%.48s",id); }
static char status_code(const char *id) {
    char p[160]; wd_upload_status_t state; session_path(p,sizeof(p),id);
    if(!wd_upload_status_read(p,&state)) return '?';
    wd_upload_shown_t shown=wd_upload_shown(state.state);
    return shown==WD_SHOWN_SUBMITTED?'v':shown==WD_SHOWN_FAILED?'X':'-';
}
static void diagnostic(const char *s,bool valid,void *ctx) {
    (void)ctx; uint64_t now=esp_timer_get_time()/1000;
    if(ui->gps_capture.file&&!poom_gps_capture_append(&ui->gps_capture,s,valid,now)) strcpy(ui->message,"GPS SD WRITE FAILED");
    if(ui->raw && now-ui->last_raw>=1000) {
        printf("[wardrive GPS %s] %.127s\n",valid?"parsed":poom_nmea_checksum(s)?"checksum-ok/unparsed":"checksum-bad",s); ui->last_raw=now;
    }
}
static void button(const poom_sbus_msg_t *m,void *ctx) {
    (void)ctx; if(!m||m->len<sizeof(button_event_msg_t)) return;
    button_event_msg_t ev; memcpy(&ev,m->data,sizeof(ev));
    /* Hold/repeat events are ignored: holds are timed from press/release. */
    if(ev.event==BUTTON_SINGLE_CLICK||ev.event==BUTTON_PRESS_DOWN||ev.event==BUTTON_PRESS_UP) {
        wd_key_event_t key={(uint8_t)ev.button,(uint8_t)ev.event};
        portENTER_CRITICAL(&button_lock);
        if(buttons) xQueueSend(buttons,&key,0);
        portEXIT_CRITICAL(&button_lock);
    }
}
static bool mount_sd(void) {
    bool was=sd_card_is_mounted();
    if(sd_card_mount()!=ESP_OK) { strcpy(ui->message,"SD MOUNT FAILED"); return false; }
    if(!was) ui->mounted_here=true;
    return true;
}
static void gps_capture_toggle(void) {
    if(ui->gps_capture.file) { poom_gps_capture_stop(&ui->gps_capture); return; }
    if(ui->session.active||!mount_sd()) return;
    if((mkdir(WD_ROOT,0700)&&errno!=EEXIST)||(mkdir(WD_ROOT "/diagnostics",0700)&&errno!=EEXIST)) { strcpy(ui->message,"GPS DIRECTORY FAILED"); return; }
    char path[192];
    snprintf(path,sizeof(path),WD_ROOT "/diagnostics/gps-%08lx-%08lx.nmea",(unsigned long)(esp_timer_get_time()/1000),(unsigned long)esp_random());
    if(!poom_gps_capture_start(&ui->gps_capture,path,(uint64_t)esp_timer_get_time()/1000)) { ui->gps_capture.fault=true; strcpy(ui->message,"GPS SD START FAILED"); }
    else WD_DEBUG("[wardrive] GPS capture %.191s\n",path);
}
static void start(void) {
    if(ui->session.active) { ui->screen=WD_SCREEN_LIVE; return; }
    if(ui->gps_capture.file) { strcpy(ui->message,"STOP GPS CAPTURE"); return; }
    if(atomic_load(&ui->job.busy)) { strcpy(ui->message,"WAIT FOR SD TASK"); return; }
    wd_ui_profiles_close(&ui->wifi_profiles); wd_ui_networks_clear();
    if(!mount_sd()) return;
    char id[49];
    if(!wd_session_id(id,app_utc(),(uint32_t)(esp_timer_get_time()/1000),esp_random())) { strcpy(ui->message,"SESSION ID FAILED"); return; }
    if(!wd_session_start(&ui->session,WD_ROOT,id,&ui->settings)) { strcpy(ui->message,"SD START FAILED"); return; }
    snprintf(ui->session.receiver,sizeof(ui->session.receiver),"%.23s",ui->gps.parser.fix.receiver);
    ui->session.display_zone=(uint8_t)ui->time_zone.zone; ui->session.display_source=(uint8_t)ui->time_zone.source;
    ui->session.started_ms=esp_timer_get_time()/1000;
    wd_session_time(&ui->session,app_utc(),ui->session.started_ms);
    if(!wd_session_checkpoint(&ui->session,"Active")) { ui->session.fault=true; wd_session_stop(&ui->session); strcpy(ui->message,"SD CHECKPOINT FAILED"); return; }
    if(!wd_radio_start(&ui->settings)) { wd_session_stop(&ui->session); wd_radio_release(); strcpy(ui->message,"RADIO START FAILED"); return; }
    ui->started=esp_timer_get_time()/1000; ui->session.started_ms=ui->started; ui->last_gps_log=0; ui->last_checkpoint=ui->started; memset(&ui->distance,0,sizeof(ui->distance));
    wd_elapsed_start(&ui->elapsed,ui->started); ui->had_fix=false;
    WD_DEBUG("[wardrive session] started=%s\n",ui->session.directory);
    ui->screen=WD_SCREEN_LIVE; ui->stopping=false; ui->message[0]=0;
}
static void stop(void) { if(ui->session.active) { ui->stopping=true; wd_elapsed_pause(&ui->elapsed,esp_timer_get_time()/1000); wd_radio_stop(); } }
static void pause_toggle(uint64_t now) {
    if(!ui->session.active||ui->stopping) return;
    wd_distance_break(&ui->distance);
    ui->session.paused=!ui->session.paused; wd_radio_pause(ui->session.paused);
    if(ui->session.paused) wd_elapsed_pause(&ui->elapsed,now); else wd_elapsed_resume(&ui->elapsed,now);
    if(!wd_session_checkpoint(&ui->session,ui->session.paused?"Paused":"Active")) { ui->session.fault=true; stop(); }
}
/* Cache sorted metadata on browser entry. Row shifts reuse that snapshot;
 * only the arriving row's upload status is read. Large lists/allocation failure
 * retain the bounded pager. Refresh the snapshot on the next main-menu entry. */
/* Bounded string copy (always terminated). */
static void copy_text(char *out,size_t capacity,const char *text) {
    size_t n=strnlen(text,capacity-1); memcpy(out,text,n); out[n]=0;
}
static void saved_rows(void) {
    memset(ui->rows,0,sizeof(ui->rows)); ui->selected_id[0]=0; ui->selected_label[0]=0;
    for(unsigned i=0;i<4;i++) copy_text(ui->rows[i],sizeof(ui->rows[i]),ui->saved_page.entries[i].label);
    ui->saved_index=ui->saved_page.selected_rank+ui->saved_row;
    const wd_session_info_t *entry=&ui->saved_page.entries[ui->saved_row];
    copy_text(ui->selected_id,sizeof(ui->selected_id),entry->id);
    copy_text(ui->selected_label,sizeof(ui->selected_label),entry->detail[0]?entry->detail:entry->label);
}
static void saved_details(void) {
    ui->selected_status[0]=0; ui->selected_csv=false;
    if(!ui->selected_id[0]) return;
    char p[160]; session_path(p,sizeof(p),ui->selected_id);
    wd_upload_status_t state;
    static const char *const names[]={"Pending","Submitted","Failed"};
    if(wd_upload_status_read(p,&state)) {
        snprintf(ui->selected_status,sizeof(ui->selected_status),"Upload: %s",names[wd_upload_shown(state.state)]);
        ui->row_status[ui->saved_row]=status_code(ui->selected_id);
    } else strcpy(ui->selected_status,"Upload: unreadable");
    ui->selected_csv=wd_export_ready(p);
}
static bool saved_shift(int direction) {
    wd_session_info_t next;
    const char *edge=direction>0?ui->saved_page.entries[3].id:ui->saved_page.entries[0].id;
    if(!edge[0]) return false;
    if(ui->saved_cache.ready) {
        unsigned rank=ui->saved_page.selected_rank;
        if(direction>0) {
            if(rank+4>=ui->saved_cache.count) return false;
            next=ui->saved_cache.entries[rank+4];
        } else {
            if(!rank) return false;
            next=ui->saved_cache.entries[rank-1];
        }
    } else if(!wd_session_neighbor(WD_ROOT "/sessions",edge,direction,ui->time_zone.zone,&next)) return false;
    if(direction>0) {
        memmove(&ui->saved_page.entries[0],&ui->saved_page.entries[1],3*sizeof(ui->saved_page.entries[0])); memmove(ui->row_status,ui->row_status+1,3);
        ui->saved_page.entries[3]=next; ui->row_status[3]=status_code(next.id); ui->saved_page.selected_rank++;
    } else {
        memmove(&ui->saved_page.entries[1],&ui->saved_page.entries[0],3*sizeof(ui->saved_page.entries[0])); memmove(ui->row_status+1,ui->row_status,3);
        ui->saved_page.entries[0]=next; ui->row_status[0]=status_code(next.id); if(ui->saved_page.selected_rank) ui->saved_page.selected_rank--;
    }
    return true;
}
static void saved_refresh(int direction) {
    if(!direction&&!ui->selected_id[0])
        (void)wd_session_cache_load(WD_ROOT "/sessions",ui->time_zone.zone,&ui->saved_cache);
    if(direction&&ui->selected_id[0]&&ui->saved_page.count) {
        if(direction>0) {
            if(ui->saved_index+1>=ui->saved_count) return;
            if(ui->saved_row<3&&ui->saved_page.entries[ui->saved_row+1].id[0]) { ui->saved_row++; saved_rows(); return; }
        } else {
            if(!ui->saved_index) return;
            if(ui->saved_row) { ui->saved_row--; saved_rows(); return; }
        }
        if(saved_shift(direction)) { saved_rows(); return; }
        /* Neighbour missing (files changed): fall back to a full rescan. */
    }
    char anchor[49]={0};
    if(!ui->selected_id[0]) ui->saved_row=0;
    else if(ui->saved_page.count&&ui->saved_row<4&&!strcmp(ui->saved_page.entries[ui->saved_row].id,ui->selected_id)) snprintf(anchor,sizeof(anchor),"%s",ui->saved_page.entries[0].id);
    else { snprintf(anchor,sizeof(anchor),"%s",ui->selected_id); ui->saved_row=0; }
    /* Never pass a cursor pointing inside the page: the page reader clears
     * its output before scanning. Keep the viewport anchor separately. */
    bool ok=true;
    if(ui->saved_cache.ready) wd_session_cache_page(&ui->saved_cache,anchor,&ui->saved_page);
    else ok=wd_session_page_local(WD_ROOT "/sessions",anchor,0,ui->time_zone.zone,&ui->saved_page);
    ui->saved_count=ok?ui->saved_page.count:0;
    if(!ok) { strcpy(ui->message,"SESSION LIST FAILED"); ui->saved_row=0; }
    while(ui->saved_row&&!ui->saved_page.entries[ui->saved_row].id[0]) ui->saved_row--;
    memset(ui->row_status,0,sizeof(ui->row_status));
    for(unsigned i=0;i<4;i++) if(ui->saved_page.entries[i].id[0]) ui->row_status[i]=status_code(ui->saved_page.entries[i].id);
    saved_rows(); saved_details();
}
static void saved_key(uint8_t key) {
    if(key==BUTTON_DOWN) saved_refresh(1);
    if(key==BUTTON_UP) saved_refresh(-1);
    if(key==BUTTON_A&&ui->selected_id[0]) { saved_details(); ui->screen=WD_SCREEN_SESSION; ui->session_action=0; ui->message[0]=0; }
}
static unsigned session_actions(wd_session_action_t out[3]) {
    unsigned n=0; out[n++]=WD_ACTION_CSV;
    if(ui->selected_csv) out[n++]=WD_ACTION_UPLOAD;
    out[n++]=WD_ACTION_OPTIONS; return n;
}
static void launch(wd_job_t type) {
    if(atomic_load(&ui->job.busy)) return;
    if(ui->session.active) { strcpy(ui->message,"STOP SESSION FIRST"); return; }
    if(ui->gps_capture.file) { strcpy(ui->message,"STOP GPS CAPTURE"); return; }
    if((type==WD_JOB_UPLOAD||type==WD_JOB_ALL)&&(!api_ready()||!ui->credentials.ssid[0])) { strcpy(ui->message,!ui->credentials.ssid[0]?"Choose WiFi first":!ui->credentials.api_name[0]?"WiGLE name missing":"WiGLE token missing"); return; }
    if(type==WD_JOB_TRANSFER&&!ui->credentials.ssid[0]) { ui->screen=WD_SCREEN_TRANSFER; return; }
    if(!mount_sd()) return;
    wd_ui_profiles_close(&ui->wifi_profiles);
    if(ui->transfer_on) { wd_ui_disconnect(); ui->transfer_on=false; }
    ui->job.job=type; ui->job.credentials=ui->credentials;
    ui->job.display_zone=ui->time_zone.zone;
    ui->job.clock=ui->clock;
    snprintf(ui->job.directory,sizeof(ui->job.directory),WD_ROOT "/sessions/%.48s",ui->selected_id);
    if(wd_ui_job_start(&ui->job)) {
        ui->screen=type==WD_JOB_UPLOAD||type==WD_JOB_ALL?WD_SCREEN_PROGRESS:WD_SCREEN_WORKING;
        strcpy(ui->message,(type==WD_JOB_TRANSFER||type==WD_JOB_WIFI_JOIN)?"Joining WiFi...":type==WD_JOB_WIFI_SCAN?"Scanning networks...":
            type==WD_JOB_ARCHIVE?"Preparing download...":type==WD_JOB_EXPORT?"Creating WiGLE CSV...":type==WD_JOB_RECOVER?"Recovering data...":"Working...");
    }
    else strcpy(ui->message,"TASK START FAILED");
}
static void wifi_choose(void) {
    wd_ui_profiles_close(&ui->wifi_profiles); wd_ui_networks_clear();
    ui->wifi_selection=0; ui->screen=WD_SCREEN_WIFI_CHOOSE; ui->message[0]=0;
}
/* Choose/join a network for uploads or settings, then return to `back`. */
static void wifi_join_for(unsigned back) {
    ui->wifi_purpose=WD_JOB_WIFI_JOIN; ui->wifi_return=back; wifi_choose();
}
static void transfer_setup_key(uint8_t key) {
    ui->wifi_purpose=WD_JOB_TRANSFER;
    if(key==BUTTON_RIGHT||(key==BUTTON_A&&!ui->credentials.ssid[0])) wifi_choose();
    else if(key==BUTTON_A) launch(WD_JOB_TRANSFER);
}
static void wifi_password_entry(const char *ssid,unsigned return_screen) {
    snprintf(ui->wifi_ssid,sizeof(ui->wifi_ssid),"%.32s",ssid);
    memset(ui->edit,0,sizeof(ui->edit)); ui->edit_field=16; ui->wifi_keyboard_return=return_screen;
    poom_ui_keyboard_init(&ui->keyboard,ui->edit,65,ui->wifi_ssid); ui->screen=WD_SCREEN_KEYBOARD;
}
static bool wifi_password_valid(const char *password) {
    return password[0]&&wd_ui_wifi_password_valid(password);
}
static bool wifi_keyboard_accept(void) {
    if(ui->edit_field==17) {
        if(!ui->edit[0]) poom_ui_keyboard_init(&ui->keyboard,ui->edit,33,"Enter network name");
        else wifi_password_entry(ui->edit,WD_SCREEN_WIFI_CHOOSE);
        return true;
    }
    if(ui->edit_field!=16) return false;
    if(!wifi_password_valid(ui->edit)) { poom_ui_keyboard_init(&ui->keyboard,ui->edit,65,"8-63 chars or 64 hex"); return true; }
    snprintf(ui->credentials.ssid,sizeof(ui->credentials.ssid),"%.32s",ui->wifi_ssid);
    snprintf(ui->credentials.password,sizeof(ui->credentials.password),"%.64s",ui->edit);
    ui->offer_wifi_save=true;
    memset(ui->edit,0,sizeof(ui->edit)); memset(ui->wifi_ssid,0,sizeof(ui->wifi_ssid)); wd_ui_networks_clear();
    launch(ui->wifi_purpose); return true;
}
static void keyboard_cancel(void) {
    memset(ui->edit,0,sizeof(ui->edit)); ui->screen=ui->edit_field>=16?ui->wifi_keyboard_return:ui->edit_return_screen; memset(ui->wifi_ssid,0,sizeof(ui->wifi_ssid));
}
static void wifi_key(uint8_t key) {
    unsigned count=ui->screen==WD_SCREEN_WIFI_CHOOSE?3:ui->screen==WD_SCREEN_WIFI_PROFILES?ui->wifi_profiles.count:wd_ui_network_count();
    if(key==BUTTON_UP&&ui->wifi_selection) ui->wifi_selection--;
    if(key==BUTTON_DOWN&&ui->wifi_selection+1<count) ui->wifi_selection++;
    if((ui->screen==WD_SCREEN_WIFI_NETWORKS||ui->screen==WD_SCREEN_WIFI_SCAN_ERROR)&&key==BUTTON_RIGHT) { ui->wifi_selection=0; launch(WD_JOB_WIFI_SCAN); return; }
    if(key!=BUTTON_A) return;
    ui->message[0]=0;
    if(ui->screen==WD_SCREEN_WIFI_CHOOSE) {
        if(ui->wifi_selection==0) {
            if(!wd_ui_profiles_open(&ui->wifi_profiles)) { strcpy(ui->message,"Profile load failed"); return; }
            ui->wifi_selection=0; ui->screen=WD_SCREEN_WIFI_PROFILES;
        } else if(ui->wifi_selection==1) { ui->wifi_selection=0; launch(WD_JOB_WIFI_SCAN); }
        else { memset(ui->edit,0,sizeof(ui->edit)); ui->edit_field=17; ui->wifi_keyboard_return=WD_SCREEN_WIFI_CHOOSE; poom_ui_keyboard_init(&ui->keyboard,ui->edit,33,"Network name (SSID)"); ui->screen=WD_SCREEN_KEYBOARD; }
    } else if(ui->screen==WD_SCREEN_WIFI_PROFILES) {
        if(!count) { launch(WD_JOB_WIFI_SCAN); return; }
        if(wd_ui_profile_pick(&ui->wifi_profiles,ui->wifi_selection,&ui->credentials)) { ui->offer_wifi_save=false; launch(ui->wifi_purpose); }
        else strcpy(ui->message,"Profile unavailable");
    } else if(ui->screen==WD_SCREEN_WIFI_NETWORKS) {
        if(!count) { launch(WD_JOB_WIFI_SCAN); return; }
        wd_ui_network_t network={0};
        if(!wd_ui_network_get(ui->wifi_selection,&network)) { strcpy(ui->message,"Network unavailable"); return; }
        if(!network.ssid[0]) { strcpy(ui->message,"Use Enter SSID"); return; }
        if(network.open) {
            snprintf(ui->credentials.ssid,sizeof(ui->credentials.ssid),"%s",network.ssid);
            memset(ui->credentials.password,0,sizeof(ui->credentials.password));
            ui->offer_wifi_save=true;
            wd_ui_networks_clear(); launch(ui->wifi_purpose); return;
        }
        if(!network.personal) { strcpy(ui->message,"Use WPA2/3 password"); return; }
        wifi_password_entry(network.ssid,WD_SCREEN_WIFI_NETWORKS);
    } else if(ui->screen==WD_SCREEN_WIFI_SCAN_ERROR) launch(WD_JOB_WIFI_SCAN);
}
static unsigned joined_screen(void) { return ui->transfer_on?WD_SCREEN_TRANSFER_READY:ui->wifi_return; }
static void complete_job(void) {
    wd_ui_job_reap();
    atomic_store(&ui->job.done,false); atomic_store(&ui->job.busy,false);
    snprintf(ui->message,sizeof(ui->message),"%.63s",ui->job.message);
    if(ui->exiting) { wd_ui_disconnect(); ui->screen=WD_SCREEN_MAIN; return; }
    if(ui->job.job==WD_JOB_TRANSFER||ui->job.job==WD_JOB_WIFI_JOIN) {
        if(ui->job.success&&!atomic_load(&ui->job.cancel)) { ui->transfer_on=ui->job.job==WD_JOB_TRANSFER; ui->screen=ui->offer_wifi_save?WD_SCREEN_WIFI_SAVE:joined_screen(); ui->message[0]=0; }
        else ui->screen=WD_SCREEN_WIFI_ERROR;
    } else if(ui->job.job==WD_JOB_UPLOAD||ui->job.job==WD_JOB_ALL) { ui->screen=WD_SCREEN_UPLOAD_RESULT; if(ui->job.job==WD_JOB_UPLOAD) { char keep[64]; strcpy(keep,ui->message); saved_refresh(0); strcpy(ui->message,keep); } }
    else if(ui->job.job==WD_JOB_WIFI_SCAN) { ui->wifi_selection=0; ui->screen=ui->job.success?WD_SCREEN_WIFI_NETWORKS:WD_SCREEN_WIFI_SCAN_ERROR; }
    else if(ui->job.job==WD_JOB_ARCHIVE) ui->screen=WD_SCREEN_ARCHIVE_RESULT;
    else if(ui->job.job==WD_JOB_EXPORT||ui->job.job==WD_JOB_RECOVER) { char keep[64]; strcpy(keep,ui->message); saved_refresh(0); strcpy(ui->message,keep); ui->screen=WD_SCREEN_FILE_RESULT; }
    else ui->screen=WD_SCREEN_MAIN;
}
static void profile_save_key(uint8_t key) {
    if(key==BUTTON_B) { ui->offer_wifi_save=false; ui->message[0]=0; ui->screen=joined_screen(); return; }
    if(key!=BUTTON_A) return;
    esp_err_t status=wd_ui_profile_save(&ui->credentials);
    if(status==ESP_OK) { ui->offer_wifi_save=false; ui->message[0]=0; ui->screen=joined_screen(); }
    else if(status==ESP_ERR_NO_MEM) strcpy(ui->message,"No space for profile");
    else if(status==ESP_ERR_NOT_SUPPORTED) strcpy(ui->message,"64-char PSK not saved");
    else strcpy(ui->message,"Profile save failed");
}
static void file_result_key(uint8_t key) {
    if(ui->screen==WD_SCREEN_FILE_RESULT) {
        if(key==BUTTON_B) { ui->screen=WD_SCREEN_SESSION; ui->message[0]=0; saved_refresh(0); ui->session_action=0; }
        else if(key==BUTTON_A&&ui->job.success&&ui->job.job==WD_JOB_EXPORT) { ui->choice_row=0; ui->message[0]=0; ui->screen=WD_SCREEN_UPLOAD_CHOICE; }
        return;
    }
    if(key==BUTTON_B) { ui->screen=WD_SCREEN_TRANSFER_FORMAT; ui->message[0]=0; saved_refresh(0); }
    else if(key==BUTTON_A) {
        if(ui->job.success) { ui->message[0]=0; ui->screen=WD_SCREEN_TRANSFER; }
        else launch(WD_JOB_ARCHIVE);
    }
}
/* Credentials: start blank so a saved value is never echoed on screen. */
static void edit_start(unsigned field) {
    ui->edit_field=field; ui->edit_return_screen=WD_SCREEN_API; memset(ui->edit,0,sizeof(ui->edit));
    size_t cap=field==8?sizeof(ui->credentials.api_name):sizeof(ui->credentials.api_token);
    poom_ui_keyboard_init(&ui->keyboard,ui->edit,cap,field==8?"WiGLE API Name":"WiGLE API Token"); ui->screen=WD_SCREEN_KEYBOARD;
}
static void api_saved(const char *ok_text) {
    strcpy(ui->message,wd_credentials_save(&ui->credentials)==ESP_OK?ok_text:"KEY SAVE FAILED");
}
static void api_open(unsigned back) {
    ui->api_return=back; ui->api_row=0; ui->message[0]=0; ui->screen=WD_SCREEN_API;
    struct stat st; bool was=sd_card_is_mounted();
    ui->setup_present=(was||sd_card_mount()==ESP_OK)&&stat(WD_SETUP_FILE,&st)==0;
    if(!was&&sd_card_is_mounted()) ui->mounted_here=true;
}
static unsigned api_rows(void) { return ui->setup_present?5:4; }
static void import_setup_key(uint8_t key) {
    if(key==BUTTON_B) { ui->screen=WD_SCREEN_API; ui->message[0]=0; return; }
    if(key!=BUTTON_A) return;
    if(mount_sd()) {
        if(wd_credentials_import(&ui->credentials)) {
            ui->credentials.auto_upload=false; ui->setup_imported=true; ui->setup_present=true;
            api_saved("Credentials imported"); ui->screen=WD_SCREEN_DELETE_SETUP;
        }
        else strcpy(ui->message,"Invalid SD setup file");
    }
}
static void api_key(uint8_t key) {
    unsigned count=api_rows();
    if(key==BUTTON_UP) ui->api_row=(ui->api_row+count-1)%count;
    if(key==BUTTON_DOWN) ui->api_row=(ui->api_row+1)%count;
    if(key!=BUTTON_A) return;
    ui->message[0]=0;
    if(ui->api_row==0||ui->api_row==1) edit_start(ui->api_row==0?8:9);
    else if(ui->api_row==2) ui->screen=WD_SCREEN_IMPORT; /* No SD read until explicit A:Yes. */
    else if(ui->api_row==3) {
        memset(ui->credentials.api_name,0,sizeof(ui->credentials.api_name)); memset(ui->credentials.api_token,0,sizeof(ui->credentials.api_token));
        wd_credentials_forget(); strcpy(ui->message,"API keys cleared");
    } else if(ui->api_row==4) { ui->setup_imported=false; ui->screen=WD_SCREEN_DELETE_SETUP; }
}
static void upload_open(unsigned menu) {
    ui->upload_row=0; ui->message[0]=0;
    if(wd_ui_link_up()&&ui->credentials.ssid[0]) ui->screen=menu;
    else wifi_join_for(menu);
}
static unsigned upload_rows(wd_upload_row_t out[3]) {
    unsigned n=0; out[n++]=WD_UPLOAD_ROW_RUN; out[n++]=WD_UPLOAD_ROW_WIFI;
    if(!api_ready()) out[n++]=WD_UPLOAD_ROW_API;
    return n;
}
static void upload_key(uint8_t key) {
    wd_upload_row_t items[3]; unsigned count=upload_rows(items);
    if(ui->upload_row>=count) ui->upload_row=0;
    if(key==BUTTON_UP) ui->upload_row=(ui->upload_row+count-1)%count;
    if(key==BUTTON_DOWN) ui->upload_row=(ui->upload_row+1)%count;
    if(key!=BUTTON_A) return;
    ui->message[0]=0;
    if(items[ui->upload_row]==WD_UPLOAD_ROW_WIFI) wifi_join_for(ui->screen);
    else if(items[ui->upload_row]==WD_UPLOAD_ROW_API) api_open(ui->screen);
    else if(!api_ready()) strcpy(ui->message,"Set WiGLE API keys");
    else if(!ui->credentials.ssid[0]) wifi_join_for(ui->screen);
    else ui->screen=ui->screen==WD_SCREEN_BULK_MENU?WD_SCREEN_BULK_HOLD:WD_SCREEN_CONSENT;
}
static const char *notice_title(void) {
    static const char *const titles[]={"RECOVER SESSION DATA","MARK AS SUBMITTED","RESET UPLOAD STATUS","BULK WIGLE UPLOAD"};
    return titles[ui->notice];
}
static const char *notice_text(void) {
    switch(ui->notice) {
        case WD_NOTICE_RECOVER: return "Repairs a session that didn't close cleanly, for example after power loss, an SD error or a crash during recording.\n\n"
            "This rebuilds the session's summary and index from the raw log and writes a recovery report. Your original log is never deleted.\n\n"
            "Use this option when a session shows an SD fault, or when export fails or looks incomplete.\n\n"
            "The WiGLE CSV must be created again afterwards.\n\nWould you like to continue?";
        case WD_NOTICE_MARK: return "Marks this session as submitted after manually checking WiGLE account.\n\nContinue?";
        case WD_NOTICE_RESET: return "Wipes the upload state back to Pending: attempts, transaction ID, retry wait and error details are all cleared.\n\n"
            "If WiGLE already has this run, uploading it again may create a duplicate.\n\nContinue?";
        default: return "Creating any outstanding CSVs and uploading all pending runs to WiGLE takes significant time.\n\nAre you sure you wish to continue?";
    }
}
#define WD_NOTICE_LINES 5U
/* Only the line count is kept; visible lines are re-wrapped per frame into a
 * 5-line stack buffer. A static copy of the whole text cost ~700 bytes of the
 * HP SRAM that Bluetooth needs while recording. */
static bool notice_at_end(void) { return ui->notice_top+WD_NOTICE_LINES>=ui->notice_count; }
static void notice_open(unsigned kind,unsigned back) {
    ui->notice=kind; ui->notice_top=0; ui->notice_return=back; ui->message[0]=0; ui->screen=WD_SCREEN_NOTICE;
    ui->notice_count=wd_wrap_from(notice_text(),WD_UI_COLUMNS,0,NULL,0);
}
static void upload_state_change(bool submitted) {
    char p[160]; session_path(p,sizeof(p),ui->selected_id);
    wd_upload_status_t status; bool ok=wd_upload_status_read(p,&status);
    if(submitted&&ok) {
        status.state=WD_SUBMITTED; status.manual=true; status.problem=WD_UPLOAD_OK; status.retry_after=0;
    } else if(!submitted) { status=(wd_upload_status_t){0}; ok=true; }
    if(ok) ok=wd_upload_status_write(p,&status);
    saved_refresh(0); ui->session_action=0; ui->screen=WD_SCREEN_SESSION;
    strcpy(ui->message,!ok?"STATUS UPDATE FAILED":submitted?"Marked as Submitted":"Reset to Pending");
}
static void notice_key(uint8_t key) {
    if(key==BUTTON_DOWN&&!notice_at_end()) ui->notice_top++;
    if(key==BUTTON_UP&&ui->notice_top) ui->notice_top--;
    /* A confirms only after the whole explanation has been shown. */
    if(key!=BUTTON_A||!notice_at_end()) return;
    if(ui->notice==WD_NOTICE_RECOVER) launch(WD_JOB_RECOVER);
    else if(ui->notice==WD_NOTICE_MARK) upload_state_change(true);
    else if(ui->notice==WD_NOTICE_RESET) upload_state_change(false);
    else upload_open(WD_SCREEN_BULK_MENU);
}
static void settings_open(void) {
    ui->settings_before=ui->settings; ui->timezone_before=ui->time_zone.config; ui->setting_row=0; ui->message[0]=0; ui->screen=WD_SCREEN_SETTINGS;
}
static bool settings_changed(void) {
    return memcmp(&ui->settings,&ui->settings_before,sizeof(ui->settings))||memcmp(&ui->time_zone.config,&ui->timezone_before,sizeof(ui->timezone_before));
}
static void settings_finish(bool save,const poom_gps_fix_t *f,uint64_t now) {
    if(save) {
        bool ok=poom_secrets_set_blob("wd_settings",&ui->settings,sizeof(ui->settings))==ESP_OK;
        ok=timezone_save()&&ok;
        strcpy(ui->message,ok?"SETTINGS SAVED":"SETTINGS SAVE FAILED");
    } else {
        ui->settings=ui->settings_before;
        if(memcmp(&ui->time_zone.config,&ui->timezone_before,sizeof(ui->timezone_before))) { wd_timezone_init(&ui->time_zone,&ui->timezone_before); wd_timezone_update(&ui->time_zone,f,now); }
        strcpy(ui->message,"CHANGES DISCARDED");
    }
    ui->screen=WD_SCREEN_MAIN;
}
static void main_select(void) {
    wd_menu_item_t items[8]; unsigned count=wd_main_menu(ui->session.active,items);
    if(ui->selection>=count) ui->selection=0;
    ui->message[0]=0;
    switch(items[ui->selection]) {
        case WD_MENU_START: if(ui->session.active) ui->screen=WD_SCREEN_LIVE; else { wd_ui_disconnect(); ui->transfer_on=false; start(); } break;
        case WD_MENU_STOP: ui->stop_return=WD_SCREEN_MAIN; ui->screen=WD_SCREEN_STOP; break;
        case WD_MENU_GPS: ui->screen=WD_SCREEN_GPS; ui->gps_page=0; break;
        case WD_MENU_API: api_open(WD_SCREEN_MAIN); break;
        default:
            /* SD, radio and WiFi stay with the recording until it stops. */
            if(ui->session.active) { strcpy(ui->message,"STOP SESSION FIRST"); break; }
            if(items[ui->selection]==WD_MENU_SAVED) { if(mount_sd()) { ui->selected_id[0]=0; saved_refresh(0); ui->screen=WD_SCREEN_SAVED; } }
            else if(items[ui->selection]==WD_MENU_BULK) notice_open(WD_NOTICE_BULK,WD_SCREEN_MAIN);
            else if(items[ui->selection]==WD_MENU_WIFI) { ui->choice_row=0; ui->screen=WD_SCREEN_WIFI_SETTINGS; }
            else settings_open();
            break;
    }
}
/* Long holds: which screens accept which button, and for how long. */
static uint32_t hold_duration(uint8_t button) {
    if(ui->screen==WD_SCREEN_LIVE&&button==BUTTON_A&&ui->session.active&&!ui->stopping) return WD_HOLD_PAUSE_MS;
    if(wd_ui_is_live_page(ui->screen)&&button==BUTTON_B&&ui->session.active&&!ui->stopping) return WD_HOLD_STOP_MS;
    if(ui->screen==WD_SCREEN_DELETE_SETUP&&button==BUTTON_A) return WD_HOLD_DELETE_MS;
    if(ui->screen==WD_SCREEN_BULK_HOLD&&button==BUTTON_A) return WD_HOLD_BULK_MS;
    return 0;
}
static void hold_fired(uint64_t now) {
    ui->suppress_click[ui->hold.button]=true;
    if(ui->screen==WD_SCREEN_LIVE&&ui->hold.button==BUTTON_A) pause_toggle(now);
    else if(wd_ui_is_live_page(ui->screen)&&ui->hold.button==BUTTON_B) { ui->stop_return=ui->screen; ui->raw=false; ui->screen=WD_SCREEN_STOP; }
    else if(ui->screen==WD_SCREEN_DELETE_SETUP) {
        /* Removes only the SD file; keys already stored in NVS stay. */
        strcpy(ui->message,remove(WD_SETUP_FILE)==0?"SETUP FILE DELETED":"DELETE FAILED");
        ui->setup_present=false; ui->api_row=0; ui->screen=WD_SCREEN_API;
    } else if(ui->screen==WD_SCREEN_BULK_HOLD) launch(WD_JOB_ALL);
}
/* The 100 kHz OLED writes hardware pages sequentially. Hide the transfer only
 * on page transitions, showing the complete prepared frame when it finishes.
 * Ordinary live updates retain the driver's incremental, always-on refresh. */
static void present(void) {
    bool transition=ui->screen!=ui->presented_screen||(ui->screen==WD_SCREEN_GPS&&ui->gps_page!=ui->presented_gps_page);
    if(transition) poom_arduboy_set_display_enabled(false);
    poom_arduboy_display();
    if(transition) poom_arduboy_set_display_enabled(true);
    ui->presented_screen=ui->screen; ui->presented_gps_page=ui->gps_page;
}
static void draw_live(uint64_t now,poom_gps_fix_t f) {
    char b[32];
    live_header(ui->session.paused?"SESSION PAUSED":"ACTIVE SESSION");
    /* Radio row: only enabled radios; Wi-Fi = unique BSSIDs, BLE = unique devices. */
    char wifi[5],ble[5]; wd_count_text(wifi,ui->session.wifi_count); wd_count_text(ble,ui->session.ble_count);
    bool show_wifi=ui->settings.mode!=WD_BLE,show_ble=ui->settings.mode!=WD_WIFI;
    snprintf(b,sizeof(b),"WIFI: %s",wifi);
    int16_t wifi_width=(int16_t)(strlen(b)*6),ble_width=(int16_t)(8+strlen(ble)*6);
    int16_t wifi_x=show_ble?(int16_t)((64-wifi_width)/2):(int16_t)((128-wifi_width)/2);
    int16_t ble_x=show_wifi?(int16_t)(64+(64-ble_width)/2):(int16_t)((128-ble_width)/2);
    if(show_wifi) text_at(wifi_x,17,b);
    if(show_ble) { glyph(ble_x,17,wd_glyph_bluetooth); text_at((int16_t)(ble_x+8),17,ble); }
    /* GPS state, active recording time and distance share one row. */
    text_at(0,33,"GPS:"); gps_mark(25,33,wd_gps_icon(f.connected,f.fix,ui->had_fix));
    char time_text[9],metres[8]; wd_elapsed_text(wd_elapsed_ms(&ui->elapsed,now),time_text); wd_distance_text(ui->distance.metres,metres);
    text_at(40,33,time_text); text_at((int16_t)(128-strlen(metres)*6),33,metres);
    bool holding_a=wd_hold_visible(&ui->hold,BUTTON_A,ui->screen,now),holding_b=wd_hold_visible(&ui->hold,BUTTON_B,ui->screen,now);
    if(holding_a||holding_b) progress_bar(46,(int)wd_hold_permille(&ui->hold,ui->screen,now),now);
    if(ui->stopping) line(56,"STOPPING...");
    else if(holding_b) line(56,"Keep holding: Stop");
    else if(holding_a) line(56,ui->session.paused?"Keep holding: Resume":"Keep holding: Pause");
    else line(56,ui->session.paused?"HOLD A:Resume  B:BACK":"HOLD A:Pause   B:BACK");
}
static void draw_gps(uint64_t now,poom_gps_fix_t f) {
    char b[32];
    static const char *const titles[]={"GPS POSITION","GPS MOVEMENT","GPS DIAGNOSTICS"};
    if(ui->session.active) live_header("GPS STATUS"); else text_at(0,0,titles[ui->gps_page%3]);
    if(ui->gps_page==0) {
        if(f.fields&GPS_POSITION) { snprintf(b,sizeof(b),"LAT %.6f",f.latitude); gline(11,b); snprintf(b,sizeof(b),"LON %.6f",f.longitude); gline(20,b); }
        else { gline(11,"LAT --"); gline(20,"LON --"); }
        if(f.fields&GPS_SATELLITES) snprintf(b,sizeof(b),"SAT %u",f.satellites); else strcpy(b,"SAT --"); gline(29,b);
        text_at(0,38,"FIX"); gps_mark(24,38,wd_gps_icon(f.connected,f.fix,ui->had_fix));
    } else if(ui->gps_page==1) {
        if(f.fields&GPS_SPEED) snprintf(b,sizeof(b),"SPEED %.1fkm/h",f.speed_kmh); else strcpy(b,"SPEED --"); gline(11,b);
        if(f.fields&GPS_ALTITUDE) snprintf(b,sizeof(b),"ALT %.1fm",f.altitude_m); else strcpy(b,"ALT --"); gline(20,b);
        if(f.fields&GPS_HDOP) snprintf(b,sizeof(b),"HDOP %.1f",f.hdop); else strcpy(b,"HDOP --"); gline(29,b);
        char age[16];
        if(f.position_ms) {
            unsigned long seconds=(unsigned long)((now-f.position_ms)/1000);
            if(seconds>999999) strcpy(age,">999999s"); else snprintf(age,sizeof(age),"%lus",seconds);
        } else strcpy(age,"--");
        snprintf(b,sizeof(b),"FIX AGE %.9s",age); gline(38,b);
    } else {
        gline(11,ui->gps.opened?"I2C 0x10 ACK":"I2C NO RESPONSE");
        snprintf(b,sizeof(b),"NMEA %lu BAD %lu",(unsigned long)ui->gps.parser.valid_sentences,(unsigned long)(ui->gps.parser.checksum_errors+ui->gps.parser.framing_errors)); gline(20,b);
        static const char *const types[]={"--","NONE","2D","3D"};
        unsigned type=(f.fields&GPS_TYPE)&&f.fix_type>=1&&f.fix_type<=3?f.fix_type:0;
        snprintf(b,sizeof(b),"FIX TYPE %s",types[type]); gline(29,b);
        gline(38,"SparkFun XA1110"); /* configured receiver model, not probed */
    }
    bool holding=wd_hold_visible(&ui->hold,BUTTON_B,ui->screen,now);
    if(holding) { gps_chevrons(); progress_bar(47,(int)wd_hold_permille(&ui->hold,ui->screen,now),now); line(56,"Keep holding: Stop"); return; }
    if(ui->gps_capture.file) {
        unsigned seconds=(unsigned)((now-ui->gps_capture.started_ms)/1000);
        unsigned gsv=ui->gps_capture.gsv;
        if(seconds>1200) seconds=1200;
        if(gsv>99999) gsv=99999;
        snprintf(b,sizeof(b),"REC %us GSV:%u",seconds,gsv); line(47,b);
    }
    else if(ui->gps_capture.fault) line(47,"GPS SD ERROR");
    else if(ui->gps_capture.complete) line(47,"GPS SD SAVED");
    gps_chevrons();
    line(56,ui->session.active?(ui->raw?"RAW:ON B:BACK":"A:RAW B:BACK"):(ui->gps_capture.file?"R:STOP SD B:SAVE":"A:RAW R:SD B:BACK"));
    snprintf(b,sizeof(b),"%u/3",ui->gps_page%3+1); text_at(110,56,b);
}
static void draw_stats(uint64_t now) {
    char b[32];
    live_header("SESSION STATS");
    snprintf(b,sizeof(b),"OBSERVATIONS %lu",(unsigned long)ui->session.observations); line(11,b);
    snprintf(b,sizeof(b),"UNLOCATED %lu",(unsigned long)ui->session.unlocated); line(20,b);
    snprintf(b,sizeof(b),"GPS SAMPLES %lu",(unsigned long)ui->session.gps_count); line(29,b);
    snprintf(b,sizeof(b),"QUEUE LOST %lu",(unsigned long)wd_radio_dropped()); line(38,b);
    snprintf(b,sizeof(b),"SD %s RADIO ERR %x",ui->session.fault?"FAULT":"LOG",(unsigned)wd_radio_error()); line(47,b);
    if(wd_hold_visible(&ui->hold,BUTTON_B,ui->screen,now)) { progress_bar(56,(int)wd_hold_permille(&ui->hold,ui->screen,now),now); return; }
    line(56,"B:BACK  HOLD B:STOP");
}
/* Remembered window per screen: the cursor moves inside it and the list
 * scrolls only when the cursor would leave it (also across wrap-around). */
static unsigned menu_view(unsigned count,unsigned selected,unsigned visible) {
    unsigned top=ui->menu_tops[ui->screen];
    if(selected<top) top=selected;
    else if(selected>=top+visible) top=selected-visible+1;
    if(count<=visible) top=0; else if(top>count-visible) top=count-visible;
    ui->menu_tops[ui->screen]=(uint8_t)(top<255?top:255);
    return top;
}
static void draw_menu(const char *const *labels,unsigned count,unsigned selected,unsigned first_y) {
    unsigned visible=first_y<=12?4:3;
    unsigned top=menu_view(count,selected,visible);
    for(unsigned i=top;i<count&&i<top+visible;i++) menu_row(first_y+10*(i-top),i==selected,labels[i]);
}
static void draw(uint64_t now,poom_gps_fix_t f) {
    char b[32]; poom_arduboy_clear(); poom_arduboy_set_text_size(1);
    /* Draw exactly one title: printing white text twice at the same cursor
     * ORs the glyphs together, even though the framebuffer was cleared first. */
    const char *title="WARDRIVE";
    switch(ui->screen) {
        case WD_SCREEN_STOP:title="STOP SESSION?";break;
        case WD_SCREEN_SAVED:title="SAVED SESSIONS";break; case WD_SCREEN_SESSION:title="SESSION";break;
        case WD_SCREEN_UPLOAD_CHOICE:title="UPLOAD SESSION";break; case WD_SCREEN_TRANSFER_FORMAT:title="TRANSFER OVER WIFI";break;
        case WD_SCREEN_OPTIONS:title="ADDITIONAL OPTIONS";break; case WD_SCREEN_NOTICE:title=notice_title();break;
        case WD_SCREEN_TRANSFER:title="Transfer over WiFi";break; case WD_SCREEN_TRANSFER_READY:title="Transfer ready";break;
        case WD_SCREEN_PROGRESS:title=ui->job.job==WD_JOB_ALL?"BULK WIGLE UPLOAD":"UPLOADING TO WIGLE";break;
        case WD_SCREEN_FILE_RESULT:title=ui->job.success?(ui->job.job==WD_JOB_RECOVER?"Recovery report":"CSV ready"):"File operation failed";break;
        case WD_SCREEN_ARCHIVE_RESULT:title=ui->job.success?"Download ready":"Download not ready";break;
        case WD_SCREEN_UPLOAD_MENU:title="UPLOAD TO WIGLE";break; case WD_SCREEN_BULK_MENU:title="BULK WIGLE UPLOAD";break;
        case WD_SCREEN_BULK_HOLD:title="UPLOAD ALL PENDING";break; case WD_SCREEN_CONSENT:title="Upload to WiGLE?";break;
        case WD_SCREEN_UPLOAD_RESULT:title="WiGLE result";break; case WD_SCREEN_API:title="WIGLE API SETTINGS";break;
        case WD_SCREEN_IMPORT:title="Import SD setup";break;
        case WD_SCREEN_DELETE_SETUP:title=ui->setup_imported?"SD setup imported":"SD setup file";break;
        case WD_SCREEN_WIFI_SETTINGS:title="WIFI UPLOAD SETTINGS";break; case WD_SCREEN_WIFI_ERROR:title="WiFi error";break;
        case WD_SCREEN_WIFI_CHOOSE:title="Choose WiFi";break; case WD_SCREEN_WIFI_PROFILES:title="Saved WiFi profiles";break;
        case WD_SCREEN_WIFI_NETWORKS:title="Nearby networks";break; case WD_SCREEN_WIFI_SCAN_ERROR:title="WiFi scan";break;
        case WD_SCREEN_WIFI_SAVE:title="Save WiFi profile?";break; case WD_SCREEN_SETTINGS:title="ADDITIONAL SETTINGS";break;
        case WD_SCREEN_TIMEZONE:title="TIME ZONE";break; case WD_SCREEN_SAVE_CHANGES:title="SAVE CHANGES?";break;
        default:break;
    }
    if(ui->screen==WD_SCREEN_KEYBOARD) { ui->presented_screen=WD_SCREEN_KEYBOARD; poom_ui_keyboard_draw(&ui->keyboard); return; }
    if(ui->screen!=WD_SCREEN_LIVE&&ui->screen!=WD_SCREEN_GPS&&ui->screen!=WD_SCREEN_STATS) text_at(0,0,title);
    if(ui->screen==WD_SCREEN_MAIN) {
        wd_menu_item_t items[8]; const char *labels[8]; unsigned count=wd_main_menu(ui->session.active,items);
        for(unsigned i=0;i<count;i++) labels[i]=wd_main_menu_label(items[i],ui->session.active);
        if(ui->selection>=count) ui->selection=0;
        draw_menu(labels,count,ui->selection,12);
        line(56,ui->message[0]?ui->message:ui->session.active?"A:SELECT B:EXIT+STOP":"A:SELECT   B:EXIT");
    } else if(ui->screen==WD_SCREEN_LIVE) draw_live(now,f);
    else if(ui->screen==WD_SCREEN_GPS) draw_gps(now,f);
    else if(ui->screen==WD_SCREEN_STATS) draw_stats(now);
    else if(ui->screen==WD_SCREEN_STOP) {
        line(16,"Recording will end"); line(26,"and the session is"); line(36,"saved.");
        line(56,"A:Yes  B:No");
    } else if(ui->screen==WD_SCREEN_SAVED) {
        if(!ui->saved_count) line(20,"NO SAVED SESSIONS");
        for(unsigned i=0;i<4&&ui->rows[i][0];i++) {
            text_at(2,12+i*10,ui->rows[i]);
            if(ui->row_status[i]) status_mark(120,12+i*10,ui->row_status[i]);
            if(i==ui->saved_row) row_highlight(12+i*10);
        }
        if(ui->saved_count) snprintf(b,sizeof(b),"%u/%u A:OPEN B:BACK",ui->saved_index+1,ui->saved_count);
        else snprintf(b,sizeof(b),"%.21s",ui->message[0]?ui->message:"B:BACK");
        line(56,b);
    } else if(ui->screen==WD_SCREEN_SESSION) {
        line(11,ui->selected_label);
        wd_session_action_t items[3]; unsigned count=session_actions(items); const char *labels[3];
        for(unsigned i=0;i<count;i++) labels[i]=items[i]==WD_ACTION_CSV?(ui->selected_csv?"Regenerate WiGLE CSV":"Create WiGLE CSV"):items[i]==WD_ACTION_UPLOAD?"Upload Session":"Additional Options";
        if(ui->session_action>=count) ui->session_action=0;
        draw_menu(labels,count,ui->session_action,22);
        line(56,ui->message[0]?ui->message:ui->selected_status);
    } else if(ui->screen==WD_SCREEN_UPLOAD_CHOICE) {
        static const char *const labels[]={"Transfer over WiFi","Upload to WiGLE"};
        draw_menu(labels,2,ui->choice_row%2,14); line(56,ui->message[0]?ui->message:"A:SELECT   B:BACK");
    } else if(ui->screen==WD_SCREEN_TRANSFER_FORMAT) {
        static const char *const labels[]={"WiGLE CSV only","Raw data + WiGLE CSV"};
        draw_menu(labels,2,ui->choice_row%2,14); line(56,ui->message[0]?ui->message:"A:SELECT   B:BACK");
    } else if(ui->screen==WD_SCREEN_OPTIONS) {
        static const char *const labels[]={"Recover Session Data","Mark as Submitted","Reset Upload Status"};
        draw_menu(labels,3,ui->option_row%3,14); line(56,ui->message[0]?ui->message:"A:SELECT   B:BACK");
    } else if(ui->screen==WD_SCREEN_NOTICE) {
        char visible[WD_NOTICE_LINES][WD_UI_COLUMNS+1]; memset(visible,0,sizeof(visible));
        wd_wrap_from(notice_text(),WD_UI_COLUMNS,ui->notice_top,visible,WD_NOTICE_LINES);
        for(unsigned i=0;i<WD_NOTICE_LINES&&ui->notice_top+i<ui->notice_count;i++) if(visible[i][0]) line(11+9*i,visible[i]);
        if(ui->message[0]) line(56,ui->message);
        else if(!notice_at_end()) line(56,"DOWN:More  B:Back");
        else line(56,ui->notice==WD_NOTICE_RECOVER?"A:Yes  B:Back":"A:Yes  B:No");
    } else if(ui->screen==WD_SCREEN_TRANSFER_READY) {
        snprintf(b,sizeof(b),"http://%s/",ui->job.ip); compact_line(12,b);
        line(23,"Temporary access code");
        line(33,ui->job.token); line(43,"10 min from start");
        line(56,"UNENCRYPTED  B:STOP");
    } else if(ui->screen==WD_SCREEN_UPLOAD_MENU||ui->screen==WD_SCREEN_BULK_MENU) {
        wd_upload_row_t items[3]; unsigned count=upload_rows(items); const char *labels[3]; char wifi[32];
        if(ui->credentials.ssid[0]) snprintf(wifi,sizeof(wifi),"WiFi: %.14s",ui->credentials.ssid); else strcpy(wifi,"Choose WiFi");
        for(unsigned i=0;i<count;i++) labels[i]=items[i]==WD_UPLOAD_ROW_RUN?(ui->screen==WD_SCREEN_BULK_MENU?"Upload All Pending":"Upload Run"):items[i]==WD_UPLOAD_ROW_WIFI?wifi:"Configure WiGLE API";
        if(ui->upload_row>=count) ui->upload_row=0;
        draw_menu(labels,count,ui->upload_row,14);
        line(56,ui->message[0]?ui->message:!api_ready()?"API keys needed":wd_ui_link_up()?"WiFi connected":"WiFi not connected");
    } else if(ui->screen==WD_SCREEN_BULK_HOLD) {
        line(11,"Large amounts of"); line(20,"data may take time.");
        line(31,"Hold A for 3 seconds"); line(40,"to proceed.");
        if(wd_hold_visible(&ui->hold,BUTTON_A,ui->screen,now)) progress_bar(49,(int)wd_hold_permille(&ui->hold,ui->screen,now),now);
        else line(56,ui->message[0]?ui->message:"B:Back");
    } else if(ui->screen==WD_SCREEN_PROGRESS) {
        static const char *const stages[]={"Preparing...","Preparing...","Connecting...","Sending CSV","Awaiting response...","Complete","Failed"};
        unsigned stage=atomic_load(&ui->job.stage),sent=atomic_load(&ui->job.sent),total=atomic_load(&ui->job.total);
        if(stage>WD_STAGE_FAILED) stage=WD_STAGE_IDLE;
        bool measured=total&&(stage==WD_STAGE_SENDING||stage==WD_STAGE_AWAITING);
        unsigned current=measured?(unsigned)((uint64_t)sent*1000U/total):0;
        if(ui->job.job==WD_JOB_ALL) {
            unsigned index=atomic_load(&ui->job.index),count=atomic_load(&ui->job.count);
            if(!count) { line(11,"Checking sessions..."); progress_bar(31,-1,now); }
            else {
                snprintf(b,sizeof(b),"Run %u of %u",index?index:1,count); line(11,b);
                if(measured&&stage==WD_STAGE_SENDING) snprintf(b,sizeof(b),"Sending CSV %u%%",current/10); else snprintf(b,sizeof(b),"%s",stages[stage]);
                line(20,b);
                /* Overall: finished runs plus measured bytes of the current run. */
                unsigned done=index?index-1:0;
                progress_bar(31,(int)(((uint64_t)done*1000U+current)/count),now);
            }
            snprintf(b,sizeof(b),"OK %u  Failed %u",atomic_load(&ui->job.submitted),atomic_load(&ui->job.failures)); line(40,b);
        } else {
            line(14,stages[stage]);
            if(measured) { snprintf(b,sizeof(b),"%u%%",current/10); line(26,b); }
            progress_bar(38,measured?(int)current:-1,now);
        }
        line(56,atomic_load(&ui->job.cancel)?"Cancelling...":"B:Cancel");
    } else if(ui->screen==WD_SCREEN_API) {
        char name[24],token[24]; const char *labels[5];
        snprintf(name,sizeof(name),"API Name: %s",ui->credentials.api_name[0]?"********":"<Blank>");
        snprintf(token,sizeof(token),"API Token: %s",ui->credentials.api_token[0]?"********":"<Blank>");
        labels[0]=name; labels[1]=token; labels[2]="Import SD Setup"; labels[3]="Clear API Keys"; labels[4]="Delete Setup File";
        unsigned count=api_rows(); if(ui->api_row>=count) ui->api_row=0;
        draw_menu(labels,count,ui->api_row,12);
        line(56,ui->message[0]?ui->message:"A:SELECT   B:BACK");
    } else if(ui->screen==WD_SCREEN_DELETE_SETUP) {
        line(12,ui->setup_imported?"Credentials imported":"SD provisioning file"); line(24,"Delete setup file?");
        line(36,"Not secure erasure"); hold_ring(118,34,wd_hold_permille(&ui->hold,ui->screen,now));
        line(56,"HOLD A:Delete B:Keep");
    } else if(ui->screen==WD_SCREEN_CONSENT) {
        line(12,"Shares network names,"); line(22,"addresses and GPS"); line(32,"positions with WiGLE.");
        line(44,ui->message[0]?ui->message:"Completed runs only"); line(56,"A:Upload B:Cancel");
    } else if(ui->screen==WD_SCREEN_WORKING) { line(20,ui->message); line(40,"B REQUEST CANCEL");
    } else if(ui->screen==WD_SCREEN_TRANSFER) {
        line(12,"Receiving device and"); line(22,"POOM must be on the");
        line(32,"same WiFi network.");
        if(ui->credentials.ssid[0]) snprintf(b,sizeof(b),"WiFi: %.15s%s",ui->credentials.ssid,strlen(ui->credentials.ssid)>15?"...":"");
        else snprintf(b,sizeof(b),"Choose WiFi to continue.");
        compact_line(43,b); line(56,ui->credentials.ssid[0]?"A:Join R:WiFi B:Back":"A:Choose WiFi B:Back");
    } else if(ui->screen==WD_SCREEN_WIFI_ERROR) {
        line(12,ui->message); line(32,"CHECK WIFI SETTINGS"); line(56,"A:CHOOSE WIFI B:BACK");
    } else if(ui->screen==WD_SCREEN_WIFI_SETTINGS) {
        snprintf(b,sizeof(b),"WiFi: %.15s",ui->credentials.ssid[0]?ui->credentials.ssid:"None"); line(12,b);
        line(22,wd_ui_link_up()?"Connected":"Not connected");
        static const char *const labels[]={"Choose WiFi","Disconnect"};
        draw_menu(labels,wd_ui_link_up()?2:1,ui->choice_row%(wd_ui_link_up()?2:1),34);
        line(56,ui->message[0]?ui->message:"A:SELECT   B:BACK");
    } else if(ui->screen==WD_SCREEN_TIMEZONE) {
        snprintf(b,sizeof(b),"MODE %s",ui->time_zone.config.automatic?"AUTO GPS":"MANUAL"); menu_row(12,ui->timezone_row==0,b);
        snprintf(b,sizeof(b),"%s %.15s",ui->time_zone.config.automatic?"FB":"TZ",wd_timezone_name((wd_timezone_t)ui->time_zone.config.manual_zone)); menu_row(22,ui->timezone_row==1,b);
        static const char *const sources[]={"FB","GPS","MAN","LAST"};
        snprintf(b,sizeof(b),"USING %.10s %s",wd_timezone_name(ui->time_zone.zone),sources[ui->time_zone.source]); line(32,b);
        local_clock(42); line(56,"L/R:SET  B:BACK");
    } else if(ui->screen==WD_SCREEN_WIFI_CHOOSE) {
        static const char *const choices[]={"Saved WiFi profiles","Scan networks","Enter SSID"};
        for(unsigned i=0;i<3;i++) menu_row(14+i*11,i==ui->wifi_selection,choices[i]);
        line(56,ui->message[0]?ui->message:"U/D A:Select B:Back");
    } else if(ui->screen==WD_SCREEN_WIFI_PROFILES) {
        unsigned top=menu_view(ui->wifi_profiles.count,ui->wifi_selection,4);
        if(!ui->wifi_profiles.count) { line(18,"No saved profiles"); line(31,"Try Scan networks"); }
        for(unsigned i=top;i<ui->wifi_profiles.count&&i<top+4;i++) {
            snprintf(b,sizeof(b),"%.20s",ui->wifi_profiles.items[i].ssid); menu_row(12+(i-top)*10,i==ui->wifi_selection,b);
        }
        line(56,ui->message[0]?ui->message:ui->wifi_profiles.count?"U/D A:Join B:Back":"A:Scan B:Back");
    } else if(ui->screen==WD_SCREEN_WIFI_NETWORKS) {
        unsigned count=wd_ui_network_count(),top=menu_view(count,ui->wifi_selection,4);
        if(!count) line(20,"No networks found");
        for(unsigned i=top;i<count&&i<top+4;i++) {
            wd_ui_network_t network; if(!wd_ui_network_get(i,&network)) break;
            snprintf(b,sizeof(b),"%.15s %d",network.ssid[0]?network.ssid:"(hidden)",network.rssi); menu_row(12+(i-top)*10,i==ui->wifi_selection,b);
        }
        line(56,ui->message[0]?ui->message:count?"U/D A:Pick R:Scan B":"A/R:Scan B:Back");
    } else if(ui->screen==WD_SCREEN_ARCHIVE_RESULT) {
        line(11,ui->selected_label);
        if(ui->job.success) {
            line(22,"Logs and CSV bundled"); line(32,"Saved as session.tar");
            if(ui->job.processed) { char count[5]; wd_count_text(count,ui->job.processed); snprintf(b,sizeof(b),"CSV rows: %s",count); }
            else snprintf(b,sizeof(b),"No located CSV rows");
            line(42,b); line(56,"A:Transfer B:Back");
        } else { compact_line(24,ui->message); line(42,"Original logs kept"); line(56,"A:Retry B:Back"); }
    } else if(ui->screen==WD_SCREEN_FILE_RESULT) {
        line(11,ui->selected_label);
        if(ui->job.success&&ui->job.job==WD_JOB_EXPORT) {
            snprintf(b,sizeof(b),"CSV rows: %lu",(unsigned long)ui->job.processed); line(22,b);
            snprintf(b,sizeof(b),"Skipped: %lu",(unsigned long)ui->job.failed); line(32,b);
        } else if(ui->job.success) { line(22,"Summary rebuilt."); line(32,"Create the CSV again"); }
        else compact_line(24,ui->message);
        line(42,"Original logs kept"); line(56,ui->job.success&&ui->job.job==WD_JOB_EXPORT?"A:Upload B:Back":"B:Back");
    } else if(ui->screen==WD_SCREEN_WIFI_SCAN_ERROR) {
        compact_line(18,ui->message); line(35,"Choose WiFi or retry"); line(56,"A/R:Retry B:Back");
    } else if(ui->screen==WD_SCREEN_WIFI_SAVE) {
        line(12,"WiFi connection OK"); snprintf(b,sizeof(b),"%.21s",ui->credentials.ssid); line(22,b);
        line(33,"Save for next time?"); line(43,ui->message);
        line(56,"A:Save B:Skip");
    } else if(ui->screen==WD_SCREEN_UPLOAD_RESULT) {
        line(12,ui->message); snprintf(b,sizeof(b),"Submitted: %u",ui->job.processed); line(23,b);
        snprintf(b,sizeof(b),"Failed: %u",ui->job.failed); line(33,b);
        snprintf(b,sizeof(b),"Skipped: %u",ui->job.skipped); line(43,b); line(56,"B:Back");
    } else if(ui->screen==WD_SCREEN_IMPORT) {
        line(12,"Do you wish to import"); line(22,"your WiGLE API keys"); line(32,"from the SD card?");
        if(ui->message[0]) compact_line(44,ui->message);
        line(56,"A:Yes B:No");
    } else if(ui->screen==WD_SCREEN_SETTINGS) {
        char texts[WD_SETTINGS_ROWS][WD_UI_COLUMNS+1]; const char *labels[WD_SETTINGS_ROWS];
        for(unsigned i=0;i<WD_SETTINGS_ROWS;i++) { wd_settings_row_text(&ui->settings,&ui->time_zone.config,i,texts[i]); labels[i]=texts[i]; }
        draw_menu(labels,WD_SETTINGS_ROWS,ui->setting_row%WD_SETTINGS_ROWS,12);
        line(56,ui->message[0]?ui->message:ui->setting_row==WD_SETTINGS_TIMEZONE_ROW?"A:OPEN     B:EXIT":"L/R:ADJUST B:EXIT");
    } else if(ui->screen==WD_SCREEN_SAVE_CHANGES) {
        line(24,"A: Yes"); line(36,"B: No");
    } else { line(20,ui->message); line(56,"B BACK"); }
    present();
}
/* Short B: go back one level. */
static void back_key(void) {
    switch(ui->screen) {
        case WD_SCREEN_MAIN: ui->exiting=true; stop(); return;
        case WD_SCREEN_LIVE: case WD_SCREEN_GPS: case WD_SCREEN_STATS:
            if(ui->screen==WD_SCREEN_GPS) poom_gps_capture_stop(&ui->gps_capture);
            ui->screen=wd_ui_session_back(ui->screen); ui->selection=0; ui->raw=false; ui->message[0]=0; return;
        case WD_SCREEN_STOP: ui->screen=ui->stop_return; return;
        case WD_SCREEN_SAVED:
            wd_session_cache_close(&ui->saved_cache);
            ui->screen=WD_SCREEN_MAIN; ui->message[0]=0; return;
        case WD_SCREEN_SESSION: ui->screen=WD_SCREEN_SAVED; ui->message[0]=0; saved_refresh(0); return;
        case WD_SCREEN_UPLOAD_CHOICE: case WD_SCREEN_OPTIONS: ui->screen=WD_SCREEN_SESSION; ui->message[0]=0; return;
        case WD_SCREEN_TRANSFER_FORMAT: ui->choice_row=0; ui->screen=WD_SCREEN_UPLOAD_CHOICE; ui->message[0]=0; return;
        case WD_SCREEN_NOTICE: ui->screen=ui->notice_return; ui->message[0]=0; return;
        case WD_SCREEN_TRANSFER: ui->screen=WD_SCREEN_TRANSFER_FORMAT; ui->message[0]=0; return;
        case WD_SCREEN_TRANSFER_READY: wd_ui_disconnect(); ui->transfer_on=false; memset(ui->job.token,0,sizeof(ui->job.token)); ui->screen=WD_SCREEN_SESSION; ui->message[0]=0; saved_refresh(0); return;
        case WD_SCREEN_UPLOAD_MENU: ui->screen=WD_SCREEN_UPLOAD_CHOICE; ui->message[0]=0; return;
        case WD_SCREEN_CONSENT: ui->screen=WD_SCREEN_UPLOAD_MENU; ui->message[0]=0; return;
        case WD_SCREEN_BULK_HOLD: ui->screen=WD_SCREEN_BULK_MENU; ui->message[0]=0; return;
        case WD_SCREEN_UPLOAD_RESULT:
            ui->message[0]=0;
            if(ui->job.job==WD_JOB_ALL) ui->screen=WD_SCREEN_MAIN; else { ui->screen=WD_SCREEN_SESSION; ui->session_action=0; saved_refresh(0); }
            return;
        case WD_SCREEN_API: ui->screen=ui->api_return; ui->message[0]=0; return;
        case WD_SCREEN_DELETE_SETUP: ui->screen=WD_SCREEN_API; ui->message[0]=0; return;
        case WD_SCREEN_WIFI_CHOOSE: wd_ui_profiles_close(&ui->wifi_profiles); wd_ui_networks_clear(); ui->screen=ui->wifi_purpose==WD_JOB_WIFI_JOIN?ui->wifi_return:WD_SCREEN_TRANSFER; return;
        case WD_SCREEN_WIFI_PROFILES: case WD_SCREEN_WIFI_NETWORKS: case WD_SCREEN_WIFI_SCAN_ERROR: wifi_choose(); return;
        case WD_SCREEN_WIFI_ERROR: ui->message[0]=0; ui->screen=ui->wifi_purpose==WD_JOB_WIFI_JOIN?ui->wifi_return:WD_SCREEN_TRANSFER; return;
        case WD_SCREEN_TIMEZONE: ui->screen=WD_SCREEN_SETTINGS; return;
        default: ui->screen=ui->session.active?WD_SCREEN_LIVE:WD_SCREEN_MAIN; ui->raw=false; ui->message[0]=0; return;
    }
}
static void key_press(uint8_t key,poom_gps_fix_t f,uint64_t now) {
    if(ui->screen==WD_SCREEN_WORKING||ui->screen==WD_SCREEN_PROGRESS) { if(key==BUTTON_B) atomic_store(&ui->job.cancel,true); return; }
    if(ui->screen==WD_SCREEN_FILE_RESULT||ui->screen==WD_SCREEN_ARCHIVE_RESULT) { file_result_key(key); return; }
    if(ui->screen==WD_SCREEN_WIFI_SAVE) { profile_save_key(key); return; }
    if(ui->screen==WD_SCREEN_IMPORT) { import_setup_key(key); return; }
    if(ui->screen==WD_SCREEN_KEYBOARD) {
        if(key==BUTTON_B) { keyboard_cancel(); return; }
        uint8_t logical=key==BUTTON_A?0:key==BUTTON_LEFT?2:key==BUTTON_RIGHT?3:key==BUTTON_UP?4:5;
        if(poom_ui_keyboard_handle_button(&ui->keyboard,logical)==POOM_UI_KEYBOARD_ACTION_ACCEPT) {
            if(wifi_keyboard_accept()) return;
            if(!ui->edit[0]) strcpy(ui->message,"UNCHANGED");
            else if(ui->edit_field==8&&strchr(ui->edit,':')) strcpy(ui->message,"API NAME: NO COLON");
            else if(ui->edit_field==8) { snprintf(ui->credentials.api_name,sizeof(ui->credentials.api_name),"%.64s",ui->edit); api_saved("API name saved"); }
            else if(ui->edit_field==9) { snprintf(ui->credentials.api_token,sizeof(ui->credentials.api_token),"%.128s",ui->edit); api_saved("API token saved"); }
            memset(ui->edit,0,sizeof(ui->edit)); ui->screen=ui->edit_return_screen;
        }
        return;
    }
    if(ui->screen==WD_SCREEN_SAVE_CHANGES) {
        if(key==BUTTON_A||key==BUTTON_B) settings_finish(key==BUTTON_A,&f,now);
        return;
    }
    if(key==BUTTON_B) {
        if(ui->screen==WD_SCREEN_SETTINGS) { if(settings_changed()) ui->screen=WD_SCREEN_SAVE_CHANGES; else { ui->screen=WD_SCREEN_MAIN; ui->message[0]=0; } return; }
        back_key(); return;
    }
    if(ui->session.active&&wd_ui_is_live_page(ui->screen)&&(key==BUTTON_LEFT||key==BUTTON_RIGHT)) {
        ui->screen=wd_ui_session_page(ui->screen,key==BUTTON_RIGHT);
        if(ui->screen==WD_SCREEN_GPS) ui->gps_page=0;
        ui->raw=false; return;
    }
    switch(ui->screen) {
        case WD_SCREEN_MAIN: {
            wd_menu_item_t items[8]; unsigned count=wd_main_menu(ui->session.active,items);
            if(key==BUTTON_UP) ui->selection=(ui->selection+count-1)%count;
            if(key==BUTTON_DOWN) ui->selection=(ui->selection+1)%count;
            if(key==BUTTON_A) main_select();
            break;
        }
        case WD_SCREEN_LIVE: break; /* pause/resume needs the 2 s hold; taps do nothing */
        case WD_SCREEN_STOP: if(key==BUTTON_A) { stop(); ui->screen=WD_SCREEN_LIVE; } break;
        case WD_SCREEN_GPS:
            if(key==BUTTON_RIGHT&&!ui->session.active) gps_capture_toggle();
            if(key==BUTTON_A) ui->raw=!ui->raw;
            if(key==BUTTON_UP||key==BUTTON_DOWN) ui->gps_page=wd_ui_gps_page(ui->gps_page,key==BUTTON_DOWN);
            break;
        case WD_SCREEN_SAVED: saved_key(key); break;
        case WD_SCREEN_SESSION: {
            wd_session_action_t items[3]; unsigned count=session_actions(items);
            if(key==BUTTON_DOWN) ui->session_action=(ui->session_action+1)%count;
            if(key==BUTTON_UP) ui->session_action=(ui->session_action+count-1)%count;
            if(key==BUTTON_A) {
                ui->message[0]=0; if(ui->session_action>=count) ui->session_action=0;
                if(items[ui->session_action]==WD_ACTION_CSV) launch(WD_JOB_EXPORT);
                else if(items[ui->session_action]==WD_ACTION_UPLOAD) { ui->choice_row=0; ui->screen=WD_SCREEN_UPLOAD_CHOICE; }
                else { ui->option_row=0; ui->screen=WD_SCREEN_OPTIONS; }
            }
            break;
        }
        case WD_SCREEN_UPLOAD_CHOICE: case WD_SCREEN_TRANSFER_FORMAT:
            if(key==BUTTON_UP||key==BUTTON_DOWN) ui->choice_row^=1;
            if(key!=BUTTON_A) break;
            ui->message[0]=0;
            if(ui->screen==WD_SCREEN_UPLOAD_CHOICE) { if(ui->choice_row) upload_open(WD_SCREEN_UPLOAD_MENU); else { ui->choice_row=0; ui->screen=WD_SCREEN_TRANSFER_FORMAT; } }
            else if(ui->choice_row) launch(WD_JOB_ARCHIVE);
            else ui->screen=WD_SCREEN_TRANSFER; /* existing CSV served as is; no TAR */
            break;
        case WD_SCREEN_OPTIONS:
            if(key==BUTTON_DOWN) ui->option_row=(ui->option_row+1)%3;
            if(key==BUTTON_UP) ui->option_row=(ui->option_row+2)%3;
            if(key==BUTTON_A) notice_open(ui->option_row==0?WD_NOTICE_RECOVER:ui->option_row==1?WD_NOTICE_MARK:WD_NOTICE_RESET,WD_SCREEN_OPTIONS);
            break;
        case WD_SCREEN_NOTICE: notice_key(key); break;
        case WD_SCREEN_TRANSFER: transfer_setup_key(key); break;
        case WD_SCREEN_UPLOAD_MENU: case WD_SCREEN_BULK_MENU: upload_key(key); break;
        case WD_SCREEN_CONSENT: if(key==BUTTON_A) launch(WD_JOB_UPLOAD); break;
        case WD_SCREEN_API: api_key(key); break;
        case WD_SCREEN_WIFI_ERROR: if(key==BUTTON_A) wifi_choose(); break;
        case WD_SCREEN_WIFI_SETTINGS: {
            unsigned count=wd_ui_link_up()?2:1;
            if(key==BUTTON_UP||key==BUTTON_DOWN) ui->choice_row=(ui->choice_row+1)%count;
            if(key==BUTTON_A) {
                ui->message[0]=0;
                if(ui->choice_row%count==0) wifi_join_for(WD_SCREEN_WIFI_SETTINGS);
                else { wd_ui_disconnect(); ui->transfer_on=false; ui->choice_row=0; strcpy(ui->message,"WiFi disconnected"); }
            }
            break;
        }
        case WD_SCREEN_WIFI_CHOOSE: case WD_SCREEN_WIFI_PROFILES: case WD_SCREEN_WIFI_NETWORKS: case WD_SCREEN_WIFI_SCAN_ERROR: wifi_key(key); break;
        case WD_SCREEN_TIMEZONE:
            if(key==BUTTON_UP||key==BUTTON_DOWN) ui->timezone_row^=1;
            if(key==BUTTON_A||key==BUTTON_LEFT||key==BUTTON_RIGHT) {
                if(!ui->timezone_row) ui->time_zone.config.automatic=!ui->time_zone.config.automatic;
                else ui->time_zone.config.manual_zone=(uint8_t)((ui->time_zone.config.manual_zone+WD_TZ_COUNT+(key==BUTTON_LEFT?-1:1))%WD_TZ_COUNT);
                wd_timezone_init(&ui->time_zone,&ui->time_zone.config); wd_timezone_update(&ui->time_zone,&f,now);
            }
            break;
        case WD_SCREEN_SETTINGS:
            ui->message[0]=0;
            if(key==BUTTON_DOWN) ui->setting_row=(ui->setting_row+1)%WD_SETTINGS_ROWS;
            if(key==BUTTON_UP) ui->setting_row=(ui->setting_row+WD_SETTINGS_ROWS-1)%WD_SETTINGS_ROWS;
            if(key==BUTTON_LEFT||key==BUTTON_RIGHT) wd_settings_adjust(&ui->settings,ui->setting_row,key==BUTTON_RIGHT?1:-1);
            if(key==BUTTON_A&&ui->setting_row==WD_SETTINGS_TIMEZONE_ROW) { ui->timezone_row=0; ui->screen=WD_SCREEN_TIMEZONE; }
            break;
        default: break;
    }
}
static void input_event(wd_key_event_t key,poom_gps_fix_t f,uint64_t now) {
    if(key.button>BUTTON_DOWN) return;
    if(key.event==BUTTON_PRESS_DOWN) {
        ui->suppress_click[key.button]=false;
        uint32_t duration=hold_duration(key.button);
        if(duration) wd_hold_press(&ui->hold,key.button,ui->screen,duration,now);
        return;
    }
    if(key.event==BUTTON_PRESS_UP) { wd_hold_release(&ui->hold,key.button); return; }
    /* A completed hold never also performs the short-press action. */
    if(ui->suppress_click[key.button]) { ui->suppress_click[key.button]=false; return; }
    key_press(key.button,f,now);
}
static void task(void *ctx) {
    (void)ctx; wd_settings_default(&ui->settings);
    size_t settings_len=sizeof(ui->settings);
    if(poom_secrets_get_blob("wd_settings",&ui->settings,&settings_len)!=ESP_OK||settings_len!=sizeof(ui->settings)||!wd_settings_valid(&ui->settings)) wd_settings_default(&ui->settings);
    timezone_load();
    memset(&ui->credentials,0,sizeof(ui->credentials)); wd_credentials_load(&ui->credentials);
    ui->transfer_on=false; ui->offer_wifi_save=false; ui->wifi_purpose=WD_JOB_TRANSFER; ui->upload_row=0; ui->setup_imported=false;
    poom_gps_service_init(&ui->gps,poom_gps_xa1110()); ui->gps.diagnostic=diagnostic;
    WD_DEBUG("[wardrive] enter heap=%lu largest=%lu\n",(unsigned long)esp_get_free_heap_size(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    while(!ui->exiting||ui->session.active||atomic_load(&ui->job.busy)) {
        uint64_t now=esp_timer_get_time()/1000;
        poom_gps_capture_poll(&ui->gps_capture,now);
        /* Drain up to the receiver's 256-byte stream budget between OLED
         * frames; one 32-byte read per frame cannot keep up at 100 kHz. */
        for(unsigned poll=0;poll<8;poll++) poom_gps_service_poll(&ui->gps,now);
        poom_gps_fix_t f=poom_nmea_snapshot(&ui->gps.parser,now); wd_radio_set_gps(&f);
        if(f.connected&&f.fix) ui->had_fix=true;
        wd_timezone_update(&ui->time_zone,&f,now);
        (void)wd_clock_update_gps(&ui->clock,&f,now);
        if(ui->session.active) wd_session_time(&ui->session,app_utc(),now);
        if(ui->session.active&&!ui->session.paused&&!ui->session.fault&&now-ui->last_gps_log>=1000) {
            if(!wd_session_gps(&ui->session,&f,now)) stop();
            ui->last_gps_log=now;
            wd_distance_update(&ui->distance,&f,now);
        }
        if(atomic_load(&ui->job.done)) complete_job();
        if(ui->transfer_on&&wd_transfer_expired()) { wd_ui_disconnect(); ui->transfer_on=false; ui->screen=WD_SCREEN_MAIN; strcpy(ui->message,"TRANSFER EXPIRED"); }
        if(ui->session.active&&!ui->session.fault&&now-ui->last_checkpoint>=30000) {
            if(!wd_session_checkpoint(&ui->session,ui->session.paused?"Paused":"Active")) { ui->session.fault=true; stop(); }
            ui->last_checkpoint=now;
        }
        wd_observation_t o;
        for(unsigned i=0;i<16&&wd_radio_next(&o);i++) {
            if(ui->session.active&&!ui->session.paused&&!ui->session.fault&&!wd_session_observe(&ui->session,&o)) { stop(); strcpy(ui->message,"SD FAULT: KEPT LOG"); }
        }
        if(ui->stopping&&wd_radio_finished()) {
            ui->session.ended_ms=now;
            bool saved=wd_session_stop(&ui->session); wd_radio_release(); ui->stopping=false; ui->screen=WD_SCREEN_MAIN; ui->selection=0; wd_hold_cancel(&ui->hold);
            WD_DEBUG("[wardrive session] saved=%u observations=%lu wifi=%lu ble=%lu errors=%lu directory=%s\n",
                (unsigned)saved,(unsigned long)ui->session.observations,(unsigned long)ui->session.wifi_count,
                (unsigned long)ui->session.ble_count,(unsigned long)ui->session.write_errors,ui->session.directory);
            if(!saved) strcpy(ui->message,"SD ERROR: RECOVER LOG");
            WD_DEBUG("[wardrive] stop radio_stack_free=%u ui_stack_free=%u heap=%lu largest=%lu\n",wd_radio_stack_free(),(unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned long)esp_get_free_heap_size(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        wd_key_event_t key;
        while(xQueueReceive(buttons,&key,0)==pdTRUE) input_event(key,f,now);
        if(wd_hold_poll(&ui->hold,ui->screen,now)) hold_fired(now);
        draw(now,f); vTaskDelay(pdMS_TO_TICKS(40));
    }
    poom_gps_capture_stop(&ui->gps_capture);
    poom_gps_service_close(&ui->gps);
    wd_ui_disconnect(); wd_radio_release();
    /* Never hand the menu back while a Wardrive radio cleanup is incomplete. */
    while(!wd_radio_quiescent()) {
        strcpy(ui->message,"RADIO CLEANUP RETRY");
        draw((uint64_t)esp_timer_get_time()/1000,(poom_gps_fix_t){0});
        vTaskDelay(pdMS_TO_TICKS(100));
        wd_ui_disconnect(); wd_radio_release();
    }
    wd_ui_profiles_close(&ui->wifi_profiles); wd_ui_networks_clear();
    wd_credentials_clear(&ui->credentials); memset(ui->edit,0,sizeof(ui->edit)); memset(ui->job.token,0,sizeof(ui->job.token));
    poom_sbus_unsubscribe_cb("input/button",button,NULL);
    if(ui->mounted_here) sd_card_unmount();
    portENTER_CRITICAL(&button_lock); QueueHandle_t old_buttons=buttons; buttons=NULL; portEXIT_CRITICAL(&button_lock);
    vQueueDelete(old_buttons); ui_free(); atomic_store(&visible,false);
    uint8_t token=1; poom_sbus_publish("poom/menu/resume",&token,sizeof(token),0);
    vTaskDelete(NULL);
}
/* Internal RAM: the job worker and radio code share this state; it only
 * exists while Wardrive is open. */
static bool ui_alloc(void) {
    ui=heap_caps_calloc(1,sizeof(*ui),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!ui) return false;
    ui->presented_screen=UINT_MAX; ui->presented_gps_page=UINT_MAX;
    ui->wifi_return=WD_SCREEN_WIFI_SETTINGS; ui->edit_return_screen=WD_SCREEN_API;
    ui->api_return=WD_SCREEN_MAIN; ui->stop_return=WD_SCREEN_MAIN; ui->notice_return=WD_SCREEN_OPTIONS;
    ui->wifi_purpose=WD_JOB_TRANSFER;
    return true;
}
static void ui_free(void) {
    wd_ui_state_t *state=ui; ui=NULL;
    if(!state) return;
    wd_session_cache_close(&state->saved_cache);
    /* Wipe secrets (API keys, WiFi password, edit buffer) before release. */
    volatile unsigned char *bytes=(volatile unsigned char *)state;
    for(size_t i=0;i<sizeof(*state);i++) bytes[i]=0;
    heap_caps_free(state);
}
void menu_wardrive_show(void) {
    if(atomic_exchange(&visible,true)) return;
    if(!ui_alloc()) {
        WD_DEBUG("[wardrive] state allocation failed\n");
        atomic_store(&visible,false);
        uint8_t token=1; poom_sbus_publish("poom/menu/resume",&token,sizeof(token),0);
        return;
    }
    ui->presented_screen=UINT_MAX; ui->presented_gps_page=UINT_MAX;
    ui->screen=WD_SCREEN_MAIN; ui->selection=0; ui->exiting=false; ui->stopping=false; ui->mounted_here=false; ui->raw=false; ui->message[0]=0;
    ui->had_fix=false; wd_hold_cancel(&ui->hold); memset(ui->suppress_click,0,sizeof(ui->suppress_click));
    buttons=xQueueCreate(16,sizeof(wd_key_event_t));
    if(buttons&&poom_sbus_subscribe_cb("input/button",button,NULL)) {
        if(xTaskCreate(task,"wardrive",6144,NULL,3,NULL)==pdPASS) return;
        poom_sbus_unsubscribe_cb("input/button",button,NULL);
    }
    portENTER_CRITICAL(&button_lock); QueueHandle_t old_buttons=buttons; buttons=NULL; portEXIT_CRITICAL(&button_lock);
    if(old_buttons) vQueueDelete(old_buttons);
    ui_free();
    atomic_store(&visible,false);
    uint8_t token=1; poom_sbus_publish("poom/menu/resume",&token,sizeof(token),0);
}
