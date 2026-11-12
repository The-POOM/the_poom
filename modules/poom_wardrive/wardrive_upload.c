// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
#include "wardrive_files.h"
#include "wardrive_connect.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls_errors.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
const char *wd_upload_problem_text(wd_upload_problem_t problem) {
    static const char *const names[]={"No upload error","TLS / connect failed","API keys rejected","WiGLE rate limit","CSV / SD read failed","Response needs review","Interrupted / review","DNS lookup failed","TLS date / UTC check","TLS certificate fail","TLS memory exhausted","HTTPS timeout","HTTP header too long"};
    return problem>=WD_UPLOAD_OK&&problem<=WD_UPLOAD_HEADERS?names[problem]:"Unknown upload error";
}
static cJSON *bounded_json(const char *s) {
    unsigned depth=0; bool quoted=false,escaped=false;
    for(const char *p=s;*p;p++) {
        if(quoted) {
            if(escaped) escaped=false;
            else if(*p=='\\') escaped=true;
            else if(*p=='"') quoted=false;
        } else if(*p=='"') quoted=true;
        else if(*p=='{'||*p=='[') { if(++depth>8) return NULL; }
        else if(*p=='}'||*p==']') { if(!depth) return NULL; depth--; }
    }
    if(depth||quoted) return NULL;
    return cJSON_ParseWithLengthOpts(s,strlen(s)+1,NULL,true);
}
static bool status_path(char *p,size_t n,const char *d,const char *name) { int k=snprintf(p,n,"%s/%s",d,name); return k>0&&(size_t)k<n; }
bool wd_upload_status_read(const char *d,wd_upload_status_t *s) {
    memset(s,0,sizeof(*s)); char path[192];
    if(!status_path(path,sizeof(path),d,"upload.json")) return false;
    FILE *f=wd_file_open_read(path); if(!f) return errno==ENOENT;
    char b[512]; size_t n=fread(b,1,sizeof(b)-1,f); bool ok=!ferror(f)&&!(!feof(f)&&n==sizeof(b)-1); fclose(f); b[n]=0;
    cJSON *j=ok?bounded_json(b):NULL; if(!j) return false;
    cJSON *state=cJSON_GetObjectItemCaseSensitive(j,"state"),*attempts=cJSON_GetObjectItemCaseSensitive(j,"attempts");
    cJSON *id=cJSON_GetObjectItemCaseSensitive(j,"transaction"),*retry=cJSON_GetObjectItemCaseSensitive(j,"retry_after");
    ok=cJSON_IsNumber(state)&&state->valueint>=WD_PENDING&&state->valueint<=WD_NEEDS_REVIEW&&state->valuedouble==state->valueint&&cJSON_IsNumber(attempts)&&attempts->valueint>=0&&attempts->valueint<=5&&attempts->valuedouble==attempts->valueint&&cJSON_IsString(id)&&strlen(id->valuestring)<sizeof(s->transaction)&&cJSON_IsNumber(retry)&&isfinite(retry->valuedouble)&&retry->valuedouble>=0&&retry->valuedouble<=4102444800.0&&floor(retry->valuedouble)==retry->valuedouble;
    cJSON *problem=cJSON_GetObjectItemCaseSensitive(j,"problem");
    if(problem&&(!cJSON_IsNumber(problem)||problem->valuedouble!=problem->valueint||problem->valueint<WD_UPLOAD_OK||problem->valueint>WD_UPLOAD_HEADERS)) ok=false;
    static const char *const fields[]={"net_error","tls_error","tls_code","tls_flags","socket_error"};
    int *values[]={&s->net_error,&s->tls_error,&s->tls_code,&s->tls_flags,&s->socket_error};
    for(unsigned i=0;i<5&&ok;i++) {
        cJSON *v=cJSON_GetObjectItemCaseSensitive(j,fields[i]);
        if(v) {
            if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<INT_MIN||v->valuedouble>INT_MAX||floor(v->valuedouble)!=v->valuedouble) ok=false;
            else *values[i]=(int)v->valuedouble;
        }
    }
    if(ok) {
        if(problem) s->problem=problem->valueint;
        s->state=state->valueint; s->attempts=attempts->valueint; strcpy(s->transaction,id->valuestring); s->retry_after=(uint64_t)retry->valuedouble;
        for(char *p=s->transaction;*p;p++) if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='-'||*p=='_')) ok=false;
        cJSON *sent=cJSON_GetObjectItemCaseSensitive(j,"may_have_sent"),*manual=cJSON_GetObjectItemCaseSensitive(j,"manual");
        if((sent&&!cJSON_IsBool(sent))||(manual&&!cJSON_IsBool(manual))) ok=false;
        s->may_have_sent=cJSON_IsTrue(sent); s->manual=cJSON_IsTrue(manual);
        /* Power loss mid-request: WiGLE may hold a copy. Keep the legacy
         * internal state; the UI shows it as Failed and allows manual retry. */
        if(s->state==WD_UPLOADING) { s->state=WD_NEEDS_REVIEW; s->problem=WD_UPLOAD_INTERRUPTED; s->may_have_sent=true; }
    }
    cJSON_Delete(j); return ok;
}
bool wd_upload_status_write(const char *d,const wd_upload_status_t *s) {
    char p[192],tmp[192]; if(!status_path(p,sizeof(p),d,"upload.json")||!status_path(tmp,sizeof(tmp),d,"upload.tmp")) return false;
    cJSON *j=cJSON_CreateObject(); if(!j) return false;
    cJSON_AddNumberToObject(j,"state",s->state); cJSON_AddNumberToObject(j,"problem",s->problem); cJSON_AddNumberToObject(j,"attempts",s->attempts);
    cJSON_AddStringToObject(j,"transaction",s->transaction); cJSON_AddNumberToObject(j,"retry_after",(double)s->retry_after);
    cJSON_AddNumberToObject(j,"net_error",s->net_error);cJSON_AddNumberToObject(j,"tls_error",s->tls_error);
    cJSON_AddNumberToObject(j,"tls_code",s->tls_code);cJSON_AddNumberToObject(j,"tls_flags",s->tls_flags);cJSON_AddNumberToObject(j,"socket_error",s->socket_error);
    if(s->may_have_sent) cJSON_AddBoolToObject(j,"may_have_sent",true);
    if(s->manual) cJSON_AddBoolToObject(j,"manual",true);
    char *data=cJSON_PrintUnformatted(j); cJSON_Delete(j); if(!data) return false;
    FILE *f=fopen(tmp,"w"); bool ok=false;
    if(f) { ok=fputs(data,f)>=0; if(fflush(f)||fsync(fileno(f))) ok=false; if(fclose(f)) ok=false; if(ok&&!wd_file_publish(tmp,p)) ok=false; }
    free(data); return ok;
}
static void network_log(const char *stage,const wd_upload_status_t *s) {
    unsigned long heap=0,largest=0,hp=0,hp_largest=0;
#ifdef ESP_PLATFORM
    heap=esp_get_free_heap_size();largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    hp=heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    hp_largest=heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
#endif
    WD_DEBUG("[wardrive WiGLE] stage=%s utc=%lld net=0x%x tls=0x%x code=%d flags=0x%x socket=%d heap=%lu largest=%lu hp_free=%lu hp_largest=%lu\n",
        stage,(long long)time(NULL),(unsigned)s->net_error,(unsigned)s->tls_error,s->tls_code,(unsigned)s->tls_flags,s->socket_error,heap,largest,hp,hp_largest);
}
static wd_upload_problem_t connect_problem(esp_http_client_handle_t h,esp_err_t error,wd_upload_status_t *s) {
    s->net_error=error;
    if(h) {
        s->socket_error=esp_http_client_get_errno(h);
        s->tls_error=esp_http_client_get_and_clear_last_tls_error(h,&s->tls_code,&s->tls_flags);
    }
    network_log("open_failed",s);
    if(error==ESP_ERR_NO_MEM||s->tls_code==MBEDTLS_ERR_SSL_ALLOC_FAILED||s->tls_code==-MBEDTLS_ERR_SSL_ALLOC_FAILED||s->tls_code==MBEDTLS_ERR_X509_ALLOC_FAILED||s->tls_code==-MBEDTLS_ERR_X509_ALLOC_FAILED) return WD_UPLOAD_MEMORY;
    if(s->tls_error==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) return WD_UPLOAD_DNS;
    if(s->tls_flags&(MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE)) return WD_UPLOAD_CERT_TIME;
    if(s->tls_flags) return WD_UPLOAD_CERT;
    if(error==ESP_ERR_TIMEOUT||s->tls_error==ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT||s->socket_error==ETIMEDOUT) return WD_UPLOAD_TIMEOUT;
#ifdef ESP_ERR_HTTP_HEADER_TOO_LONG
    if(error==ESP_ERR_HTTP_HEADER_TOO_LONG) return WD_UPLOAD_HEADERS;
#endif
    return WD_UPLOAD_CONNECT;
}
/* Report only byte counts and transport status; never log credentials, CSV
 * content, response bodies or WiFi observations. */
static void transfer_trace(const char *stage,size_t done,size_t expected,esp_http_client_handle_t h) {
    unsigned long heap=0,largest=0,hp=0;
#ifdef ESP_PLATFORM
    heap=esp_get_free_heap_size();
    largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    /* HP SRAM: the only memory Wi-Fi buffers and crypto DMA can use. */
    hp=heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
#endif
    WD_DEBUG("[wardrive upload] stage=%s bytes=%lu total=%lu socket=%d heap=%lu largest=%lu hp_free=%lu\n",
        stage,(unsigned long)done,(unsigned long)expected,
        h?esp_http_client_get_errno(h):0,heap,largest,hp);
}
/* Read transport errors immediately after the failed write, before any
 * subsequent HTTP calls can overwrite them. Preserve these in the queue
 * diagnostics, while keeping a potentially transmitted request in review. */
static void failed_write_trace(esp_http_client_handle_t h,const char *stage,
    int result,size_t requested,size_t sent,wd_upload_status_t *status) {
    status->socket_error=esp_http_client_get_errno(h);
    status->tls_error=esp_http_client_get_and_clear_last_tls_error(h,
        &status->tls_code,&status->tls_flags);
    unsigned long dma_free=0,dma_largest=0;
#ifdef ESP_PLATFORM
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT;
    dma_free=heap_caps_get_free_size(caps);
    dma_largest=heap_caps_get_largest_free_block(caps);
#endif
    WD_DEBUG("[wardrive upload] stage=%s return=%d requested=%lu sent=%lu "
        "socket=%d tls=0x%x code=%d flags=0x%x dma_free=%lu dma_largest=%lu\n",
        stage,result,(unsigned long)requested,(unsigned long)sent,
        status->socket_error,(unsigned)status->tls_error,status->tls_code,
        (unsigned)status->tls_flags,dma_free,dma_largest);
    transfer_trace("write_failed_heap",sent,requested,h);
}
/* ESP-IDF v6.1 opens the WiGLE socket in blocking mode with SO_SNDTIMEO and
 * SO_RCVTIMEO equal to timeout_ms. On that socket esp_http_client_write():
 *  - returns 0 when select() sees no TCP send space within timeout_ms. lwIP
 *    stops reporting the socket writable once unacknowledged data leaves less
 *    than TCP_SNDLOWAT (2881 bytes with the default 5760-byte send buffer), so
 *    this happens whenever the peer's ACKs take longer than timeout_ms (e.g.
 *    while TCP retransmission backs off after loss on a mobile hotspot);
 *  - returns MBEDTLS_ERR_SSL_WANT_WRITE/READ when lwIP's SO_SNDTIMEO expired
 *    (EAGAIN). IDF's net_sockets port records no errno/TLS error for this, and
 *    mbedTLS keeps the encrypted record pending: the caller must repeat the
 *    same write with the same buffer and length.
 * Neither is a connection failure; TCP itself aborts a dead connection with a
 * hard error. Previously one 15 s stall therefore discarded a healthy upload
 * after ~2.9 KiB of body and left it in Needs Review. Tolerate a bounded number
 * of consecutive no-progress waits instead. */
#define WD_UPLOAD_STALL_RETRIES 5U
#define WD_UPLOAD_CHUNK 512U
#ifdef CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
/* One application write must map to exactly one TLS record: esp_mbedtls_write
 * then returns either the full length or no progress, so retrying the same
 * slice can never duplicate or skip body bytes. */
_Static_assert(WD_UPLOAD_CHUNK<=CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,"upload chunk must fit one TLS record");
#endif
static bool transient_io(int result) {
    return result==0||result==MBEDTLS_ERR_SSL_WANT_WRITE||result==MBEDTLS_ERR_SSL_WANT_READ;
}
static bool write_all(esp_http_client_handle_t h,const char *p,size_t n,
    bool (*cancelled)(void *),void *context,size_t *written,const char *stage,
    wd_upload_status_t *status) {
    unsigned stalls=0;
    while(n) {
        if(cancelled&&cancelled(context)) {
            transfer_trace("cancelled",*written,*written+n,h);
            return false;
        }
        int k=esp_http_client_write(h,p,(int)n);
        if(k>0&&(size_t)k<=n) {
            stalls=0;
            *written+=(size_t)k;
            p+=k;n-=(size_t)k;
        } else if(transient_io(k)&&stalls<WD_UPLOAD_STALL_RETRIES) {
            stalls++;
            WD_DEBUG("[wardrive upload] stage=write_waiting return=%d wait=%u/%u sent=%lu\n",
                k,stalls,WD_UPLOAD_STALL_RETRIES,(unsigned long)*written);
            /* retry with the same pointer and length, as mbedTLS requires */
        } else {
            failed_write_trace(h,stage,k,n,*written,status);
            return false;
        }
    }
    return true;
}
/* After a failed body write the server may already have answered (e.g. an
 * early 401/403/429 followed by a reset). lwIP keeps received data queued ahead
 * of the reset, so read only the status line with a short timeout. The status
 * feeds the existing classification; anything other than an explicit
 * rejection keeps the session in Needs Review. */
static int early_status(esp_http_client_handle_t h) {
    if(esp_http_client_set_timeout_ms(h,3000)!=ESP_OK) return 0;
    int64_t headers=esp_http_client_fetch_headers(h);
    int code=headers>=0?esp_http_client_get_status_code(h):0;
    WD_DEBUG("[wardrive upload] stage=early_response headers=%lld status=%d\n",(long long)headers,code);
    return code>0?code:0;
}
static cJSON *response(esp_http_client_handle_t h,int *status,bool (*cancelled)(void *),void *context) {
    int64_t headers;
    unsigned waits=0;
    /* WiGLE answers only after storing the body; on a slow link that can take
     * longer than one timeout_ms window. fetch_headers keeps parser state, so
     * EAGAIN can be retried without losing a partially received header. */
    while((headers=esp_http_client_fetch_headers(h))==-ESP_ERR_HTTP_EAGAIN&&waits<WD_UPLOAD_STALL_RETRIES) {
        if(cancelled&&cancelled(context)) { transfer_trace("response_cancelled",0,0,h); return NULL; }
        waits++;
        WD_DEBUG("[wardrive upload] stage=response_waiting wait=%u/%u\n",waits,WD_UPLOAD_STALL_RETRIES);
    }
    if(headers<0) { transfer_trace("response_headers_failed",0,0,h); return NULL; }
    *status=esp_http_client_get_status_code(h);
    WD_DEBUG("[wardrive upload] stage=response_headers status=%d length=%lld\n",*status,(long long)headers);
    /* Heap, not stack: this 4 KiB buffer was the file-job stack peak (672 bytes
     * left). Allocated only after the body is sent, preferring LP SRAM, which
     * is fine because the HTTP client memcpy()s into it (no DMA). */
    const size_t cap=4096;
#ifdef ESP_PLATFORM
    char *b=heap_caps_malloc_prefer(cap,2,MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT,MALLOC_CAP_8BIT);
#else
    char *b=malloc(cap);
#endif
    if(!b) { transfer_trace("response_buffer_failed",0,cap,h); return NULL; }
    cJSON *parsed=NULL; size_t used=0; waits=0;
    while(used<cap-1) {
        if(cancelled&&cancelled(context)) { transfer_trace("response_cancelled",used,0,h); goto done; }
        int n=esp_http_client_read(h,b+used,(int)(cap-1-used));
        if(n==-ESP_ERR_HTTP_EAGAIN&&waits<WD_UPLOAD_STALL_RETRIES) { waits++; continue; }
        if(n<0) { transfer_trace("response_read_failed",used,0,h); goto done; }
        if(!n) break;
        waits=0;
        used+=(size_t)n;
    }
    if(used==cap-1 && !esp_http_client_is_complete_data_received(h)) {
        transfer_trace("response_too_large",used,cap-1,h);
        goto done;
    }
    b[used]=0;
    parsed=bounded_json(b);
    WD_DEBUG("[wardrive upload] stage=response_body bytes=%lu json=%u status=%d\n",
        (unsigned long)used,(unsigned)(parsed!=NULL),*status);
done:
    free(b);
    return parsed;
}
static void report(wd_upload_progress_fn progress,void *context,wd_upload_stage_t stage,size_t sent,size_t total) {
    if(progress) progress(context,stage,sent,total);
}
static bool upload(const char *directory,const wd_credentials_t *c,bool verify,bool (*cancelled)(void *),void *context,wd_upload_progress_fn progress,void *progress_context) {
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    (void)directory;(void)c;(void)verify;(void)cancelled;(void)context;(void)progress;(void)progress_context;
    return false; /* fail closed: no insecure TLS fallback */
#else
    if((cancelled&&cancelled(context))||!c->api_name[0]||!c->api_token[0]||time(NULL)<1704067200) return false;
    wd_upload_status_t s; if(!wd_upload_status_read(directory,&s)) return false;
    if(verify) { if(s.state!=WD_SUBMITTED||!s.transaction[0]) return false; }
    else if((s.state!=WD_PENDING&&s.state!=WD_FAILED&&s.state!=WD_NEEDS_REVIEW)||s.retry_after>(uint64_t)time(NULL)||s.attempts>=5) return false;
    esp_http_client_config_t config={
        .url=verify?"https://api.wigle.net/api/v2/file/transactions?pagestart=0&pageend=100":"https://api.wigle.net/api/v2/file/upload",
        .username=c->api_name,.password=c->api_token,.auth_type=HTTP_AUTH_TYPE_BASIC,
        .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=15000,
        .disable_auto_redirect=true,.buffer_size=512,.buffer_size_tx=512,
    };
    s.net_error=s.tls_error=s.tls_code=s.tls_flags=s.socket_error=0;
    bool ok=false,may_have_sent=false,write_failed=false; int status=0;
    wd_upload_problem_t problem=WD_UPLOAD_FILE;
    esp_http_client_handle_t h=NULL; cJSON *j=NULL; FILE *file=NULL;
    char path[192]={0}; size_t payload=0,total=0;
    const char *head="--poomWardriveBoundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"poom-session.csv\"\r\nContent-Type: text/csv\r\n\r\n";
    const char *tail="\r\n--poomWardriveBoundary--\r\n";
    if(!verify) {
        struct stat st;
        if(!status_path(path,sizeof(path),directory,"wigle.csv")||stat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>180*1024*1024) goto end;
        payload=(size_t)st.st_size; total=strlen(head)+payload+strlen(tail);
        if(total>INT_MAX) goto end;
        /* Commit queue state before allocating HTTP/TLS. No CSV handle or its
         * FAT/stdio cache may split the TLS input-buffer allocation window. */
        s.state=WD_UPLOADING;s.problem=WD_UPLOAD_OK;s.attempts++;s.may_have_sent=false;s.manual=false;
        if(!wd_upload_status_write(directory,&s)) return false;
    }
#ifdef ESP_PLATFORM
    WD_DEBUG("[wardrive TLS config] incoming=%u outgoing=%u dynamic=%u\n",
        (unsigned)CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
        (unsigned)CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
#ifdef CONFIG_MBEDTLS_DYNAMIC_BUFFER
        1U
#else
        0U
#endif
    );
#endif
    report(progress,progress_context,WD_STAGE_CONNECTING,0,total);
    network_log("open_begin",&s);
    h=esp_http_client_init(&config);
    if(!h) { problem=connect_problem(NULL,ESP_ERR_NO_MEM,&s); goto end; }
    if(!verify&&(esp_http_client_set_method(h,HTTP_METHOD_POST)!=ESP_OK||
        esp_http_client_set_header(h,"Content-Type","multipart/form-data; boundary=poomWardriveBoundary")!=ESP_OK)) { goto end; }
    problem=WD_UPLOAD_CONNECT;
    esp_err_t error=esp_http_client_open(h,(int)total);
    if(error!=ESP_OK) { problem=connect_problem(h,error,&s); goto end; }
    network_log("tls_ready",&s);
    if(!verify) {
        if(cancelled&&cancelled(context)) { problem=WD_UPLOAD_INTERRUPTED; transfer_trace("pre_body_cancelled",0,total,h); goto end; }
        /* TLS and certificate checks are complete. Now acquire exactly one
         * source handle, revalidate its size, and stream a bounded body. */
        problem=WD_UPLOAD_FILE;
        transfer_trace("csv_open_begin",0,payload,h);
        file=fopen(path,"rb");
        if(!file) { WD_DEBUG("[wardrive upload] stage=csv_open_failed errno=%d\n",errno); goto end; }
        /* The source is streamed in 512-byte slices. Avoid allocating an
         * additional libc stdio buffer while TLS owns scarce internal RAM.
         * Unbuffered reads trade SD throughput for predictable heap usage. */
        if(setvbuf(file,NULL,_IONBF,0)!=0) {
            WD_DEBUG("[wardrive upload] stage=csv_unbuffered_failed errno=%d\n",errno);
            goto end;
        }
        struct stat current;
        if(fstat(fileno(file),&current)||!S_ISREG(current.st_mode)||(uint64_t)current.st_size!=payload) {
            WD_DEBUG("[wardrive upload] stage=csv_validate_failed errno=%d\n",errno);
            goto end;
        }
        transfer_trace("csv_ready",0,payload,h);
        size_t written=0,body_sent=0;
        may_have_sent=true;problem=WD_UPLOAD_INTERRUPTED;
        if(!write_all(h,head,strlen(head),cancelled,context,&written,"header_write_failed",&s)) { write_failed=true; goto end; }
        transfer_trace("header_sent",written,total,h);
        report(progress,progress_context,WD_STAGE_SENDING,written,total);
        char chunk[WD_UPLOAD_CHUNK];size_t remaining=payload;
        while(remaining) {
            size_t amount=remaining<sizeof(chunk)?remaining:sizeof(chunk);
            size_t n=fread(chunk,1,amount,file);
            if(!n) {
                WD_DEBUG("[wardrive upload] stage=csv_read_failed errno=%d ferror=%d remaining=%lu\n",
                    errno,ferror(file),(unsigned long)remaining);
                goto end;
            }
            if(!write_all(h,chunk,n,cancelled,context,&written,"csv_write_failed",&s)) { write_failed=true; goto end; }
            remaining-=n;body_sent+=n;
            report(progress,progress_context,WD_STAGE_SENDING,written,total);
            if(body_sent/4096!=(body_sent-n)/4096) transfer_trace("csv_progress",body_sent,payload,h);
        }
        int extra=fgetc(file);
        if(extra!=EOF||ferror(file)) { transfer_trace("csv_end_failed",body_sent,payload,h); goto end; }
        transfer_trace("csv_sent",body_sent,payload,h);
        if(!write_all(h,tail,strlen(tail),cancelled,context,&written,"trailer_write_failed",&s)) { write_failed=true; goto end; }
        transfer_trace("trailer_sent",written,total,h);
        report(progress,progress_context,WD_STAGE_AWAITING,written,total);
        int closed=fclose(file);file=NULL;
        if(closed) { WD_DEBUG("[wardrive upload] stage=csv_close_failed errno=%d\n",errno); goto end; }
    }
    problem=WD_UPLOAD_RESPONSE;
    if(verify) report(progress,progress_context,WD_STAGE_AWAITING,0,0);
    j=response(h,&status,cancelled,context);
    if(!j||status!=200||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"success"))) {
        WD_DEBUG("[wardrive upload] stage=response_rejected status=%d json=%u success=%u\n",
            status,(unsigned)(j!=NULL),
            (unsigned)(j&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"success"))));
        goto end;
    }
    cJSON *results=cJSON_GetObjectItemCaseSensitive(j,"results");
    if(verify) {
        cJSON *entry;
        cJSON_ArrayForEach(entry,results) {
            cJSON *id=cJSON_GetObjectItemCaseSensitive(entry,"transid");
            cJSON *percent=cJSON_GetObjectItemCaseSensitive(entry,"percentDone");
            /* Without a documented terminal status vocabulary, 100% alone
             * isn't proof of successful incorporation. Keep Submitted and
             * require account review; never report a false Confirmed state. */
            if(cJSON_IsString(id)&&!strcmp(id->valuestring,s.transaction)&&cJSON_IsNumber(percent)) { ok=true; break; }
        }
    } else {
        cJSON *ids=cJSON_GetObjectItemCaseSensitive(results,"transids");
        cJSON *id=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ids,0),"transId");
        if(cJSON_IsString(id)&&strlen(id->valuestring)<sizeof(s.transaction)) {
            bool safe=true; for(const char *p=id->valuestring;*p;p++) if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='-'||*p=='_')) safe=false;
            if(safe&&id->valuestring[0]) { strcpy(s.transaction,id->valuestring); s.state=WD_SUBMITTED; ok=true; }
        }
    }
end:
    if(file) { fclose(file); file=NULL; }
    if(write_failed&&!(cancelled&&cancelled(context))) status=early_status(h);
    if(!ok) WD_DEBUG("[wardrive upload] stage=finish_failed problem=%d http=%d may_have_sent=%u\n",
        (int)problem,status,(unsigned)may_have_sent);
    cJSON_Delete(j); if(h) { esp_http_client_close(h); esp_http_client_cleanup(h); }
    if(!verify&&!ok) {
        s.problem=status==401||status==403?WD_UPLOAD_AUTH:status==429?WD_UPLOAD_RATE_LIMIT:problem;
        /* No success acknowledgment means Failed for the user, including
         * timeouts after transmission (owner decision, Nov 2026). Whether
         * bytes may have reached WiGLE is kept for troubleshooting; the
         * attempt cap and backoff still bound manual retries. */
        s.state=WD_FAILED; unsigned delay=60U<<(s.attempts>5?5:s.attempts);
        s.retry_after=(uint64_t)time(NULL)+delay;
        s.may_have_sent=may_have_sent&&status!=429&&status!=401&&status!=403;
        if(status==401||status==403) s.attempts=5;
    }
    if(ok) { s.problem=WD_UPLOAD_OK; s.may_have_sent=false; s.retry_after=0; }
    if(verify&&!ok) s.problem=problem;
    if(!wd_upload_status_write(directory,&s)) return false;
    return ok;
#endif
}

bool wd_upload_one_cancellable(const char *directory,const wd_credentials_t *c,bool verify,bool (*cancelled)(void *),void *context) {
    return upload(directory,c,verify,cancelled,context,NULL,NULL);
}
bool wd_upload_one_progress(const char *directory,const wd_credentials_t *c,bool (*cancelled)(void *),void *context,wd_upload_progress_fn progress,void *progress_context) {
    bool ok=upload(directory,c,false,cancelled,context,progress,progress_context);
    report(progress,progress_context,ok?WD_STAGE_COMPLETE:WD_STAGE_FAILED,0,0);
    return ok;
}
bool wd_upload_one(const char *directory,const wd_credentials_t *c,bool verify) {
    return wd_upload_one_cancellable(directory,c,verify,NULL,NULL);
}
