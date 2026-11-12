// SPDX-License-Identifier: MIT
#pragma once
#include "poom_gps.h"
#include <time.h>

/* Stable values: persisted non-secret configuration and session metadata. */
typedef enum {
    WD_TZ_UTC, WD_TZ_SYDNEY, WD_TZ_MELBOURNE, WD_TZ_BRISBANE,
    WD_TZ_ADELAIDE, WD_TZ_DARWIN, WD_TZ_PERTH, WD_TZ_HOBART,
    WD_TZ_EUCLA, WD_TZ_LORD_HOWE, WD_TZ_NORFOLK, WD_TZ_AUCKLAND,
    WD_TZ_CHATHAM, WD_TZ_LONDON, WD_TZ_BERLIN, WD_TZ_NEW_YORK,
    WD_TZ_CHICAGO, WD_TZ_DENVER, WD_TZ_LOS_ANGELES, WD_TZ_PHOENIX,
    WD_TZ_HONOLULU, WD_TZ_TOKYO, WD_TZ_SINGAPORE, WD_TZ_KOLKATA,
    WD_TZ_KATHMANDU, WD_TZ_BROKEN_HILL, WD_TZ_COUNT
} wd_timezone_t;
typedef enum { WD_TZ_FALLBACK, WD_TZ_GPS, WD_TZ_MANUAL, WD_TZ_LAST_GPS } wd_timezone_source_t;
typedef struct { uint8_t version, automatic, manual_zone, reserved; } wd_timezone_config_t;
typedef struct {
    wd_timezone_config_t config;
    wd_timezone_t zone;
    wd_timezone_source_t source;
    bool have_gps_zone;
} wd_timezone_state_t;
typedef struct {
    struct tm calendar;
    int offset_minutes;
    const char *abbreviation;
    bool daylight;
} wd_local_time_t;

void wd_timezone_default(wd_timezone_config_t *config);
bool wd_timezone_config_valid(const wd_timezone_config_t *config);
void wd_timezone_init(wd_timezone_state_t *state,const wd_timezone_config_t *config);
void wd_timezone_update(wd_timezone_state_t *state,const poom_gps_fix_t *fix,uint64_t now_ms);
bool wd_timezone_detect(double latitude,double longitude,wd_timezone_t *zone);
const char *wd_timezone_id(wd_timezone_t zone);
const char *wd_timezone_name(wd_timezone_t zone);
const char *wd_timezone_source(wd_timezone_source_t source);
bool wd_timezone_from_id(const char *id,wd_timezone_t *zone);
/* No TZ environment variable, tzset(), localtime() or global clock mutation. */
bool wd_timezone_local(int64_t utc,wd_timezone_t zone,wd_local_time_t *local);
