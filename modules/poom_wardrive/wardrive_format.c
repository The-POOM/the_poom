// SPDX-License-Identifier: MIT
#include "poom_wardrive.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
static void put(uint8_t *p,uint64_t x,unsigned n) { for(unsigned i=0;i<n;i++) p[i]=(uint8_t)(x>>(8*i)); }
static uint64_t get(const uint8_t *p,unsigned n) { uint64_t x=0; for(unsigned i=0;i<n;i++) x|=(uint64_t)p[i]<<(8*i); return x; }
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=0xffffffff;
    for(size_t i=0;i<n;i++) { c^=p[i]; for(unsigned j=0;j<8;j++) c=(c>>1)^(0xedb88320U&-(c&1)); }
    return ~c;
}
void wd_settings_default(wd_settings_t *s) { *s=(wd_settings_t){WD_COMBINED,10000,4000,10000,-100}; }
bool wd_settings_valid(const wd_settings_t *s) {
    return s && s->mode>=WD_WIFI && s->mode<=WD_COMBINED && s->wifi_interval_ms>=1000 && s->wifi_interval_ms<=60000 &&
        s->ble_window_ms>=1000 && s->ble_window_ms<=30000 && s->observation_interval_ms>=1000 &&
        s->observation_interval_ms<=300000 && s->rssi_threshold>=-127 && s->rssi_threshold<=0;
}
bool wd_safe_id(const char *id) {
    size_t n=strlen(id); if(!n||n>48) return false;
    for(size_t i=0;i<n;i++) if(!isalnum((unsigned char)id[i])&&id[i]!='-'&&id[i]!='_') return false;
    return true;
}
void wd_encode(const wd_observation_t *o,uint8_t b[WD_RECORD_BYTES]) {
    memset(b,0,WD_RECORD_BYTES); memcpy(b,"WD01",4);
    b[4]=o->kind; b[5]=o->address_type; memcpy(b+6,o->address,6);
    put(b+12,(uint16_t)o->rssi,2); put(b+14,o->channel,2); put(b+16,o->frequency,2); put(b+18,o->security,2);
    put(b+20,o->monotonic_ms,8);
    uint32_t fields=o->gps.fields;
    if(!o->gps.fix||!o->gps.connected) fields&=~GPS_POSITION;
    put(b+28,fields,4);
    if(fields&GPS_POSITION) {
        put(b+32,(uint32_t)(int32_t)llround(o->gps.latitude*1e7),4);
        put(b+36,(uint32_t)(int32_t)llround(o->gps.longitude*1e7),4);
    }
    if(fields&GPS_ALTITUDE) put(b+40,(uint32_t)(int32_t)llround(o->gps.altitude_m*100),4);
    if(fields&GPS_HDOP) put(b+44,(uint32_t)llround(o->gps.hdop*100),4);
    if(fields&GPS_SPEED) put(b+48,(uint32_t)llround(o->gps.speed_kmh*100),4);
    if(fields&GPS_COURSE) put(b+52,(uint32_t)llround(o->gps.course_deg*100),4);
    put(b+56,o->gps.year,2); b[58]=o->gps.month; b[59]=o->gps.day;
    b[60]=o->gps.hour; b[61]=o->gps.minute; b[62]=o->gps.second;
    b[63]=o->gps.satellites; b[64]=o->gps.quality; b[65]=o->gps.fix_type;
    b[66]=o->name_length<=32?o->name_length:32; memcpy(b+67,o->name,b[66]);
    b[99]=o->adv_length<=31?o->adv_length:31; memcpy(b+100,o->adv,b[99]);
    b[131]=(o->gps.connected?1U:0U)|(o->gps.fix?2U:0U);
    put(b+132,o->gps.position_ms,8); put(b+140,o->gps.sentence_ms,8);
    put(b+188,crc(b,188),4);
}
bool wd_decode(const uint8_t b[WD_RECORD_BYTES],wd_observation_t *o) {
    if(memcmp(b,"WD01",4)||get(b+188,4)!=crc(b,188)||b[4]>2||b[66]>32||b[99]>31) return false;
    memset(o,0,sizeof(*o)); o->kind=b[4]; o->address_type=b[5]; memcpy(o->address,b+6,6);
    o->rssi=(int16_t)get(b+12,2); o->channel=get(b+14,2); o->frequency=get(b+16,2); o->security=get(b+18,2);
    o->monotonic_ms=get(b+20,8); o->gps.fields=get(b+28,4);
    o->gps.latitude=(int32_t)get(b+32,4)/1e7; o->gps.longitude=(int32_t)get(b+36,4)/1e7;
    o->gps.altitude_m=(int32_t)get(b+40,4)/100.; o->gps.hdop=get(b+44,4)/100.;
    o->gps.speed_kmh=get(b+48,4)/100.; o->gps.course_deg=get(b+52,4)/100.;
    o->gps.year=get(b+56,2); o->gps.month=b[58]; o->gps.day=b[59];
    o->gps.hour=b[60]; o->gps.minute=b[61]; o->gps.second=b[62];
    o->gps.satellites=b[63]; o->gps.quality=b[64]; o->gps.fix_type=b[65];
    o->name_length=b[66]; memcpy(o->name,b+67,b[66]); o->adv_length=b[99]; memcpy(o->adv,b+100,b[99]);
    o->gps.position_ms=get(b+132,8); o->gps.sentence_ms=get(b+140,8);
    o->gps.connected=(b[131]&1)!=0; o->gps.fix=(b[131]&2)!=0&&(o->gps.fields&GPS_POSITION)!=0;
    return fabs(o->gps.latitude)<=90 && fabs(o->gps.longitude)<=180;
}

/* Gregorian civil date to Unix UTC seconds, independent of libc time zone. */
int64_t wd_gps_epoch(const poom_gps_fix_t *f) {
    if((f->fields&(GPS_DATE|GPS_TIME))!=(GPS_DATE|GPS_TIME)||f->year<2024||f->year>2099||!f->month||f->month>12||!f->day||f->day>31||f->hour>23||f->minute>59||f->second>59) return -1;
    int y=(int)f->year-(f->month<=2);
    int era=y/400, yoe=y-era*400;
    int m=(int)f->month+(f->month>2?-3:9);
    int doy=(153*m+2)/5+(int)f->day-1;
    int64_t days=(int64_t)era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
    return days*86400+f->hour*3600+f->minute*60+f->second;
}

bool wd_observation_exportable(const wd_observation_t *o) {
    const unsigned required=GPS_POSITION|GPS_DATE|GPS_TIME;
    return o->kind<=WD_BLE && o->gps.connected && o->gps.fix &&
        (o->gps.fields&required)==required && wd_gps_epoch(&o->gps)>=0;
}
