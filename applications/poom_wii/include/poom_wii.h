// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WII_H
#define POOM_WII_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runtime configuration for the air mouse processing pipeline.
 */
typedef struct
{
    float complementary_alpha;
    float smooth_beta;
    float gyro_deadzone_dps;
    float gain_x;
    float gain_y;
    float tilt_gain_x;
    float tilt_gain_y;
    uint32_t task_period_ms;
    uint16_t calibration_samples;
    uint16_t calibration_sample_delay_ms;
} poom_wii_config_t;

/**
 * @brief BLE connection callback for air mouse state notifications.
 */
typedef void (*poom_wii_connection_handler_t)(bool connected);

/**
 * @brief Fill configuration with default values.
 * @param[out] out_cfg Pointer to output configuration structure.
 * @return esp_err_t
 */
void poom_wii_get_default_config(poom_wii_config_t *out_cfg);

/**
 * @brief Initialize the air mouse module with default configuration.
 * @return esp_err_t
 */
uint8_t poom_wii_init(void);

/**
 * @brief Initialize the air mouse module with custom configuration.
 * @param[in] config Pointer to input configuration structure.
 * @return esp_err_t
 */
uint8_t poom_wii_init_with_config(const poom_wii_config_t *config);

/**
 * @brief Enable and run air mouse processing.
 * @return esp_err_t
 */
void poom_wii_start(void);

/**
 * @brief Disable and stop air mouse processing.
 * @return esp_err_t
 */
void poom_wii_stop(void);

/**
 * @brief Press and hold the left mouse button.
 * @return esp_err_t
 */
void poom_wii_left_button_press(void);

/**
 * @brief Release the left mouse button.
 * @return esp_err_t
 */
void poom_wii_left_button_release(void);

/**
 * @brief Press and hold the right mouse button.
 * @return esp_err_t
 */
void poom_wii_right_button_press(void);

/**
 * @brief Release the right mouse button.
 * @return esp_err_t
 */
void poom_wii_right_button_release(void);

/**
 * @brief Press and hold an ASCII keyboard key.
 * @param[in] key ASCII key to press.
 * @return esp_err_t
 */
void poom_wii_key_press_ascii(char key);

/**
 * @brief Release a specific ASCII keyboard key if currently active.
 * @param[in] key ASCII key to release.
 * @return esp_err_t
 */
void poom_wii_key_release_ascii(char key);

/**
 * @brief Release all currently active keyboard keys.
 * @return esp_err_t
 */
void poom_wii_key_release_all(void);

/**
 * @brief Enable or disable IMU-driven mouse movement while keeping BLE active.
 * @param[in] enabled True to send movement, false to pause movement reports.
 * @return esp_err_t
 */
void poom_wii_set_motion_enabled(bool enabled);

/**
 * @brief Toggle IMU-driven mouse movement on/off while keeping BLE active.
 * @return esp_err_t
 */
void poom_wii_toggle_motion_enabled(void);

/**
 * @brief Returns current IMU-driven mouse movement state.
 * @return esp_err_t
 */
bool poom_wii_is_motion_enabled(void);

/**
 * @brief Check whether air mouse is currently running.
 * @return esp_err_t
 */
bool poom_wii_is_running(void);

/**
 * @brief Check whether BLE HID link is connected.
 * @return esp_err_t
 */
bool poom_wii_is_connected(void);

/**
 * @brief Register a BLE connection state handler.
 * @param[in] handler Callback pointer, NULL to clear.
 * @return esp_err_t
 */
void poom_wii_set_connection_handler(poom_wii_connection_handler_t handler);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WII_H */
