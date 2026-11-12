// SPDX-License-Identifier: MIT
/* Synthetic, offline GPS -> observations -> journal -> CSV integration test.
 * No captured addresses/locations, radio transmissions or real uploads. */
#include "poom_wardrive.h"
#include "wardrive_timezone.h"
#include <assert.h>
#include <string.h>
#include <time.h>
/* Reproduce an unset device clock while GPS supplies recording UTC. */
time_t time(time_t *out) { if(out) *out=0;return 0; }

static void sentence(poom_nmea_t *parser,const char *body,uint64_t now) {
    unsigned checksum=0;
    for(const char *p=body;*p;p++) checksum^=(unsigned char)*p;
    char line[128];
    snprintf(line,sizeof(line),"$%s*%02X\r\n",body,checksum);
    for(const char *p=line;*p;p++) poom_nmea_feed(parser,(uint8_t)*p,now);
}

int main(int argc,char **argv) {
    assert(argc==2);
    wd_settings_t settings; wd_settings_default(&settings);
    settings.observation_interval_ms=60000;
    wd_session_t session;
    assert(wd_session_start(&session,argv[1],"gps-transitions",&settings));
    session.started_ms=10000;assert(!session.start_utc[0]&&time(NULL)==0);
    session.display_zone=WD_TZ_SYDNEY; session.display_source=WD_TZ_GPS;
    poom_nmea_t parser; poom_nmea_init(&parser);
    unsigned expected_logged_seconds[]={0,3,13,23},next_logged=0;
    for(unsigned second=0;second<30;second++) {
        uint64_t now=10000+1000*second;
        bool disconnected=second==20||second==21;
        bool fixed=second>=3&&second!=18&&second!=19&&
            !(second>=20&&second<=22);
        bool rmc_missing=second>=8&&second<=12;
        char body[120];
        if(disconnected) {
            /* Actual transport failure clears connection immediately. */
            parser.fix.connected=false;
        } else {
            if(!rmc_missing) {
                snprintf(body,sizeof(body),fixed?
                    "GNRMC,1200%02u,A,4807.038,N,01131.000,E,0,0,081126,,,A":
                    "GNRMC,1200%02u,V,,,,,,,081126,,,N",second);
                sentence(&parser,body,now);
            }
            snprintf(body,sizeof(body),fixed?
                "GNGGA,1200%02u,4807.038,N,01131.000,E,1,08,1.1,20,M,0,M,,":
                "GNGGA,1200%02u,,,,,0,00,,,M,,M,,",second);
            sentence(&parser,body,now);
        }
        poom_gps_fix_t fix=poom_nmea_snapshot(&parser,now);
        if(second>=10&&second<=12) {
            assert(fix.fix&&(fix.fields&GPS_POSITION));
            assert(!(fix.fields&GPS_DATE));
        }
        if(disconnected) assert(!fix.connected&&!fix.fields&&!fix.fix);
        if(second==18||second==19||second==22) assert(!fix.fix);
        assert(wd_session_gps(&session,&fix,now));
        for(unsigned kind=WD_WIFI;kind<=WD_BLE;kind++) {
            wd_observation_t o={.kind=kind,.address={2,0,0,0,0,1},.rssi=-60,
                .channel=6,.frequency=2437,.security=3,.monotonic_ms=now,.gps=fix};
            if(kind==WD_BLE) { o.address_type=1; o.channel=0; o.frequency=0; }
            assert(wd_session_observe(&session,&o));
        }
        if(next_logged<4&&second==expected_logged_seconds[next_logged]) next_logged++;
        assert(session.observations==2*next_logged);
        assert(session.wifi_count==1&&session.ble_count==1);
    }
    assert(session.gps_count==30&&session.unlocated==2&&session.observations==8);
    session.ended_ms=39000;
    assert(wd_session_stop(&session));
    assert(!strcmp(session.start_utc,"2026-11-08 12:00:00")&&!strcmp(session.end_utc,"2026-11-08 12:00:29"));
    char csv[224]; snprintf(csv,sizeof(csv),"%s/wigle.csv",session.directory);
    uint32_t exported,skipped;
    assert(wd_export(session.directory,csv,&exported,&skipped));
    assert(exported==6&&skipped==2);
    assert(wd_session_recover(session.directory));
    /* Rebuilding the disposable index leaves source journal/CSV unchanged. */
    char journal[224]; snprintf(journal,sizeof(journal),"%s/observations.wd",session.directory);
    FILE *f=fopen(journal,"rb"); assert(f);
    uint8_t record[WD_RECORD_BYTES]; wd_observation_t decoded;
    unsigned gps=0,radio=0,located=0;
    while(fread(record,1,sizeof(record),f)==sizeof(record)) {
        assert(wd_decode(record,&decoded));
        if(decoded.kind==2) gps++;
        else { radio++; located+=wd_observation_exportable(&decoded); }
    }
    assert(!ferror(f)); fclose(f);
    assert(gps==30&&radio==8&&located==6);
    /* App time supplied after session start backfills the start using elapsed
     * monotonic time, never the unrelated platform wall clock. */
    wd_session_t late;
    assert(wd_session_start(&late,argv[1],"late-clock",&settings));
    late.started_ms=5000;
    poom_gps_fix_t utc_fix={.fields=GPS_DATE|GPS_TIME,.year=2026,.month=11,.day=11,.hour=11,.minute=12,.second=13};
    wd_session_time(&late,wd_gps_epoch(&utc_fix),15000);
    assert(!strcmp(late.start_utc,"2026-11-11 11:12:03"));
    late.ended_ms=17000;assert(wd_session_stop(&late));
    assert(!strcmp(late.end_utc,"2026-11-11 11:12:15")&&time(NULL)==0);
    char manifest[224];snprintf(manifest,sizeof(manifest),"%s/session.json",late.directory);
    FILE *metadata=fopen(manifest,"rb");assert(metadata);char text[2048]={0};
    assert(fread(text,1,sizeof(text)-1,metadata)>0&&!fclose(metadata));
    assert(strstr(text,"2026-11-11 11:12:03")&&strstr(text,"2026-11-11 11:12:15"));
    puts("GPS/session integration: no fix, UTC expiry, disconnect/reconnect, reacquisition, dedup and export passed");
}
