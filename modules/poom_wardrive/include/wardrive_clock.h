// SPDX-License-Identifier: MIT
#pragma once
#include "poom_gps.h"
#include <stdbool.h>
#include <stdint.h>

/* App-owned UTC anchor, advanced only by monotonic elapsed time. */
typedef struct { int64_t utc; uint64_t monotonic_ms; } wd_clock_t;
bool wd_clock_valid(int64_t utc);
bool wd_clock_update_gps(wd_clock_t *clock, const poom_gps_fix_t *fix, uint64_t now_ms);
int64_t wd_clock_now(const wd_clock_t *clock, uint64_t now_ms, int64_t system_utc);

/* A bounded loan of GPS UTC to the platform TLS verifier. No SNTP service,
 * timezone or TLS verification settings are changed. */
typedef enum { WD_CLOCK_READY, WD_CLOCK_NEEDS_GPS, WD_CLOCK_SYNC_BUSY, WD_CLOCK_IO_ERROR } wd_clock_error_t;
typedef struct {
    bool active;
    wd_clock_error_t error;
    int64_t saved_wall_us, started_us, loan_wall_us;
} wd_clock_scope_t;
bool wd_clock_scope_begin(wd_clock_scope_t *scope, int64_t utc);
bool wd_clock_scope_end(wd_clock_scope_t *scope);
