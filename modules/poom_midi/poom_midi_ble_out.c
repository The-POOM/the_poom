// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_midi_ble_out.h"

#include <string.h>

#include "ble_midi.h"

#define MIDI_PORT_INDEX (0U)

static bool s_started = false;

void poom_midi_ble_out_init(void)
{
    if (s_started)
    {
        return;
    }

    (void)blemidi_init(NULL);
    s_started = true;
}

void poom_midi_ble_out_deinit(void)
{
    if (!s_started)
    {
        return;
    }

    blemidi_deinit();
    s_started = false;
}

bool poom_midi_ble_out_is_connected(void)
{
    return blemidi_is_connected();
}

/**
 * @brief Internal helper for `status`.
 *
 * @param[in] base Parameter passed to the function.
 * @param[in] channel Parameter passed to the function.
 * @return inline uint8_t
 */
static inline uint8_t status_(uint8_t base, uint8_t channel)
{
    return (uint8_t)(base | (channel & 0x0FU));
}

void poom_midi_ble_out_program_change(uint8_t channel, uint8_t program)
{
    poom_midi_ble_out_init();

    uint8_t msg[2] = {status_(0xC0U, channel), (uint8_t)(program & 0x7FU)};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

void poom_midi_ble_out_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    poom_midi_ble_out_init();

    uint8_t msg[3] = {status_(0x90U, channel), (uint8_t)(note & 0x7FU), (uint8_t)(velocity & 0x7FU)};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

void poom_midi_ble_out_note_off(uint8_t channel, uint8_t note)
{
    poom_midi_ble_out_init();

    uint8_t msg[3] = {status_(0x80U, channel), (uint8_t)(note & 0x7FU), 0x00U};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

void poom_midi_ble_out_all_notes_off(uint8_t channel)
{
    poom_midi_ble_out_init();

    uint8_t msg[3] = {status_(0xB0U, channel), 123U, 0U};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}
