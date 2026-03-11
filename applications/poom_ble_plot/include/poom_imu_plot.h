// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_imu_plot.h
 * @brief Public API for IMU BLE plotting helper.
 */

/**
 * @brief Publishing mode.
 */
typedef enum {
  IMUPLOT_ACCEL_ONLY = 0,  /**< ax, ay, az (g) */
  IMUPLOT_GYRO_ONLY,       /**< gx, gy, gz (dps) */
  IMUPLOT_ACCEL_GYRO       /**< ax, ay, az, gx, gy, gz (g, dps) */
} imuplot_mode_t;

/**
 * @brief Initializes BLE plot transport and output format.
 *
 * @param device_name BLE device name. Uses default when NULL.
 * @param sep CSV separator.
 * @param precision Decimal precision.
 * @return true on success, false on error.
 */
bool poom_ble_plot_imu_init(const char *device_name,
                        char sep,
                        uint8_t precision);

/**
 * @brief Stops the publishing task and unsubscribes button events.
 */
void poom_ble_plot_imu_stop(void);

/**
 * @brief Indicates whether the task is running.
 *
 * @return true if running.
 */
bool poom_ble_plot_imu_is_running(void);

/**
 * @brief Starts the publishing task with selected mode and period.
 *
 * @param mode Publishing mode.
 * @param period_ms Task period in milliseconds.
 * @return true if task was created, false otherwise.
 */
bool poom_ble_plot_imu_start(imuplot_mode_t mode,
                         uint32_t period_ms);

#ifdef __cplusplus
}
#endif
