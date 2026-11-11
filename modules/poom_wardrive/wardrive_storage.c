// SPDX-License-Identifier: MIT
#include "wardrive_files.h"
#include "poom_wardrive.h"
#include "wardrive_timezone.h"
#include "wardrive_clock.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
/* Derived disk hash index. 256 32-bit bucket heads; 64-byte linked nodes.
 * Never authoritative: reconstruct from CRC-validated observation journal. */
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(i*8)); }
static uint64_t get64(const uint8_t *p) { return get32(p)|((uint64_t)get32(p+4)<<32); }
static void put64(uint8_t *p,uint64_t n) { put32(p,(uint32_t)n); put32(p+4,n>>32); }
static bool sync_file(FILE *f) { return fflush(f)==0 && fsync(fileno(f))==0; }
static bool path(char *out,size_t cap,const char *dir,const char *file) {
    int n=snprintf(out,cap,"%s/%s",dir,file); return n>0 && (size_t)n<cap;
}
static void format_utc(char out[21],int64_t utc) {
    time_t t=(time_t)utc;struct tm tm;out[0]=0;
    if(wd_clock_valid(utc)&&gmtime_r(&t,&tm)) strftime(out,21,"%Y-%m-%d %H:%M:%S",&tm);
}
void wd_session_time(wd_session_t *s,int64_t utc,uint64_t now_ms) {
    if(!s||!wd_clock_valid(utc)||now_ms<s->started_ms) return;
    s->clock_utc=utc;s->clock_ms=now_ms;
    if(!s->start_utc[0]) format_utc(s->start_utc,utc-(int64_t)((now_ms-s->started_ms)/1000));
}
static void gps_time(wd_session_t *s,const poom_gps_fix_t *fix,uint64_t now_ms) {
    wd_clock_t clock={0};
    if(wd_clock_update_gps(&clock,fix,now_ms)) wd_session_time(s,clock.utc,now_ms);
}
static bool mkdir_ok(const char *p) { return mkdir(p,0700)==0 || errno==EEXIST; }
static bool at(FILE *f,long off,void *b,size_t n,bool write) {
    return fseek(f,off,SEEK_SET)==0 && (write?fwrite(b,1,n,f):fread(b,1,n,f))==n;
}
/* Return node offset or -1 on storage/corruption error. */
static long index_node(wd_session_t *s,const wd_observation_t *o,uint8_t node[64],bool *fresh) {
    uint8_t key[8]={o->kind,o->address_type}; memcpy(key+2,o->address,6);
    uint32_t hash=2166136261U;
    for(unsigned i=0;i<8;i++) hash=(hash^key[i])*16777619U;
    long bucket=(hash%WD_INDEX_BUCKETS)*4; uint8_t head[4];
    if(!at(s->index,bucket,head,4,false)) return -1;
    uint32_t off=get32(head); unsigned steps=0;
    while(off) {
        if(off<1024 || (off-1024)%64 || steps++>s->wifi_count+s->ble_count || !at(s->index,off,node,64,false)) return -1;
        if(!memcmp(key,node+4,8)) { *fresh=false; return off; }
        off=get32(node);
    }
    if(fseek(s->index,0,SEEK_END)) return -1;
    long end=ftell(s->index);
    if(end<1024 || end>0x7fffffbf) return -1;
    memset(node,0,64); memcpy(node,head,4); memcpy(node+4,key,8);
    put64(node+12,o->monotonic_ms); put64(node+28,o->monotonic_ms);
    put32(node+44,(uint32_t)(int32_t)o->rssi);
    if(!at(s->index,end,node,64,true)) return -1;
    put32(head,(uint32_t)end);
    if(!at(s->index,bucket,head,4,true)) return -1;
    *fresh=true; return end;
}
static bool index_open(wd_session_t *s) {
    char p[192]; if(!path(p,sizeof(p),s->directory,"devices.idx")) return false;
    s->index=fopen(p,"w+b"); if(!s->index) return false;
    uint8_t zero[64]={0};
    for(unsigned i=0;i<16;i++) if(fwrite(zero,1,64,s->index)!=64) return false;
    return true;
}
bool wd_session_checkpoint(wd_session_t *s,const char *state) {
    char p[192],tmp[192];
    if(!path(p,sizeof(p),s->directory,"session.json")||!path(tmp,sizeof(tmp),s->directory,"session.tmp")) return false;
    /* Only two FAT file slots needed: journal/index released by caller for
     * checkpoints? index closed/reopened here, keeping journal held. */
    if(s->index) { if(!sync_file(s->index)) { printf("[wardrive] SD checkpoint sync-index errno=%d\n",errno); return false; } fclose(s->index); s->index=NULL; }
    FILE *f=fopen(tmp,"w"); bool ok=false;
    if(f) {
        ok=fprintf(f,"{\"version\":1,\"state\":\"%s\",\"receiver\":\"%.23s\",\"mode\":%u,\"wifi\":%lu,\"ble\":%lu,\"observations\":%lu,\"unlocated\":%lu,\"write_errors\":%lu,\"wifi_interval_ms\":%u,\"ble_window_ms\":%u,\"observation_interval_ms\":%u,\"rssi_threshold\":%d,\"gps_samples\":%lu,\"start_ms\":%llu,\"end_ms\":%llu,\"start_utc\":\"%s\",\"end_utc\":\"%s\",\"display_timezone\":\"%s\",\"display_timezone_source\":\"%s\",\"upload_status_file\":\"upload.json\",\"export_status_file\":\"export.json\"}\n",
            state,s->receiver,(unsigned)s->settings.mode,(unsigned long)s->wifi_count,(unsigned long)s->ble_count,
            (unsigned long)s->observations,(unsigned long)s->unlocated,(unsigned long)s->write_errors,
            s->settings.wifi_interval_ms,s->settings.ble_window_ms,s->settings.observation_interval_ms,s->settings.rssi_threshold,(unsigned long)s->gps_count,(unsigned long long)s->started_ms,(unsigned long long)s->ended_ms,s->start_utc,s->end_utc,wd_timezone_id((wd_timezone_t)s->display_zone),wd_timezone_source((wd_timezone_source_t)s->display_source))>0;
        if(!sync_file(f)) ok=false;
        if(fclose(f)) ok=false;
        if(ok && !wd_file_publish(tmp,p)) ok=false;
    }
    char idx[192];
    if(path(idx,sizeof(idx),s->directory,"devices.idx")) s->index=fopen(idx,"r+b");
    if(!ok||!s->index) printf("[wardrive] SD checkpoint failed state=%s index=%u errno=%d\n",state,s->index!=NULL,errno);
    return ok && s->index;
}
bool wd_session_start(wd_session_t *s,const char *root,const char *id,const wd_settings_t *settings) {
    if(!wd_safe_id(id)||!wd_settings_valid(settings)) return false;
    memset(s,0,sizeof(*s)); s->settings=*settings;
    if(!mkdir_ok(root)) return false;
    char sessions[192];
    if(!path(sessions,sizeof(sessions),root,"sessions")||!mkdir_ok(sessions)||
       !path(s->directory,sizeof(s->directory),sessions,id)||mkdir(s->directory,0700)) return false;
    char p[192];
    if(!index_open(s)||!path(p,sizeof(p),s->directory,"observations.wd")) goto fail;
    s->journal=fopen(p,"wb"); if(!s->journal) goto fail;
    s->active=true;
    if(!wd_session_checkpoint(s,"Active")) goto fail;
    return true;
fail:
    if(s->journal) fclose(s->journal);
    if(s->index) fclose(s->index);
    s->journal=NULL; s->index=NULL; s->active=false; s->fault=true; return false;
}
bool wd_session_observe(wd_session_t *s,const wd_observation_t *o) {
    if(!s->active||s->paused||s->fault||o->kind>1) return false;
    gps_time(s,&o->gps,o->monotonic_ms);
    uint8_t node[64]; bool fresh;
    long off=index_node(s,o,node,&fresh); if(off<0) goto error;
    if(fresh) { if(o->kind==WD_WIFI) s->wifi_count++; else s->ble_count++; }
    put64(node+20,o->monotonic_ms);
    uint32_t count=get32(node+36); put32(node+36,count+1);
    int strongest=(int32_t)get32(node+44);
    if(o->rssi>strongest) put32(node+44,(uint32_t)(int32_t)o->rssi);
    bool exportable=wd_observation_exportable(o);
    /* Retain the first geographically exportable report after acquisition or
     * reacquisition even within the normal repeat interval. Track processed
     * eligibility, including suppressed no-fix reports, rather than backfilling
     * the location of an earlier discovery. Node +40 was reserved/zero. */
    bool gained_location=exportable&&!get32(node+40);
    put32(node+40,exportable?1:0);
    /* Periodic history and meaningful signal improvement. Strongest position
     * isn't a router location; raw observations retain their sampling point. */
    bool log=fresh || gained_location || o->monotonic_ms-get64(node+28)>=s->settings.observation_interval_ms || o->rssi>=strongest+6;
    if(log) {
        uint8_t b[WD_RECORD_BYTES]; wd_encode(o,b);
        if(fwrite(b,1,sizeof(b),s->journal)!=sizeof(b) || !sync_file(s->journal)) goto error;
        put64(node+28,o->monotonic_ms); s->observations++;
        if(!o->gps.connected||!o->gps.fix||!(o->gps.fields&GPS_POSITION)) s->unlocated++;
    }
    if(!at(s->index,off,node,64,true)||fflush(s->index)) goto error;
    return true;
error:
    s->fault=true; s->write_errors++; return false;
}
bool wd_session_stop(wd_session_t *s) {
    bool ok=!s->fault;
    int64_t end=s->clock_utc;
    if(s->ended_ms>=s->clock_ms) end+=(int64_t)((s->ended_ms-s->clock_ms)/1000);
    format_utc(s->end_utc,end);
    if(s->journal && !sync_file(s->journal)) ok=false;
    if(!wd_session_checkpoint(s,ok?"Completed":"NeedsRecovery")) ok=false;
    if(s->journal && fclose(s->journal)) ok=false;
    if(s->index && fclose(s->index)) ok=false;
    s->journal=NULL; s->index=NULL; s->active=false; return ok;
}
bool wd_session_recover(const char *directory) {
    wd_session_t s={0}; wd_settings_default(&s.settings);
    if(strlen(directory)>=sizeof(s.directory)) return false;
    strcpy(s.directory,directory);
    char p[192]; if(!path(p,sizeof(p),directory,"observations.wd")) return false;
    FILE *f=fopen(p,"rb"); if(!f) return false;
    if(!index_open(&s)) { if(s.index) fclose(s.index); fclose(f); return false; }
    uint8_t b[WD_RECORD_BYTES],node[64]; wd_observation_t o;
    bool ok=true; size_t n;
    while((n=fread(b,1,sizeof(b),f))==sizeof(b)) {
        if(!wd_decode(b,&o)) { ok=false; break; }
        if(o.kind==2) { s.gps_count++; continue; }
        bool fresh; long off=index_node(&s,&o,node,&fresh);
        if(off<0) { ok=false; break; }
        if(fresh) { if(o.kind==WD_WIFI) s.wifi_count++; else s.ble_count++; }
        put64(node+20,o.monotonic_ms); put64(node+28,o.monotonic_ms);
        put32(node+36,get32(node+36)+1);
        put32(node+40,wd_observation_exportable(&o)?1:0);
        if(o.rssi>(int32_t)get32(node+44)) put32(node+44,(uint32_t)(int32_t)o.rssi);
        if(!at(s.index,off,node,64,true)) { ok=false; break; }
        s.observations++; if(!o.gps.connected||!o.gps.fix||!(o.gps.fields&GPS_POSITION)) s.unlocated++;
    }
    if(n || ferror(f)) ok=false;
    fclose(f);
    /* Separate recovery report: preserve original settings/manifest and queue. */
    char report[192]; path(report,sizeof(report),directory,"recovery.json");
    if(!sync_file(s.index)) ok=false;
    fclose(s.index);
    FILE *r=fopen(report,"w");
    if(!r) return false;
    bool saved=fprintf(r,"{\"valid_observations\":%lu,\"wifi\":%lu,\"ble\":%lu,\"tail_damaged\":%s}\n",
        (unsigned long)s.observations,(unsigned long)s.wifi_count,(unsigned long)s.ble_count,ok?"false":"true")>0;
    if(!sync_file(r)) saved=false;
    if(fclose(r)) saved=false;
    return saved;
}

bool wd_session_gps(wd_session_t *s,const poom_gps_fix_t *f,uint64_t now) {
    if(!s->active||s->paused||s->fault) return false;
    gps_time(s,f,now);
    wd_observation_t o={0}; o.kind=2; o.gps=*f; o.monotonic_ms=now;
    uint8_t b[WD_RECORD_BYTES]; wd_encode(&o,b);
    if(fwrite(b,1,sizeof(b),s->journal)!=sizeof(b)||!sync_file(s->journal)) {
        s->write_errors++; s->fault=true; return false;
    }
    s->gps_count++; return true;
}
