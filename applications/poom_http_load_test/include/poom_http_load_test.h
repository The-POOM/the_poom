// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ssid;
    const char *password;
    const char *host;
    const char *port;
    const char *path;
    uint8_t worker_count;
} poom_http_load_test_config_t;

/**
 * @brief Start Wi-Fi and HTTP load test using the provided configuration.
 *
 * @param[in] config Configuration pointer. Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_http_load_test_start(const poom_http_load_test_config_t *config);

/**
 * @brief Override active attack target host, port, and path.
 *
 * @param[in] host Hostname or IP. Must not be NULL.
 * @param[in] port Service port string. Must not be NULL.
 * @param[in] path HTTP path. Must not be NULL.
 * @return esp_err_t
 */
esp_err_t poom_http_load_test_switch_target(const char *host,
                                            const char *port,
                                            const char *path);

/**
 * @brief Stop HTTP load test workers.
 *
 * @return void
 */
void poom_http_load_test_stop(void);

#ifdef __cplusplus
}
#endif
