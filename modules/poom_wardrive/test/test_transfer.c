// SPDX-License-Identifier: MIT
#include "esp_http_server.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int64_t tick;
static int64_t clock_step;
static char content_type[64];
static char issued_cookie[96];
static bool zero_code;
int64_t esp_timer_get_time(void) { int64_t result=tick;tick+=clock_step;return result; }
void esp_fill_random(void *p,size_t n) { memset(p,zero_code&&n==8?0:0xab,n); } /* synthetic only */
esp_err_t httpd_start(httpd_handle_t *s,const httpd_config_t *c) { assert(c->max_open_sockets==2); *s=(void *)1; return ESP_OK; }
esp_err_t httpd_stop(httpd_handle_t s) { (void)s; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t s,const httpd_uri_t *u) { (void)s;(void)u; return ESP_OK; }
static int copy(const char *s,char *out,size_t cap) { if(!s||strlen(s)>=cap) return ESP_FAIL; strcpy(out,s); return ESP_OK; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t cap) { assert(!strcmp(name,"Cookie")); return copy(r->cookie,out,cap); }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r,char *out,size_t cap) { return copy(r->query,out,cap); }
esp_err_t httpd_query_key_value(const char *q,const char *key,char *out,size_t cap) {
    for(const char *p=q;*p;) {
        size_t n=strlen(key);
        if(!strncmp(p,key,n)&&p[n]=='=') { p+=n+1; size_t len=strcspn(p,"&"); if(len>=cap) return ESP_FAIL; memcpy(out,p,len); out[len]=0; return ESP_OK; }
        const char *end=strchr(p,'&'); if(!end) break; p=end+1;
    }
    return ESP_FAIL;
}
int httpd_req_recv(httpd_req_t *r,char *out,size_t cap) { size_t n=strlen(r->body)-r->body_read; if(n>cap) n=cap; if(n>7) n=7; memcpy(out,r->body+r->body_read,n); r->body_read+=n; return n; }
esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *body) { r->status=code; return copy(body,r->response,sizeof(r->response)); }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) { (void)r;if(!strcmp(k,"Set-Cookie"))snprintf(issued_cookie,sizeof(issued_cookie),"%s",v);return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *s) { r->status=atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *s) { (void)r;snprintf(content_type,sizeof(content_type),"%s",s);return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *s,size_t n) { if(r->sent<sizeof(r->response)) { size_t copy=n<sizeof(r->response)-r->sent?n:sizeof(r->response)-r->sent; if(copy) memcpy(r->response+r->sent,s,copy); } r->sent+=n; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *r,const char *s,int n) { return httpd_resp_send_chunk(r,s,n<0?strlen(s):(size_t)n); }
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r,const char *s) { return httpd_resp_send_chunk(r,s,strlen(s)); }
#include "../wardrive_transfer.c"
int main(void) {
    assert(mkdir(WD_ROOT "/sessions/tz-legacy",0700)==0);
    FILE *metadata=fopen(WD_ROOT "/sessions/tz-legacy/session.json","w"); assert(metadata);
    fputs("{\"start_utc\":\"2026-11-08 06:56:43\"}",metadata); fclose(metadata);
    wd_transfer_set_timezone(WD_TZ_SYDNEY);
    char token[33]; assert(wd_transfer_start(token)); assert(strlen(token)==10&&strspn(token,"0123456789")==10);
    httpd_req_t r={0}; assert(!auth(&r)); assert(index_page(&r)==ESP_OK); assert(strstr(r.response,"does not encrypt"));
    r=(httpd_req_t){.query="id=synthetic&file=observations.wd"}; download(&r); assert(r.status==401);
    char body[39]; snprintf(body,sizeof(body),"token=%s",token);
    r=(httpd_req_t){.content_len=16,.body=body}; assert(login(&r)==ESP_OK); assert(r.status==303);
    char cookie[64]; snprintf(cookie,sizeof(cookie),"other=1; %.35s; xyz=2",issued_cookie);
    r=(httpd_req_t){.cookie=cookie}; assert(auth(&r));
    char guessed_cookie[48];snprintf(guessed_cookie,sizeof(guessed_cookie),"wd=%s",token);
    httpd_req_t guessed={.cookie=guessed_cookie};assert(!auth(&guessed)); /* code cannot bypass throttled login */
    r.query="id=..&file=observations.wd"; download(&r); assert(r.status==400);
    r.query="id=synthetic&file=wardrive-setup.json"; download(&r); assert(r.status==403);
    r.query="id=synthetic&file=observations.wd"; r.sent=0; download(&r); assert(r.sent>192);
    assert(gps_filename("gps-01234567-89abcdef.nmea"));
    assert(!gps_filename("../wardrive-setup.json"));
    assert(!gps_filename("gps-01234567-89abcdef.json"));
    assert(mkdir(WD_ROOT "/diagnostics",0700)==0);
    FILE *gps_file=fopen(WD_ROOT "/diagnostics/gps-01234567-89abcdef.nmea","w");
    assert(gps_file); fputs("synthetic GPS capture",gps_file); fclose(gps_file);
    r=(httpd_req_t){.query="id=gps-01234567-89abcdef.nmea"}; gps_download(&r); assert(r.status==401);
    r=(httpd_req_t){.cookie=cookie,.query="id=../config/wardrive-setup.json"}; gps_download(&r); assert(r.status==400);
    r=(httpd_req_t){.cookie=cookie,.query="id=gps-01234567-89abcdef.nmea"}; gps_download(&r);
    assert(r.sent==21&&!memcmp(r.response,"synthetic GPS capture",21));
    r=(httpd_req_t){.cookie=cookie}; index_page(&r);
    assert(strstr(r.response,"/gps?id=gps-01234567-89abcdef.nmea"));
    assert(strstr(r.response,"2026-11-08 17:56:43 AEDT (UTC+11:00)"));
    assert(strstr(r.response,"UTC start: 2026-11-08 06:56:43"));
    assert(strstr(r.response,"Session bundle (.tar)"));
    for(unsigned i=0;i<5;i++) {
        char path[192];snprintf(path,sizeof(path),WD_ROOT "/sessions/order-%u",i);assert(!mkdir(path,0700));
        snprintf(path,sizeof(path),WD_ROOT "/sessions/order-%u/session.json",i);FILE *file=fopen(path,"w");assert(file);
        fprintf(file,"{\"start_utc\":\"2040-01-%02u 01:02:03\",\"state\":\"Completed\",\"observations\":1}",i+1);fclose(file);
    }
    r=(httpd_req_t){.cookie=cookie};assert(index_page(&r)==ESP_OK);
    const char *four=strstr(r.response,"Session ID: order-4"),*three=strstr(r.response,"Session ID: order-3"),*two=strstr(r.response,"Session ID: order-2"),*one=strstr(r.response,"Session ID: order-1");
    assert(four&&three&&two&&one&&four<three&&three<two&&two<one);
    assert(!strstr(r.response,"Session ID: order-0")&&strstr(r.response,"/?after=order-1"));
    r=(httpd_req_t){.cookie=cookie,.query="after=order-1"};assert(index_page(&r)==ESP_OK);
    assert(strstr(r.response,"Session ID: order-0")&&!strstr(r.response,"Session ID: order-4"));
    r=(httpd_req_t){.cookie=cookie,.query="after=../config"};index_page(&r);assert(r.status==400);
    assert(!mkdir(WD_ROOT "/sessions/empty-incomplete",0700));
    FILE *file=fopen(WD_ROOT "/sessions/empty-incomplete/session.json","w");assert(file);fputs("{\"state\":\"Active\",\"observations\":0,\"start_utc\":\"\"}",file);fclose(file);
    file=fopen(WD_ROOT "/sessions/empty-incomplete/observations.wd","wb");assert(file);fclose(file);
    r=(httpd_req_t){.cookie=cookie,.query="after=tz-legacy"};assert(index_page(&r)==ESP_OK&&strstr(r.response,"Empty recording:"));
    wd_observation_t observation={.kind=WD_WIFI,.address={2,0,0,0,0,1},.rssi=-50,.channel=6,.frequency=2437,
        .gps={.connected=true,.fix=true,.fields=GPS_POSITION|GPS_DATE|GPS_TIME,.latitude=1,.longitude=2,.year=2040,.month=1,.day=5,.hour=1,.minute=2,.second=3}};
    uint8_t record[WD_RECORD_BYTES];wd_encode(&observation,record);
    file=fopen(WD_ROOT "/sessions/order-4/observations.wd","wb");assert(file&&fwrite(record,1,sizeof(record),file)==sizeof(record));fclose(file);
    r=(httpd_req_t){.query="id=order-4&file=wigle.csv"};download(&r);assert(r.status==401);
    assert(access(WD_ROOT "/sessions/order-4/wigle.csv",F_OK));
    r=(httpd_req_t){.cookie=cookie,.query="id=order-4&file=wigle.csv"};assert(download(&r)==ESP_OK);
    uint64_t future_deadline=now()+1000;atomic_store(&stopping,true);assert(export_cancelled(&future_deadline));atomic_store(&stopping,false);
    assert(strstr(r.response,"WigleWifi-1.6")&&strstr(r.response,"2040-01-05 01:02:03"));
    assert(!strcmp(content_type,"text/csv; charset=utf-8"));
    assert(!unlink(WD_ROOT "/sessions/order-4/wigle.csv"));clock_step=20000000;
    r=(httpd_req_t){.cookie=cookie,.query="id=order-4&file=wigle.csv"};download(&r);
    assert(r.status==500&&access(WD_ROOT "/sessions/order-4/wigle.csv",F_OK));clock_step=0;tick=0;
    r=(httpd_req_t){.cookie=cookie,.query="id=order-4&file=wigle.csv"};assert(download(&r)==ESP_OK);
    for(unsigned i=0;i<5;i++) {
        httpd_req_t bad={.content_len=16,.body="token=0000000000"};
        login(&bad); assert(bad.status==401);
    }
    r=(httpd_req_t){.content_len=16,.body=body}; login(&r); assert(r.status==401);
    tick=60000001; r=(httpd_req_t){.content_len=16,.body=body}; login(&r); assert(r.status==303);
    tick=600000001; assert(wd_transfer_expired()); assert(!auth(&r));
    wd_transfer_stop(); assert(!secret[0]&&!cookie_secret[0]&&!server);
    zero_code=true;tick=0;assert(wd_transfer_start(token)&&!strcmp(token,"0000000000"));
    r=(httpd_req_t){.content_len=16,.body="token=0000000000"};assert(login(&r)==ESP_OK&&r.status==303);wd_transfer_stop();
    puts("Simulated transfer authentication, path restrictions, stream and expiry tests passed");
}
