// SPDX-License-Identifier: MIT
/* Real private clock and scoped TLS loan with synthetic platform time. */
#include "wardrive_clock.h"
#include <assert.h>
#include <stdio.h>
#include <sys/time.h>

static int64_t monotonic, platform_wall;
static unsigned writes;
static bool sntp, fail_read, fail_write;
int64_t esp_timer_get_time(void) { return monotonic; }
bool esp_sntp_enabled(void) { return sntp; }
int gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    if(fail_read) return -1;
    tv->tv_sec = platform_wall / 1000000;
    tv->tv_usec = platform_wall % 1000000;
    return 0;
}
int settimeofday(const struct timeval *tv, const struct timezone *zone)
{
    (void)zone;
    if(fail_write) return -1;
    platform_wall = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    ++writes;
    return 0;
}
static void advance(int64_t us) { monotonic += us; platform_wall += us; }

int main(void)
{
    wd_clock_t clock = {0};
    poom_gps_fix_t fix = {.connected=true,.fields=GPS_DATE|GPS_TIME,
        .year=2026,.month=11,.day=11,.hour=23,.minute=59,.second=59};
    assert(!wd_clock_now(&clock,1000,0));
    assert(wd_clock_update_gps(&clock,&fix,1000));
    int64_t gps = clock.utc;
    assert(wd_clock_now(&clock,3000,0)==gps+2); /* Includes midnight rollover. */
    assert(wd_clock_update_gps(&clock,&fix,1100)&&clock.monotonic_ms==1000);
    fix.connected=false;assert(!wd_clock_update_gps(&clock,&fix,2000));
    assert(wd_clock_now(&clock,900,0)==0); /* Monotonic rollback fails closed. */
    assert(wd_clock_now(&clock,86402000,0)==0); /* Bounded holdover. */
    assert(wd_clock_now(&clock,86402000,gps)==gps); /* Valid platform fallback. */
    fix.connected=true;fix.month=2;fix.day=30;assert(!wd_clock_update_gps(&clock,&fix,3000));
    fix.day=29;assert(!wd_clock_update_gps(&clock,&fix,3000));
    fix.year=2028;assert(wd_clock_update_gps(&clock,&fix,3000));
    fix.fields=GPS_TIME;assert(!wd_clock_update_gps(&clock,&fix,4000));
    assert(writes==0&&platform_wall==0); /* Local display never sets platform time. */

    wd_clock_scope_t scope={0};
    platform_wall=12345678;monotonic=5000000;
    assert(!wd_clock_scope_begin(&scope,0)&&!scope.active&&!writes);
    sntp=true;assert(!wd_clock_scope_begin(&scope,gps)&&!scope.active);sntp=false;
    fail_read=true;assert(!wd_clock_scope_begin(&scope,gps));fail_read=false;
    fail_write=true;assert(!wd_clock_scope_begin(&scope,gps)&&!scope.active);fail_write=false;
    assert(wd_clock_scope_begin(&scope,gps)&&scope.active&&platform_wall==gps*1000000);
    unsigned before=writes;assert(wd_clock_scope_begin(&scope,gps)&&writes==before);
    advance(7500123);
    fail_write=true;assert(!wd_clock_scope_end(&scope)&&scope.active);fail_write=false;
    assert(wd_clock_scope_end(&scope)&&!scope.active&&platform_wall==12345678+7500123);
    before=writes;assert(wd_clock_scope_end(&scope)&&writes==before); /* Idempotent. */

    /* Success, request failure and cancellation all use the same return path. */
    for(unsigned outcome=0;outcome<3;outcome++) {
        int64_t saved=platform_wall;
        assert(wd_clock_scope_begin(&scope,gps));advance(3000000);
        assert(wd_clock_scope_end(&scope)&&platform_wall==saved+3000000);
    }
    platform_wall=gps*1000000;sntp=true;before=writes;
    assert(wd_clock_scope_begin(&scope,gps)&&!scope.active&&writes==before);sntp=false;
    platform_wall=0;assert(wd_clock_scope_begin(&scope,gps));
    advance(1000000);platform_wall+=3600000000LL;
    int64_t external=platform_wall;
    assert(wd_clock_scope_end(&scope)&&platform_wall==external); /* Preserve external update. */
    platform_wall=0;assert(wd_clock_scope_begin(&scope,gps));sntp=true;before=writes;
    assert(wd_clock_scope_end(&scope)&&writes==before);sntp=false;
    puts("Private clock: GPS/date validation, local display/holdover, TLS loan, elapsed-time restoration, failure retry and external owner protection passed");
}
