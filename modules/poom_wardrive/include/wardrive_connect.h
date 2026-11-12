// SPDX-License-Identifier: MIT
#pragma once
#include "poom_wardrive.h"
#include "wardrive_timezone.h"
#include "esp_err.h"
typedef struct {
    char ssid[33],password[65],api_name[65],api_token[129];
    bool auto_upload;
} wd_credentials_t;
#define WD_TRANSFER_CODE_DIGITS 10
bool wd_credentials_parse(const char *json,size_t length,wd_credentials_t *out);
/* Import updates only supplied groups; preserves chosen WiFi for API-only files.
 * Supplied WiFi replaces that group (omitted password means open network).
 * UI must obtain fresh consent before honouring imported auto_upload=true. */
bool wd_credentials_import(wd_credentials_t *out);
void wd_credentials_clear(wd_credentials_t *c);
/* Persist only the WiGLE API name/token in the poom_nvs secrets namespace
 * (same storage and protection level as saved WiFi profiles; not encrypted at
 * rest). WiFi credentials and auto_upload are never persisted here. Values are
 * never logged. load() fills only api_name/api_token; true when both exist. */
esp_err_t wd_credentials_save(const wd_credentials_t *c);
bool wd_credentials_load(wd_credentials_t *c);
void wd_credentials_forget(void);
bool wd_transfer_start(char token[33]);
void wd_transfer_set_timezone(wd_timezone_t zone);
void wd_transfer_stop(void);
bool wd_transfer_expired(void);
typedef enum { WD_PENDING,WD_UPLOADING,WD_SUBMITTED,WD_CONFIRMED,WD_FAILED,WD_NEEDS_REVIEW } wd_upload_state_t;
typedef enum { WD_UPLOAD_OK,WD_UPLOAD_CONNECT,WD_UPLOAD_AUTH,WD_UPLOAD_RATE_LIMIT,WD_UPLOAD_FILE,WD_UPLOAD_RESPONSE,WD_UPLOAD_INTERRUPTED,WD_UPLOAD_DNS,WD_UPLOAD_CERT_TIME,WD_UPLOAD_CERT,WD_UPLOAD_MEMORY,WD_UPLOAD_TIMEOUT,WD_UPLOAD_HEADERS } wd_upload_problem_t;
const char *wd_upload_problem_text(wd_upload_problem_t problem);
typedef struct {
    wd_upload_state_t state;
    wd_upload_problem_t problem;
    unsigned attempts;
    char transaction[80];
    uint64_t retry_after;
    int net_error,tls_error,tls_code,tls_flags,socket_error;
    /* may_have_sent: the last failure happened after request bytes were
     * written, so WiGLE might hold a copy (kept for troubleshooting; the UI
     * shows Failed). manual: Submitted was set by the user, not by WiGLE. */
    bool may_have_sent,manual;
} wd_upload_status_t;
/* Simplified user-facing status. Legacy Confirmed counts as Submitted and
 * legacy Needs Review/interrupted uploads count as Failed. */
typedef enum { WD_SHOWN_PENDING,WD_SHOWN_SUBMITTED,WD_SHOWN_FAILED } wd_upload_shown_t;
static inline wd_upload_shown_t wd_upload_shown(wd_upload_state_t state) {
    return state==WD_SUBMITTED||state==WD_CONFIRMED?WD_SHOWN_SUBMITTED:state==WD_PENDING?WD_SHOWN_PENDING:WD_SHOWN_FAILED;
}
typedef enum { WD_STAGE_IDLE,WD_STAGE_PREPARING,WD_STAGE_CONNECTING,WD_STAGE_SENDING,WD_STAGE_AWAITING,WD_STAGE_COMPLETE,WD_STAGE_FAILED } wd_upload_stage_t;
/* Reports real request bytes written (multipart head + CSV + trailer). */
typedef void (*wd_upload_progress_fn)(void *context,wd_upload_stage_t stage,size_t sent,size_t total);
bool wd_upload_status_read(const char *directory,wd_upload_status_t *s);
bool wd_upload_status_write(const char *directory,const wd_upload_status_t *s);
/* Uploads the session's existing wigle.csv. Accepts Pending, Failed and
 * legacy Needs Review (explicit user retry); never Submitted/Confirmed.
 * Any outcome without WiGLE's success acknowledgment is persisted as Failed
 * with backoff and the transport details. Caller must stop radio/transfer and
 * obtain a trusted STA link first. verify only queries the transaction list. */
bool wd_upload_one(const char *directory,const wd_credentials_t *credentials,bool verify);
bool wd_upload_one_cancellable(const char *directory,const wd_credentials_t *credentials,bool verify,bool (*cancelled)(void *),void *context);
bool wd_upload_one_progress(const char *directory,const wd_credentials_t *credentials,bool (*cancelled)(void *),void *context,wd_upload_progress_fn progress,void *progress_context);
