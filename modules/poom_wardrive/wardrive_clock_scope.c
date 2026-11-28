// SPDX-License-Identifier: MIT
#include "wardrive_debug.h"
#include "wardrive_clock.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include <stdio.h>

static int64_t wall_us(const struct timeval *tv)
{
    return (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
}
static int set_wall_us(int64_t us)
{
    struct timeval tv = {.tv_sec = (time_t)(us / 1000000LL), .tv_usec = (suseconds_t)(us % 1000000LL)};
    return settimeofday(&tv, NULL);
}
bool wd_clock_scope_begin(wd_clock_scope_t *scope, int64_t utc)
{
    if(!scope) return false;
    if(scope->active) return true;
    scope->error = WD_CLOCK_READY;
    struct timeval current;
    if(gettimeofday(&current, NULL) != 0) { scope->error = WD_CLOCK_IO_ERROR; return false; }
    if(wd_clock_valid((int64_t)current.tv_sec)) return true; /* No loan needed. */
    if(!wd_clock_valid(utc)) { scope->error = WD_CLOCK_NEEDS_GPS; return false; }
    if(esp_sntp_enabled()) { scope->error = WD_CLOCK_SYNC_BUSY; return false; }
    scope->saved_wall_us = wall_us(&current);
    scope->started_us = esp_timer_get_time();
    scope->loan_wall_us = utc * 1000000LL;
    if(set_wall_us(scope->loan_wall_us) != 0) { scope->error = WD_CLOCK_IO_ERROR; return false; }
    scope->active = true;
    WD_DEBUG("[wardrive clock] TLS time borrowed from GPS\n");
    return true;
}
bool wd_clock_scope_end(wd_clock_scope_t *scope)
{
    if(!scope) return false;
    if(!scope->active) return true;
    int64_t now = esp_timer_get_time();
    if(now < scope->started_us) return false;
    int64_t elapsed = now - scope->started_us;
    struct timeval current;
    if(gettimeofday(&current, NULL) != 0) return false;
    int64_t difference = wall_us(&current) - (scope->loan_wall_us + elapsed);
    /* Do not overwrite another owner's time update or newly started service.
     * Small differences allow for the two clock reads and scheduler latency. */
    if(!esp_sntp_enabled() && difference >= -250000 && difference <= 250000) {
        if(set_wall_us(scope->saved_wall_us + elapsed) != 0) return false;
        WD_DEBUG("[wardrive clock] previous timeline restored\n");
    } else WD_DEBUG("[wardrive clock] external time owner preserved\n");
    scope->active = false;
    return true;
}
