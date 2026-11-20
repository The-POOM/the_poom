// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opens BEAST > WIFI GUARD, a passive beacon consistency monitor. */
void menu_wifi_guard_show(void);
/** Opens the bounded passive AP/station/probe relationship session. */
void menu_wifi_recon_show(void);
/** Request stop and wait up to five seconds; false retains ownership. */
bool menu_wifi_observer_stop(void);
bool menu_wifi_observer_is_active(void);

#ifdef __cplusplus
}
#endif
