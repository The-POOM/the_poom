// SPDX-License-Identifier: MIT
#include "poom_gps.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *s, double low, double high, double *v)
{
    if (!*s || isspace((unsigned char)*s)) return false;
    const char *digits=s;
    if(*digits=='-') digits++;
    bool digit=false,point=false;
    for(;*digits;digits++) {
        if(*digits=='.'&&!point) point=true;
        else if(*digits>='0'&&*digits<='9') digit=true;
        else return false;
    }
    if(!digit) return false;
    char *end;
    double x = strtod(s, &end);
    if (*end || !isfinite(x) || x < low || x > high) return false;
    *v=x; return true;
}
static bool integer(const char *s, unsigned lo, unsigned hi, unsigned *v)
{
    double x;
    if (!number(s, lo, hi, &x) || floor(x)!=x) return false;
    *v=(unsigned)x; return true;
}
static bool clock_time(const char *s, poom_gps_fix_t *f)
{
    double x;
    if (strcspn(s,".")!=6 || !number(s,0,235959.999,&x)) return false;
    unsigned t=(unsigned)x;
    f->hour=t/10000; f->minute=t/100%100; f->second=t%100;
    return f->hour<24 && f->minute<60 && f->second<60;
}
static bool date(const char *s, poom_gps_fix_t *f)
{
    unsigned x;
    if (strlen(s)!=6 || !integer(s,10100,311299,&x)) return false;
    f->day=x/10000; f->month=x/100%100;
    f->year=2000+x%100; /* receiver epoch policy: 2000..2099 */
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if (!f->month || f->month>12 || !f->day) return false;
    unsigned max=days[f->month-1]+(f->month==2 && f->year%4==0);
    return f->day<=max;
}
static bool coordinate(const char *s, const char *hemi, bool lat, double *v)
{
    double n;
    if (strcspn(s,".")!=(lat?4U:5U) || strlen(hemi)!=1 || !number(s,0,lat?9000:18000,&n)) return false;
    double deg=floor(n/100), min=n-deg*100;
    if (min>=60 || deg+min/60>(lat?90:180)) return false;
    if (!(lat?(*hemi=='N'||*hemi=='S'):(*hemi=='E'||*hemi=='W'))) return false;
    *v=(deg+min/60)*((*hemi=='S'||*hemi=='W')?-1:1); return true;
}
static int hex(char c)
{
    if(c>='0'&&c<='9') return c-'0';
    if(c>='A'&&c<='F') return c-'A'+10;
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
/* -1 malformed, 0 checksum mismatch, 1 valid framing/checksum. This is
 * deliberately independent of support for any particular sentence type. */
static int checksum_status(const char *line) {
    size_t len=strlen(line);
    if(len<10||len>=128||*line!='$') return -1;
    const char *star=strchr(line,'*');
    if(!star||strlen(star)<3||hex(star[1])<0||hex(star[2])<0) return -1;
    for(const char *t=star+3;*t;t++) if(*t!='\r'&&*t!='\n') return -1;
    unsigned sum=0; for(const char *t=line+1;t<star;t++) sum^=(unsigned char)*t;
    return sum==(unsigned)(hex(star[1])*16+hex(star[2]));
}
bool poom_nmea_checksum(const char *line) { return checksum_status(line)==1; }
void poom_nmea_init(poom_nmea_t *p) { memset(p,0,sizeof(*p)); }
bool poom_nmea_sentence(poom_nmea_t *p, const char *line, uint64_t now)
{
    int checksum=checksum_status(line);
    if(checksum!=1) { if(checksum==0) p->checksum_errors++; else p->framing_errors++; return false; }
    const char *star=strchr(line,'*');
    char buf[128]; memcpy(buf,line+1,(size_t)(star-line-1)); buf[star-line-1]=0;
    char *v[24]; unsigned n=1; v[0]=buf;
    for(char *t=buf;*t;++t) if(*t==',') { *t=0; if(n==24) return false; v[n++]=t+1; }
    if(strlen(v[0])!=5) return false;
    poom_gps_fix_t f=p->fix;
    bool position=false;
    if(!strcmp(v[0]+2,"RMC")) {
        if(n<10 || !clock_time(v[1],&f) || !date(v[9],&f) ||
           (strcmp(v[2],"A") && strcmp(v[2],"V"))) return false;
        f.fields|=GPS_TIME|GPS_DATE; f.time_ms=now; f.date_ms=now;
        f.fields&=~(GPS_POSITION|GPS_SPEED|GPS_COURSE);
        f.fix=!strcmp(v[2],"A") && !(n>12 && !strcmp(v[12],"N"));
        if(f.fix) {
            if(!coordinate(v[3],v[4],true,&f.latitude)||!coordinate(v[5],v[6],false,&f.longitude)) return false;
            position=true;
        }
        double x;
        if(number(v[7],0,2000,&x)) { f.speed_kmh=x*1.852; f.fields|=GPS_SPEED; }
        if(number(v[8],0,360,&f.course_deg)) f.fields|=GPS_COURSE;
    } else if(!strcmp(v[0]+2,"GGA")) {
        if(n<15 || !clock_time(v[1],&f) || !integer(v[6],0,8,&f.quality)) return false;
        /* GGA has no date. Do not carry yesterday's RMC date across midnight. */
        if(f.hour<p->fix.hour) f.fields&=~GPS_DATE;
        f.fields|=GPS_TIME|GPS_QUALITY; f.time_ms=now;
        f.fields&=~(GPS_POSITION|GPS_SATELLITES|GPS_HDOP|GPS_ALTITUDE);
        /* quality 6 (dead reckoning), 7/8 (manual/simulated) aren't GNSS fixes */
        f.fix=f.quality>=1 && f.quality<=5;
        if(f.fix) {
            if(!coordinate(v[2],v[3],true,&f.latitude)||!coordinate(v[4],v[5],false,&f.longitude)) return false;
            position=true;
        }
        if(integer(v[7],0,99,&f.satellites)) f.fields|=GPS_SATELLITES;
        if(number(v[8],0,999,&f.hdop)) f.fields|=GPS_HDOP;
        if(!strcmp(v[10],"M") && number(v[9],-20000,100000,&f.altitude_m)) f.fields|=GPS_ALTITUDE;
    } else if(!strcmp(v[0]+2,"GSA")) {
        if(n<18 || !integer(v[2],1,3,&f.fix_type)) return false;
        f.fields|=GPS_TYPE;
        if(f.fix_type==1) { f.fix=false; f.fields&=~GPS_POSITION; }
        f.fields&=~GPS_HDOP;
        if(number(v[16],0,999,&f.hdop)) f.fields|=GPS_HDOP;
    } else return false;
    if(position) { f.fields|=GPS_POSITION; f.position_ms=now; }
    f.connected=true; f.updated_ms=now; f.sentence_ms=now;
    p->fix=f; p->valid_sentences++; return true;
}
bool poom_nmea_feed(poom_nmea_t *p,uint8_t b,uint64_t now)
{
    if(b=='$') p->length=0;
    else if(!p->length) return false;
    /* XA1110 idle filler is LF. Sentences terminate at CR, so LF is optional. */
    if(b=='\r'||b=='\n') {
        p->sentence[p->length]=0;
        bool ok=poom_nmea_sentence(p,p->sentence,now); p->length=0; return ok;
    }
    if(b<32||b>126||p->length>=sizeof(p->sentence)-1) { p->length=0; p->overflows++; return false; }
    p->sentence[p->length++]=(char)b; return false;
}
static bool expired(uint64_t stamp,uint64_t now,uint64_t limit)
{
    return stamp>now || now-stamp>limit;
}
poom_gps_fix_t poom_gps_fix_at(const poom_gps_fix_t *fix,uint64_t now)
{
    poom_gps_fix_t f=*fix;
    if(!f.connected || expired(f.sentence_ms,now,5000)) {
        f.connected=false; f.fields=0; f.fix=false;
    } else if(!f.fix || expired(f.position_ms,now,2500)) { f.fields&=~GPS_POSITION; f.fix=false; }
    /* Date, speed and course all originate in RMC. Fresh GGA positions must
     * not indefinitely keep an old RMC motion estimate alive. */
    if(expired(f.date_ms,now,2500)) f.fields&=~(GPS_DATE|GPS_SPEED|GPS_COURSE);
    if(expired(f.time_ms,now,2500)) f.fields&=~GPS_TIME;
    return f;
}
poom_gps_fix_t poom_nmea_snapshot(const poom_nmea_t *p,uint64_t now)
{
    return poom_gps_fix_at(&p->fix,now);
}
