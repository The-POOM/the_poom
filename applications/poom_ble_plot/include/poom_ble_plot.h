// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_ble_plot.h
 * @brief Public BLE plot transport API.
 */

/* =========================
 * Default configuration
 * ========================= */
#ifndef BLEPLOT_TAG
#define BLEPLOT_TAG "BLEPLOT"
#endif

#ifndef BLEPLOT_DEVICE_NAME
#define BLEPLOT_DEVICE_NAME "PPLOT"
#endif

/** @brief Maximum ASCII line length sent per notification (includes '\n'). */
#ifndef BLEPLOT_MAX_LINE_LEN
#define BLEPLOT_MAX_LINE_LEN 244
#endif

/** @brief Recommended local MTU (usable ATT payload = MTU-3). */
#ifndef BLEPLOT_LOCAL_MTU
#define BLEPLOT_LOCAL_MTU 247
#endif

/* =========================
 * Public API
 * ========================= */

/**
 * @brief Initializes BLE stack, GAP/GATT and NUS service.
 *
 * @param device_name Advertised name. Uses BLEPLOT_DEVICE_NAME when NULL.
 * @return 0 on success, negative value on error.
 */
int32_t poom_ble_plot_init(const char *device_name);

/**
 * @brief Sets how many series (columns) are sent per plot line.
 *
 * @param n_series Number of series. Practical limit depends on MTU and precision.
 * @return 0 on success, negative value on error.
 */
int32_t poom_ble_plot_set_series_count(uint8_t n_series);

/**
 * @brief Configures output format.
 *
 * @param sep Output separator (',' recommended for Bluefruit Plotter).
 * @param precision Decimal precision (0..9).
 */
void poom_ble_plot_set_format(char sep, uint8_t precision);

/**
 * @brief Sends a plot line with N float values.
 *
 * Format: v0<sep>v1<sep>...vN-1'\n'
 *
 * @param values Values array.
 * @param n Number of values.
 * @return 0 on success, negative value on error.
 */
int32_t poom_ble_plot_send_line(const float *values, size_t n);

/**
 * @brief Returns current BLE connection state.
 *
 * @return true if connected.
 */
bool poom_ble_plot_is_connected(void);

/**
 * @brief Stops BLE plot transport and releases BLE stack resources.
 *
 * This function is idempotent and can be called multiple times safely.
 */
void poom_ble_plot_stop(void);

#ifdef __cplusplus
}
#endif
