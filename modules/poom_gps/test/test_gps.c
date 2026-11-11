// SPDX-License-Identifier: MIT
#include "poom_gps.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static char line[128];
static void make(const char *body) {
    unsigned c=0; for(const char *s=body;*s;s++) c^=(unsigned char)*s;
    snprintf(line,sizeof(line),"$%s*%02X\r\n",body,c);
}
static void sentence(poom_nmea_t *p,const char *body,unsigned t) {
    make(body); assert(poom_nmea_sentence(p,line,t));
}
static bool online=true; static unsigned opens;
static bool fake_open(void *c) { (void)c; opens++; return online; }
static int fake_read(void *c,uint8_t *o,size_t n) { (void)c;(void)o;(void)n; return online?0:-1; }
int main(void) {
    poom_nmea_t p; poom_nmea_init(&p);
    sentence(&p,"GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,071126,,,A",100);
    assert(p.fix.fix && fabs(p.fix.latitude-48.1173)<1e-6);
    assert(p.fix.year==2026 && p.fix.month==11 && p.fix.day==7);
    assert(fabs(p.fix.longitude-11.5166667)<1e-6);
    assert(!(poom_nmea_snapshot(&p,2700).fields&GPS_POSITION));
    assert(!poom_nmea_snapshot(&p,5200).connected);
    sentence(&p,"GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,",200);
    assert(p.fix.fields&GPS_ALTITUDE); assert(p.fix.satellites==8);
    sentence(&p,"GNGSA,A,3,04,05,,,,,,,,,,,1.8,1.0,1.5",300);
    assert(p.fix.fix_type==3 && p.fix.hdop==1);
    make("GNRMC,123520,A,4867.038,N,01131.000,E,0,0,071126,,,A");
    assert(!poom_nmea_sentence(&p,line,400));
    line[10]^=1; assert(!poom_nmea_sentence(&p,line,400));
    assert(p.checksum_errors==1);
    sentence(&p,"GNRMC,123520,V,,,,,,,071126,,,N",500);
    assert(!p.fix.fix && !(p.fix.fields&GPS_POSITION));
    sentence(&p,"GNGGA,123521,,,,,0,00,99.9,,M,,M,,",600);
    assert(!p.fix.fix && !(p.fix.fields&GPS_ALTITUDE));
    make("GNRMC,123519,A,4807.038,N,01131.000,E,0,0,310226,,,A");
    assert(!poom_nmea_sentence(&p,line,700));
    make("GNRMC,123519,A,9000.001,N,01131.000,E,0,0,071126,,,A");
    assert(!poom_nmea_sentence(&p,line,700));
    make("GNRMC,123519,A,4807.038,S,01131.000,W,,,071126,,,A");
    for(size_t i=0;i<strlen(line);i++) poom_nmea_feed(&p,(uint8_t)line[i],800);
    assert(p.fix.latitude<0 && p.fix.longitude<0);
    assert(!(p.fix.fields&(GPS_SPEED|GPS_COURSE)));
    poom_nmea_feed(&p,'$',900);
    for(int i=0;i<200;i++) poom_nmea_feed(&p,'a',900);
    assert(p.overflows);
    make("GNRMC,235959,A,4807.038,N,01131.000,E,0,0,071126,,,A");
    assert(poom_nmea_sentence(&p,line,1000));
    sentence(&p,"GNGGA,000000,4807.038,N,01131.000,E,1,08,0.9,1,M,0,M,,",1100);
    assert(!(p.fix.fields&GPS_DATE));
    sentence(&p,"GNRMC,000000,A,4807.038,N,01131.000,E,0,0,081126,,,A",1200);
    assert((p.fix.fields&GPS_DATE)&&p.fix.day==8);
    sentence(&p,"GNGSA,A,3,04,05,,,,,,,,,,,1.8,1.0,1.5",4000);
    assert(!(poom_nmea_snapshot(&p,4000).fields&(GPS_POSITION|GPS_DATE|GPS_TIME)));
    make("GNRMC,000000,A,4807e-1,N,01131.000,E,0,0,081126,,,A");
    assert(!poom_nmea_sentence(&p,line,4100));
    sentence(&p,"GNGGA,000001,4807.038,N,01131.000,E,6,08,0.9,1,M,0,M,,",4200);
    assert(!p.fix.fix&&!(p.fix.fields&GPS_POSITION));
    uint32_t old_framing=p.framing_errors;
    assert(!poom_nmea_sentence(&p,"$GNRMC,123*5",4300));
    assert(p.framing_errors==old_framing+1);
    poom_gps_adapter_t a={"simulated",fake_open,fake_read,NULL,NULL};
    poom_gps_service_t s; poom_gps_service_init(&s,&a);
    poom_gps_service_poll(&s,0); assert(s.opened && opens==1);
    online=false; poom_gps_service_poll(&s,1); assert(!s.opened);
    online=true; poom_gps_service_poll(&s,100); assert(!s.opened);
    poom_gps_service_poll(&s,2001); assert(s.opened && opens==2);
    poom_gps_service_close(&s); assert(!s.parser.fix.connected);
    printf("GPS parser/service tests passed; parser=%zu service=%zu fix=%zu bytes (host)\n",sizeof(p),sizeof(s),sizeof(p.fix));
}
