// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_ATTACKS_H
#define POOM_WIFI_ATTACKS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of clients a single multi-target deauth run may cycle. */
#define POOM_WIFI_ATTACKS_MAX_CLIENTS (16U)

/**
 * @brief Defines supported Wi-Fi attack types.
 */
typedef enum
{
    poom_wifi_attacks_type_broadcast = 0,
    poom_wifi_attacks_type_rogue_ap,
    poom_wifi_attacks_type_combine,
    poom_wifi_attacks_type_multi_ap,
    poom_wifi_attacks_type_password,
    poom_wifi_attacks_type_count
} poom_wifi_attacks_type_t;

/**
 * @brief Starts the selected attack against the target AP.
 * @param[in] attack_type Selected attack mode.
 * @param[in,out] ap_target Target AP record.
 * @return void
 */
void poom_wifi_attacks_handle(
    poom_wifi_attacks_type_t attack_type,
    wifi_ap_record_t *ap_target);

/**
 * @brief Stops all running attacks.
 * @param[in,out] none Not used.
 * @return void
 */
void poom_wifi_attacks_stop(void);

/**
 * @brief Stops all Wi-Fi attack services and frees resources.
 *
 * This is a stronger stop than `poom_wifi_attacks_stop()`:
 * - Signals attack tasks to exit and waits briefly
 * - Stops captive portal (if running)
 * - Disables promiscuous mode
 * - Deinitializes Wi-Fi control
 */
void poom_wifi_attacks_stop_all(void);

/**
 * @brief Returns the number of supported attack types.
 * @param[in,out] none Not used.
 * @return int
 */
int poom_wifi_attacks_get_attack_count(void);

/**
 * @brief Sends one broadcast deauthentication frame for a target AP.
 * @param[in,out] ap Target AP record.
 * @return void
 */
void poom_wifi_attacks_broadcast_once(wifi_ap_record_t *ap);

/**
 * @brief Sends one targeted (unicast) deauthentication frame.
 *
 * Builds a deauth frame addressed to a single associated station rather than
 * the broadcast address. Intended for authorized use against clients on a
 * network you own or operate.
 *
 * @param[in] ap Target AP record (provides BSSID and channel).
 * @param[in] client_mac Six-byte MAC of the client to deauthenticate.
 * @return void
 */
void poom_wifi_attacks_deauth_client_once(const wifi_ap_record_t *ap,
                                          const uint8_t client_mac[6]);

/**
 * @brief Starts a continuous targeted deauth loop against one client.
 *
 * Spawns a task that repeatedly sends unicast deauth frames to @p client_mac
 * on @p ap until `poom_wifi_attacks_stop()` or `poom_wifi_attacks_stop_all()`
 * is called. Reuses the same Wi-Fi apsta setup as the broadcast attack.
 *
 * @param[in] ap Target AP record (provides BSSID and channel).
 * @param[in] client_mac Six-byte MAC of the client to deauthenticate.
 * @return void
 */
void poom_wifi_attacks_deauth_client_start(const wifi_ap_record_t *ap,
                                           const uint8_t client_mac[6]);

/**
 * @brief Starts a continuous targeted deauth loop against several clients.
 *
 * Spawns a task that round-robins unicast deauth frames across the given set of
 * clients (all associated to @p ap, which supplies the BSSID and channel) until
 * `poom_wifi_attacks_stop()` / `poom_wifi_attacks_stop_all()` is called. Shares
 * the same running-flag and task slot as the single-client loop, so
 * `poom_wifi_attacks_deauth_client_is_running()` reports it too. Intended for
 * authorized use against clients on a network you own or operate.
 *
 * @param[in] ap Target AP record (provides BSSID and channel).
 * @param[in] clients Array of six-byte client MACs to deauthenticate.
 * @param[in] count Number of entries in @p clients (capped at
 *                  POOM_WIFI_ATTACKS_MAX_CLIENTS).
 * @return void
 */
void poom_wifi_attacks_deauth_clients_start(const wifi_ap_record_t *ap,
                                            const uint8_t (*clients)[6],
                                            size_t count);

/**
 * @brief Reports whether the targeted deauth loop is currently running.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_attacks_deauth_client_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_ATTACKS_H */
