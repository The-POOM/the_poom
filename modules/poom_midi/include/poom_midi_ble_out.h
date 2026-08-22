// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_MIDI_BLE_OUT_H
#define POOM_MIDI_BLE_OUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ensure BLE MIDI transport is started.
 */
void poom_midi_ble_out_init(void);

/**
 * @brief Stop BLE MIDI transport (best-effort).
 */
void poom_midi_ble_out_deinit(void);

/**
 * @brief Returns whether BLE MIDI is currently connected.
 */
bool poom_midi_ble_out_is_connected(void);

/**
 * @brief Sends a MIDI program change event over BLE.
 *
 * @param[in] channel MIDI channel.
 * @param[in] program Program number.
 * @return void
 */
void poom_midi_ble_out_program_change(uint8_t channel, uint8_t program);

/**
 * @brief Sends a MIDI note-on event over BLE.
 *
 * @param[in] channel MIDI channel.
 * @param[in] note MIDI note number.
 * @param[in] velocity MIDI velocity.
 * @return void
 */
void poom_midi_ble_out_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

/**
 * @brief Sends a MIDI note-off event over BLE.
 *
 * @param[in] channel MIDI channel.
 * @param[in] note MIDI note number.
 * @return void
 */
void poom_midi_ble_out_note_off(uint8_t channel, uint8_t note);

/**
 * @brief Sends an all-notes-off event over BLE.
 *
 * @param[in] channel MIDI channel.
 * @return void
 */
void poom_midi_ble_out_all_notes_off(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* POOM_MIDI_BLE_OUT_H */
