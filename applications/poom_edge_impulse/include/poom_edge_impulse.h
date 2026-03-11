// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_EDGE_IMPULSE_H
#define POOM_EDGE_IMPULSE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef POOM_EDGE_IMPULSE_LABEL_MAX_LEN
#define POOM_EDGE_IMPULSE_LABEL_MAX_LEN (64U)
#endif

typedef struct
{
    const char* wifi_ssid;
    const char* wifi_password;
    const char* label;
    uint32_t sample_period_ms;
    uint32_t capture_timeout_ms;
} poom_edge_impulse_config_t;

typedef void (*poom_edge_impulse_capture_done_cb_t)(esp_err_t result, const char* label, void* user_ctx);

/**
 * @brief Fills configuration with default values.
 *
 * @param[out] out_config Output configuration structure.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_get_default_config(poom_edge_impulse_config_t* out_config);

/**
 * @brief Starts Edge Impulse uploader module and Wi-Fi connection.
 *
 * @param[in] config Runtime configuration.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_start(const poom_edge_impulse_config_t* config);

/**
 * @brief Stops Edge Impulse uploader module and releases runtime resources.
 *
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_stop(void);

/**
 * @brief Triggers one capture and upload cycle with the provided label.
 *
 * @param[in] label Capture label.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_trigger_capture(const char* label);

/**
 * @brief Registers callback invoked when one capture/upload cycle finishes.
 *
 * @param[in] callback Completion callback. Set NULL to disable callbacks.
 * @param[in] user_ctx User context passed back in callback.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_set_capture_done_cb(poom_edge_impulse_capture_done_cb_t callback, void* user_ctx);

/**
 * @brief Reads module state flags.
 *
 * @param[out] out_initialized True when module has been started.
 * @param[out] out_wifi_connected True when STA has a valid IP.
 * @param[out] out_capture_running True when capture task is active.
 * @return esp_err_t
 */
esp_err_t poom_edge_impulse_get_state(bool* out_initialized,
                                      bool* out_wifi_connected,
                                      bool* out_capture_running);

#ifdef __cplusplus
}
#endif

#endif /* POOM_EDGE_IMPULSE_H */
