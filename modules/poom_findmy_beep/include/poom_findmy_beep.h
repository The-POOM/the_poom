// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "poom_findmy_protocol.h"

typedef enum { POOM_FINDMY_STARTING, POOM_FINDMY_SCANNING, POOM_FINDMY_CONNECTING,
               POOM_FINDMY_DISCOVERING, POOM_FINDMY_SUBSCRIBING, POOM_FINDMY_WRITING,
               POOM_FINDMY_WAITING, POOM_FINDMY_FINISHED } poom_findmy_phase_t;
typedef enum { POOM_FINDMY_PENDING, POOM_FINDMY_ACKNOWLEDGED, POOM_FINDMY_REJECTED,
               POOM_FINDMY_UNSUPPORTED, POOM_FINDMY_TIMEOUT, POOM_FINDMY_STALE,
               POOM_FINDMY_CANCELLED, POOM_FINDMY_FAILED } poom_findmy_result_code_t;
typedef struct {
    poom_findmy_key_t key;
    poom_findmy_result_code_t code;
    poom_findmy_protocol_t protocol;
    bool command_submitted;
    bool write_acknowledged;
    poom_findmy_reply_t reply;
    uint16_t att_status;
    uint16_t response_status;
    esp_err_t error;
} poom_findmy_result_t;
typedef struct {
    bool active;
    bool batch_started;
    bool radio_released;
    poom_findmy_phase_t phase;
    size_t current;
    size_t count;
    esp_err_t error;
    poom_findmy_result_t results[POOM_FINDMY_MAX_DEVICES];
} poom_findmy_status_t;

/* Owns an otherwise idle Bluedroid controller. Discovery never starts sound. */
esp_err_t poom_findmy_beep_scan(void);
size_t poom_findmy_beep_devices(poom_findmy_device_t *out, size_t capacity);
/* Copies a frozen set, rejects unknown/stale/nonconnectable identities. One pass. */
esp_err_t poom_findmy_beep_begin(const poom_findmy_key_t *keys, size_t count);
void poom_findmy_beep_status(poom_findmy_status_t *out);
/* Waits for worker termination. ESP_OK confirms the owned radio was released;
 * a driver teardown failure is retained in status and blocks subsequent starts. */
esp_err_t poom_findmy_beep_stop(void);
const char *poom_findmy_result_name(poom_findmy_result_code_t code);
