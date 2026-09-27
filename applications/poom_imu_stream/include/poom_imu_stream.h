// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_IMU_STREAM_H
#define POOM_IMU_STREAM_H

#include <stdbool.h>

/**
 * @file poom_imu_stream.h
 * @brief Public API for IMU stream access.
 */

/**
 * @brief IMU sample container.
 */
typedef struct
{
    /**< Acceleration in milli-g for X, Y, Z. */
    float acceleration_mg[3];
    /**< Angular rate in milli-degrees/s for X, Y, Z. */
    float angular_rate_mdps[3];
    /**< Temperature in degrees Celsius. */
    float temperature_degC;
    /**< True only when this call refreshed the corresponding motion channel. */
    bool acceleration_fresh;
    bool angular_rate_fresh;
} poom_imu_data_t;

/**
 * @brief Initializes the IMU device and stream configuration.
 */
void poom_imu_stream_init(void);

/** Initializes the sensor with a bounded reset wait; false on I2C/device error.
 */
bool poom_imu_stream_try_init(void);

/** Initializes gesture-friendly filters: faster acceleration, DC gyro retained.
 * Other applications keep the default filters through init()/try_init(). */
bool poom_imu_stream_init_gestures(void);

/**
 * @brief Reads one IMU sample when new data is available.
 *
 * @param out Output sample pointer.
 * @return true if at least one channel has new data.
 * @return false if no new data is available or argument is invalid.
 */
bool poom_imu_stream_read_data(poom_imu_data_t* out);

#endif /* POOM_IMU_STREAM_H */
