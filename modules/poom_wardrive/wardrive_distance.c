// SPDX-License-Identifier: MIT
#include "wardrive_distance.h"
#include <math.h>

void wd_distance_break(wd_distance_t *d) {
    d->anchored=false; d->samples=0;
}
static void anchor(wd_distance_t *d,const poom_gps_fix_t *f) {
    d->anchor_latitude=f->latitude; d->anchor_longitude=f->longitude;
    d->first_ms=d->last_ms=f->position_ms; d->samples=1; d->anchored=true;
}
static double separation(double lat,double lon,double other_lat,double other_lon) {
    const double radians=0.0174532925199433;
    double a=(lat-other_lat)*radians,b=(lon-other_lon)*radians;
    double h=sin(a/2)*sin(a/2)+cos(lat*radians)*cos(other_lat*radians)*sin(b/2)*sin(b/2);
    return 6371000*2*asin(sqrt(fmin(1,h)));
}
void wd_distance_update(wd_distance_t *d,const poom_gps_fix_t *fix,uint64_t now) {
    poom_gps_fix_t f=poom_gps_fix_at(fix,now);
    unsigned needed=GPS_POSITION|GPS_SPEED|GPS_HDOP;
    /* A fresh position alone can drift considerably indoors. Require useful
     * geometry and receiver-reported motion; HDOP remains dimensionless. */
    if(!f.connected||!f.fix||(f.fields&needed)!=needed||
       !isfinite(f.latitude)||!isfinite(f.longitude)||fabs(f.latitude)>90||fabs(f.longitude)>180||
       !isfinite(f.speed_kmh)||f.speed_kmh<3||f.speed_kmh>252||
       !isfinite(f.hdop)||f.hdop<=0||f.hdop>2.5||
       ((f.fields&GPS_TYPE)&&f.fix_type!=3)) {
        wd_distance_break(d); return;
    }
    if(d->anchored&&f.position_ms==d->last_ms) return;
    if(!d->anchored||f.position_ms<d->last_ms||f.position_ms-d->last_ms>2500) {
        anchor(d,&f); return;
    }
    d->last_ms=f.position_ms; d->samples++;
    double elapsed=(f.position_ms-d->first_ms)/1000.;
    double metres=separation(f.latitude,f.longitude,d->anchor_latitude,d->anchor_longitude);
    /* Reject jumps inconsistent with reported motion. Confirm five fresh
     * samples and 15 m displacement before adding a chord rather than every
     * jitter step. This undercounts short walks, turns and weak fixes. */
    if(metres>f.speed_kmh/3.6*elapsed*2+5) { anchor(d,&f); return; }
    if(d->samples>=5&&metres>=15) { d->metres+=metres; anchor(d,&f); }
}
