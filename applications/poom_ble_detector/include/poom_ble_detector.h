// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_DETECTOR_H
#define POOM_BLE_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_BLE_DETECTOR_NAME_LEN (24U)
#define POOM_BLE_DETECTOR_TEXT_LEN (24U)
#define POOM_BLE_DETECTOR_ADV_LEN  (62U)

typedef enum
{
    POOM_BLE_DETECTOR_FILTER_DEVICES = 0,
    POOM_BLE_DETECTOR_FILTER_TRACKERS,
    POOM_BLE_DETECTOR_FILTER_WEARABLES,
    POOM_BLE_DETECTOR_FILTER_FINDMY,
    POOM_BLE_DETECTOR_FILTER_IBEACON,
    POOM_BLE_DETECTOR_FILTER_FLIPPER,
    POOM_BLE_DETECTOR_FILTER_FLOCK,
    POOM_BLE_DETECTOR_FILTER_META,
    POOM_BLE_DETECTOR_FILTER_HC_NAMES,
    POOM_BLE_DETECTOR_FILTER_ANALYZER,
    POOM_BLE_DETECTOR_FILTER_COUNT,
} poom_ble_detector_filter_t;

typedef enum
{
    POOM_BLE_DETECTOR_CLASS_UNKNOWN = 0,
    POOM_BLE_DETECTOR_CLASS_AIRTAG,
    POOM_BLE_DETECTOR_CLASS_SMARTTAG,
    POOM_BLE_DETECTOR_CLASS_TILE,
    POOM_BLE_DETECTOR_CLASS_OTHER_TRACKER,
    POOM_BLE_DETECTOR_CLASS_SMART_GLASSES,
    POOM_BLE_DETECTOR_CLASS_BODY_CAMERA,
} poom_ble_detector_class_t;

typedef enum
{
    POOM_DETECT_CONFIDENCE_INFO = 0,
    POOM_DETECT_CONFIDENCE_POSSIBLE,
    POOM_DETECT_CONFIDENCE_PROBABLE,
    POOM_DETECT_CONFIDENCE_HIGH,
} poom_detect_confidence_t;

/* Advertising signatures are candidates, not authenticated device identities. */
enum {
    POOM_BLE_SIGNATURE_FINDMY = 1U << 0,
    POOM_BLE_SIGNATURE_FMNA = 1U << 1,
    POOM_BLE_SIGNATURE_DULT = 1U << 2,
    POOM_BLE_SIGNATURE_IBEACON = 1U << 3,
    POOM_BLE_SIGNATURE_FLIPPER = 1U << 4,
    POOM_BLE_SIGNATURE_FLOCK = 1U << 5,
    POOM_BLE_SIGNATURE_META = 1U << 6,
    POOM_BLE_SIGNATURE_HC_NAME = 1U << 7,
    POOM_BLE_SIGNATURE_LEGACY_FINDMY = 1U << 8,
};
#define POOM_BLE_FINDMY_SIGNATURES (POOM_BLE_SIGNATURE_FINDMY | POOM_BLE_SIGNATURE_FMNA | \
                                 POOM_BLE_SIGNATURE_DULT | POOM_BLE_SIGNATURE_LEGACY_FINDMY)

typedef struct
{
    char name[POOM_BLE_DETECTOR_NAME_LEN];
    char vendor[POOM_BLE_DETECTOR_TEXT_LEN];
    uint8_t mac_address[6];
    int rssi;
    float distance_m;
    poom_ble_detector_class_t device_class;
    poom_detect_confidence_t confidence;
    uint8_t adv_data[POOM_BLE_DETECTOR_ADV_LEN];
    uint8_t adv_data_length;
    uint8_t primary_adv_length;
    uint16_t signatures;
    uint16_t identifier;
    uint8_t ibeacon_uuid[16];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t ibeacon_power;
    char flock_serial[16];
    uint32_t last_seen_ms;
    uint32_t reports;
} poom_ble_detector_record_t;

typedef struct {
    uint32_t reports;
    uint32_t matched_reports;
    uint32_t elapsed_ms;
    uint32_t rate_reports;
    uint32_t rate_window_ms;
    int rssi_min;
    int rssi_max;
    int rssi_last;
} poom_ble_detector_stats_t;

typedef void (*poom_ble_detector_cb_t)(const poom_ble_detector_record_t *record);

/* NULL acknowledges that in-flight record callbacks have returned, allowing
 * their owner to release/reuse memory. Call start/stop/register from an owner
 * task, not from inside the record callback itself. */
void poom_ble_detector_register_cb(poom_ble_detector_cb_t callback);
esp_err_t poom_ble_detector_start(poom_ble_detector_filter_t filter);
esp_err_t poom_ble_detector_stop(void);
void poom_ble_detector_get_stats(poom_ble_detector_stats_t *stats);

bool poom_ble_detector_parse(const uint8_t *adv_data,
                             size_t adv_len,
                             const uint8_t mac_address[6],
                             int rssi,
                             poom_ble_detector_record_t *out_record);

/* AD fields cannot cross the advertising/scan-response boundary. Each legacy
 * part is at most 31 bytes; malformed trailing fields stop only that part. */
bool poom_ble_detector_parse_parts(const uint8_t *adv_data, size_t adv_len,
                                   const uint8_t *response_data, size_t response_len,
                                   const uint8_t mac_address[6], int rssi,
                                   poom_ble_detector_record_t *out_record);

bool poom_ble_detector_record_matches_filter(const poom_ble_detector_record_t *record,
                                             poom_ble_detector_filter_t filter);

const char *poom_ble_detector_filter_label(poom_ble_detector_filter_t filter);
const char *poom_ble_detector_class_label(poom_ble_detector_class_t device_class);
const char *poom_detect_confidence_label(poom_detect_confidence_t confidence);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_DETECTOR_H */
