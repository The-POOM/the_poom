// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_POOM_WIFI_ARP_H
#define MENU_POOM_WIFI_ARP_H
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows Wi-Fi ARP scan menu.
 * @return void
 */
void menu_poom_wifi_arp_show(void);
bool menu_poom_wifi_arp_is_active(void);
/* True only after socket workers, callbacks and session storage are released.
 * A false result means the caller must not take over the radio yet. */
bool menu_poom_wifi_arp_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_POOM_WIFI_ARP_H */
