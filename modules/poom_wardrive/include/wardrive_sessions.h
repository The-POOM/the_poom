// SPDX-License-Identifier: MIT
#pragma once
#include "poom_wardrive.h"
#include "wardrive_timezone.h"
typedef struct {
    char id[49], utc[21], label[20], detail[22];
    int64_t epoch;
    wd_timezone_t display_zone;
    uint32_t observations;
    bool count_known;
    char state[16];
} wd_session_info_t;
typedef struct {
    wd_session_info_t entries[4];
    unsigned count, selected_rank;
} wd_session_page_t;
bool wd_session_id(char out[49],int64_t utc,uint32_t boot_ms,uint32_t random);
bool wd_session_info(const char *root,const char *id,wd_session_info_t *info);
bool wd_session_info_local(const char *root,const char *id,wd_timezone_t fallback,wd_session_info_t *info);
bool wd_session_time_text(const wd_session_info_t *info,char *out,size_t capacity);
/* Bounded-memory, newest-first browsing. Direction -1/newer, 0/current,
 * +1/older. Unknown UTC sorts after dated sessions with deterministic ID ties. */
bool wd_session_page(const char *root,const char *cursor,int direction,wd_session_page_t *page);
/* The adjacent session in list order (+1 older, -1 newer) in one directory
 * pass; false at either end. Lets the list scroll one row without a page scan. */
bool wd_session_neighbor(const char *root,const char *cursor,int direction,wd_timezone_t fallback,wd_session_info_t *out);
bool wd_session_page_local(const char *root,const char *cursor,int direction,wd_timezone_t fallback,wd_session_page_t *page);

/* Optional browser snapshot. Metadata is read/sorted once, then page movement
 * does not scan directories or reopen manifests. Bounded to 256 KiB; callers
 * can retain the constant-memory pager if loading fails or the list is larger.
 * Zero-initialize before first use and close when leaving the browser. */
#define WD_SESSION_CACHE_BYTES (256U * 1024U)
typedef struct {
    wd_session_info_t *entries;
    unsigned count;
    bool ready;
} wd_session_cache_t;
bool wd_session_cache_load(const char *root,wd_timezone_t fallback,wd_session_cache_t *cache);
void wd_session_cache_close(wd_session_cache_t *cache);
void wd_session_cache_page(const wd_session_cache_t *cache,const char *cursor,wd_session_page_t *page);
