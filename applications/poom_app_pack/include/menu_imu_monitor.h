// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_IMU_MONITOR_H
#define MENU_IMU_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shows IMU monitor (accelerometer/gyroscope/temperature).
 *
 * Exits back to the main menu on B.
 */
void menu_imu_monitor_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_IMU_MONITOR_H */
