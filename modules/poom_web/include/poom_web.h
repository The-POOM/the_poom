// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WEB_H
#define POOM_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stddef.h>

#ifndef POOM_WEB_ENABLE_TONE_PAGE
#define POOM_WEB_ENABLE_TONE_PAGE 0
#endif

#ifndef POOM_WEB_ENABLE_MIDI_PAGE
#define POOM_WEB_ENABLE_MIDI_PAGE 0
#endif

/**
 * @brief Callback invoked when a terminal command is received from the web UI.
 *
 * @param[in] command Null-terminated command text.
 * @param[in] user_ctx User context passed during callback registration.
 */
/**
 * @brief Initializes POOM Web service.
 *
 * This function starts Manager AP mode using `poom_wifi_ctrl`, initializes
 * mDNS service, and starts the embedded HTTP/WebSocket server.
 *
 * @return
 * - ESP_OK on success
 * - Error code from Wi-Fi, mDNS, or HTTP server setup on failure
 */
typedef void (*poom_web_command_cb_t)(const char* command, void* user_ctx);

esp_err_t poom_web_init(void);

/**
 * @brief Initializes POOM Web in STA mode (connects to existing Wi-Fi).
 *
 * Connects to the provided SSID/password, retries twice on failure to obtain an IP,
 * and returns the assigned IPv4 address string on success.
 *
 * @param[in] ssid Wi-Fi SSID to connect.
 * @param[in] password Wi-Fi password (NULL or empty for open networks).
 * @param[out] out_ip Output buffer for IPv4 string (e.g. "192.168.1.10").
 * @param[in] out_ip_len Output buffer length.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t poom_web_init_sta(const char* ssid,
                           const char* password,
                           char* out_ip,
                           size_t out_ip_len);

/**
 * @brief Initializes POOM Web in STA mode using saved credentials.
 *
 * Uses credentials stored via `poom_secrets_store` keys `wifi_ssid`/`wifi_pass`.
 *
 * @param[out] out_ip Output buffer for IPv4 string.
 * @param[in] out_ip_len Output buffer length.
 * @return ESP_OK on success, otherwise an ESP error code (ESP_ERR_NOT_FOUND when no SSID saved).
 */
esp_err_t poom_web_init_sta_saved(char* out_ip, size_t out_ip_len);

/**
 * @brief Stops POOM Web service.
 *
 * This function stops the HTTP server, tears down mDNS, and stops AP mode.
 *
 * @return
 * - ESP_OK on success
 * - Error code from shutdown sequence on failure
 */
esp_err_t poom_web_deinit(void);

/**
 * @brief Registers a command callback for WebSocket terminal input.
 *
 * @param[in] cb Callback function pointer. Pass NULL to clear callback.
 * @param[in] user_ctx Opaque pointer forwarded back to callback.
 * @return ESP_OK
 */
esp_err_t poom_web_set_command_cb(poom_web_command_cb_t cb, void* user_ctx);

/**
 * @brief Sends text output to the active WebSocket client.
 *
 * @param[in] text Null-terminated text payload.
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE when service is not started
 * - ESP_ERR_NOT_FOUND when there is no connected WebSocket client
 */
esp_err_t poom_web_send_text(const char* text);

/**
 * @brief Returns AP SSID used by POOM Web.
 *
 * @return Null-terminated SSID string.
 */
const char* poom_web_get_wifi_ap_ssid(void);

/**
 * @brief Returns AP password used by POOM Web.
 *
 * @return Null-terminated password string.
 */
const char* poom_web_get_wifi_ap_password(void);

/**
 * @brief Returns current AP IPv4 address used by POOM Web (may be empty).
 */
const char* poom_web_get_wifi_ap_ip(void);

/**
 * @brief Returns last SSID used for STA mode (may be empty).
 */
const char* poom_web_get_wifi_sta_ssid(void);

/**
 * @brief Returns last obtained STA IPv4 (may be empty).
 */
const char* poom_web_get_wifi_sta_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WEB_H */
