// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_BLE_HID_H_
#define POOM_BLE_HID_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Define BLE HID connection state callback type.
 * @param[in] connected True when connected, false when disconnected.
 * @return esp_err_t
 */
typedef void (*hid_event_callback_f)(bool connected);

/**
 * @brief Register BLE connection state callback.
 * @param[in] callback Callback function pointer, NULL to unregister.
 * @return esp_err_t
 */
void poom_ble_hid_set_connection_callback(hid_event_callback_f callback);

/**
 * @brief Initialize BLE HID stack and start advertising.
 * @return esp_err_t
 */
void poom_ble_hid_start(void);

/**
 * @brief Stop BLE HID stack and advertising.
 */
void poom_ble_hid_stop(void);

/**
 * @brief Returns true if BLE HID is started.
 */
bool poom_ble_hid_is_started(void);

/**
 * @brief Send consumer volume-up key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_volume_up(bool press);

/**
 * @brief Send consumer volume-down key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_volume_down(bool press);

/**
 * @brief Send consumer play key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_play(bool press);

/**
 * @brief Send consumer pause key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_pause(bool press);

/**
 * @brief Send consumer next-track key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_next_track(bool press);

/**
 * @brief Send consumer previous-track key state.
 * @param[in] press True to press, false to release.
 * @return esp_err_t
 */
void poom_ble_hid_send_prev_track(bool press);

/**
 * @brief Send mouse relative movement and button state.
 * @param[in] dx Relative X displacement.
 * @param[in] dy Relative Y displacement.
 * @param[in] buttons HID mouse buttons bitmask.
 * @return esp_err_t
 */
void poom_ble_hid_send_mouse_move(int dx, int dy, uint8_t buttons);

/**
 * @brief Press and hold one ASCII character key.
 * @param[in] c ASCII character to press.
 * @return esp_err_t
 */
void poom_ble_hid_key_press_ascii(char c);

/**
 * @brief Release all pressed keyboard keys.
 * @return esp_err_t
 */
void poom_ble_hid_key_release_all(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_BLE_HID_H_ */
