// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_ARP_POISONER_H
#define POOM_WIFI_ARP_POISONER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Starts POOM Wi-Fi ARP poisoner runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_arp_poisoner_start(void);

/**
 * @brief Stops POOM Wi-Fi ARP poisoner runtime.
 * @param[in,out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_arp_poisoner_stop(void);

/**
 * @brief Reports whether ARP poisoner runtime was initialized.
 * @param[in,out] none Not used.
 * @return bool
 */
bool poom_wifi_arp_poisoner_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_ARP_POISONER_H */
