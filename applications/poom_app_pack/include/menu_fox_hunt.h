// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_FOX_HUNT_H
#define MENU_FOX_HUNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Launches the BLE "fox hunt" proximity game.
 *
 * Locks onto a fixed BLE target (the "fox") and guides the player to it using
 * smoothed RSSI: an on-screen heat meter, a blue->red LED ramp, and a
 * Geiger-counter buzzer that speeds up as the player gets closer.
 *
 * @param[in,out] none Not used.
 * @return void
 */
void app_fox_hunt(void);

/**
 * @brief Launches Fox Hunt locked directly onto a specific Wi-Fi client.
 *
 * Starts the app straight on the hunt screen, tracking @p client_mac's live
 * RSSI in promiscuous mode on the AP's channel (the same heat bar / LED ramp /
 * Geiger buzzer as the BLE hunt). While hunting, unicast deauth frames can be
 * sent to the client during explicitly authorised testing. A+UP toggles
 * continuous deauth (OFF by default); A+DOWN sends a short manual burst.
 * UP/DOWN adjust audio volume and LEFT/RIGHT adjust LED brightness.
 *
 * @param[in] ap_bssid   BSSID of the AP the client is associated to.
 * @param[in] channel    Primary channel of that AP.
 * @param[in] client_mac Six-byte MAC of the client to hunt.
 * @param[in] name       Optional display name (NULL -> client MAC).
 * @return void
 */
void app_fox_hunt_wifi_client(const uint8_t ap_bssid[6],
                              uint8_t channel,
                              const uint8_t client_mac[6],
                              const char *name);

/** Launch a BLE hunt directly for a discovered BLE address. */
void app_fox_hunt_ble_device(const uint8_t address[6], const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MENU_FOX_HUNT_H */
