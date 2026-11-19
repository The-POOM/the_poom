// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#ifndef POOM_WIFI_MARAUDER_H
#define POOM_WIFI_MARAUDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POOM_MARAUDER_MAX_CLIENTS 16U
#define POOM_MARAUDER_MAX_BEACONS 32U
#define POOM_MARAUDER_DEFAULT_DURATION_MS 60000U
#define POOM_MARAUDER_MAX_DURATION_MS 60000U
#define POOM_MARAUDER_DEFAULT_INTERVAL_MS 100U

typedef enum {
    POOM_MARAUDER_BAD_MSG = 0,
    POOM_MARAUDER_ASSOC_SLEEP,
    POOM_MARAUDER_QUIET,
    POOM_MARAUDER_CSA,
    POOM_MARAUDER_SAE_COMMIT,
    POOM_MARAUDER_PROBE_REQUEST,
    POOM_MARAUDER_AP_CLONE,
    POOM_MARAUDER_BEACON_LIST,
} poom_marauder_mode_t;

typedef struct {
    poom_marauder_mode_t mode;
    wifi_ap_record_t ap;
    uint8_t clients[POOM_MARAUDER_MAX_CLIENTS][6];
    size_t client_count;
    uint32_t duration_ms; /* Zero selects 60 seconds; larger values are rejected. */
    uint32_t interval_ms; /* Zero selects 100 ms; accepted range 100..5000 ms. */
    uint8_t csa_channel;  /* Zero selects a different channel in the same band. */
    uint8_t beacon_count;
    char beacon_ssids[POOM_MARAUDER_MAX_BEACONS][33];
    bool random_beacon_cycles; /* Renew random identities every ten seconds. */
} poom_marauder_config_t;

typedef struct {
    bool running;
    poom_marauder_mode_t mode;
    uint32_t attempted;
    uint32_t sent;   /* Driver-accepted frames, not proof of reception or effect. */
    uint32_t errors;
    esp_err_t last_error;
    uint32_t elapsed_ms;
} poom_marauder_status_t;

/* The caller must stop other radio services before starting. Owns STA radio
 * until completion. Bad Msg/Sleep require explicitly supplied client MACs;
 * SAE requires a WPA3-Personal or WPA2/WPA3 transition AP. One frame is sent
 * per interval, round-robin across clients. Unsupported driver operations
 * terminate the run and are reported in status. No automatic retries. */
esp_err_t poom_wifi_marauder_start(const poom_marauder_config_t *config);
/* Returns only after the worker has stopped and its radio cleanup completed. */
void poom_wifi_marauder_stop(void);
void poom_wifi_marauder_get_status(poom_marauder_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
