// SPDX-License-Identifier: MIT
#pragma once
#include "wardrive_connect.h"
#include "wardrive_clock.h"
#include <stdatomic.h>
typedef enum { WD_JOB_NONE,WD_JOB_TRANSFER,WD_JOB_EXPORT,WD_JOB_RECOVER,WD_JOB_ARCHIVE,WD_JOB_UPLOAD,WD_JOB_ALL,WD_JOB_VERIFY,WD_JOB_WIFI_SCAN,WD_JOB_WIFI_JOIN } wd_job_t;
typedef struct {
    atomic_bool busy,done,cancel;
    wd_job_t job;
    wd_credentials_t credentials;
    char directory[160],message[64],ip[20],token[33];
    bool success,credentials_rejected;
    unsigned processed,failed,skipped;
    wd_timezone_t display_zone;
    wd_clock_t clock;
    wd_clock_scope_t clock_scope;
    /* Live progress for the UI (worker writes, UI reads): upload stage,
     * request bytes written/total for the current run, and for bulk the
     * 1-based run number, eligible runs and running results. */
    atomic_uint stage,sent,total,index,count,submitted,failures;
} wd_ui_job_t;
bool wd_ui_job_start(wd_ui_job_t *job);
void wd_ui_disconnect(void);
/* True while Wardrive owns a station link that has an IP address. */
bool wd_ui_link_up(void);


/* Join the completed worker before reusing its job state or returning to stock. */
void wd_ui_job_reap(void);
