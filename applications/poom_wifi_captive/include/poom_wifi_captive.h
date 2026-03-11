// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_CAPTIVE_H
#define POOM_WIFI_CAPTIVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/**
 * @file poom_wifi_captive.h
 * @brief Public API for captive portal module (AP+STA + HTTP + DNS).
 */

/* =========================
 * Logging control
 * ========================= */

/** SD folder where HTML portal files are searched. */
#define CAPTIVE_PORTALS_FOLDER_PATH              "/portals"
/** SD folder where captured user data and STA credentials are stored. */
#define CAPTIVE_DATAUSER_PATH                    "/captive_data"

/** Relative SD folder name for portal HTML pages. */
#define CAPTIVE_PORTAL_PATH_NAME                 "portals"
/** Relative endpoint name for post-capture redirect page. */
#define CAPTIVE_PORTAL_REDIRECT_PATH_NAME        "redirect"
/** Default root portal filename. */
#define CAPTIVE_PORTAL_DEFAULT_NAME              "root.html"
/** Default redirect filename. */
#define CAPTIVE_PORTAL_REDIRECT_DEFAULT_NAME     "redirect.html"
#define CAPTIVE_PORTAL_LIMIT_PORTALS             (20)
#define CAPTIVE_PORTAL_MAX_DEFAULT_LEN           (24)
#define CAPTIVE_PORTAL_MODE_FS_KEY               "cpmode"
#define CAPTIVE_PORTAL_FS_KEY                    "cpportal"
#define CAPTIVE_PORTAL_REDIRECT_FS_KEY           "cpredirect"
#define CAPTIVE_PORTAL_PREF_FS_KEY               "cpprefs"
#define CAPTIVE_PORTAL_CHANNEL                   "cpchan"
#define CAPTIVE_PORTAL_NET_NAME                  "WIFI_AP_DEF"
#define CAPTIVE_PORTAL_FS_NAME                   "cpname"
#define CAPTIVE_PORTAL_MAX_NAME                  (32)

/** Query key used for first captured value. */
#define CAPTIVE_USER_INPUT1                      "user1"
/** Query key used for second captured value. */
#define CAPTIVE_USER_INPUT2                      "user2"
/** Query key used for third captured value. */
#define CAPTIVE_USER_INPUT3                      "user3"
/** Query key used for fourth captured value. */
#define CAPTIVE_USER_INPUT4                      "user4"

#define CAPTIVE_DATA_FILENAME                    "user_creds.txt"
#define SSID_DATA_FILENAME                       "ssid.txt"

/** Relative SD path where captured values are appended. */
#define CAPTIVE_DATA_PATH                        CAPTIVE_DATAUSER_PATH "/" CAPTIVE_DATA_FILENAME
/** Relative SD path where STA `ssid,password` is loaded from. */
#define SSID_DATA_PATH                           CAPTIVE_DATAUSER_PATH "/" SSID_DATA_FILENAME

/**
 * @brief Start captive module.
 */
void poom_wifi_captive_start(void);

/**
 * @brief Stop captive module and free resources.
 */
void poom_wifi_captive_stop(void);

/**
 * @brief Set portal HTML filename to serve from SD folder.
 *
 * @param filename File name (e.g. "root.html").
 */
void poom_wifi_captive_set_portal_file(const char *filename);

/**
 * @brief Override SoftAP clone SSID and auth mode used by captive start.
 *
 * @param[in] ssid AP SSID override. NULL or empty resets to default behavior.
 * @param[in] open_auth true forces OPEN AP (no password), false keeps normal auth selection.
 */
void poom_wifi_captive_set_ap_clone(const char *ssid, bool open_auth);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_CAPTIVE_H */
