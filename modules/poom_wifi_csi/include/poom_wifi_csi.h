// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct poom_wifi_csi poom_wifi_csi_t;
typedef enum { POOM_CSI_CALIBRATING, POOM_CSI_INSUFFICIENT, POOM_CSI_STILL,
               POOM_CSI_POSSIBLE, POOM_CSI_MOTION } poom_csi_state_t;
typedef struct {
    poom_csi_state_t state;
    uint8_t calibration, score, reliability, amplitude, phase, correlation, channel;
    uint16_t samples_per_second;
    int8_t rssi;
    uint32_t accepted, dropped;
    bool calibrated;
    uint8_t history[64]; /* oldest to newest, 255 means insufficient quality */
} poom_csi_status_t;
/* Single-task ownership. Callback only copies a target-filtered bounded sample. */
esp_err_t poom_wifi_csi_start(poom_wifi_csi_t **out, const uint8_t bssid[6], uint8_t channel);
void poom_wifi_csi_poll(poom_wifi_csi_t *session, poom_csi_status_t *status);
void poom_wifi_csi_recalibrate(poom_wifi_csi_t *session);
esp_err_t poom_wifi_csi_stop(poom_wifi_csi_t **session);
#ifdef __cplusplus
}
#endif
