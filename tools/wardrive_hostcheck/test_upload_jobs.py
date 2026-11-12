"""Run production upload selection/queue functions against offline session mocks."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

def function(source,name):
    m=re.search(r'static (?:void|bool|unsigned) '+name+r'\([^;{}]*\)\s*\{',source)
    assert m,name
    end,depth=m.end(),1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[m.start():end]
source=Path('applications/poom_app_pack/src/wardrive_ui_jobs.c').read_text()
harness=r'''
#include "wardrive_ui_internal.h"
#include "wardrive_debug.h"
#include "wardrive_sessions.h"
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#define pdMS_TO_TICKS(x) (x)
static unsigned connections,exports,submissions,verifications,delays,open_directories,progress_calls;
static bool directory_error;
static struct dirent *tracked_read(DIR *d) { if(directory_error){errno=EIO;return NULL;}return readdir(d); }
static DIR *tracked_open(const char *p) { DIR *d=opendir(p);if(d)open_directories++;return d; }
static int tracked_close(DIR *d) { assert(open_directories);open_directories--;return closedir(d); }
#define opendir tracked_open
#define closedir tracked_close
#define readdir tracked_read
bool wd_safe_id(const char *s) { size_t n=strlen(s);if(!n||n>48)return false;for(unsigned i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='-'&&s[i]!='_')return false;return true; }

static bool connect_ok=true,export_ok=true,upload_ok=true,zero_rows,cancel_in_export,cancel_in_upload;
static bool reject_after_upload,rate_limit_after_upload,selected_ready;
static unsigned selected_rows=7;
static char submitted[20][49],rejected[49];
static wd_upload_status_t persisted;
static time_t wall=1794117600;
static time_t fake_time(time_t *out) { if(out)*out=wall;return wall; }
#define time fake_time
static const char *basename(const char *p) { const char *q=strrchr(p,'/');return q?q+1:p; }
static bool connect(wd_ui_job_t *j) {
    connections++;if(!connect_ok)strcpy(j->message,"WIFI / DHCP TIMEOUT");
    wall=1794117600;return connect_ok;
}
static void vTaskDelay(unsigned n) { assert(n==1);delays++; }
bool wd_session_info_local(const char *root,const char *id,wd_timezone_t zone,wd_session_info_t *info) {
    (void)root;assert(zone==WD_TZ_UTC);memset(info,0,sizeof(*info));
    if(!strcmp(id,".")||!strcmp(id,"..")||!strcmp(id,"malformed"))return false;
    strcpy(info->state,!strcmp(id,"active")?"Active":!strcmp(id,"paused")?"Paused":!strcmp(id,"recovery")?"NeedsRecovery":"Completed");
    info->count_known=strcmp(id,"unknown-count")!=0;info->observations=strcmp(id,"empty")?9:0;return true;
}
bool wd_upload_status_read(const char *p,wd_upload_status_t *state) {
    const char *id=basename(p);*state=(wd_upload_status_t){0};
    if(!strcmp(id,"unreadable"))return false;
    if(!strcmp(id,"submitted")){state->state=WD_SUBMITTED;strcpy(state->transaction,"synthetic-id");}
    else if(!strcmp(id,"confirmed"))state->state=WD_CONFIRMED;
    else if(!strcmp(id,"review"))state->state=WD_NEEDS_REVIEW;
    else if(!strcmp(id,"exhausted")){state->state=WD_FAILED;state->attempts=5;}
    else if(!strcmp(id,"waiting")){state->state=WD_FAILED;state->retry_after=1794118000;}
    else if(!strcmp(id,"failed-retry")){state->state=WD_FAILED;state->retry_after=1794117500;}
    else if(!strcmp(id,"selected"))*state=persisted;
    if(rejected[0]&&!strcmp(id,rejected)){state->state=WD_FAILED;state->problem=rate_limit_after_upload?WD_UPLOAD_RATE_LIMIT:WD_UPLOAD_AUTH;state->attempts=rate_limit_after_upload?1:5;}
    else if(!upload_ok&&contains_submitted(id)){state->state=WD_FAILED;state->problem=WD_UPLOAD_TIMEOUT;state->may_have_sent=true;}
    return true;
}
const char *wd_upload_problem_text(wd_upload_problem_t p) { return p==WD_UPLOAD_AUTH?"API keys rejected":p==WD_UPLOAD_RATE_LIMIT?"WiGLE rate limit":"Upload timed out"; }
bool wd_export_cancellable(const char *p,const char *out,uint32_t *rows,uint32_t *skipped,bool (*cancelled)(void *),void *context) {
    assert(strstr(out,"wigle.csv"));exports++;*rows=zero_rows||!strcmp(basename(p),"no-gps")?0:7;*skipped=0;
    if(cancel_in_export)atomic_store(&((wd_ui_job_t *)context)->cancel,true);
    return export_ok&&!cancelled(context);
}
bool wd_export_ready(const char *p) { return !strcmp(basename(p),"eligible-ready")||(!strcmp(basename(p),"selected")&&selected_ready); }
bool wd_export_rows(const char *p,uint32_t *rows) {
    assert(wd_export_ready(p));*rows=zero_rows?0:!strcmp(basename(p),"selected")?selected_rows:4;return true;
}
bool wd_upload_one_progress(const char *p,const wd_credentials_t *c,bool (*cancelled)(void *),void *context,wd_upload_progress_fn progress,void *progress_context) {
    assert(!open_directories);assert(!strcmp(c->api_name,"synthetic-name"));assert(progress&&progress_context==context);
    progress(progress_context,WD_STAGE_SENDING,10,40);progress(progress_context,WD_STAGE_AWAITING,40,40);progress_calls++;
    wd_ui_job_t *j=context;assert(atomic_load(&j->sent)==40&&atomic_load(&j->total)==40&&atomic_load(&j->stage)==WD_STAGE_AWAITING);
    if(cancel_in_upload)atomic_store(&((wd_ui_job_t *)context)->cancel,true);
    assert(submissions<20);snprintf(submitted[submissions++],49,"%s",basename(p));
    if(reject_after_upload||rate_limit_after_upload){if(!rejected[0])snprintf(rejected,sizeof(rejected),"%s",basename(p));return false;}
    return upload_ok&&!cancelled(context);
}
static bool archive_ok=true,recover_ok=true,invalidate_ok=true;static unsigned archives,recoveries,invalidations;
bool wd_archive(const char *p) { (void)p;archives++;return archive_ok; }
bool wd_session_recover(const char *p) { (void)p;recoveries++;return recover_ok; }
bool wd_export_invalidate(const char *p) { assert(recoveries);(void)p;invalidations++;return invalidate_ok; }
bool wd_upload_one_cancellable(const char *p,const wd_credentials_t *c,bool verify,bool (*cancelled)(void *),void *context) {
    (void)p;(void)c;(void)cancelled;(void)context;assert(verify);verifications++;return upload_ok;
}
'''
harness=harness.replace('bool wd_upload_status_read(','static bool contains_submitted(const char *id);\nbool wd_upload_status_read(',1)
for name in ('cancelled','export_file','current_csv','progress','progress_reset','upload_candidate','next_session_id','count_candidates','account_problem','upload_all','upload_selected','prepare_download','recover'):
    harness+=function(source,name)+'\n'
harness+=r'''
static void reset(wd_ui_job_t *j) {
    *j=(wd_ui_job_t){.job=WD_JOB_ALL};strcpy(j->credentials.api_name,"synthetic-name");
    connections=exports=submissions=verifications=delays=progress_calls=0;rejected[0]=0;
    connect_ok=export_ok=upload_ok=true;zero_rows=cancel_in_export=cancel_in_upload=reject_after_upload=rate_limit_after_upload=false;
}
static bool contains_submitted(const char *id) { for(unsigned i=0;i<submissions;i++)if(!strcmp(submitted[i],id))return true;return false; }
int main(void) {
    wd_ui_job_t job;reset(&job);wd_upload_status_t state;bool queue_error;
    /* Bulk eligibility: completed Pending runs only. */
    const char *excluded[]={"active","paused","recovery","empty","unknown-count","malformed","unreadable","submitted","confirmed","review","exhausted","failed-retry","waiting"};
    for(unsigned i=0;i<sizeof(excluded)/sizeof(*excluded);i++)assert(!upload_candidate("unused",excluded[i],&state,&queue_error));
    assert(upload_candidate("unused","eligible-pending",&state,&queue_error));
    /* Missing CSVs created, current CSV reused, zero-GPS runs skipped. */
    assert(upload_all(&job)&&connections==1&&submissions==2&&job.processed==2&&job.skipped==1&&job.failed==0);
    assert(exports==2&&contains_submitted("eligible-pending")&&contains_submitted("eligible-ready")&&!contains_submitted("no-gps"));
    assert(atomic_load(&job.count)==3&&atomic_load(&job.index)==3&&atomic_load(&job.submitted)==2&&!strcmp(job.message,"Bulk upload complete"));
    reset(&job);zero_rows=true;assert(upload_all(&job)&&!connections&&!submissions&&job.skipped==3&&!strcmp(job.message,"No located GPS rows"));
    reset(&job);directory_error=true;assert(!upload_all(&job)&&!submissions&&!open_directories&&!strcmp(job.message,"Session list unavailable"));directory_error=false;
    reset(&job);connect_ok=false;assert(!upload_all(&job)&&connections==1&&!submissions&&!strcmp(job.message,"WIFI / DHCP TIMEOUT"));
    /* One run's CSV failure doesn't stop the batch. */
    reset(&job);export_ok=false;assert(!upload_all(&job)&&job.failed==2&&connections==1&&submissions==1&&contains_submitted("eligible-ready"));
    assert(atomic_load(&job.failures)==2);
    reset(&job);cancel_in_export=true;assert(!upload_all(&job)&&!connections&&!submissions&&atomic_load(&job.cancel));
    reset(&job);cancel_in_upload=true;assert(!upload_all(&job)&&connections==1&&submissions==1);
    /* Ordinary per-run failures (timeouts) continue to the next run. */
    reset(&job);upload_ok=false;assert(!upload_all(&job)&&submissions==2&&job.failed==2&&!job.processed&&!strcmp(job.message,"Upload timed out"));
    /* Account-wide rejection stops uploading; remaining runs stay Pending (skipped). */
    reset(&job);reject_after_upload=true;assert(!upload_all(&job)&&submissions==1&&job.failed==1&&job.skipped==2&&job.credentials_rejected&&!strcmp(job.message,"API keys rejected"));
    reset(&job);rate_limit_after_upload=true;assert(!upload_all(&job)&&submissions==1&&job.skipped==2&&!job.credentials_rejected&&!strcmp(job.message,"WiGLE rate limit"));
    char bad_queue[256];snprintf(bad_queue,sizeof(bad_queue),"%s/sessions/unreadable",WD_ROOT);assert(!mkdir(bad_queue,0700));
    reset(&job);assert(!upload_all(&job)&&job.failed==1&&!strcmp(job.message,"Upload state unreadable"));assert(!rmdir(bad_queue));
    /* Single run: uses the current CSV, never regenerates; manual retries. */
    reset(&job);job.job=WD_JOB_UPLOAD;snprintf(job.directory,sizeof(job.directory),"%s/sessions/selected",WD_ROOT);selected_ready=true;
    persisted=(wd_upload_status_t){.state=WD_SUBMITTED};assert(!upload_selected(&job)&&!connections&&!strcmp(job.message,"Already submitted"));
    persisted.state=WD_CONFIRMED;assert(!upload_selected(&job)&&!connections&&!strcmp(job.message,"Already submitted"));
    persisted=(wd_upload_status_t){.state=WD_FAILED,.attempts=5};assert(!upload_selected(&job)&&!connections&&!strcmp(job.message,"Reset status to retry"));
    persisted=(wd_upload_status_t){0};selected_ready=false;assert(!upload_selected(&job)&&!connections&&!exports&&!strcmp(job.message,"Create WiGLE CSV first"));
    selected_ready=true;selected_rows=0;assert(!upload_selected(&job)&&!connections&&!strcmp(job.message,"No located CSV rows"));selected_rows=7;
    persisted=(wd_upload_status_t){.state=WD_FAILED,.attempts=1,.retry_after=1794117700};
    assert(!upload_selected(&job)&&connections==1&&!submissions&&!strcmp(job.message,"Retry waiting"));
    persisted=(wd_upload_status_t){.state=WD_NEEDS_REVIEW,.attempts=1}; /* legacy review: explicit manual retry */
    assert(upload_selected(&job)&&connections==2&&job.processed==1&&!exports&&progress_calls==1);
    persisted=(wd_upload_status_t){.state=WD_FAILED,.attempts=2,.may_have_sent=true};
    reset(&job);job.job=WD_JOB_UPLOAD;snprintf(job.directory,sizeof(job.directory),"%s/sessions/selected",WD_ROOT);
    assert(upload_selected(&job)&&job.processed==1&&!exports);
    reset(&job);job.job=WD_JOB_VERIFY;snprintf(job.directory,sizeof(job.directory),"%s/sessions/selected",WD_ROOT);
    persisted=(wd_upload_status_t){.state=WD_SUBMITTED};strcpy(persisted.transaction,"synthetic-id");
    assert(upload_selected(&job)&&verifications==1&&!exports&&!submissions);
    /* Raw + CSV download: reuse a current CSV, else create; bundle even with 0 rows. */
    reset(&job);job.job=WD_JOB_ARCHIVE;snprintf(job.directory,sizeof(job.directory),"%s/sessions/selected",WD_ROOT);
    selected_ready=true;assert(prepare_download(&job)&&!exports&&archives==1&&job.processed==7);
    selected_ready=false;assert(prepare_download(&job)&&exports==1&&archives==2&&job.processed==7);
    zero_rows=true;assert(prepare_download(&job)&&archives==3&&!job.processed);zero_rows=false;
    export_ok=false;assert(!prepare_download(&job)&&archives==3&&!strcmp(job.message,"CSV preparation failed"));export_ok=true;
    archive_ok=false;assert(!prepare_download(&job)&&!strcmp(job.message,"Bundle save failed"));archive_ok=true;
    cancel_in_export=true;assert(!prepare_download(&job)&&archives==4);
    /* Recovery rebuilds from the journal, then invalidates derived exports. */
    reset(&job);job.job=WD_JOB_RECOVER;assert(recover(&job)&&recoveries==1&&invalidations==1);
    recover_ok=false;assert(!recover(&job)&&invalidations==1&&!strcmp(job.message,"Recovery failed"));recover_ok=true;
    invalidate_ok=false;assert(!recover(&job)&&!strcmp(job.message,"Old CSV not removed"));invalidate_ok=true;
    puts("Actual download/recovery jobs: CSV reuse, raw-only bundles, failures, cancellation and export invalidation passed");
    puts("Actual upload jobs: Pending-only bulk, CSV reuse/creation, zero-GPS skip, per-run failure continuation, account-error halt, cancellation, manual retry and duplicate gates passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wardrive-upload-jobs-') as tmp:
    root=Path(tmp)/'data';sessions=root/'sessions';sessions.mkdir(parents=True)
    for name in ('eligible-pending','eligible-ready','failed-retry','waiting','no-gps','active','paused','recovery','empty','unknown-count','malformed','submitted','confirmed','review','exhausted'):
        (sessions/name).mkdir()
    c,exe=Path(tmp)/'jobs.c',Path(tmp)/'jobs';c.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
        '-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
        '-Iapplications/poom_app_pack/src','-Imodules/poom_gps/include','-Imodules/poom_wardrive/include',
        '-Itools/wardrive_hostcheck/stubs',f'-DWD_ROOT="{root}"',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
