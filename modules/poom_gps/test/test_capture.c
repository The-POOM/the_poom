// SPDX-License-Identifier: MIT
#include "poom_gps_capture.h"
#include "poom_gps.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
static void make(char line[128],const char *body) {
    unsigned sum=0; for(const char *p=body;*p;p++) sum^=(unsigned char)*p;
    snprintf(line,128,"$%s*%02X",body,sum);
}
int main(int argc,char **argv) {
    assert(argc==2); poom_gps_capture_t c={0}; char path[192];
    snprintf(path,sizeof(path),"%s/capture.nmea",argv[1]);
    assert(poom_gps_capture_start(&c,path,100));
    char line[128]; make(line,"GPGSV,1,1,01,03,45,180,35");
    assert(poom_nmea_checksum(line));
    poom_nmea_t parser; poom_nmea_init(&parser);
    assert(!poom_nmea_sentence(&parser,line,100)); /* supported framing, unparsed GSV */
    for(unsigned i=0;i<50;i++) assert(poom_gps_capture_append(&c,line,false,100+i));
    assert(c.sentences==50&&c.gsv==50&&!c.checksum_bad); /* no one-Hz sampling */
    assert(poom_gps_capture_append(&c,"$GNVTG,0.00,T,,M,0.00,N,0.00,K,N*2C",false,1000));
    assert(!c.checksum_bad);
    line[7]^=1; assert(!poom_nmea_checksum(line));
    assert(poom_gps_capture_append(&c,line,false,1100)&&c.checksum_bad==1);
    assert(poom_gps_capture_stop(&c)&&c.complete&&!c.file);
    assert(!poom_gps_capture_start(&c,path,1200)); /* no overwrite */
    FILE *f=fopen(path,"r"); assert(f); char all[8192]; size_t n=fread(all,1,sizeof(all)-1,f); all[n]=0; fclose(f);
    assert(strstr(all,"$GPGSV")&&strstr(all,"checksum=ok parsed=no"));
    snprintf(path,sizeof(path),"%s/time-limit.nmea",argv[1]);
    assert(poom_gps_capture_start(&c,path,0));
    assert(poom_gps_capture_poll(&c,GPS_CAPTURE_MAX_MS)&&c.complete&&!c.file);
    snprintf(path,sizeof(path),"%s/size-limit.nmea",argv[1]);
    assert(poom_gps_capture_start(&c,path,0)); c.bytes=GPS_CAPTURE_MAX_BYTES-2;
    assert(poom_gps_capture_append(&c,line,false,10)&&!c.file&&c.complete);
    snprintf(path,sizeof(path),"%s/write-fail.nmea",argv[1]);
    assert(poom_gps_capture_start(&c,path,0)); close(fileno(c.file));
    assert(!poom_gps_capture_poll(&c,2000)&&c.fault&&!c.file);
    puts("Portable GPS capture: GSV/unparsed checksums, unsampled stream, exclusive files, limits, flush/close and write faults passed");
}
