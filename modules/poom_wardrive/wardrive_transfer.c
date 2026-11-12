// SPDX-License-Identifier: MIT
#include "wardrive_connect.h"
#include "wardrive_sessions.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdatomic.h>
static httpd_handle_t server;
static wd_timezone_t display_zone;
static atomic_bool stopping;
static char secret[33];
static char cookie_secret[33];
static uint64_t expires,locked_until;
static unsigned failures;
static uint64_t now(void) { return (uint64_t)esp_timer_get_time()/1000; }
bool wd_transfer_expired(void) { return server&&now()>=expires; }
static bool equal(const char *a,const char *b,size_t n) { unsigned diff=0; for(size_t i=0;i<n;i++) diff|=(unsigned char)a[i]^(unsigned char)b[i]; return !diff; }
static bool auth(httpd_req_t *r) {
    if(now()>=expires) return false;
    char cookie[192]={0};
    if(httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie))!=ESP_OK) return false;
    char *p=cookie;
    while(*p) {
        while(*p==' '||*p==';') p++;
        char *end=strchr(p,';'); size_t n=end?(size_t)(end-p):strlen(p);
        if(n==35&&!memcmp(p,"wd=",3)&&equal(p+3,cookie_secret,32)) return true;
        if(!end) break;
        p=end+1;
    }
    return false;
}
static esp_err_t deny(httpd_req_t *r) { return httpd_resp_send_err(r,HTTPD_401_UNAUTHORIZED,"Open / and enter the current 10-digit POOM code. Repeated failures lock login for one minute."); }
static esp_err_t login(httpd_req_t *r) {
    const size_t length=6+WD_TRANSFER_CODE_DIGITS;
    if(now()>=expires||now()<locked_until||r->content_len!=(int)length) return deny(r);
    char data[39]={0}; size_t got=0;
    while(got<length) { int n=httpd_req_recv(r,data+got,length-got); if(n<=0) return ESP_FAIL; got+=(size_t)n; }
    if(memcmp(data,"token=",6)||!equal(data+6,secret,WD_TRANSFER_CODE_DIGITS)) {
        if(++failures>=5) { locked_until=now()+60000; failures=0; }
        return deny(r);
    }
    char cookie[96]; snprintf(cookie,sizeof(cookie),"wd=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=600",cookie_secret);
    httpd_resp_set_hdr(r,"Set-Cookie",cookie); httpd_resp_set_status(r,"303 See Other"); httpd_resp_set_hdr(r,"Location","/");
    return httpd_resp_send(r,"",0);
}
static bool gps_filename(const char *s) {
    if(strlen(s)!=26||memcmp(s,"gps-",4)||s[12]!='-'||strcmp(s+21,".nmea")) return false;
    for(unsigned i=4;i<21;i++) {
        if(i==12) continue;
        if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false;
    }
    return true;
}
static esp_err_t index_page(httpd_req_t *r) {
    httpd_resp_set_type(r,"text/html; charset=utf-8"); httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'none'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    if(!auth(r)) return httpd_resp_send(r,"<!doctype html><title>POOM Transfer</title><h1>POOM Transfer over WiFi</h1><p>HTTP does not encrypt access codes or location data. Use a trusted private network.</p><form method=post action=/login><label>10-digit code on POOM <input name=token inputmode=numeric pattern=\"[0-9]{10}\" minlength=10 maxlength=10 required autocomplete=off></label><button>Open sessions</button></form>",HTTPD_RESP_USE_STRLEN);
    char cursor[49]={0},query[96];
    if(httpd_req_get_url_query_str(r,query,sizeof(query))==ESP_OK) {
        if(httpd_query_key_value(query,"after",cursor,sizeof(cursor))!=ESP_OK||!wd_safe_id(cursor))
            return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid session page");
    }
    wd_session_page_t *page=calloc(1,sizeof(*page));
    if(!page) return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Not enough memory");
    if(!wd_session_page_local(WD_ROOT "/sessions",cursor,cursor[0]?1:0,display_zone,page)) {
        free(page); return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Session list unavailable");
    }
    esp_err_t result=ESP_OK;
    if(httpd_resp_sendstr_chunk(r,"<!doctype html><title>POOM Sessions</title><h1>Download sessions</h1><p>Newest dated sessions first; unknown dates last. Choose <b>Download WiGLE CSV</b> for WiGLE. Other files are optional diagnostics.</p><p>HTTP is unencrypted. Access expires after ten minutes.</p>")!=ESP_OK) { free(page); return ESP_FAIL; }
    unsigned shown=0;
    for(unsigned i=0;i<4&&page->entries[i].id[0];i++) {
        wd_session_info_t *info=&page->entries[i]; char chunk[512],local[64],path[192]; struct stat st;
        bool timed=wd_session_time_text(info,local,sizeof(local));
        const char *status=!strcmp(info->state,"Active")||!strcmp(info->state,"Paused")?"Unfinished recording":info->state;
        snprintf(chunk,sizeof(chunk),"<section><h2>%s</h2><p>%s; UTC start: %s<br>Session ID: %s</p>",timed?local:info->label,status,info->utc[0]?info->utc:"unknown",info->id);
        if((result=httpd_resp_sendstr_chunk(r,chunk))!=ESP_OK) break;
        snprintf(path,sizeof(path),WD_ROOT "/sessions/%s/observations.wd",info->id);
        bool raw=!stat(path,&st)&&S_ISREG(st.st_mode),empty=raw&&st.st_size==0;
        snprintf(path,sizeof(path),WD_ROOT "/sessions/%s/wigle.csv",info->id);
        bool csv=!stat(path,&st)&&S_ISREG(st.st_mode);
        if(empty&&!csv) result=httpd_resp_sendstr_chunk(r,"<p><b>Empty recording:</b> no observations were saved. There is no WiGLE CSV to download.</p>");
        else if(raw||csv) {
            snprintf(chunk,sizeof(chunk),"<p><a href=\"/download?id=%s&amp;file=wigle.csv\"><b>Download WiGLE CSV (.csv)</b></a>%s</p>",info->id,csv?"":" (prepared when downloaded)");
            result=httpd_resp_sendstr_chunk(r,chunk);
        }
        if(result!=ESP_OK) break;
        if(info->count_known) {
            snprintf(chunk,sizeof(chunk),"<p>%lu recorded radio observations. CSV excludes observations without valid GPS/time.</p>",(unsigned long)info->observations);
            if((result=httpd_resp_sendstr_chunk(r,chunk))!=ESP_OK) break;
        }
        snprintf(path,sizeof(path),WD_ROOT "/sessions/%s/session.tar",info->id);
        if(!stat(path,&st)&&S_ISREG(st.st_mode)) {
            snprintf(chunk,sizeof(chunk),"<p><a href=\"/download?id=%s&amp;file=session.tar\">Session bundle (.tar)</a> &mdash; optional snapshot; choose Transfer over WiFi &gt; Raw data + WiGLE CSV on POOM to refresh it.</p>",info->id);
            if((result=httpd_resp_sendstr_chunk(r,chunk))!=ESP_OK) break;
        }
        if((result=httpd_resp_sendstr_chunk(r,"<details><summary>Raw files and reports (diagnostics)</summary>"))!=ESP_OK) break;
        static const char *const files[]={"session.json","observations.wd","recovery.json","upload.json","export.json"};
        static const char *const labels[]={"Session details (.json)","Raw observations (.wd)","Recovery report (.json)","Upload status (.json)","Export report (.json)"};
        for(unsigned f=0;f<5;f++) {
            snprintf(path,sizeof(path),WD_ROOT "/sessions/%s/%s",info->id,files[f]);
            if(stat(path,&st)||!S_ISREG(st.st_mode)) continue;
            snprintf(chunk,sizeof(chunk),"<p><a href=\"/download?id=%s&amp;file=%s\">%s (%lu bytes)</a></p>",info->id,files[f],labels[f],(unsigned long)st.st_size);
            if((result=httpd_resp_sendstr_chunk(r,chunk))!=ESP_OK) break;
        }
        if(result!=ESP_OK||(result=httpd_resp_sendstr_chunk(r,"</details></section>"))!=ESP_OK) break;
        shown++;
    }
    if(result==ESP_OK&&shown&&page->selected_rank+shown<page->count) {
        char link[160]; snprintf(link,sizeof(link),"<p><a href=\"/?after=%s\">Older sessions</a></p>",page->entries[shown-1].id);
        result=httpd_resp_sendstr_chunk(r,link);
    }
    free(page);
    if(result!=ESP_OK) return result;
    if(httpd_resp_sendstr_chunk(r,"<p><a href=\"/\">Newest sessions</a></p><details><summary>GPS diagnostics (.nmea)</summary>")!=ESP_OK) return ESP_FAIL;
    DIR *d=opendir(WD_ROOT "/diagnostics");
    if(d) {
        struct dirent *e;
        while((e=readdir(d))) {
            if(!gps_filename(e->d_name)) continue;
            char link[192]; snprintf(link,sizeof(link),"<p><a href=\"/gps?id=%.26s\">%.26s</a></p>",e->d_name,e->d_name);
            if((result=httpd_resp_sendstr_chunk(r,link))!=ESP_OK) break;
        }
        closedir(d);
    }
    if(result!=ESP_OK) return result;
    if(httpd_resp_sendstr_chunk(r,"</details>")!=ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(r,NULL,0);
}
static esp_err_t stream_file(httpd_req_t *r,const char *path,const char *filename) {
    FILE *f=fopen(path,"rb"); if(!f) return httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"File unavailable");
    char disposition[128]; snprintf(disposition,sizeof(disposition),"attachment; filename=\"%s\"",filename);
    httpd_resp_set_hdr(r,"Content-Disposition",disposition); httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff"); httpd_resp_set_type(r,"application/octet-stream");
    const char *extension=strrchr(filename,'.');
    if(extension&&!strcmp(extension,".csv")) httpd_resp_set_type(r,"text/csv; charset=utf-8");
    char b[512]; size_t n; esp_err_t result=ESP_OK;
    while((n=fread(b,1,sizeof(b),f))) {
        if(now()>=expires||(result=httpd_resp_send_chunk(r,b,n))!=ESP_OK) { result=ESP_FAIL; break; }
    }
    if(ferror(f)) result=ESP_FAIL;
    fclose(f);
    if(result==ESP_OK) result=httpd_resp_send_chunk(r,NULL,0);
    return result;
}
static bool export_cancelled(void *context) {
    uint64_t deadline=*(const uint64_t *)context;
    return atomic_load(&stopping)||now()>=expires||now()>=deadline;
}
static esp_err_t download(httpd_req_t *r) {
    if(!auth(r)) return deny(r);
    char query[160],id[49],file[32];
    if(httpd_req_get_url_query_str(r,query,sizeof(query))!=ESP_OK||
       httpd_query_key_value(query,"id",id,sizeof(id))!=ESP_OK||!wd_safe_id(id)||
       httpd_query_key_value(query,"file",file,sizeof(file))!=ESP_OK) return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid selection");
    static const char *const allowed[]={"session.json","observations.wd","wigle.csv","recovery.json","upload.json","export.json","session.tar"};
    bool permitted=false; for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++) if(!strcmp(file,allowed[i])) permitted=true;
    if(!permitted) return httpd_resp_send_err(r,HTTPD_403_FORBIDDEN,"Not a session download");
    char p[192]; snprintf(p,sizeof(p),WD_ROOT "/sessions/%s/%s",id,file);
    if(!strcmp(file,"wigle.csv")) {
        struct stat st;
        if(stat(p,&st)) {
            char directory[160]; snprintf(directory,sizeof(directory),WD_ROOT "/sessions/%s",id);
            uint32_t rows,skipped; uint64_t deadline=now()+30000;
            if(!wd_export_cancellable(directory,p,&rows,&skipped,export_cancelled,&deadline))
                return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"CSV preparation failed or timed out. Use Saved Sessions > Create WiGLE CSV on POOM, then retry.");
        }
    }
    char filename[96]; snprintf(filename,sizeof(filename),"%s-%s",id,file);
    return stream_file(r,p,filename);
}
static esp_err_t gps_download(httpd_req_t *r) {
    if(!auth(r)) return deny(r);
    char query[96],id[32];
    if(httpd_req_get_url_query_str(r,query,sizeof(query))!=ESP_OK||
       httpd_query_key_value(query,"id",id,sizeof(id))!=ESP_OK||!gps_filename(id))
        return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Invalid GPS capture");
    char path[192]; snprintf(path,sizeof(path),WD_ROOT "/diagnostics/%s",id);
    return stream_file(r,path,id);
}

bool wd_transfer_start(char token[33]) {
    if(server) return false;
    uint64_t number=0,range=10000000000ULL,limit=UINT64_MAX-(UINT64_MAX%10000000000ULL);
    bool generated=false;
    for(unsigned attempt=0;attempt<8;attempt++) {
        esp_fill_random(&number,sizeof(number));
        if(number<limit) { generated=true; break; }
    }
    if(!generated) return false;
    memset(secret,0,sizeof(secret)); snprintf(secret,sizeof(secret),"%010llu",(unsigned long long)(number%range));
    /* The short code is accepted only by the throttled login. Protected routes
     * require a separate 128-bit cookie, never an unthrottled numeric guess. */
    uint8_t random[16]; esp_fill_random(random,sizeof(random));
    for(unsigned i=0;i<16;i++) snprintf(cookie_secret+i*2,3,"%02x",random[i]);
    expires=now()+600000; locked_until=0; failures=0; atomic_store(&stopping,false);
    httpd_config_t c=HTTPD_DEFAULT_CONFIG(); c.max_uri_handlers=4; c.max_open_sockets=2;
    c.stack_size=6144; c.lru_purge_enable=true; c.recv_wait_timeout=3; c.send_wait_timeout=3;
    if(httpd_start(&server,&c)!=ESP_OK) { memset(secret,0,sizeof(secret)); memset(cookie_secret,0,sizeof(cookie_secret)); return false; }
    httpd_uri_t u={.uri="/",.method=HTTP_GET,.handler=index_page};
    bool ok=httpd_register_uri_handler(server,&u)==ESP_OK;
    u.uri="/login"; u.method=HTTP_POST; u.handler=login; ok=ok&&httpd_register_uri_handler(server,&u)==ESP_OK;
    u.uri="/download"; u.method=HTTP_GET; u.handler=download; ok=ok&&httpd_register_uri_handler(server,&u)==ESP_OK;
    u.uri="/gps"; u.handler=gps_download; ok=ok&&httpd_register_uri_handler(server,&u)==ESP_OK;
    if(!ok) { wd_transfer_stop(); return false; }
    memcpy(token,secret,33); return true;
}
void wd_transfer_set_timezone(wd_timezone_t zone) {
    if(!server) display_zone=zone>=0&&zone<WD_TZ_COUNT?zone:WD_TZ_UTC;
}
void wd_transfer_stop(void) { atomic_store(&stopping,true); if(server) { httpd_stop(server); server=NULL; } memset(secret,0,sizeof(secret)); memset(cookie_secret,0,sizeof(cookie_secret)); }
