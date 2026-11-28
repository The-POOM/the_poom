// SPDX-License-Identifier: MIT
#include "wardrive_clock.h"
#include "poom_wardrive.h"

bool wd_clock_valid(int64_t utc) { return utc >= 1704067200LL && utc < 4102444800LL; }

bool wd_clock_update_gps(wd_clock_t *clock, const poom_gps_fix_t *fix, uint64_t now_ms)
{
    if(!clock || !fix || !fix->connected) return false;
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if(fix->month < 1 || fix->month > 12) return false;
    unsigned max_day = days[fix->month - 1];
    if(fix->month == 2 && fix->year % 4 == 0 && (fix->year % 100 != 0 || fix->year % 400 == 0)) ++max_day;
    if(fix->day < 1 || fix->day > max_day) return false;
    int64_t utc = wd_gps_epoch(fix);
    if(!wd_clock_valid(utc)) return false;
    /* Do not reset the sub-second anchor on every frame for the same sentence. */
    if(clock->utc != utc || now_ms < clock->monotonic_ms) {
        clock->utc = utc;
        clock->monotonic_ms = now_ms;
    }
    return true;
}

int64_t wd_clock_now(const wd_clock_t *clock, uint64_t now_ms, int64_t system_utc)
{
    if(clock && wd_clock_valid(clock->utc) && now_ms >= clock->monotonic_ms &&
       now_ms - clock->monotonic_ms <= 86400000ULL) {
        int64_t utc = clock->utc + (int64_t)((now_ms - clock->monotonic_ms) / 1000);
        if(wd_clock_valid(utc)) return utc;
    }
    return wd_clock_valid(system_utc) ? system_utc : 0;
}
