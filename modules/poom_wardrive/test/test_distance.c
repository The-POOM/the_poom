// SPDX-License-Identifier: MIT
#include "wardrive_distance.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static poom_gps_fix_t fix(unsigned second,double north_metres,double speed,double hdop) {
    uint64_t now=10000+1000*second;
    return (poom_gps_fix_t){.fields=GPS_POSITION|GPS_SPEED|GPS_HDOP|GPS_TYPE,
        .fix=true,.connected=true,.latitude=48+north_metres/111194.92664456,
        .longitude=11,.speed_kmh=speed,.hdop=hdop,.fix_type=3,
        .position_ms=now,.sentence_ms=now,.date_ms=now};
}
static void update(wd_distance_t *d,poom_gps_fix_t f) { wd_distance_update(d,&f,f.position_ms); }
int main(void) {
    wd_distance_t d={0};
    /* Drift could add hundreds of metres with the old step-summing logic. */
    for(unsigned i=0;i<600;i++) update(&d,fix(i,i%60,0.4,1.2));
    assert(d.metres==0);
    for(unsigned i=0;i<600;i++) update(&d,fix(i,i%60,5,4));
    assert(d.metres==0);
    for(unsigned i=0;i<600;i++) update(&d,fix(i,(i%2)*2,4,1.2));
    assert(d.metres==0);
    d=(wd_distance_t){0};
    for(unsigned i=0;i<=120;i++) update(&d,fix(i,i,3.6,1.2));
    assert(d.metres>=104&&d.metres<=120.1);
    double before=d.metres;
    poom_gps_fix_t f=fix(121,121,3.6,1.2);
    f.fix=false; update(&d,f);
    for(unsigned i=122;i<126;i++) update(&d,fix(i,1000,3.6,1.2));
    assert(d.metres==before); /* no bridge over lost fix */
    wd_distance_break(&d);
    for(unsigned i=126;i<130;i++) update(&d,fix(i,2000,3.6,1.2));
    assert(d.metres==before); /* pause/restart also breaks continuity */
    d=(wd_distance_t){0};
    for(unsigned i=0;i<=20;i++) update(&d,fix(i,20*i,72,1.2));
    assert(fabs(d.metres-400)<0.01);
    before=d.metres;
    f=fix(20,400,72,1.2);
    for(unsigned i=0;i<100;i++) update(&d,f);
    assert(d.metres==before);
    f=fix(21,420,72,1.2); f.hdop=NAN; update(&d,f);
    assert(!d.anchored&&d.metres==before);
    f=fix(22,440,72,1.2); f.fields&=~GPS_SPEED; update(&d,f);
    assert(!d.anchored&&d.metres==before);
    f=fix(23,460,72,1.2); f.fix_type=2; update(&d,f);
    assert(!d.anchored&&d.metres==before);
    f=fix(24,480,72,1.2); wd_distance_update(&d,&f,f.position_ms+3000);
    assert(!d.anchored&&d.metres==before);
    d=(wd_distance_t){0};
    update(&d,fix(0,0,4,1));
    for(unsigned i=1;i<5;i++) update(&d,fix(i,500+i,4,1));
    assert(d.metres==0);
    puts("Distance estimate: stationary drift, quality/speed gates, movement, gaps, pauses, duplicates and jumps passed");
}
