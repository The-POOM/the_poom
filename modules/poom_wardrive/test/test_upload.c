// SPDX-License-Identifier: MIT
#include "wardrive_connect.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls_errors.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static bool tls_rejected,write_failed,init_failed;
static unsigned sent,calls;
static FILE *csv_handle;
static bool tls_opened,csv_open_failed,mutate_csv_on_open;
static char csv_path[256];
static const char *test_directory;
FILE *__real_fopen(const char *,const char *);
int __real_fclose(FILE *);
FILE *__wrap_fopen(const char *path,const char *mode) {
    const char *name=strrchr(path,'/');bool csv=name&&!strcmp(name,"/wigle.csv");
    if(csv) { assert(tls_opened&&!csv_handle);if(csv_open_failed){errno=ENOMEM;return NULL;} }
    FILE *f=__real_fopen(path,mode);if(csv)csv_handle=f;return f;
}
int __wrap_fclose(FILE *f) { if(f==csv_handle)csv_handle=NULL;return __real_fclose(f); }

static int http_status=200;
static int mock_tls,mock_code,mock_flags,mock_errno;
int esp_http_client_get_errno(esp_http_client_handle_t h) { (void)h;return mock_errno; }
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h,int *code,int *flags) { (void)h;*code=mock_code;*flags=mock_flags;return mock_tls; }
static bool cancel_now,cancel_on_write;
static char wire[8192];
static size_t wire_length;
static bool cancelled(void *p) { (void)p;return cancel_now; }
static const char *reply;
static size_t reply_at;
esp_err_t esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(c->crt_bundle_attach==esp_crt_bundle_attach);
    assert(c->disable_auto_redirect);
    assert(!strncmp(c->url,"https://api.wigle.net/",22));
    assert(!strcmp(c->username,"synthetic")&&!strcmp(c->password,"synthetic-only"));
    assert(!csv_handle);tls_opened=false;
    if(strstr(c->url,"/upload")) { wd_upload_status_t state;assert(wd_upload_status_read(test_directory,&state)&&state.state==WD_NEEDS_REVIEW); }
    calls++; reply_at=0;wire_length=0;wire[0]=0; return init_failed?NULL:(void *)1;
}
/* Transport stall injection: from write call stall_at, return stall_value
 * stall_count times. mbedTLS requires the retry after WANT_WRITE to repeat the
 * identical buffer and length, so every stalled call and the first call after
 * the stall must match the first stalled request. */
static unsigned write_calls,stall_at,stall_count,stall_pending;static int stall_value;
static const char *stall_ptr;static int stall_len;static bool cancel_on_stall;
static unsigned fetch_calls,headers_eagain,timeout_calls;static int last_timeout;
int esp_http_client_write(esp_http_client_handle_t h,const char *p,int n) { (void)h; write_calls++; if(cancel_on_write){cancel_now=true;return -1;} if(write_failed) return -1;
    if(stall_count&&write_calls>=stall_at) { if(stall_pending) assert(p==stall_ptr&&n==stall_len); else {stall_ptr=p;stall_len=n;} stall_pending++;stall_count--; if(cancel_on_stall) cancel_now=true; return stall_value; }
    if(stall_pending) { assert(p==stall_ptr&&n==stall_len); stall_pending=0; }
    if(n>13) { n=13; }
    sent+=(unsigned)n;assert(wire_length+(size_t)n<sizeof(wire));memcpy(wire+wire_length,p,n);wire_length+=n;wire[wire_length]=0; return n; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { (void)h; assert(!csv_handle);fetch_calls++; if(headers_eagain){headers_eagain--;return -ESP_ERR_HTTP_EAGAIN;} if(!reply) return -1; return strlen(reply); }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h,int ms) { (void)h; assert(!csv_handle);timeout_calls++;last_timeout=ms; return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return http_status; }
int esp_http_client_read(esp_http_client_handle_t h,char *p,int n) { (void)h; size_t k=strlen(reply)-reply_at; if(k>(size_t)n) k=n; memcpy(p,reply+reply_at,k); reply_at+=k; return k; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { (void)h; return reply_at==strlen(reply); }
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n) { (void)h;(void)n; assert(!csv_handle);tls_opened=!tls_rejected; if(tls_opened&&mutate_csv_on_open){FILE *f=__real_fopen(csv_path,"a");assert(f);assert(fputs("x",f)>=0&&!__real_fclose(f));} return tls_rejected?ESP_FAIL:ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h,int m) { (void)h;(void)m; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v) { (void)h;(void)k;(void)v; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { (void)h;tls_opened=false;assert(!csv_handle); return ESP_OK; }
static unsigned progress_events;
static size_t progress_last,progress_total;
static bool progress_seen[WD_STAGE_FAILED+1];
static int progress_marker;
static void record_progress(void *context,wd_upload_stage_t stage,size_t sent,size_t total) {
    assert(context==&progress_marker&&stage<=WD_STAGE_FAILED);progress_events++;progress_seen[stage]=true;
    if(stage==WD_STAGE_SENDING||stage==WD_STAGE_AWAITING) {
        assert(total&&sent<=total&&sent>=progress_last);progress_last=sent;
        assert(!progress_total||progress_total==total);progress_total=total;
    }
}
int main(int argc,char **argv) {
    assert(argc==2); const char *d=argv[1];test_directory=d;snprintf(csv_path,sizeof(csv_path),"%s/wigle.csv",d); wd_upload_status_t s;
    assert(wd_upload_status_read(d,&s)&&s.state==WD_PENDING);
    s.state=WD_UPLOADING; assert(wd_upload_status_write(d,&s));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_NEEDS_REVIEW);
    char state_file[256],backup[256];
    snprintf(state_file,sizeof(state_file),"%s/upload.json",d);
    snprintf(backup,sizeof(backup),"%s/upload.json.bak",d);
    assert(rename(state_file,backup)==0); /* interrupted replacement */
    assert(wd_upload_status_read(d,&s)&&s.state==WD_NEEDS_REVIEW&&s.may_have_sent);
    wd_credentials_t c={0}; strcpy(c.api_name,"synthetic"); strcpy(c.api_token,"synthetic-only");
    s=(wd_upload_status_t){.state=WD_SUBMITTED,.manual=true}; assert(wd_upload_status_write(d,&s));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_SUBMITTED&&s.manual&&!s.may_have_sent);
    assert(!wd_upload_one(d,&c,false)&&calls==0); /* never resubmit Submitted */
    s=(wd_upload_status_t){.state=WD_CONFIRMED}; assert(wd_upload_status_write(d,&s));
    assert(!wd_upload_one(d,&c,false)&&calls==0);
    s=(wd_upload_status_t){0}; assert(wd_upload_status_write(d,&s));
    init_failed=true;assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.problem==WD_UPLOAD_MEMORY&&s.attempts==1);
    init_failed=false;s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    tls_rejected=true; assert(!wd_upload_one(d,&c,false)); assert(sent==0);
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.problem==WD_UPLOAD_CONNECT&&s.retry_after>(uint64_t)time(NULL));
    const int tls_errors[]={ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME,0,0,0,ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT};
    const int codes[]={0,0,0,MBEDTLS_ERR_SSL_ALLOC_FAILED,0};
    const int flags[]={0,MBEDTLS_X509_BADCERT_EXPIRED,MBEDTLS_X509_BADCERT_CN_MISMATCH,0,0};
    const wd_upload_problem_t expected[]={WD_UPLOAD_DNS,WD_UPLOAD_CERT_TIME,WD_UPLOAD_CERT,WD_UPLOAD_MEMORY,WD_UPLOAD_TIMEOUT};
    for(unsigned i=0;i<5;i++) {
        s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
        mock_tls=tls_errors[i];mock_code=codes[i];mock_flags=flags[i];
        assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s)&&s.problem==expected[i]);
        assert(s.net_error==ESP_FAIL&&s.tls_error==mock_tls&&s.tls_code==mock_code&&s.tls_flags==mock_flags);
    }
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    mock_tls=0x8017;mock_code=141;mock_flags=0;
    assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s)&&s.problem==WD_UPLOAD_MEMORY&&s.tls_code==141);
    mock_tls=mock_code=mock_flags=0;
    s=(wd_upload_status_t){0}; assert(wd_upload_status_write(d,&s));
    tls_rejected=false; write_failed=true;
    mock_errno=ENOBUFS; mock_tls=0x8017; mock_code=-1234; mock_flags=0;
    assert(!wd_upload_one(d,&c,false));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.problem==WD_UPLOAD_INTERRUPTED);
    assert(s.socket_error==ENOBUFS&&s.tls_error==0x8017&&s.tls_code==-1234);
    mock_errno=mock_tls=mock_code=mock_flags=0;
    s=(wd_upload_status_t){0}; assert(wd_upload_status_write(d,&s));
    write_failed=false;csv_open_failed=true;unsigned before_body=sent;
    assert(!wd_upload_one(d,&c,false)&&sent==before_body);
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.problem==WD_UPLOAD_FILE);
    csv_open_failed=false;s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    struct stat original_csv;assert(!stat(csv_path,&original_csv));mutate_csv_on_open=true;before_body=sent;
    assert(!wd_upload_one(d,&c,false)&&sent==before_body);
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.problem==WD_UPLOAD_FILE);
    mutate_csv_on_open=false;assert(!truncate(csv_path,original_csv.st_size));
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    write_failed=false; reply="{\"success\":true,\"results\":{\"transids\":[{\"transId\":\"test-123\"}]}}";
    assert(wd_upload_one(d,&c,false)&&sent>0);
    assert(strstr(wire,"Content-Disposition: form-data; name=\"file\"; filename=\"poom-session.csv\""));
    assert(strstr(wire,"WigleWifi-1.6")&&strstr(wire,"--poomWardriveBoundary--\r\n"));
    assert(!strstr(wire,"name=\"donate\""));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_SUBMITTED&&!strcmp(s.transaction,"test-123"));
    unsigned previous=calls; assert(!wd_upload_one(d,&c,false)&&calls==previous);
    reply="{\"success\":true,\"results\":[{\"transid\":\"test-123\",\"percentDone\":100}]}";
    assert(wd_upload_one(d,&c,true));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_SUBMITTED); /* no invented success */
    const int rejections[]={401,403,429};
    for(unsigned i=0;i<3;i++) {
        s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
        http_status=rejections[i];reply="{\"success\":false}";
        assert(!wd_upload_one(d,&c,false));
        assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED);
        assert(s.problem==(http_status==429?WD_UPLOAD_RATE_LIMIT:WD_UPLOAD_AUTH));
        assert(s.attempts==(http_status==429?1:5)&&s.retry_after>(uint64_t)time(NULL));
    }
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    http_status=500;assert(!wd_upload_one(d,&c,false));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_RESPONSE);
    http_status=200;
    char oversized[5000];memset(oversized,'x',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));reply=oversized;
    assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL));
    const char *bad[]={"not-json","{\"success\":true,\"results\":{}}","{\"success\":true,\"results\":{\"transids\":[{\"transId\":\"../unsafe\"}]}}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));reply=bad[i];
        assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL));
        previous=calls;assert(!wd_upload_one(d,&c,false)&&calls==previous);
    }
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));previous=calls;cancel_now=true;
    assert(!wd_upload_one_cancellable(d,&c,false,cancelled,NULL)&&calls==previous);
    assert(wd_upload_status_read(d,&s)&&s.state==WD_PENDING); /* cancellation before any request */
    cancel_now=false;cancel_on_write=true;
    assert(!wd_upload_one_cancellable(d,&c,false,cancelled,NULL));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_INTERRUPTED);
    cancel_now=cancel_on_write=false;
    /* Exact multipart body expected on the wire for this session. */
    static char expected_wire[sizeof(wire)];
    {
        FILE *f=__real_fopen(csv_path,"rb");assert(f);
        int k=snprintf(expected_wire,sizeof(expected_wire),"--poomWardriveBoundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"poom-session.csv\"\r\nContent-Type: text/csv\r\n\r\n");
        size_t body=fread(expected_wire+k,1,sizeof(expected_wire)-(size_t)k-64,f);assert(feof(f)&&!ferror(f)&&!__real_fclose(f));
        strcat(expected_wire+k+body,"\r\n--poomWardriveBoundary--\r\n");
    }
    const char *accepted="{\"success\":true,\"results\":{\"transids\":[{\"transId\":\"stall-ok\"}]}}";
    /* Up to 5 consecutive no-progress results (select timeout = 0, lwIP
     * SO_SNDTIMEO = WANT_WRITE, WANT_READ) mid-CSV recover without
     * duplicating or skipping bytes and the upload is accepted. */
    const int transient[]={0,MBEDTLS_ERR_SSL_WANT_WRITE,MBEDTLS_ERR_SSL_WANT_READ};
    for(unsigned i=0;i<3;i++) {
        s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
        reply=accepted;stall_at=write_calls+20;stall_count=5;stall_value=transient[i];
        assert(wd_upload_one(d,&c,false));
        assert(!stall_count&&!stall_pending&&!strcmp(wire,expected_wire));
        assert(wd_upload_status_read(d,&s)&&s.state==WD_SUBMITTED&&!strcmp(s.transaction,"stall-ok"));
    }
    /* A sixth consecutive stall is treated as a dead connection: Needs Review,
     * never Submitted even if some reply is readable afterwards. */
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    reply=accepted;stall_at=write_calls+20;stall_count=6;stall_value=MBEDTLS_ERR_SSL_WANT_WRITE;
    unsigned fetch_before=fetch_calls;
    assert(!wd_upload_one(d,&c,false)&&!stall_count);stall_pending=0;
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_INTERRUPTED&&!s.transaction[0]);
    assert(fetch_calls==fetch_before+1&&last_timeout==3000);
    /* Hard write error after an early explicit rejection is classified from
     * the server's status instead of becoming an ambiguous review. */
    write_failed=true;reply="{\"success\":false}";
    const int early[]={401,403,429,500};
    for(unsigned i=0;i<4;i++) {
        s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));http_status=early[i];
        assert(!wd_upload_one(d,&c,false));assert(wd_upload_status_read(d,&s));
        if(early[i]==500) assert(s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_INTERRUPTED);
        else assert(s.state==WD_FAILED&&s.problem==(early[i]==429?WD_UPLOAD_RATE_LIMIT:WD_UPLOAD_AUTH));
    }
    write_failed=false;http_status=200;
    /* Slow server: response header EAGAIN is retried within the bound. */
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    reply=accepted;headers_eagain=5;assert(wd_upload_one(d,&c,false)&&!headers_eagain);
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    headers_eagain=6;assert(!wd_upload_one(d,&c,false));headers_eagain=0;
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_RESPONSE);
    /* Cancellation during a stall stops promptly without an early-response read. */
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    stall_at=write_calls+20;stall_count=3;stall_value=0;cancel_on_stall=true;fetch_before=fetch_calls;unsigned timeout_before=timeout_calls;
    assert(!wd_upload_one_cancellable(d,&c,false,cancelled,NULL));
    assert(stall_count==2&&fetch_calls==fetch_before&&timeout_calls==timeout_before);
    assert(wd_upload_status_read(d,&s)&&s.state==WD_FAILED&&s.may_have_sent&&s.retry_after>(uint64_t)time(NULL)&&s.problem==WD_UPLOAD_INTERRUPTED);
    stall_count=stall_pending=0;cancel_on_stall=cancel_now=false;
    /* Explicit manual retry of a legacy Needs Review session, with real
     * byte progress: monotonic, ends at the request size, then awaits reply. */
    s=(wd_upload_status_t){.state=WD_NEEDS_REVIEW,.attempts=1};assert(wd_upload_status_write(d,&s));
    reply=accepted;progress_events=0;progress_last=0;progress_total=0;
    assert(wd_upload_one_progress(d,&c,NULL,NULL,record_progress,&progress_marker));
    assert(progress_events>4&&progress_seen[WD_STAGE_CONNECTING]&&progress_seen[WD_STAGE_SENDING]&&progress_seen[WD_STAGE_AWAITING]&&progress_seen[WD_STAGE_COMPLETE]&&!progress_seen[WD_STAGE_FAILED]);
    assert(progress_total&&progress_last==progress_total&&progress_total==strlen(expected_wire));
    assert(wd_upload_status_read(d,&s)&&s.state==WD_SUBMITTED&&s.attempts==2&&!s.may_have_sent&&!s.retry_after);
    memset(progress_seen,0,sizeof(progress_seen));progress_last=0;progress_total=0;
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));http_status=500;
    assert(!wd_upload_one_progress(d,&c,NULL,NULL,record_progress,&progress_marker)&&progress_seen[WD_STAGE_FAILED]&&!progress_seen[WD_STAGE_COMPLETE]);
    http_status=200;
    s=(wd_upload_status_t){.problem=WD_UPLOAD_HEADERS+1};assert(wd_upload_status_write(d,&s));assert(!wd_upload_status_read(d,&s));
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    FILE *legacy=fopen(state_file,"w");assert(legacy);
    assert(fputs("{\"state\":0,\"attempts\":0,\"transaction\":\"\",\"retry_after\":0}",legacy)>=0&&!fclose(legacy));
    assert(wd_upload_status_read(d,&s)&&s.net_error==0&&s.problem==WD_UPLOAD_OK);
    s=(wd_upload_status_t){.state=WD_FAILED,.problem=WD_UPLOAD_DNS,.attempts=5,.retry_after=4102444800ULL,.net_error=INT_MIN,.tls_error=INT_MAX,.tls_code=INT_MIN,.tls_flags=INT_MAX,.socket_error=INT_MIN};
    memset(s.transaction,'x',sizeof(s.transaction)-1);assert(wd_upload_status_write(d,&s));
    assert(wd_upload_status_read(d,&s)&&s.net_error==INT_MIN&&s.tls_error==INT_MAX&&s.socket_error==INT_MIN);
    s=(wd_upload_status_t){0};assert(wd_upload_status_write(d,&s));
    assert(!strcmp(wd_upload_problem_text(WD_UPLOAD_AUTH),"API keys rejected"));
    puts("Simulated HTTPS: no CSV handle during TLS, queue committed before HTTP, bounded body, auth/rate limits, recovery and cancellation passed");
}
