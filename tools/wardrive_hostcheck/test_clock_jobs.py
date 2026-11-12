"""Run the production job task and assert clock cleanup precedes publication."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

source = Path("applications/poom_app_pack/src/wardrive_ui_jobs.c").read_text()
match = re.search(r"static void task\(void \*context\)\s*\{", source)
assert match
end, depth = match.end(), 1
while depth:
    if source[end] == "{": depth += 1
    elif source[end] == "}": depth -= 1
    end += 1
task = source[match.start():end]
harness = r'''
#include "wardrive_ui_internal.h"
#include "wardrive_debug.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define pdMS_TO_TICKS(x) (x)
static wd_ui_job_t *active;
static bool outcome,cancel_during_operation,restore_retry;
static unsigned restorations,delays,suspends,disconnects;
static void memory_log(const char *s,unsigned n) { (void)s;(void)n; }
static bool operation(wd_ui_job_t *j) {
    j->clock_scope.active=true;
    if(cancel_during_operation)atomic_store(&j->cancel,true);
    return outcome;
}
static bool upload_selected(wd_ui_job_t *j) { return operation(j); }
static bool upload_all(wd_ui_job_t *j) { return operation(j); }
bool wd_export(const char *d,const char *f,uint32_t *r,uint32_t *s) { (void)d;(void)f;*r=1;*s=0;return true; }
static bool recover(wd_ui_job_t *j) { (void)j;return true; }
static bool prepare_download(wd_ui_job_t *j) { (void)j;return true; }
bool wd_ui_wifi_scan(wd_ui_job_t *j) { (void)j;return true; }
static bool connect(wd_ui_job_t *j) { (void)j;return true; }
void wd_transfer_set_timezone(wd_timezone_t z) { (void)z; }
bool wd_transfer_start(char token[33]) { (void)token;return true; }
static bool keep_link(wd_ui_job_t *j) { (void)j;return false; }
bool wd_clock_scope_end(wd_clock_scope_t *s) {
    assert(!atomic_load(&active->done));
    if(s->active) {
        ++restorations;
        if(restore_retry){restore_retry=false;return false;}
        s->active=false;
    }
    return true;
}
static void vTaskDelay(unsigned ticks) { assert(ticks==100&&!atomic_load(&active->done));delays++; }
void wd_ui_disconnect(void) { assert(!active->clock_scope.active);disconnects++; }
void wd_credentials_clear(wd_credentials_t *c) { assert(!active->clock_scope.active);memset(c,0,sizeof(*c)); }
static unsigned uxTaskGetStackHighWaterMark(void *p) { (void)p;return 1000; }
static unsigned esp_get_free_heap_size(void) { return 100000; }
static void vTaskSuspend(void *p) { assert(!p&&atomic_load(&active->done)&&!active->clock_scope.active);suspends++; }
'''
harness += task + r'''
int main(void) {
    const wd_job_t jobs[]={WD_JOB_UPLOAD,WD_JOB_VERIFY,WD_JOB_ALL};
    for(unsigned kind=0;kind<3;kind++)for(unsigned result=0;result<3;result++) {
        wd_ui_job_t j={.job=jobs[kind]};active=&j;
        outcome=result!=0;cancel_during_operation=result==2;restore_retry=true;
        unsigned old_restores=restorations,old_delays=delays;
        task(&j);
        assert(atomic_load(&j.done)&&j.success==(result==1));
        assert(restorations==old_restores+2&&delays==old_delays+1);
    }
    wd_ui_job_t offline={.job=WD_JOB_EXPORT};active=&offline;
    unsigned old=restorations;task(&offline);assert(offline.success&&restorations==old);
    assert(suspends==10&&disconnects==10);
    puts("Production job task: clock cleanup/retry before completion on HTTPS success, failure and cancellation; offline export takes no loan");
}
'''
with tempfile.TemporaryDirectory(prefix="wardrive-clock-jobs-") as tmp:
    c, exe = Path(tmp)/"jobs.c", Path(tmp)/"jobs"
    c.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Iapplications/poom_app_pack/src", "-Imodules/poom_wardrive/include",
        "-Imodules/poom_gps/include", "-Itools/wardrive_hostcheck/stubs",
        str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
