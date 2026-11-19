// SPDX-License-Identifier: MIT
// Native bounded observation model; Marauder-derived signatures are documented in PROVENANCE.md.
#ifndef POOM_WIFI_OBSERVER_MODEL_H
#define POOM_WIFI_OBSERVER_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define POOM_OBSERVER_APS 32U
#define POOM_OBSERVER_STATIONS 48U
#define POOM_OBSERVER_PROBES 32U
#define POOM_OBSERVER_NAMES 3U
#define POOM_OBSERVER_JSON_MAX 1024U
#define POOM_OBSERVER_STALE_MS 30000U

enum {
    POOM_OBSERVER_MULTI_SSID = 1U << 0,
    POOM_OBSERVER_SECURITY_CONFLICT = 1U << 1,
    POOM_OBSERVER_PINE_OUI = 1U << 2,
    POOM_OBSERVER_PINE_PATTERN = 1U << 3,
    POOM_OBSERVER_PINE_CONFIRMED = 1U << 4,
    POOM_OBSERVER_PWNAGOTCHI = 1U << 5,
    POOM_OBSERVER_FLOCK_NAME = 1U << 6,
    POOM_OBSERVER_FLOCK_OUI = 1U << 7,
};
typedef enum {
    POOM_OBSERVER_OPEN, POOM_OBSERVER_WEP, POOM_OBSERVER_WPA,
    POOM_OBSERVER_WPA2, POOM_OBSERVER_WPA3
} poom_observer_security_t;
typedef enum {
    POOM_OBSERVER_AUTH, POOM_OBSERVER_PROBE, POOM_OBSERVER_CSA,
    POOM_OBSERVER_LOGOFF, POOM_OBSERVER_SAE, POOM_OBSERVER_DEAUTH,
    POOM_OBSERVER_ACTIVITY_COUNT
} poom_observer_activity_kind_t;
typedef struct {
    bool used, active;
    uint8_t bssid[6], channel, name_count, flags, pine_hits;
    int8_t rssi;
    poom_observer_security_t security;
    char ssid[33], names[POOM_OBSERVER_NAMES][33];
    uint8_t ssid_length, raw_ssid[32], name_lengths[POOM_OBSERVER_NAMES];
    uint8_t raw_names[POOM_OBSERVER_NAMES][32];
    char pwn_name[25], pwn_version[17];
    uint32_t pwn_total, pwn_uptime, first_ms, last_ms, observations, pine_last_ms;
    bool pwn_deauth;
} poom_observer_ap_t;
typedef struct {
    bool used, active, has_ap;
    uint8_t mac[6], bssid[6], channel;
    int8_t rssi;
    uint32_t first_ms, last_ms, observations, relationship_changes;
} poom_observer_station_t;
typedef struct {
    bool used;
    uint8_t mac[6], channel, flags;
    int8_t rssi;
    char ssid[33];
    uint8_t ssid_length, raw_ssid[32];
    uint32_t count, first_ms, last_ms;
} poom_observer_probe_t;
typedef struct {
    uint32_t count, window_count, window_ms, last_ms;
    uint8_t bssid[6], channel;
} poom_observer_activity_t;
typedef struct {
    poom_observer_ap_t aps[POOM_OBSERVER_APS];
    poom_observer_station_t stations[POOM_OBSERVER_STATIONS];
    poom_observer_probe_t probes[POOM_OBSERVER_PROBES];
    poom_observer_activity_t activity[POOM_OBSERVER_ACTIVITY_COUNT];
    uint32_t started_ms, frames, malformed, management, data, retries;
    uint32_t probe_count, probe_repeats, ap_joins, ap_departures;
    uint32_t station_joins, station_departures, relationship_changes;
    uint32_t ap_overflow, station_overflow, probe_overflow;
    uint16_t ap_count, station_count, probe_entries, active_aps, active_stations;
} poom_observer_model_t;

void poom_observer_model_init(poom_observer_model_t *model, uint32_t now_ms);
/* frame excludes FCS; channel/rssi/time are the receive metadata. No radio or UI calls. */
bool poom_observer_model_frame(poom_observer_model_t *model, const uint8_t *frame,
                               size_t length, uint8_t channel, int8_t rssi, uint32_t now_ms);
void poom_observer_model_tick(poom_observer_model_t *model, uint32_t now_ms);
const char *poom_observer_security_name(poom_observer_security_t security);
const char *poom_observer_activity_name(poom_observer_activity_kind_t kind);
bool poom_observer_activity_alert(const poom_observer_activity_t *activity,
                                 poom_observer_activity_kind_t kind, uint32_t now_ms);
/* JSON Lines avoids ambiguous CSV quoting and spreadsheet formula interpretation. */
bool poom_observer_model_write_report(const poom_observer_model_t *model, FILE *file,
                                      uint32_t now_ms, uint32_t dropped, uint32_t tune_errors);
#endif
