// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "poom_wifi_observer_model.h"

typedef struct poom_wifi_observer poom_wifi_observer_t;
/* One task owns start/poll/snapshot/stop. RX only enqueues bounded copies. */
esp_err_t poom_wifi_observer_start(poom_wifi_observer_t **out);
void poom_wifi_observer_poll(poom_wifi_observer_t *session);
const poom_observer_model_t *poom_wifi_observer_model(const poom_wifi_observer_t *session);
uint8_t poom_wifi_observer_channel(const poom_wifi_observer_t *session);
uint32_t poom_wifi_observer_dropped(const poom_wifi_observer_t *session);
uint32_t poom_wifi_observer_tune_errors(const poom_wifi_observer_t *session);
/* Channel 0 resumes hopping. Other channels remain locked only on successful tune. */
esp_err_t poom_wifi_observer_focus(poom_wifi_observer_t *session, uint8_t channel);
/* Failure retains the detached owner; caller must retry before another radio app. */
esp_err_t poom_wifi_observer_stop(poom_wifi_observer_t **session);
