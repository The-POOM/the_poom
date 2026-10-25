// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_DETECTOR_H
#define POOM_WIFI_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_WIFI_DETECTOR_SSID_LEN (33U)
#define POOM_WIFI_DETECTOR_TEXT_LEN (24U)

typedef enum
{
    POOM_WIFI_DETECTOR_FILTER_DEVICES = 0,
    POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS,
    POOM_WIFI_DETECTOR_FILTER_FLOCK_ALPR,
    POOM_WIFI_DETECTOR_FILTER_IP_CAMERAS,
    POOM_WIFI_DETECTOR_FILTER_COUNT,
} poom_wifi_detector_filter_t;

typedef enum
{
    POOM_WIFI_DETECTOR_CLASS_UNKNOWN = 0,
    POOM_WIFI_DETECTOR_CLASS_FLOCK,
    POOM_WIFI_DETECTOR_CLASS_FLOCK_MANUFACTURER,
    POOM_WIFI_DETECTOR_CLASS_ALPR,
    POOM_WIFI_DETECTOR_CLASS_SOUNDTHINKING,
    POOM_WIFI_DETECTOR_CLASS_IP_CAMERA,
} poom_wifi_detector_class_t;

typedef enum
{
    POOM_WIFI_DETECTOR_CONFIDENCE_INFO = 0,
    POOM_WIFI_DETECTOR_CONFIDENCE_POSSIBLE,
    POOM_WIFI_DETECTOR_CONFIDENCE_PROBABLE,
    POOM_WIFI_DETECTOR_CONFIDENCE_HIGH,
} poom_wifi_detector_confidence_t;

typedef struct
{
    char ssid[POOM_WIFI_DETECTOR_SSID_LEN];
    char vendor[POOM_WIFI_DETECTOR_TEXT_LEN];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth_mode;
    bool rssi_known;
    bool ssid_hidden;
    bool is_access_point;
    bool auth_known;
    uint32_t last_seen_ms;
    poom_wifi_detector_class_t device_class;
    poom_wifi_detector_confidence_t confidence;
} poom_wifi_detector_record_t;

esp_err_t poom_wifi_detector_scan_channel(poom_wifi_detector_filter_t filter,
                                          uint8_t channel,
                                          poom_wifi_detector_record_t *const *out_records,
                                          size_t capacity,
                                          size_t *out_count);

/** Classifies one already-scanned AP using the shared Flock/ALPR/camera rules.
 * This copies only into caller storage and has no persistent allocation. */
void poom_wifi_detector_classify_ap(const wifi_ap_record_t *ap,
                                    poom_wifi_detector_record_t *out_record);

/**
 * Starts passive promiscuous monitoring for a classified detector filter.
 * The optional seed records normally come from one active AP scan.
 */
esp_err_t poom_wifi_detector_monitor_start(
    poom_wifi_detector_filter_t filter,
    poom_wifi_detector_record_t *const *seed_records,
    size_t seed_count,
    size_t capacity);

/** Copies the current passive-monitor records into caller-owned storage. */
esp_err_t poom_wifi_detector_monitor_snapshot(
    poom_wifi_detector_record_t *const *out_records,
    size_t capacity,
    size_t *out_count);

/** Channel 0 resumes hopping; target_mac protects the device held in detail. */
esp_err_t poom_wifi_detector_monitor_set_channel(uint8_t channel,
                                                 const uint8_t target_mac[6]);

/** Selects an AP and clears the monitor list to begin passive client tracking. */
esp_err_t poom_wifi_detector_monitor_select_ap(uint8_t channel,
                                               const uint8_t bssid[6]);

/** Clears the selected AP and resumes passive AP discovery. */
esp_err_t poom_wifi_detector_monitor_discover_aps(void);
esp_err_t poom_wifi_detector_monitor_stop(void);

esp_err_t poom_wifi_detector_stop(void);

const char *poom_wifi_detector_filter_label(poom_wifi_detector_filter_t filter);
const char *poom_wifi_detector_auth_label(uint8_t auth_mode);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_DETECTOR_H */
