// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
typedef enum {
    WD_SCREEN_MAIN,WD_SCREEN_LIVE,WD_SCREEN_GPS,WD_SCREEN_STATS,WD_SCREEN_STOP,
    WD_SCREEN_SAVED,WD_SCREEN_SESSION,WD_SCREEN_UPLOAD_CHOICE,WD_SCREEN_TRANSFER_FORMAT,WD_SCREEN_OPTIONS,
    WD_SCREEN_NOTICE,WD_SCREEN_TRANSFER,WD_SCREEN_TRANSFER_READY,WD_SCREEN_WORKING,WD_SCREEN_PROGRESS,
    WD_SCREEN_FILE_RESULT,WD_SCREEN_ARCHIVE_RESULT,WD_SCREEN_UPLOAD_MENU,WD_SCREEN_BULK_MENU,WD_SCREEN_BULK_HOLD,
    WD_SCREEN_CONSENT,WD_SCREEN_UPLOAD_RESULT,WD_SCREEN_API,WD_SCREEN_IMPORT,WD_SCREEN_DELETE_SETUP,
    WD_SCREEN_KEYBOARD,WD_SCREEN_WIFI_SETTINGS,WD_SCREEN_WIFI_CHOOSE,WD_SCREEN_WIFI_PROFILES,WD_SCREEN_WIFI_NETWORKS,
    WD_SCREEN_WIFI_SCAN_ERROR,WD_SCREEN_WIFI_ERROR,WD_SCREEN_WIFI_SAVE,WD_SCREEN_SETTINGS,WD_SCREEN_TIMEZONE,
    WD_SCREEN_SAVE_CHANGES,WD_SCREEN_COUNT
} wd_screen_t;
static inline bool wd_ui_is_live_page(unsigned screen) {
    return screen==WD_SCREEN_LIVE||screen==WD_SCREEN_GPS||screen==WD_SCREEN_STATS;
}
/* Live-session carousel: Active Session -> GPS Status -> Session Stats. */
static inline unsigned wd_ui_session_page(unsigned current,bool right) {
    if(current==WD_SCREEN_LIVE) return right?WD_SCREEN_GPS:WD_SCREEN_STATS;
    if(current==WD_SCREEN_GPS) return right?WD_SCREEN_STATS:WD_SCREEN_LIVE;
    if(current==WD_SCREEN_STATS) return right?WD_SCREEN_LIVE:WD_SCREEN_GPS;
    return current;
}
static inline unsigned wd_ui_gps_page(unsigned current,bool down) {
    return (current+(down?1U:2U))%3U;
}

static inline unsigned wd_ui_session_back(unsigned current) {
    return wd_ui_is_live_page(current)?WD_SCREEN_MAIN:current;
}
