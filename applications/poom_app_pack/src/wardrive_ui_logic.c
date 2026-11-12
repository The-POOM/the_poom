// SPDX-License-Identifier: MIT
#include "wardrive_ui_logic.h"
#include <stdio.h>
#include <string.h>

void wd_hold_press(wd_hold_t *h,uint8_t button,unsigned screen,uint32_t duration_ms,uint64_t now) {
    *h=(wd_hold_t){.start_ms=now,.duration_ms=duration_ms,.screen=screen,.button=button,.active=true};
}
void wd_hold_release(wd_hold_t *h,uint8_t button) {
    if(h->active&&h->button==button) wd_hold_cancel(h);
}
void wd_hold_cancel(wd_hold_t *h) { h->active=false; h->fired=false; }
bool wd_hold_poll(wd_hold_t *h,unsigned screen,uint64_t now) {
    if(!h->active||h->fired) return false;
    if(h->screen!=screen) { wd_hold_cancel(h); return false; }
    if(now<h->start_ms||now-h->start_ms<h->duration_ms) return false;
    h->fired=true; return true;
}
unsigned wd_hold_permille(const wd_hold_t *h,unsigned screen,uint64_t now) {
    if(!h->active||h->screen!=screen||!h->duration_ms) return 0;
    if(h->fired) return 1000;
    uint64_t held=now>h->start_ms?now-h->start_ms:0;
    return held>=h->duration_ms?1000U:(unsigned)(held*1000U/h->duration_ms);
}
bool wd_hold_visible(const wd_hold_t *h,uint8_t button,unsigned screen,uint64_t now) {
    return h->active&&!h->fired&&h->button==button&&h->screen==screen&&now>=h->start_ms&&now-h->start_ms>=WD_HOLD_FEEDBACK_MS;
}

void wd_elapsed_start(wd_elapsed_t *e,uint64_t now) { *e=(wd_elapsed_t){.resumed_ms=now,.running=true}; }
void wd_elapsed_pause(wd_elapsed_t *e,uint64_t now) {
    if(!e->running) return;
    e->accumulated_ms+=now>e->resumed_ms?now-e->resumed_ms:0; e->running=false;
}
void wd_elapsed_resume(wd_elapsed_t *e,uint64_t now) {
    if(e->running) return;
    e->resumed_ms=now; e->running=true;
}
uint64_t wd_elapsed_ms(const wd_elapsed_t *e,uint64_t now) {
    return e->accumulated_ms+(e->running&&now>e->resumed_ms?now-e->resumed_ms:0);
}
void wd_elapsed_text(uint64_t ms,char out[9]) {
    uint64_t seconds=ms/1000U;
    if(seconds>99U*3600U+59U*60U+59U) seconds=99U*3600U+59U*60U+59U;
    snprintf(out,9,"%02u:%02u:%02u",(unsigned)(seconds/3600U),(unsigned)(seconds/60U%60U),(unsigned)(seconds%60U));
}

void wd_clock_text(int64_t utc,wd_timezone_t zone,char out[6]) {
    wd_local_time_t local;
    /* Before 2024 the RTC hasn't been set from GPS or the network. */
    if(utc<1704067200||!wd_timezone_local(utc,zone,&local)) { strcpy(out,"--:--"); return; }
    snprintf(out,6,"%02u:%02u",(unsigned)local.calendar.tm_hour%24U,(unsigned)local.calendar.tm_min%60U);
}
void wd_count_text(char out[5],uint32_t count) {
    const uint32_t divisors[]={1000000000,1000000,1000};
    const char units[]={'G','M','k'};
    for(unsigned i=0;i<3;i++) if(count>=divisors[i]) {
        unsigned whole=count/divisors[i];
        if(whole<10) snprintf(out,5,"%u.%u%c",whole%10,(unsigned)((count/(divisors[i]/10))%10),units[i]);
        else snprintf(out,5,"%u%c",whole%1000,units[i]);
        return;
    }
    snprintf(out,5,"%u",(unsigned)(count%1000));
}
void wd_distance_text(double metres,char out[8]) {
    if(!(metres>0)) metres=0;
    if(metres<999.5) snprintf(out,8,"%um",(unsigned)(metres+0.5));
    else if(metres<99950) snprintf(out,8,"%.1fkm",metres/1000);
    else if(metres<9999500) snprintf(out,8,"%ukm",(unsigned)(metres/1000+0.5));
    else strcpy(out,">9999k");
}
wd_gps_icon_t wd_gps_icon(bool connected,bool fix,bool had_fix) {
    if(connected&&fix) return WD_GPS_ICON_FIX;
    if(connected&&!had_fix) return WD_GPS_ICON_SEARCHING;
    return WD_GPS_ICON_NONE;
}

unsigned wd_main_menu(bool active,wd_menu_item_t out[8]) {
    unsigned n=0;
    out[n++]=WD_MENU_START;
    if(active) out[n++]=WD_MENU_STOP;
    out[n++]=WD_MENU_GPS; out[n++]=WD_MENU_SAVED; out[n++]=WD_MENU_BULK;
    out[n++]=WD_MENU_API; out[n++]=WD_MENU_WIFI; out[n++]=WD_MENU_SETTINGS;
    return n;
}
const char *wd_main_menu_label(wd_menu_item_t item,bool active) {
    switch(item) {
        case WD_MENU_START: return active?"Resume Session":"Start Wardrive";
        case WD_MENU_STOP: return "Stop Session";
        case WD_MENU_GPS: return "Check GPS Status";
        case WD_MENU_SAVED: return "Saved Sessions";
        case WD_MENU_BULK: return "Bulk WiGLE Upload";
        case WD_MENU_API: return "WiGLE API Settings";
        case WD_MENU_WIFI: return "WiFi Upload Settings";
        case WD_MENU_SETTINGS: return "Additional Settings";
    }
    return "";
}

unsigned wd_wrap(const char *text,unsigned width,char lines[][WD_UI_COLUMNS+1],unsigned max) {
    return wd_wrap_from(text,width,0,lines,max);
}
unsigned wd_wrap_from(const char *text,unsigned width,unsigned first,char lines[][WD_UI_COLUMNS+1],unsigned max) {
    if(width>WD_UI_COLUMNS) width=WD_UI_COLUMNS;
    unsigned count=0; char current[WD_UI_COLUMNS+1]; size_t used=0;
#define WD_EMIT() do { if(lines&&count>=first&&count-first<max) { memcpy(lines[count-first],current,used); lines[count-first][used]=0; } count++; used=0; } while(0)
    const char *p=text;
    while(*p) {
        if(*p=='\n') { WD_EMIT(); p++; continue; }
        if(*p==' ') { p++; continue; }
        size_t word=strcspn(p," \n");
        while(word>width) { /* hard-split words longer than a line */
            if(used) WD_EMIT();
            memcpy(current,p,width); used=width; WD_EMIT();
            p+=width; word-=width;
        }
        if(!word) continue;
        if(used&&used+1+word>width) WD_EMIT();
        if(used) current[used++]=' ';
        memcpy(current+used,p,word); used+=word; p+=word;
    }
    if(used) WD_EMIT();
#undef WD_EMIT
    return count;
}

const char *wd_scan_type_text(wd_mode_t mode) {
    return mode==WD_WIFI?"WiFi":mode==WD_BLE?"BLE":"WiFi + BLE";
}
static unsigned step(unsigned value,unsigned low,unsigned high,unsigned unit,int direction) {
    unsigned v=value/unit;
    if(v<low||v>high) v=low;
    if(direction>0) v=v>=high?low:v+1;
    else if(direction<0) v=v<=low?high:v-1;
    return v*unit;
}
void wd_settings_adjust(wd_settings_t *s,unsigned row,int direction) {
    if(!direction) return;
    switch(row) {
        case 0: { /* WiFi + BLE -> WiFi -> BLE -> WiFi + BLE */
            static const wd_mode_t order[]={WD_COMBINED,WD_WIFI,WD_BLE};
            unsigned i=0; while(i<3&&order[i]!=s->mode) i++;
            if(i>=3) i=0;
            s->mode=order[(i+(direction>0?1U:2U))%3U]; break;
        }
        case 1: s->wifi_interval_ms=step(s->wifi_interval_ms,1,30,1000,direction); break;
        case 2: s->observation_interval_ms=step(s->observation_interval_ms,1,60,1000,direction); break;
        case 3: {
            int v=s->rssi_threshold;
            if(v<-110||v>-30||(v+110)%5) v=-110;
            if(direction>0) v=v>=-30?-110:v+5; else v=v<=-110?-30:v-5;
            s->rssi_threshold=v; break;
        }
        case 4: s->ble_window_ms=step(s->ble_window_ms,1,10,1000,direction); break;
        default: break;
    }
}
void wd_settings_row_text(const wd_settings_t *s,const wd_timezone_config_t *zone,unsigned row,char out[WD_UI_COLUMNS+1]) {
    switch(row) {
        case 0: snprintf(out,WD_UI_COLUMNS+1,"Scan Type %s",wd_scan_type_text(s->mode)); break;
        case 1: snprintf(out,WD_UI_COLUMNS+1,"Scan Gap %us",s->wifi_interval_ms/1000U); break;
        case 2: snprintf(out,WD_UI_COLUMNS+1,"Log Gap %us",s->observation_interval_ms/1000U); break;
        case 3: snprintf(out,WD_UI_COLUMNS+1,"RSSI %ddBm",s->rssi_threshold); break;
        case 4: snprintf(out,WD_UI_COLUMNS+1,"BLE Window %us",s->ble_window_ms/1000U); break;
        case 5: snprintf(out,WD_UI_COLUMNS+1,"Time Zone %s",zone&&zone->automatic?"Auto":"Manual"); break;
        default: out[0]=0; break;
    }
}

/* Rows 1-5: ....X / ...X. / X..X. / .XX.. / ..X.. */
const uint8_t wd_glyph_check[5]={0x08,0x10,0x30,0x0C,0x02};
/* Bluetooth rune, 5x8. */
const uint8_t wd_glyph_bluetooth[5]={0x24,0x18,0xFF,0x5A,0x24};
const wd_point_t wd_ring_points[]={
    {0,-6},{1,-6},{2,-6},{2,-5},{3,-5},{4,-5},{4,-4},{5,-4},{5,-3},{5,-2},{6,-2},{6,-1},
    {6,0},{6,1},{6,2},{5,2},{5,3},{5,4},{4,4},{4,5},{3,5},{2,5},{2,6},{1,6},
    {0,6},{-1,6},{-2,6},{-2,5},{-3,5},{-4,5},{-4,4},{-5,4},{-5,3},{-5,2},{-6,2},{-6,1},
    {-6,0},{-6,-1},{-6,-2},{-5,-2},{-5,-3},{-5,-4},{-4,-4},{-4,-5},{-3,-5},{-2,-5},{-2,-6},{-1,-6}};
const unsigned wd_ring_point_count=sizeof(wd_ring_points)/sizeof(*wd_ring_points);
