// SPDX-License-Identifier: MIT
#include "poom_wardrive.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
int main(int argc,char **argv) {
    assert(argc==2); const char *root=argv[1];
    wd_settings_t settings; wd_settings_default(&settings);
    assert(wd_settings_valid(&settings)); assert(!wd_safe_id("../secret")); assert(!wd_safe_id("a%2fb"));
    wd_session_t s;
    assert(wd_session_start(&s,root,"synthetic",&settings));
    /* The on-device start path immediately updates its initial manifest. */
    assert(wd_session_checkpoint(&s,"Active"));
    assert(wd_session_checkpoint(&s,"Active"));
    wd_observation_t o={0}; o.kind=WD_WIFI; o.address[0]=0x02; o.rssi=-60;
    o.channel=36; o.frequency=5180; o.security=3;
    o.name_length=6; memcpy(o.name,"A,\"B\nC",6);
    o.monotonic_ms=1000;
    o.gps=(poom_gps_fix_t){.fields=GPS_POSITION|GPS_TIME|GPS_DATE|GPS_HDOP|GPS_ALTITUDE,.connected=true,.fix=true,
        .latitude=-33.86,.longitude=151.2,.year=2026,.month=11,.day=7,.hour=12,.hdop=0.9,.altitude_m=23.4};
    assert(wd_gps_epoch(&o.gps)==1794052800);
    assert(wd_session_observe(&s,&o)); assert(s.wifi_count==1&&s.observations==1);
    o.monotonic_ms=2000; assert(wd_session_observe(&s,&o)); assert(s.observations==1);
    o.rssi=-50; assert(wd_session_observe(&s,&o)); assert(s.observations==2);
    o.monotonic_ms=12000; assert(wd_session_observe(&s,&o)); assert(s.observations==3);
    o.kind=WD_BLE; o.address_type=1; o.adv_length=4; memcpy(o.adv,"\x03\xff\x4c\x00",4);
    assert(wd_session_observe(&s,&o)); assert(s.ble_count==1);
    o.address_type=0; assert(wd_session_observe(&s,&o)); assert(s.ble_count==2);
    o.gps.fields=GPS_HDOP; o.address[5]=1; assert(wd_session_observe(&s,&o)); assert(s.unlocated==1);
    for(unsigned i=0;i<600;i++) {
        o.kind=WD_WIFI; o.address_type=0; o.address[4]=i>>8; o.address[5]=i; o.monotonic_ms=20000+i;
        assert(wd_session_observe(&s,&o));
    }
    assert(s.wifi_count==600); /* one of 600 overlaps original BSSID */
    assert(wd_session_gps(&s,&o.gps,30000));
    assert(wd_session_checkpoint(&s,"Paused"));
    assert(wd_session_stop(&s));
    char dir[192],out[224]; snprintf(dir,sizeof(dir),"%s/sessions/synthetic",root);
    snprintf(out,sizeof(out),"%s/wigle.csv",dir); uint32_t count,skipped;
    assert(wd_export(dir,out,&count,&skipped)); assert(count==5&&skipped==600);
    assert(wd_archive(dir));
    assert(wd_archive(dir)); /* replacing a previous archive also uses FAT semantics */
    {
        char journal[224],hidden[224],archive[224]; uint8_t before[512],after[512];
        snprintf(journal,sizeof(journal),"%s/observations.wd",dir);
        snprintf(hidden,sizeof(hidden),"%s/observations.hidden",dir);
        snprintf(archive,sizeof(archive),"%s/session.tar",dir);
        FILE *file=fopen(archive,"rb");assert(file&&fread(before,1,sizeof(before),file)==sizeof(before));fclose(file);
        assert(!rename(journal,hidden));assert(!wd_archive(dir));assert(!rename(hidden,journal));
        file=fopen(archive,"rb");assert(file&&fread(after,1,sizeof(after),file)==sizeof(after));fclose(file);
        assert(!memcmp(before,after,sizeof(before))); /* missing journal cannot publish an empty success */
    }
    snprintf(out,sizeof(out),"%s/observations.wd",dir);
    FILE *f=fopen(out,"ab"); assert(f); assert(fwrite("partial",1,7,f)==7); fclose(f);
    assert(wd_session_recover(dir));
    snprintf(out,sizeof(out),"%s/recovery.json",dir); f=fopen(out,"r"); assert(f);
    char report[256]; assert(fgets(report,sizeof(report),f)); fclose(f);
    assert(strstr(report,"\"tail_damaged\":true"));
    snprintf(out,sizeof(out),"%s/wigle.csv",dir);
    assert(wd_export(dir,out,&count,&skipped)&&count==5);
    snprintf(out,sizeof(out),"%s/export.json",dir); f=fopen(out,"r"); assert(f);
    assert(fgets(report,sizeof(report),f)); fclose(f); assert(strstr(report,"\"tail_damaged\":true"));
    wd_observation_t decoded; uint8_t b[WD_RECORD_BYTES];
    wd_encode(&o,b); assert(wd_decode(b,&decoded)); assert(decoded.rssi==o.rssi);
    b[75]^=1; assert(!wd_decode(b,&decoded));
    /* CSV currency: needs CSV + report; invalidation keeps raw/upload data. */
    assert(wd_export_ready(dir));
    uint32_t reported=0; assert(wd_export_rows(dir,&reported)&&reported==5);
    char scratch[160]; snprintf(scratch,sizeof(scratch),"%.140s/csv-state",argv[1]); assert(!mkdir(scratch,0700));
    static const char *const files[]={"wigle.csv","export.json","session.tar","observations.wd","upload.json","session.json","recovery.json"};
    for(unsigned i=0;i<7;i++) {
        snprintf(out,sizeof(out),"%s/%s",scratch,files[i]); f=fopen(out,"w"); assert(f&&fputs("x",f)>=0&&!fclose(f));
        if(i==0) assert(!wd_export_ready(scratch));
    }
    assert(wd_export_ready(scratch)&&!wd_export_rows(scratch,&reported)&&!reported); /* "x" isn't a report */
    assert(wd_export_invalidate(scratch)&&!wd_export_ready(scratch));
    for(unsigned i=0;i<7;i++) { struct stat st; snprintf(out,sizeof(out),"%s/%s",scratch,files[i]); assert((stat(out,&st)==0)==(i>=3)); }
    assert(wd_export_invalidate(scratch)); /* idempotent */
    printf("Storage/dedup/export/recovery tests passed; session=%zu observation=%zu bytes (host)\n",sizeof(s),sizeof(o));
}
