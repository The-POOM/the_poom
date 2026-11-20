// SPDX-License-Identifier: MIT
#include "wardrive_files.h"
#include "poom_wardrive.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned utf8_length(const uint8_t *s,size_t n) {
    unsigned len=s[0]>=0xc2&&s[0]<=0xdf?2:s[0]>=0xe0&&s[0]<=0xef?3:s[0]>=0xf0&&s[0]<=0xf4?4:0;
    if(!len||len>n) return 0;
    for(unsigned i=1;i<len;i++) if(s[i]<0x80||s[i]>0xbf) return 0;
    if((s[0]==0xe0&&s[1]<0xa0)||(s[0]==0xed&&s[1]>=0xa0)||(s[0]==0xf0&&s[1]<0x90)||(s[0]==0xf4&&s[1]>=0x90)) return 0;
    return len;
}
static bool quoted(FILE *f,const uint8_t *s,size_t n) {
    if(fputc('"',f)==EOF) return false;
    for(size_t i=0;i<n;i++) {
        if(s[i]>=128) {
            unsigned len=utf8_length(s+i,n-i);
            if(len) { if(fwrite(s+i,1,len,f)!=len) return false; i+=len-1; continue; }
        }
        if(s[i]=='"' && fputc('"',f)==EOF) return false;
        if((s[i]<32&&s[i]!='\n'&&s[i]!='\r'&&s[i]!='\t')||s[i]==127||s[i]>=128) {
            if(fprintf(f,"\\x%02X",s[i])<0) return false;
        } else if(fputc(s[i],f)==EOF) return false;
    }
    return fputc('"',f)!=EOF;
}
static const char *security(unsigned n) {
    /* esp_wifi_types_generic.h wifi_auth_mode_t IDF 6.1 */
    switch(n) {
        case 0:return "[ESS]";
        case 1:return "[WEP][ESS]";
        case 2:return "[WPA-PSK][ESS]";
        case 3:return "[WPA2-PSK][ESS]";
        case 4:return "[WPA-PSK][WPA2-PSK][ESS]";
        case 5:return "[WPA2-EAP][ESS]";
        case 6:return "[WPA3-SAE][ESS]";
        case 7:return "[WPA2-PSK][WPA3-SAE][ESS]";
        case 8:return "[WAPI-PSK][ESS]";
        case 9:return "[OWE][ESS]";
        default:return "[UNKNOWN][ESS]";
    }
}
bool wd_export_cancellable(const char *directory,const char *output,uint32_t *exported,uint32_t *skipped,bool (*cancelled)(void *),void *context) {
    if(exported) *exported=0;
    if(skipped) *skipped=0;
    char source[192],tmp[224];
    if(snprintf(source,sizeof(source),"%s/observations.wd",directory)>=(int)sizeof(source)||
       snprintf(tmp,sizeof(tmp),"%s.tmp",output)>=(int)sizeof(tmp)) return false;
    FILE *in=fopen(source,"rb"); if(!in) return false;
    FILE *out=fopen(tmp,"wb"); if(!out) { fclose(in); return false; }
    bool ok=fprintf(out,"WigleWifi-1.6,appRelease=1.0,model=POOM ESP32-C5,release=6.1,device=POOM,display=POOM,board=ESP32-C5,brand=POOM,star=Sol,body=3,subBody=0\nMAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n")>0;
    uint32_t written=0,excluded=0; bool damaged=false;
    uint8_t b[WD_RECORD_BYTES]; wd_observation_t o; size_t n=0;
    while(ok && (n=fread(b,1,sizeof(b),in))==sizeof(b)) {
        if(cancelled&&cancelled(context)) { ok=false; break; }
        if(!wd_decode(b,&o)) { damaged=true; break; }
        if(o.kind==2) continue;
        if(!wd_observation_exportable(&o)) { excluded++; continue; }
        ok=fprintf(out,"%02X:%02X:%02X:%02X:%02X:%02X,",o.address[0],o.address[1],o.address[2],o.address[3],o.address[4],o.address[5])>0;
        if(ok) ok=quoted(out,o.name,o.name_length);
        if(ok) ok=fprintf(out,",%s,%04u-%02u-%02u %02u:%02u:%02u,%u,",o.kind==WD_BLE?"[LE]":security(o.security),o.gps.year,o.gps.month,o.gps.day,o.gps.hour,o.gps.minute,o.gps.second,o.kind==WD_BLE?0:o.channel)>0;
        if(ok&&o.kind==WD_WIFI) ok=fprintf(out,"%u",o.frequency)>0;
        if(ok) ok=fprintf(out,",%d,%.7f,%.7f,",o.rssi,o.gps.latitude,o.gps.longitude)>0;
        if(ok&&(o.gps.fields&GPS_ALTITUDE)) ok=fprintf(out,"%.0f",o.gps.altitude_m)>0;
        /* HDOP is dimensionless, not metres. Unknown accuracy stays empty. */
        if(ok) ok=fprintf(out,",,,")>0;
        if(ok&&o.kind==WD_BLE) {
            for(unsigned i=0;i<o.adv_length;) {
                unsigned len=o.adv[i]; if(!len||i+len>=o.adv_length) break;
                if(o.adv[i+1]==0xff&&len>=3) {
                    ok=fprintf(out,"%u",o.adv[i+2]|(unsigned)o.adv[i+3]<<8)>0; break;
                }
                i+=len+1;
            }
        }
        if(ok) ok=fprintf(out,",%s\n",o.kind==WD_BLE?"BLE":"WIFI")>0;
        if(ok) written++;
    }
    if(n) damaged=true;
    if(ferror(in)) ok=false;
    fclose(in);
    if(fflush(out)||fsync(fileno(out))) ok=false;
    if(fclose(out)) ok=false;
    if(ok&&cancelled&&cancelled(context)) ok=false;
    if(ok && !wd_file_publish(tmp,output)) ok=false;
    if(exported) *exported=written;
    if(skipped) *skipped=excluded;
    if(ok) {
        char report[192];
        if(snprintf(report,sizeof(report),"%s/export.json",directory)>=(int)sizeof(report)) return false;
        FILE *f=fopen(report,"w"); if(!f) return false;
        ok=fprintf(f,"{\"format\":\"WiGLE-1.6\",\"exported\":%lu,\"unlocated_or_untimed\":%lu,\"tail_damaged\":%s}\n",(unsigned long)written,(unsigned long)excluded,damaged?"true":"false")>0;
        if(fflush(f)||fsync(fileno(f))) ok=false;
        if(fclose(f)) ok=false;
    }
    return ok;
}
bool wd_export(const char *directory,const char *output,uint32_t *exported,uint32_t *skipped) {
    return wd_export_cancellable(directory,output,exported,skipped,NULL,NULL);
}
static bool derived_path(char *out,size_t capacity,const char *directory,const char *name) {
    int n=snprintf(out,capacity,"%s/%s",directory,name);
    return n>0&&(size_t)n<capacity;
}
bool wd_export_ready(const char *directory) {
    char path[224]; struct stat st;
    static const char *const names[]={"wigle.csv","export.json"};
    for(unsigned i=0;i<2;i++)
        if(!derived_path(path,sizeof(path),directory,names[i])||stat(path,&st)||!S_ISREG(st.st_mode)) return false;
    return true;
}
bool wd_export_invalidate(const char *directory) {
    static const char *const names[]={"wigle.csv","wigle.csv.bak","export.json","session.tar","session.tar.bak"};
    bool ok=true; char path[224];
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++) {
        if(!derived_path(path,sizeof(path),directory,names[i])) { ok=false; continue; }
        if(unlink(path)&&errno!=ENOENT) ok=false;
    }
    return ok;
}
bool wd_export_rows(const char *directory,uint32_t *rows) {
    char path[224]; *rows=0;
    if(!derived_path(path,sizeof(path),directory,"export.json")) return false;
    FILE *f=fopen(path,"r"); if(!f) return false;
    char text[192]; size_t n=fread(text,1,sizeof(text)-1,f); bool ok=!ferror(f); fclose(f); text[n]=0;
    const char *field=ok?strstr(text,"\"exported\":"):NULL;
    unsigned long value;
    if(!field||sscanf(field+11,"%lu",&value)!=1||value>UINT32_MAX) return false;
    *rows=(uint32_t)value; return true;
}
