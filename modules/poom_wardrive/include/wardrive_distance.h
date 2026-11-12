// SPDX-License-Identifier: MIT
#pragma once
#include "poom_gps.h"

typedef struct {
    double metres, anchor_latitude, anchor_longitude;
    uint64_t first_ms, last_ms;
    unsigned samples;
    bool anchored;
} wd_distance_t;

/* Break continuity on pause/fix loss; retain the session's estimated total. */
void wd_distance_break(wd_distance_t *distance);
/* Conservative display estimate only. Does not change recorded GPS positions. */
void wd_distance_update(wd_distance_t *distance,const poom_gps_fix_t *fix,uint64_t now);
