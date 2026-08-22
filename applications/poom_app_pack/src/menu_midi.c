// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_midi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "ble_midi.h"
#include "i2c.h"
#include "poom_motion_midi.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

static bool s_midi_active = false;
static bool s_midi_buttons_subscribed = false;
static bool s_midi_ble_subscribed = false;
static void menu_midi_on_button_(const poom_sbus_msg_t *msg, void *user);
static void menu_midi_on_ble_(const poom_sbus_msg_t *msg, void *user);
static uint8_t s_midi_selected_row = 0U;
static bool s_midi_ble_connected = false;

#define POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED "ble/midi/connected"

#define MIDI_NOTE_MIN (0U)
#define MIDI_NOTE_MAX (127U)
#define HIT_THRESHOLD_MIN (1U)
#define HIT_THRESHOLD_MAX (127U)

#define BOX_Y (12)
#define BOX_H (40)

#define ROW0_Y (24)
#define ROW_STEP (10)
#define ROW_H (9)

/**
 * @brief Internal helper for `clamp_u8`.
 *
 * @param[in] value Parameter passed to the helper.
 * @param[in] min_value Parameter passed to the helper.
 * @param[in] max_value Parameter passed to the helper.
 * @return inline uint8_t
 */
static inline uint8_t clamp_u8_(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @param[in] note Parameter passed to the helper.
 * @return void
 */
static void format_midi_note_(char *out, size_t out_len, uint8_t note)
{
    static const char *k_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    const uint8_t name_idx = (uint8_t)(note % 12U);
    const int octave = ((int)note / 12) - 1;

    (void)snprintf(out, out_len, "Note:%s%d=%u", k_names[name_idx], octave, (unsigned)note);
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_midi_render_(void)
{
    char line_ble[22];
    char line1[22];
    char line2[22];
    char line3[22];

    g_midi_note = clamp_u8_(g_midi_note, MIDI_NOTE_MIN, MIDI_NOTE_MAX);
    g_hit_threshold = clamp_u8_(g_hit_threshold, HIT_THRESHOLD_MIN, HIT_THRESHOLD_MAX);
    if (g_midi_mode > POOM_MIDI_MODE_MELODY)
    {
        g_midi_mode = POOM_MIDI_MODE_DRUM;
    }
    if (g_midi_scale > POOM_MIDI_SCALE_PENTATONIC_MINOR)
    {
        g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
    }

    if (s_midi_selected_row > 2U)
    {
        s_midi_selected_row = 2U;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("POOM MIDI"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(4, 14);
    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", s_midi_ble_connected ? "CONNECTED" : "PAIRING");
    (void)poom_arduboy_print(line_ble);

    format_midi_note_(line1, sizeof(line1), g_midi_note);
    if (g_midi_mode == POOM_MIDI_MODE_MELODY)
    {
        (void)snprintf(line2, sizeof(line2), "Thr : N/A");
    }
    else
    {
        (void)snprintf(line2, sizeof(line2), "Thr :%3u", (unsigned)g_hit_threshold);
    }
    if (g_midi_mode == POOM_MIDI_MODE_MELODY)
    {
        (void)snprintf(line3,
                       sizeof(line3),
                       "Melody:%s",
                       (g_midi_scale == POOM_MIDI_SCALE_PENTATONIC_MINOR) ? "MINOR" : "MAJOR");
    }
    else
    {
        (void)snprintf(line3, sizeof(line3), "Mode:DRUM");
    }

    poom_arduboy_set_cursor(4, ROW0_Y);
    (void)poom_arduboy_print(line1);

    poom_arduboy_set_cursor(4, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line2);

    poom_arduboy_set_cursor(4, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    {
        const int16_t sel_y = (int16_t)(ROW0_Y + (int16_t)s_midi_selected_row * ROW_STEP);
        poom_arduboy_fill_rect(1, (int16_t)(sel_y - 1), (int16_t)(ARDUBOY_WIDTH - 2), ROW_H, INVERT);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:PANIC"));

    i2c_lock();
    poom_arduboy_display();
    i2c_unlock();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_midi_exit_(void)
{
    s_midi_active = false;

    poom_motion_midi_stop();

    if (s_midi_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_midi_on_button_, "menu_midi");
        s_midi_buttons_subscribed = false;
    }

    if (s_midi_ble_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED, menu_midi_on_ble_, "menu_midi");
        s_midi_ble_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Internal helper for `menu_midi_on_ble`.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_midi_on_ble_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;

    if (!s_midi_active)
    {
        return;
    }

    if ((msg == NULL) || (msg->len < 1U))
    {
        return;
    }

    const bool connected = (msg->data[0] != 0U);
    if (connected == s_midi_ble_connected)
    {
        return;
    }

    s_midi_ble_connected = connected;
    menu_midi_render_();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_midi_on_button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    const uint8_t max_row = 2U;

    if (!s_midi_active)
    {
        return;
    }

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BUTTON_B)
    {
        menu_midi_exit_();
        return;
    }

    if (ev.button == BUTTON_UP)
    {
        if (s_midi_selected_row > 0U)
        {
            s_midi_selected_row--;
        }
    }
    else if (ev.button == BUTTON_DOWN)
    {
        if (s_midi_selected_row < max_row)
        {
            s_midi_selected_row++;
        }
    }
    else if (ev.button == BUTTON_RIGHT)
    {
        if (s_midi_selected_row == 0U)
        {
            if (g_midi_note < MIDI_NOTE_MAX)
            {
                g_midi_note++;
            }
        }
        else if (s_midi_selected_row == 1U)
        {
            if ((g_midi_mode != POOM_MIDI_MODE_MELODY) && (g_hit_threshold < HIT_THRESHOLD_MAX))
            {
                g_hit_threshold++;
            }
        }
        else if (s_midi_selected_row == 2U)
        {
            if (g_midi_mode == POOM_MIDI_MODE_DRUM)
            {
                g_midi_mode = POOM_MIDI_MODE_MELODY;
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
            }
            else if (g_midi_scale == POOM_MIDI_SCALE_PENTATONIC_MAJOR)
            {
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MINOR;
            }
            else
            {
                g_midi_mode = POOM_MIDI_MODE_DRUM;
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
            }
        }
    }
    else if (ev.button == BUTTON_LEFT)
    {
        if (s_midi_selected_row == 0U)
        {
            if (g_midi_note > MIDI_NOTE_MIN)
            {
                g_midi_note--;
            }
        }
        else if (s_midi_selected_row == 1U)
        {
            if ((g_midi_mode != POOM_MIDI_MODE_MELODY) && (g_hit_threshold > HIT_THRESHOLD_MIN))
            {
                g_hit_threshold--;
            }
        }
        else if (s_midi_selected_row == 2U)
        {
            if (g_midi_mode == POOM_MIDI_MODE_DRUM)
            {
                g_midi_mode = POOM_MIDI_MODE_MELODY;
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MINOR;
            }
            else if (g_midi_scale == POOM_MIDI_SCALE_PENTATONIC_MINOR)
            {
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
            }
            else
            {
                g_midi_mode = POOM_MIDI_MODE_DRUM;
                g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
            }
        }
    }
    else if (ev.button == BUTTON_A)
    {
        uint8_t status = (g_midi_mode == POOM_MIDI_MODE_MELODY) ? 0xB0U : 0xB9U;
        uint8_t cc_all_notes_off[3] = {status, 123U, 0U};
        (void)blemidi_send_message(0U, cc_all_notes_off, sizeof(cc_all_notes_off));
    }

    menu_midi_render_();
}

void menu_midi_init(void)
{
    s_midi_active = true;
    s_midi_selected_row = 0U;
    s_midi_ble_connected = blemidi_is_connected();
    if (g_midi_mode > POOM_MIDI_MODE_MELODY)
    {
        g_midi_mode = POOM_MIDI_MODE_DRUM;
    }
    if (g_midi_scale > POOM_MIDI_SCALE_PENTATONIC_MINOR)
    {
        g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
    }

    if (!s_midi_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_midi_on_button_, "menu_midi");
        s_midi_buttons_subscribed = true;
    }

    if (!s_midi_ble_subscribed)
    {
        (void)poom_sbus_subscribe_cb(POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED, menu_midi_on_ble_, "menu_midi");
        s_midi_ble_subscribed = true;
    }

    menu_midi_render_();

    poom_motion_midi_start();
}
