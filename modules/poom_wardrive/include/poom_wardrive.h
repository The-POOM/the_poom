// SPDX-License-Identifier: MIT
#pragma once
#include "poom_gps.h"
#include <stdio.h>
#define WD_RECORD_BYTES 192
#define WD_INDEX_BUCKETS 256
#ifndef WD_ROOT
#define WD_ROOT "/sdcard/wardrive"
#endif
typedef enum { WD_WIFI=0, WD_BLE=1, WD_COMBINED=2 } wd_mode_t;
typedef struct {
    uint8_t kind, address_type, address[6], name_length, name[32], adv_length, adv[31];
    int16_t rssi;
    uint16_t channel, frequency, security;
    uint64_t monotonic_ms;
    poom_gps_fix_t gps;
} wd_observation_t;
typedef struct {
    wd_mode_t mode;
    unsigned wifi_interval_ms, ble_window_ms, observation_interval_ms;
    int rssi_threshold;
} wd_settings_t;
typedef struct {
    FILE *journal, *index;
    char directory[160];
    uint32_t wifi_count, ble_count, observations, unlocated, write_errors, gps_count;
    wd_settings_t settings;
    uint64_t started_ms,ended_ms;
    int64_t clock_utc;
    uint64_t clock_ms;
    char start_utc[21],end_utc[21],receiver[24];
    bool fault, active, paused;
    uint8_t display_zone, display_source;
} wd_session_t;
void wd_settings_default(wd_settings_t *s);
bool wd_settings_valid(const wd_settings_t *s);
void wd_encode(const wd_observation_t *o,uint8_t out[WD_RECORD_BYTES]);
bool wd_decode(const uint8_t in[WD_RECORD_BYTES],wd_observation_t *o);
bool wd_session_start(wd_session_t *s,const char *root,const char *id,const wd_settings_t *settings);
/* Supply Wardrive-owned UTC; backfill a start before first GPS time. */
void wd_session_time(wd_session_t *s,int64_t utc,uint64_t now_ms);
bool wd_session_gps(wd_session_t *s,const poom_gps_fix_t *fix,uint64_t now);
bool wd_archive(const char *directory);
int64_t wd_gps_epoch(const poom_gps_fix_t *fix);
/* Eligibility at reception, before serialization, or after journal decode. */
bool wd_observation_exportable(const wd_observation_t *observation);
bool wd_session_observe(wd_session_t *s,const wd_observation_t *o);
bool wd_session_checkpoint(wd_session_t *s,const char *state);
bool wd_session_stop(wd_session_t *s);
/* Rebuild derived index and report from valid journal prefix. Partial/corrupt
 * tail remains in the original journal for inspection; no source deletion. */
bool wd_session_recover(const char *directory);
bool wd_export(const char *directory,const char *output,uint32_t *exported,uint32_t *skipped);
/* A session has a current WiGLE CSV when wigle.csv and its export.json
 * report both exist (export writes the report only after publishing the CSV).
 * Invalidation removes the derived wigle.csv, export.json and session.tar
 * (plus publish backups); never the journal, manifest or upload state. */
bool wd_export_ready(const char *directory);
bool wd_export_invalidate(const char *directory);
/* Located rows recorded in export.json by the last successful export. */
bool wd_export_rows(const char *directory,uint32_t *rows);
bool wd_export_cancellable(const char *directory,const char *output,uint32_t *exported,uint32_t *skipped,bool (*cancelled)(void *),void *context);
bool wd_safe_id(const char *id);
/* ESP-side asynchronous radio worker, polled by the app owner. */
void wd_radio_set_gps(const poom_gps_fix_t *fix);
void wd_radio_release(void);
bool wd_radio_start(const wd_settings_t *settings);
void wd_radio_pause(bool pause);
void wd_radio_stop(void);
bool wd_radio_finished(void);
bool wd_radio_next(wd_observation_t *out);
uint32_t wd_radio_dropped(void);
int wd_radio_error(void);
unsigned wd_radio_stack_free(void);


/* True only after all Wardrive-owned radio resources have been released. */
bool wd_radio_quiescent(void);
