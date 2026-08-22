// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_MAC_H
#define POOM_WIFI_MAC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "esp_err.h"
#include "poom_wifi_ctrl.h"

typedef void (*poom_wifi_mac_ip_cb_t)(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx);

/**
 * @brief Starts the asynchronous MAC-rotation cycle.
 *
 * The module connects to the router, waits for the `GOT_IP` event, reports the
 * IP through the optional callback, then disconnects, reinitializes Wi-Fi,
 * applies a new random STA MAC, and reconnects again.
 *
 * @param[in] ssid Router SSID.
 * @param[in] password Router password. Use NULL for open networks.
 * @param[in] cycle_delay_ms Optional delay after each `GOT_IP` before rotating
 *                           the MAC again. Use 0 for immediate cycling.
 * @param[in] on_ip Optional callback invoked every time the STA gets an IP.
 * @param[in] user_ctx User context passed to the callback.
 * @return esp_err_t
 */
esp_err_t poom_wifi_mac_start(const char *ssid,
                              const char *password,
                              uint32_t cycle_delay_ms,
                              poom_wifi_mac_ip_cb_t on_ip,
                              void *user_ctx);

/**
 * @brief Stops the asynchronous MAC-rotation cycle and restores the original STA MAC.
 *
 * @return esp_err_t
 */
esp_err_t poom_wifi_mac_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_MAC_H */
