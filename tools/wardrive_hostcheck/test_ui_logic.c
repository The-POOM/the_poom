// SPDX-License-Identifier: MIT
#include "wardrive_ui_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    /* Hold: fires once at the threshold, never repeats, resets on release. */
    wd_hold_t h={0};
    wd_hold_press(&h,0,1,WD_HOLD_PAUSE_MS,1000);
    assert(!wd_hold_poll(&h,1,1000)&&wd_hold_permille(&h,1,1000)==0);
    assert(!wd_hold_visible(&h,0,1,1100)&&wd_hold_visible(&h,0,1,1150));
    assert(wd_hold_permille(&h,1,2000)==500&&!wd_hold_poll(&h,1,2999));
    assert(wd_hold_poll(&h,1,3000)&&wd_hold_permille(&h,1,3000)==1000);
    for(uint64_t t=3000;t<20000;t+=40) assert(!wd_hold_poll(&h,1,t)); /* held past threshold: once */
    assert(!wd_hold_visible(&h,0,1,4000));
    wd_hold_release(&h,0); assert(!h.active&&wd_hold_permille(&h,1,5000)==0);
    wd_hold_press(&h,0,1,WD_HOLD_PAUSE_MS,10000);
    wd_hold_release(&h,1); assert(h.active); /* other button's release ignored */
    wd_hold_release(&h,0); assert(!wd_hold_poll(&h,1,13000)); /* early release cancels */
    wd_hold_press(&h,1,1,WD_HOLD_STOP_MS,0);
    assert(!wd_hold_poll(&h,2,5000)&&!h.active); /* screen changed: cancelled */
    wd_hold_press(&h,1,1,WD_HOLD_STOP_MS,100); assert(!wd_hold_poll(&h,1,3099)&&wd_hold_poll(&h,1,3100));
    wd_hold_press(&h,0,7,WD_HOLD_DELETE_MS,500); assert(wd_hold_permille(&h,6,2000)==0);

    /* Elapsed recording time excludes pauses and survives re-entry. */
    wd_elapsed_t e; char text[9];
    wd_elapsed_start(&e,5000); assert(wd_elapsed_ms(&e,5000)==0);
    wd_elapsed_text(wd_elapsed_ms(&e,5000),text); assert(!strcmp(text,"00:00:00"));
    wd_elapsed_pause(&e,65000); assert(wd_elapsed_ms(&e,999999)==60000);
    wd_elapsed_pause(&e,70000); assert(wd_elapsed_ms(&e,70000)==60000);
    wd_elapsed_resume(&e,100000); assert(wd_elapsed_ms(&e,101000)==61000);
    wd_elapsed_resume(&e,200000); assert(wd_elapsed_ms(&e,101000)==61000);
    wd_elapsed_text(wd_elapsed_ms(&e,100000+(24*60+18)*1000ULL+23*60*60*1000ULL-60000),text);
    assert(!strcmp(text,"23:24:18"));
    wd_elapsed_text(1000ULL*100*3600,text); assert(!strcmp(text,"99:59:59"));

    char clock[6];
    wd_clock_text(0,WD_TZ_SYDNEY,clock); assert(!strcmp(clock,"--:--"));
    wd_clock_text(1794117600,WD_TZ_UTC,clock); assert(!strcmp(clock,"06:00"));
    wd_clock_text(1794117600,WD_TZ_SYDNEY,clock); assert(!strcmp(clock,"17:00"));

    char count[5];
    wd_count_text(count,0); assert(!strcmp(count,"0"));
    wd_count_text(count,999); assert(!strcmp(count,"999"));
    wd_count_text(count,1234); assert(!strcmp(count,"1.2k"));
    wd_count_text(count,999999); assert(!strcmp(count,"999k"));
    wd_count_text(count,4294967295U); assert(!strcmp(count,"4.2G"));
    char distance[8];
    wd_distance_text(0,distance); assert(!strcmp(distance,"0m"));
    wd_distance_text(-5,distance); assert(!strcmp(distance,"0m"));
    wd_distance_text(999.4,distance); assert(!strcmp(distance,"999m"));
    wd_distance_text(1800,distance); assert(!strcmp(distance,"1.8km"));
    wd_distance_text(99940,distance); assert(!strcmp(distance,"99.9km"));
    wd_distance_text(123456,distance); assert(!strcmp(distance,"123km"));
    wd_distance_text(9999000,distance); assert(!strcmp(distance,"9999km"));
    wd_distance_text(1e9,distance); assert(strlen(distance)<=6);

    assert(wd_gps_icon(true,true,false)==WD_GPS_ICON_FIX);
    assert(wd_gps_icon(true,false,false)==WD_GPS_ICON_SEARCHING);
    assert(wd_gps_icon(true,false,true)==WD_GPS_ICON_NONE);
    assert(wd_gps_icon(false,false,false)==WD_GPS_ICON_NONE);

    /* Menus: spec order, Stop directly below Resume, GPS third while active. */
    wd_menu_item_t items[8];
    static const char *const idle[]={"Start Wardrive","Check GPS Status","Saved Sessions","Bulk WiGLE Upload","WiGLE API Settings","WiFi Upload Settings","Additional Settings"};
    static const char *const active[]={"Resume Session","Stop Session","Check GPS Status","Saved Sessions","Bulk WiGLE Upload","WiGLE API Settings","WiFi Upload Settings","Additional Settings"};
    assert(wd_main_menu(false,items)==7);
    for(unsigned i=0;i<7;i++) { assert(!strcmp(wd_main_menu_label(items[i],false),idle[i])); assert(strlen(idle[i])<=20); }
    assert(wd_main_menu(true,items)==8);
    for(unsigned i=0;i<8;i++) assert(!strcmp(wd_main_menu_label(items[i],true),active[i]));

    /* Wrap: words kept whole, paragraphs separated, nothing past 21 columns. */
    char lines[24][WD_UI_COLUMNS+1];
    unsigned n=wd_wrap("Repairs a session that didn't close cleanly.\n\nContinue?",21,lines,24);
    assert(n==5&&!strcmp(lines[0],"Repairs a session")&&!strcmp(lines[1],"that didn't close")&&!strcmp(lines[2],"cleanly."));
    assert(!lines[3][0]&&!strcmp(lines[4],"Continue?"));
    /* Windowed wrap: same lines at an offset; NULL just counts. */
    char window[2][WD_UI_COLUMNS+1];
    assert(wd_wrap_from("Repairs a session that didn't close cleanly.\n\nContinue?",21,0,NULL,0)==5);
    assert(wd_wrap_from("Repairs a session that didn't close cleanly.\n\nContinue?",21,2,window,2)==5&&!strcmp(window[0],"cleanly.")&&!window[1][0]);
        n=wd_wrap("abcdefghijklmnopqrstuvwxyz0123456789",21,lines,24);
    assert(n==2&&strlen(lines[0])==21&&!strcmp(lines[1],"vwxyz0123456789"));
    n=wd_wrap("one two",21,lines,1); assert(n==1&&!strcmp(lines[0],"one two"));
    n=wd_wrap("aaaa bbbb cccc",9,lines,1); assert(n==2&&!strcmp(lines[0],"aaaa bbbb"));

    /* Settings: limits wrap both ways; scan type WiFi+BLE -> WiFi -> BLE. */
    wd_settings_t s; wd_settings_default(&s); wd_timezone_config_t zone={.automatic=1};
    s.mode=WD_COMBINED; wd_settings_adjust(&s,0,1); assert(s.mode==WD_WIFI);
    wd_settings_adjust(&s,0,1); assert(s.mode==WD_BLE); wd_settings_adjust(&s,0,1); assert(s.mode==WD_COMBINED);
    wd_settings_adjust(&s,0,-1); assert(s.mode==WD_BLE);
    s.wifi_interval_ms=30000; wd_settings_adjust(&s,1,1); assert(s.wifi_interval_ms==1000);
    wd_settings_adjust(&s,1,-1); assert(s.wifi_interval_ms==30000);
    s.observation_interval_ms=60000; wd_settings_adjust(&s,2,1); assert(s.observation_interval_ms==1000);
    s.rssi_threshold=-30; wd_settings_adjust(&s,3,1); assert(s.rssi_threshold==-110);
    wd_settings_adjust(&s,3,-1); assert(s.rssi_threshold==-30); wd_settings_adjust(&s,3,-1); assert(s.rssi_threshold==-35);
    s.ble_window_ms=1000; wd_settings_adjust(&s,4,-1); assert(s.ble_window_ms==10000);
    wd_settings_t before=s; wd_settings_adjust(&s,5,1); assert(!memcmp(&before,&s,sizeof(s)));
    assert(wd_settings_valid(&s));
    char row[WD_UI_COLUMNS+1];
    s.mode=WD_COMBINED; wd_settings_row_text(&s,&zone,0,row); assert(!strcmp(row,"Scan Type WiFi + BLE"));
    s.rssi_threshold=-110; wd_settings_row_text(&s,&zone,3,row); assert(!strcmp(row,"RSSI -110dBm"));
    wd_settings_row_text(&s,&zone,5,row); assert(!strcmp(row,"Time Zone Auto"));
    for(unsigned r=0;r<WD_SETTINGS_ROWS;r++) { wd_settings_row_text(&s,&zone,r,row); assert(strlen(row)<=20); }

    /* Ring: distinct, adjacent, radius 6. */
    for(unsigned i=0;i<wd_ring_point_count;i++) {
        wd_point_t p=wd_ring_points[i],q=wd_ring_points[(i+1)%wd_ring_point_count];
        assert(p.x>=-6&&p.x<=6&&p.y>=-6&&p.y<=6);
        assert((p.x-q.x)*(p.x-q.x)<=1&&(p.y-q.y)*(p.y-q.y)<=1);
        for(unsigned j=i+1;j<wd_ring_point_count;j++) assert(p.x!=wd_ring_points[j].x||p.y!=wd_ring_points[j].y);
    }
    assert(wd_glyph_bluetooth[2]==0xFF);
    puts("UI logic: holds fire once/cancel/reset, paused elapsed time, clock/count/distance text, menus, wrap, settings limits and glyphs passed");
}
