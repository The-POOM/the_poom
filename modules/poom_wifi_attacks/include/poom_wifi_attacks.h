// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_ATTACKS_H
#define POOM_WIFI_ATTACKS_H

#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_ATTACKS_H */
