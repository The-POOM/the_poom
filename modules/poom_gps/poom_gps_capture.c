// SPDX-License-Identifier: MIT
#include "poom_gps_capture.h"
#include "poom_gps.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static bool sync_capture(poom_gps_capture_t *c) {
    if(fflush(c->file)||fsync(fileno(c->file))) { c->fault=true; return false; }
    return true;
}
bool poom_gps_capture_stop(poom_gps_capture_t *c) {
    if(!c->file) return !c->fault;
    if(c->file) {
        sync_capture(c);
        if(fclose(c->file)) c->fault=true;
        c->file=NULL;
    }
    c->complete=!c->fault;
    return !c->fault;
}
bool poom_gps_capture_start(poom_gps_capture_t *c,const char *path,uint64_t now) {
    if(c->file||strlen(path)>=sizeof(c->path)) return false;
    /* Exclusive create: never truncate an earlier field test. */
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0) return false;
    FILE *f=fdopen(fd,"w"); if(!f) { close(fd); return false; }
    memset(c,0,sizeof(*c)); c->file=f; strcpy(c->path,path);
    c->started_ms=now; c->last_sync_ms=now;
    const char *header="# POOM GPS raw capture v1; timestamps are monotonic ms\n# All framed sentences, including unsupported types; coordinates may be sensitive\n";
    size_t n=strlen(header);
    if(fwrite(header,1,n,f)!=n||!sync_capture(c)) { c->fault=true; poom_gps_capture_stop(c); return false; }
    c->bytes=n; return true;
}
bool poom_gps_capture_poll(poom_gps_capture_t *c,uint64_t now) {
    if(!c->file) return !c->fault;
    if(now-c->started_ms>=GPS_CAPTURE_MAX_MS) return poom_gps_capture_stop(c);
    if(now-c->last_sync_ms>=1000) {
        if(!sync_capture(c)) { poom_gps_capture_stop(c); return false; }
        c->last_sync_ms=now;
    }
    return true;
}
bool poom_gps_capture_append(poom_gps_capture_t *c,const char *s,bool parsed,uint64_t now) {
    if(!poom_gps_capture_poll(c,now)||!c->file) return !c->fault;
    size_t length=strnlen(s,128); if(length==128) { c->fault=true; poom_gps_capture_stop(c); return false; }
    char meta[96]; bool checksum=poom_nmea_checksum(s);
    int n=snprintf(meta,sizeof(meta),"# ms=%llu checksum=%s parsed=%s\n",(unsigned long long)now,checksum?"ok":"bad",parsed?"yes":"no");
    if(n<0||(size_t)n>=sizeof(meta)) { c->fault=true; poom_gps_capture_stop(c); return false; }
    if(c->bytes+(size_t)n+length+2>GPS_CAPTURE_MAX_BYTES) return poom_gps_capture_stop(c);
    if(fwrite(meta,1,(size_t)n,c->file)!=(size_t)n||fwrite(s,1,length,c->file)!=length||fputs("\r\n",c->file)<0) {
        c->fault=true; poom_gps_capture_stop(c); return false;
    }
    c->bytes+=(uint32_t)n+length+2; c->sentences++;
    if(!checksum) c->checksum_bad++;
    if(checksum&&length>=7&&!memcmp(s+3,"GSV,",4)) c->gsv++;
    return true;
}
