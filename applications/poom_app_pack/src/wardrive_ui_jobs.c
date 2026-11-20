// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
#include "wardrive_ui_internal.h"
#include "wardrive_ui_wifi.h"
#include "wardrive_sessions.h"
#include "wardrive_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <time.h>
static atomic_int sta_event,sta_reason;
static bool station_owned;
/* ESP32-C5: general malloc fills DMA-capable HP SRAM before falling back to
 * the ~15 KiB LP SRAM heap. Wi-Fi TX/RX buffers and AES/SHA GDMA buffers can
 * only use HP SRAM, and an upload trace showed 884 bytes of it left (with
 * ~14 KiB of LP SRAM unused) when TCP stalled. The worker stack needs no DMA
 * (SPI SD, AES and SHA drivers bounce non-DMA buffers), so place it in LP SRAM:
 * that frees HP SRAM for the radio. Falls back to the previous HP-SRAM stack if LP SRAM is unavailable. */
/* 8 KiB keeps total free heap equal to the build whose TLS handshakes
 * succeeded (10 KiB cost 2 KiB and tipped a handshake into failure); the
 * uploader's 4 KiB response buffer moved to the heap to restore margin. */
#define WD_JOB_STACK_LP 8192
#define WD_JOB_STACK_HP 8192
static bool worker_stack_lp;
static TaskHandle_t worker_handle;
static void memory_log(const char *stage,unsigned job) {
    WD_DEBUG("[wardrive] %s=%u heap=%lu largest=%lu hp_free=%lu hp_largest=%lu stack_lp=%u\n",stage,job,
        (unsigned long)esp_get_free_heap_size(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),(unsigned)worker_stack_lp);
}
static atomic_bool sta_associated;
static void link_event(const poom_wifi_ctrl_evt_info_t *e,void *context) {
    (void)context; if(!e) return;
    atomic_store(&sta_event,(int)e->evt);
    if(e->evt==POOM_WIFI_CTRL_EVT_STA_DISCONNECTED) {
        atomic_store(&sta_reason,e->reason); atomic_store(&sta_associated,false);
    } else if(e->evt==POOM_WIFI_CTRL_EVT_STA_CONNECTED||e->evt==POOM_WIFI_CTRL_EVT_STA_GOT_IP) {
        atomic_store(&sta_reason,0); atomic_store(&sta_associated,true);
    }
    WD_DEBUG("[wardrive WiFi] event=%d reason=%ld\n",(int)e->evt,(long)e->reason);
}
void wd_ui_disconnect(void) {
    station_owned=false;
    wd_transfer_stop();
    /* Idle entry/exit must not tear down another app's driver or callbacks. */
    if(!wd_wifi_owned()) return;
    esp_err_t unregister_result=wd_wifi_unregister_cb();
    esp_err_t disconnect_result=wd_wifi_sta_disconnect();
    esp_err_t deinit_result=wd_wifi_deinit();
    /* A failed teardown can invalidate the memory budget for the next TLS
     * operation. Log the actual results without changing shared WiFi state. */
    WD_DEBUG("[wardrive WiFi] teardown unregister=0x%x disconnect=0x%x deinit=0x%x heap=%lu largest=%lu\n",
        (unsigned)unregister_result,(unsigned)disconnect_result,(unsigned)deinit_result,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
static bool connect(wd_ui_job_t *j) {
    if(!j->credentials.ssid[0]) { strcpy(j->message,"SET TRUSTED WIFI"); return false; }
    bool reuse=station_owned&&wd_wifi_sta_has_ip()&&wd_ui_wifi_matches(j->credentials.ssid,j->credentials.password);
    if(reuse) WD_DEBUG("[wardrive WiFi] stage=reuse_link job=%u\n",(unsigned)j->job);
    else {
        wd_ui_disconnect();
        atomic_store(&sta_event,-1); atomic_store(&sta_reason,0); atomic_store(&sta_associated,false);
        WD_DEBUG("[wardrive WiFi] stage=join_begin job=%u\n",(unsigned)j->job);
        if(wd_ui_wifi_init_transfer()!=ESP_OK) { strcpy(j->message,"WIFI INIT FAILED"); return false; }
        WD_DEBUG("[wardrive WiFi] profile=transfer rx=2 dynamic_rx=8 tx_max=8 ba=2 mgmt=6 rx_mgmt=2 heap=%lu largest=%lu\n",(unsigned long)esp_get_free_heap_size(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        station_owned=true;
        if(wd_wifi_register_cb(link_event,NULL)!=ESP_OK) { strcpy(j->message,"WIFI CALLBACK FAILED"); return false; }
        if(wd_ui_wifi_connect(j->credentials.ssid,j->credentials.password)!=ESP_OK) { strcpy(j->message,"WIFI CONNECT FAILED"); return false; }
    }
    for(unsigned i=0;i<200&&!atomic_load(&j->cancel);i++) {
        if(wd_wifi_sta_has_ip()) {
            esp_netif_ip_info_t ip; esp_netif_t *n=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(!n||esp_netif_get_ip_info(n,&ip)!=ESP_OK) { strcpy(j->message,"WIFI IP READ FAILED"); return false; }
            snprintf(j->ip,sizeof(j->ip),IPSTR,IP2STR(&ip.ip));
            /* Only HTTPS jobs may borrow GPS time for certificate checks. */
            if(j->job==WD_JOB_UPLOAD||j->job==WD_JOB_VERIFY||j->job==WD_JOB_ALL) {
                int64_t utc=wd_clock_now(&j->clock,(uint64_t)esp_timer_get_time()/1000,(int64_t)time(NULL));
                if(!wd_clock_scope_begin(&j->clock_scope,utc)) {
                    strcpy(j->message,j->clock_scope.error==WD_CLOCK_SYNC_BUSY?"WAIT TIME SYNC":
                        j->clock_scope.error==WD_CLOCK_IO_ERROR?"CLOCK SET FAILED":"WAIT GPS DATE/TIME");
                    return false;
                }
            }
            return !atomic_load(&j->cancel);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int reason=atomic_load(&sta_reason);
    WD_DEBUG("[wardrive WiFi] stage=join_timeout event=%d reason=%d associated=%u\n",atomic_load(&sta_event),reason,(unsigned)atomic_load(&sta_associated));
    if(atomic_load(&j->cancel)) strcpy(j->message,"OPERATION CANCELLED");
    else if(reason) snprintf(j->message,sizeof(j->message),"WiFi disconnect %d",reason);
    else strcpy(j->message,atomic_load(&sta_associated)?"DHCP timeout":"WIFI / DHCP TIMEOUT");
    return false;
}
bool wd_ui_link_up(void) { return station_owned&&wd_wifi_sta_has_ip(); }
static bool keep_link(wd_ui_job_t *j) {
    return !atomic_load(&j->cancel)&&station_owned&&wd_wifi_sta_has_ip()&&
        wd_ui_wifi_matches(j->credentials.ssid,j->credentials.password);
}
static bool cancelled(void *context) {
    wd_ui_job_t *j=context;
    return atomic_load(&j->cancel);
}
static bool export_file(wd_ui_job_t *j,const char *directory,uint32_t *rows) {
    char file[224];
    if(snprintf(file,sizeof(file),"%s/wigle.csv",directory)>=(int)sizeof(file)) return false;
    uint32_t skipped; return wd_export_cancellable(directory,file,rows,&skipped,cancelled,j);
}
/* Reuse a current CSV (wigle.csv + export.json); otherwise build it from the
 * raw journal. rows is the located-row count from the export report. */
static bool current_csv(wd_ui_job_t *j,const char *directory,uint32_t *rows,bool *created) {
    *created=false;
    if(wd_export_ready(directory)&&wd_export_rows(directory,rows)) return true;
    *created=true;
    return export_file(j,directory,rows);
}
static void progress(void *context,wd_upload_stage_t stage,size_t sent,size_t total) {
    wd_ui_job_t *j=context;
    if(total) { atomic_store(&j->total,(unsigned)total); atomic_store(&j->sent,(unsigned)sent); }
    atomic_store(&j->stage,(unsigned)stage);
}
static void progress_reset(wd_ui_job_t *j,wd_upload_stage_t stage) {
    atomic_store(&j->sent,0); atomic_store(&j->total,0); atomic_store(&j->stage,(unsigned)stage);
}
/* Bulk takes only closed recordings with radio observations whose upload
 * state is Pending. Failed, Submitted/Confirmed and legacy Needs Review runs
 * are never retried or resubmitted automatically. */
static bool upload_candidate(const char *root,const char *id,wd_upload_status_t *status,bool *queue_error) {
    *queue_error=false;
    wd_session_info_t info;
    if(!wd_session_info_local(root,id,WD_TZ_UTC,&info)||strcmp(info.state,"Completed")||!info.count_known||!info.observations) return false;
    char p[192];
    if(snprintf(p,sizeof(p),"%s/%s",root,id)>=(int)sizeof(p)) return false;
    *queue_error=!wd_upload_status_read(p,status);
    return !*queue_error&&status->state==WD_PENDING;
}
/* Keep one ID cursor, not a live FAT directory/cache, during TLS. Rescanning
 * costs O(N^2) directory comparisons across a batch but keeps memory bounded. */
static bool next_session_id(wd_ui_job_t *j,const char *after,char id[49],bool *error) {
    *error=false;id[0]=0;
    DIR *d=opendir(WD_ROOT "/sessions");
    if(!d) { *error=true;return false; }
    struct dirent *e; unsigned visited=0;
    while(!cancelled(j)) {
        errno=0;e=readdir(d);
        if(!e) { if(errno) *error=true;break; }
        if(wd_safe_id(e->d_name)&&strcmp(e->d_name,after)>0&&(!id[0]||strcmp(e->d_name,id)<0)) snprintf(id,49,"%.48s",e->d_name);
        if(++visited%64==0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(closedir(d)) *error=true;
    return !*error&&id[0]&&!cancelled(j);
}
static unsigned count_candidates(wd_ui_job_t *j,bool *error) {
    char cursor[49]={0},id[49]; unsigned count=0;
    while(!cancelled(j)&&next_session_id(j,cursor,id,error)) {
        snprintf(cursor,sizeof(cursor),"%s",id);
        wd_upload_status_t state; bool queue_error;
        if(upload_candidate(WD_ROOT "/sessions",id,&state,&queue_error)) count++;
    }
    return count;
}
static bool account_problem(wd_upload_problem_t problem) {
    return problem==WD_UPLOAD_AUTH||problem==WD_UPLOAD_RATE_LIMIT;
}
static bool upload_all(wd_ui_job_t *j) {
    bool linked=false,ok=true,error=false,halted=false;char cursor[49]={0},id[49];
    unsigned total=count_candidates(j,&error);
    if(error) { strcpy(j->message,"Session list unavailable");return false; }
    atomic_store(&j->count,total); atomic_store(&j->index,0);
    while(!cancelled(j)&&next_session_id(j,cursor,id,&error)) {
        snprintf(cursor,sizeof(cursor),"%s",id);
        wd_upload_status_t state; bool queue_error=false;
        if(!upload_candidate(WD_ROOT "/sessions",id,&state,&queue_error)) {
            if(queue_error) { j->failed++;ok=false;strcpy(j->message,"Upload state unreadable"); }
            continue;
        }
        atomic_fetch_add(&j->index,1); progress_reset(j,WD_STAGE_PREPARING);
        /* Account-wide rejection (keys or rate limit): leave the rest Pending. */
        if(halted) { j->skipped++; continue; }
        char p[192]; snprintf(p,sizeof(p),WD_ROOT "/sessions/%.48s",id);
        uint32_t rows=0; bool created;
        if(!current_csv(j,p,&rows,&created)) {
            if(cancelled(j)) break;
            j->failed++;ok=false;atomic_store(&j->failures,j->failed);strcpy(j->message,"CSV / SD read failed");continue;
        }
        if(!rows) { j->skipped++;continue; }
        if(!linked) {
            progress_reset(j,WD_STAGE_CONNECTING);
            if(!connect(j)) { ok=false; break; }
            linked=true;
        }
        if(cancelled(j)) break;
        if(wd_upload_one_progress(p,&j->credentials,cancelled,j,progress,j)) { j->processed++; atomic_store(&j->submitted,j->processed); }
        else {
            if(cancelled(j)) break;
            j->failed++;ok=false;atomic_store(&j->failures,j->failed);
            if(wd_upload_status_read(p,&state)) {
                snprintf(j->message,sizeof(j->message),"%s",wd_upload_problem_text(state.problem));
                j->credentials_rejected=state.problem==WD_UPLOAD_AUTH;
                if(account_problem(state.problem)) halted=true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(error) { strcpy(j->message,"Session list unavailable");return false; }
    if(ok&&!j->processed) strcpy(j->message,total?"No located GPS rows":"No pending runs");
    else if(ok) strcpy(j->message,"Bulk upload complete");
    return ok&&!cancelled(j);
}
static bool upload_selected(wd_ui_job_t *j) {
    wd_upload_status_t state;
    progress_reset(j,WD_STAGE_PREPARING);
    if(!wd_upload_status_read(j->directory,&state)) { strcpy(j->message,"Upload state unreadable"); return false; }
    bool verify=j->job==WD_JOB_VERIFY;
    if(verify) {
        if(state.state!=WD_SUBMITTED||!state.transaction[0]) { strcpy(j->message,"No submitted ID"); return false; }
    } else {
        /* Manual retry: Pending, Failed and legacy Needs Review. */
        if(state.state==WD_SUBMITTED||state.state==WD_CONFIRMED) { strcpy(j->message,"Already submitted"); return false; }
        if(state.attempts>=5) { strcpy(j->message,"Reset status to retry"); return false; }
        uint32_t rows=0;
        if(!wd_export_ready(j->directory)) { strcpy(j->message,"Create WiGLE CSV first"); return false; }
        if(!wd_export_rows(j->directory,&rows)) { strcpy(j->message,"CSV / SD read failed"); return false; }
        if(!rows) { strcpy(j->message,"No located CSV rows"); return false; }
    }
    progress_reset(j,WD_STAGE_CONNECTING);
    if(!connect(j)||cancelled(j)) return false;
    if(!verify&&state.retry_after>(uint64_t)time(NULL)) { strcpy(j->message,"Retry waiting"); return false; }
    bool ok=verify?wd_upload_one_cancellable(j->directory,&j->credentials,true,cancelled,j):
        wd_upload_one_progress(j->directory,&j->credentials,cancelled,j,progress,j);
    if(ok) j->processed=1;
    else {
        j->failed=1;
        if(verify) strcpy(j->message,"Check account queue");
        else if(wd_upload_status_read(j->directory,&state)) {
            snprintf(j->message,sizeof(j->message),"%s",wd_upload_problem_text(state.problem));
            j->credentials_rejected=state.problem==WD_UPLOAD_AUTH;
        }
    }
    return ok;
}
static bool prepare_download(wd_ui_job_t *j) {
    uint32_t rows=0; bool created;
    if(!current_csv(j,j->directory,&rows,&created)) { strcpy(j->message,"CSV preparation failed"); return false; }
    j->processed=rows;
    if(atomic_load(&j->cancel)) return false;
    if(!wd_archive(j->directory)) { strcpy(j->message,"Bundle save failed"); return false; }
    return true; /* zero geographic rows still leaves useful raw logs */
}
/* Rebuild summary/index from the journal, then drop derived exports: the CSV
 * and bundle must be regenerated from the recovered data. Upload state and
 * the original journal are untouched. */
static bool recover(wd_ui_job_t *j) {
    if(!wd_session_recover(j->directory)) { strcpy(j->message,"Recovery failed"); return false; }
    if(!wd_export_invalidate(j->directory)) { strcpy(j->message,"Old CSV not removed"); return false; }
    return true;
}
static void task(void *context) {
    wd_ui_job_t *j=context; bool ok=false;
    memory_log("file_job",(unsigned)j->job);
    switch(j->job) {
        case WD_JOB_EXPORT: {
            char file[192]; snprintf(file,sizeof(file),"%s/wigle.csv",j->directory);
            uint32_t rows=0,skipped=0; ok=wd_export(j->directory,file,&rows,&skipped);
            j->processed=rows; j->failed=skipped; break;
        }
        case WD_JOB_RECOVER: ok=recover(j); break;
        case WD_JOB_ARCHIVE: ok=prepare_download(j); break;
        case WD_JOB_WIFI_SCAN: ok=wd_ui_wifi_scan(j); break;
        case WD_JOB_TRANSFER:
            if(connect(j)&&!atomic_load(&j->cancel)) {
                wd_transfer_set_timezone(j->display_zone);
                ok=wd_transfer_start(j->token);
                if(!ok) strcpy(j->message,"HTTP START FAILED");
            }
            break;
        case WD_JOB_WIFI_JOIN: ok=connect(j); break;
        case WD_JOB_UPLOAD:
        case WD_JOB_VERIFY: ok=upload_selected(j); break;
        case WD_JOB_ALL: ok=upload_all(j); break;
        default:break;
    }
    /* Every success/failure/cancel path returns the clock before completion. */
    char operation_message[sizeof(j->message)];
    snprintf(operation_message,sizeof(operation_message),"%s",j->message);
    bool clock_retry=false;
    while(!wd_clock_scope_end(&j->clock_scope)) {
        clock_retry=true;
        strcpy(j->message,"CLOCK RESTORE RETRY");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(clock_retry) snprintf(j->message,sizeof(j->message),"%s",operation_message);
    if(!keep_link(j)) wd_ui_disconnect();
    j->success=ok&&!atomic_load(&j->cancel);
    const char *result=ok?"DONE":"FAILED / CHECK STATUS";
    if(ok&&j->job==WD_JOB_UPLOAD) result="SUBMITTED TO WiGLE";
    if(j->job==WD_JOB_UPLOAD||j->job==WD_JOB_ALL) atomic_store(&j->stage,(unsigned)(ok?WD_STAGE_COMPLETE:WD_STAGE_FAILED));
    if(ok&&j->job==WD_JOB_VERIFY) result="FOUND IN WiGLE QUEUE";
    if(atomic_load(&j->cancel)) result="OPERATION CANCELLED";
    if(atomic_load(&j->cancel)||!j->message[0]) snprintf(j->message,sizeof(j->message),"%s",result);
    wd_credentials_clear(&j->credentials);
    WD_DEBUG("[wardrive] file_job_done=%u ok=%u stack_free=%u heap=%lu\n",(unsigned)j->job,ok,(unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned long)esp_get_free_heap_size());
    atomic_store(&j->done,true);
    /* busy cleared by UI after acquiring done: job fields can't be reused
     * while this worker is still publishing its result. */
    vTaskSuspend(NULL);
}
bool wd_ui_job_start(wd_ui_job_t *j) {
    if(atomic_exchange(&j->busy,true)) return false;
    atomic_store(&j->done,false); atomic_store(&j->cancel,false); j->processed=0; j->failed=0; j->skipped=0; j->credentials_rejected=false; j->message[0]=0;
    atomic_store(&j->stage,WD_STAGE_IDLE); atomic_store(&j->sent,0); atomic_store(&j->total,0);
    atomic_store(&j->index,0); atomic_store(&j->count,0); atomic_store(&j->submitted,0); atomic_store(&j->failures,0);
    /* busy serialises workers, so worker_stack_lp is stable for this task. */
    /* Set before creation: the worker may run (and finish) before create returns. */
    worker_stack_lp=true;
    if(xTaskCreateWithCaps(task,"wardrive-files",WD_JOB_STACK_LP,j,2,&worker_handle,MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        worker_stack_lp=false;
        if(xTaskCreate(task,"wardrive-files",WD_JOB_STACK_HP,j,2,&worker_handle)!=pdPASS) { wd_credentials_clear(&j->credentials); atomic_store(&j->busy,false); return false; }
    }
    return true;
}


void wd_ui_job_reap(void) {
    if(!worker_handle) return;
    while(eTaskGetState(worker_handle)!=eSuspended) vTaskDelay(1);
    if(worker_stack_lp) vTaskDeleteWithCaps(worker_handle);
    else vTaskDelete(worker_handle);
    worker_handle=NULL;
}
