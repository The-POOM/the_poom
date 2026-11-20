// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define GPS_CAPTURE_MAX_BYTES (4U*1024U*1024U)
#define GPS_CAPTURE_MAX_MS (20U*60U*1000U)
typedef struct {
    FILE *file;
    uint64_t started_ms,last_sync_ms;
    uint32_t bytes,sentences,gsv,checksum_bad;
    bool fault,complete;
    char path[192];
} poom_gps_capture_t;
bool poom_gps_capture_start(poom_gps_capture_t *c,const char *path,uint64_t now);
bool poom_gps_capture_append(poom_gps_capture_t *c,const char *sentence,bool parsed,uint64_t now);
bool poom_gps_capture_poll(poom_gps_capture_t *c,uint64_t now);
bool poom_gps_capture_stop(poom_gps_capture_t *c);
