// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POOM_WIFI_ACTION_BAD_MSG = 0,
    POOM_WIFI_ACTION_ASSOC_SLEEP,
    POOM_WIFI_ACTION_QUIET,
    POOM_WIFI_ACTION_CHANNEL_SWITCH,
    POOM_WIFI_ACTION_SAE_COMMIT,
    POOM_WIFI_ACTION_PROBE_REQUEST,
    POOM_WIFI_ACTION_AP_CLONE,
    POOM_WIFI_ACTION_COUNT,
} poom_wifi_action_t;

/* The action owns discovery and target selection; no prior scan is needed. */
void menu_wifi_action_show(poom_wifi_action_t action);
/* Acknowledged CLI handoff includes discovery and scan workers, not just TX. */
void menu_wifi_action_stop(void);
bool menu_wifi_action_is_active(void);

#ifdef __cplusplus
}
#endif
