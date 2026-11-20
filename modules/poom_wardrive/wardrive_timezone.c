// SPDX-License-Identifier: MIT
#include "wardrive_timezone.h"
#include "poom_wardrive.h"
#include <math.h>
#include <string.h>

enum { FIXED, AU, LORD_HOWE, NZ, CHATHAM, EU, US };
typedef struct {
    const char *id,*name,*standard,*summer;
    int16_t minutes;
    uint8_t rule,shift;
} zone_t;
/* Current post-2024 rules, checked 2026-11-08; not historical tzdb. */
static const zone_t zones[WD_TZ_COUNT]={
    {"UTC","UTC","UTC","UTC",0,FIXED,0},
    {"Australia/Sydney","Sydney","AEST","AEDT",600,AU,60},
    {"Australia/Melbourne","Melbourne","AEST","AEDT",600,AU,60},
    {"Australia/Brisbane","Brisbane","AEST","AEST",600,FIXED,0},
    {"Australia/Adelaide","Adelaide","ACST","ACDT",570,AU,60},
    {"Australia/Darwin","Darwin","ACST","ACST",570,FIXED,0},
    {"Australia/Perth","Perth","AWST","AWST",480,FIXED,0},
    {"Australia/Hobart","Hobart","AEST","AEDT",600,AU,60},
    {"Australia/Eucla","Eucla","ACWST","ACWST",525,FIXED,0},
    {"Australia/Lord_Howe","Lord Howe","LHST","LHDT",630,LORD_HOWE,30},
    {"Pacific/Norfolk","Norfolk","NFT","NFDT",660,AU,60},
    {"Pacific/Auckland","Auckland","NZST","NZDT",720,NZ,60},
    {"Pacific/Chatham","Chatham","CHAST","CHADT",765,CHATHAM,60},
    {"Europe/London","London","GMT","BST",0,EU,60},
    {"Europe/Berlin","Central Europe","CET","CEST",60,EU,60},
    {"America/New_York","US Eastern","EST","EDT",-300,US,60},
    {"America/Chicago","US Central","CST","CDT",-360,US,60},
    {"America/Denver","US Mountain","MST","MDT",-420,US,60},
    {"America/Los_Angeles","US Pacific","PST","PDT",-480,US,60},
    {"America/Phoenix","Arizona","MST","MST",-420,FIXED,0},
    {"Pacific/Honolulu","Hawaii","HST","HST",-600,FIXED,0},
    {"Asia/Tokyo","Japan","JST","JST",540,FIXED,0},
    {"Asia/Singapore","Singapore","SGT","SGT",480,FIXED,0},
    {"Asia/Kolkata","India","IST","IST",330,FIXED,0},
    {"Asia/Kathmandu","Nepal","NPT","NPT",345,FIXED,0},
    {"Australia/Broken_Hill","Broken Hill","ACST","ACDT",570,AU,60}
};
typedef struct { double south,north,west,east; wd_timezone_t zone; } core_t;
/* Conservative interior windows, deliberately not country/state polygons.
 * Gaps near borders and unlisted locations require the manual fallback.
 * Units are degrees. No third-party boundary data is embedded. */
static const core_t cores[]={
    {-32.2,-31.7,141.2,141.8,WD_TZ_BROKEN_HILL},
    {-32.4,-31.5,128.0,128.9,WD_TZ_EUCLA},
    {-31.8,-31.3,158.8,159.3,WD_TZ_LORD_HOWE},
    {-29.3,-28.8,167.7,168.2,WD_TZ_NORFOLK},
    {-44.4,-43.4,-177.1,-175.5,WD_TZ_CHATHAM},
    {-47.5,-34.0,166.0,179.0,WD_TZ_AUCKLAND},
    {-43.8,-40.5,144.0,149.0,WD_TZ_HOBART},
    {-38.9,-36.5,143.0,146.5,WD_TZ_MELBOURNE},
    {-35.0,-30.0,145.0,153.5,WD_TZ_SYDNEY},
    {-35.7,-35.0,148.5,150.5,WD_TZ_SYDNEY},
    {-25.5,-10.5,138.5,150.0,WD_TZ_BRISBANE},
    {-28.5,-25.5,141.5,150.0,WD_TZ_BRISBANE},
    {-28.0,-23.0,150.5,154.0,WD_TZ_BRISBANE},
    {-37.5,-26.5,129.5,140.5,WD_TZ_ADELAIDE},
    {-25.5,-11.5,129.5,137.5,WD_TZ_DARWIN},
    {-35.5,-14.0,113.0,128.5,WD_TZ_PERTH}
};
void wd_timezone_default(wd_timezone_config_t *c) { *c=(wd_timezone_config_t){1,1,WD_TZ_UTC,0}; }
bool wd_timezone_config_valid(const wd_timezone_config_t *c) {
    return c&&c->version==1&&c->automatic<=1&&c->manual_zone<WD_TZ_COUNT&&!c->reserved;
}
void wd_timezone_init(wd_timezone_state_t *s,const wd_timezone_config_t *c) {
    wd_timezone_config_t config;
    if(wd_timezone_config_valid(c)) config=*c; else wd_timezone_default(&config);
    memset(s,0,sizeof(*s));
    s->config=config;
    s->zone=(wd_timezone_t)s->config.manual_zone;
    s->source=s->config.automatic?WD_TZ_FALLBACK:WD_TZ_MANUAL;
}
const char *wd_timezone_id(wd_timezone_t z) { return zones[z>=0&&z<WD_TZ_COUNT?z:WD_TZ_UTC].id; }
const char *wd_timezone_name(wd_timezone_t z) { return zones[z>=0&&z<WD_TZ_COUNT?z:WD_TZ_UTC].name; }
const char *wd_timezone_source(wd_timezone_source_t s) {
    static const char *const names[]={"FALLBACK","GPS","MANUAL","LAST GPS"};
    return names[s>=0&&s<=WD_TZ_LAST_GPS?s:WD_TZ_FALLBACK];
}
bool wd_timezone_from_id(const char *id,wd_timezone_t *zone) {
    if(!id||!zone) return false;
    for(unsigned i=0;i<WD_TZ_COUNT;i++) if(!strcmp(id,zones[i].id)) { *zone=(wd_timezone_t)i; return true; }
    return false;
}
bool wd_timezone_detect(double lat,double lon,wd_timezone_t *zone) {
    if(!zone||!isfinite(lat)||!isfinite(lon)||fabs(lat)>90||fabs(lon)>180) return false;
    for(unsigned i=0;i<sizeof(cores)/sizeof(cores[0]);i++) {
        const core_t *r=&cores[i];
        /* Do not extend Perth's rule into the Eucla highway communities. */
        if(r->zone==WD_TZ_PERTH&&lat<-30&&lon>124) continue;
        if(lat>r->south&&lat<r->north&&lon>r->west&&lon<r->east) { *zone=r->zone; return true; }
    }
    return false;
}
void wd_timezone_update(wd_timezone_state_t *s,const poom_gps_fix_t *f,uint64_t now) {
    if(!s->config.automatic) { s->zone=(wd_timezone_t)s->config.manual_zone; s->source=WD_TZ_MANUAL; s->have_gps_zone=false; return; }
    bool fresh=f&&f->connected&&f->fix&&(f->fields&GPS_POSITION)&&f->position_ms<=now&&now-f->position_ms<=2500;
    if(fresh) {
        s->have_gps_zone=wd_timezone_detect(f->latitude,f->longitude,&s->zone);
        if(s->have_gps_zone) { s->source=WD_TZ_GPS; return; }
    } else if(s->have_gps_zone) { s->source=WD_TZ_LAST_GPS; return; }
    s->zone=(wd_timezone_t)s->config.manual_zone; s->source=WD_TZ_FALLBACK;
}
/* nth Sunday, or last Sunday for nth=0; clock minute uses the pre-change offset. */
static int64_t transition(unsigned year,unsigned month,unsigned nth,unsigned minute,int before) {
    poom_gps_fix_t f={.fields=GPS_DATE|GPS_TIME,.year=year,.month=month,.day=1};
    int64_t first=wd_gps_epoch(&f);
    unsigned weekday=(unsigned)((first/86400+4)%7),day;
    if(nth) day=1+(7-weekday)%7+7*(nth-1);
    else {
        unsigned last=month==3||month==10?31:30;
        day=last-(weekday+last-1)%7;
    }
    return first+(day-1)*86400LL+minute*60LL-before*60LL;
}
bool wd_timezone_local(int64_t utc,wd_timezone_t zone,wd_local_time_t *out) {
    if(!out||zone<0||zone>=WD_TZ_COUNT||utc<1704067200||utc>4102444799LL) return false;
    time_t stamp=(time_t)utc; struct tm calendar;
    if(!gmtime_r(&stamp,&calendar)) return false;
    const zone_t *z=&zones[zone]; unsigned year=(unsigned)calendar.tm_year+1900;
    int64_t start=0,end=0; bool southern=false;
    switch(z->rule) {
        case AU: case LORD_HOWE:
            start=transition(year,10,1,120,z->minutes);
            end=transition(year,4,1,z->rule==LORD_HOWE?120:180,z->minutes+z->shift);
            southern=true; break;
        case NZ: case CHATHAM:
            start=transition(year,9,0,z->rule==CHATHAM?165:120,z->minutes);
            end=transition(year,4,1,z->rule==CHATHAM?225:180,z->minutes+z->shift);
            southern=true; break;
        case EU: start=transition(year,3,0,60,0); end=transition(year,10,0,60,0); break;
        case US: start=transition(year,3,2,120,z->minutes); end=transition(year,11,1,120,z->minutes+z->shift); break;
        default: break;
    }
    out->daylight=z->rule!=FIXED&&(southern?(utc>=start||utc<end):(utc>=start&&utc<end));
    out->offset_minutes=z->minutes+(out->daylight?z->shift:0);
    out->abbreviation=out->daylight?z->summer:z->standard;
    stamp=(time_t)(utc+out->offset_minutes*60LL);
    if(!gmtime_r(&stamp,&out->calendar)) return false;
    out->calendar.tm_isdst=out->daylight;
    return true;
}
