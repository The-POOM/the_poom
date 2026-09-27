// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "menu_midi.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_motion_midi.h"
#include "poom_sbus.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define VISIBLE_ROWS           3U
#define ROW_Y                  24
#define ROW_STEP               10
#define USER                   "menu_midi"

typedef enum
{
    ROW_MODE,
    ROW_SOUND,
    ROW_SENSITIVITY,
    ROW_RESPONSE,
    ROW_SCALE,
    ROW_TONIC,
    ROW_OCTAVE,
    ROW_INTENSITY,
    ROW_STYLE,
    ROW_CALIBRATE,
    ROW_HELP
} row_t;
static const row_t drum_rows[] = {ROW_MODE,     ROW_SOUND,     ROW_SENSITIVITY,
                                  ROW_RESPONSE, ROW_CALIBRATE, ROW_HELP};
static const row_t melody_rows[] = {ROW_MODE,      ROW_SCALE,     ROW_TONIC,
                                    ROW_OCTAVE,    ROW_INTENSITY, ROW_STYLE,
                                    ROW_CALIBRATE, ROW_HELP};
static const char* const drum_names[]  = {"KICK",       "SNARE", "HIHAT CLOSED",
                                          "HIHAT OPEN", "TOM",   "CRASH"};
static const char* const note_names[]  = {"C",  "C#", "D",  "D#", "E",  "F",
                                          "F#", "G",  "G#", "A",  "A#", "B"};
static const char* const scale_names[] = {"PENTA MAJ", "PENTA MIN", "MAJOR",
                                          "MINOR"};
static const char* const levels[]      = {"LOW", "NORMAL", "HIGH"};
static const char* const dynamics[]    = {"SOFT", "NORMAL", "STRONG"};
static bool s_active, s_buttons_subscribed, s_status_subscribed, s_help,
    s_calibrating, s_start_failed, s_a_down, s_calibration_pending;
static uint8_t s_selected, s_scroll;
static uint32_t s_last_hit, s_hit_display_until;
static poom_midi_config_t s_config;
static poom_midi_status_t s_status;
static void on_button_(const poom_sbus_msg_t* msg, void* user);
static void on_status_(const poom_sbus_msg_t* msg, void* user);

static uint32_t now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool melody_(void)
{
    return s_config.mode == POOM_MIDI_MELODY;
}

static uint8_t row_count_(void)
{
    return melody_() ? sizeof(melody_rows) / sizeof(row_t)
                     : sizeof(drum_rows) / sizeof(row_t);
}

static row_t row_(uint8_t index)
{
    return melody_() ? melody_rows[index] : drum_rows[index];
}

static void line_(int y, const char* text)
{
    poom_arduboy_set_cursor(4, y);
    (void)poom_arduboy_print(text);
}

static void row_label_(row_t row, char* out, size_t len)
{
    switch(row)
    {
        case ROW_MODE:
            snprintf(out, len, "Mode:%s", melody_() ? "MELODY" : "DRUM");
            break;
        case ROW_SOUND:
            snprintf(out, len, "Sound:%s", drum_names[s_config.drum]);
            break;
        case ROW_SENSITIVITY:
            snprintf(out, len, "Sens:%s", levels[s_config.sensitivity]);
            break;
        case ROW_RESPONSE:
            snprintf(out, len, "Response:%s", dynamics[s_config.response]);
            break;
        case ROW_SCALE:
            snprintf(out, len, "Scale:%s", scale_names[s_config.scale]);
            break;
        case ROW_TONIC:
            snprintf(out, len, "Root:%s", note_names[s_config.tonic]);
            break;
        case ROW_OCTAVE:
            snprintf(out, len, "Octave:%u", s_config.octave);
            break;
        case ROW_INTENSITY:
            snprintf(out, len, "Velocity:%s", dynamics[s_config.intensity]);
            break;
        case ROW_STYLE:
            snprintf(out, len, "Play:%s",
                     s_config.moving_note ? "MOVING" : "FIXED");
            break;
        case ROW_CALIBRATE:
            snprintf(out, len, "%s",
                     melody_() ? "Center / calibrate" : "Calibrate");
            break;
        case ROW_HELP:
            snprintf(out, len, "Help");
            break;
    }
}

static void render_(void)
{
    char text[22];
    const char* action = "A:PLAY";
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(4, 2);
    (void)poom_arduboy_print("POOM MIDI");
    /* Musical feedback has its own place in the header, never in the BLE line.
     */
    if(!s_calibrating && !s_status.error && !s_start_failed && !s_help)
    {
        if(s_status.sensor_wait)
        {
            snprintf(text, sizeof(text), "WAIT");
        }
        else if(melody_())
        {
            snprintf(text, sizeof(text), "%s%d%s",
                     note_names[s_status.note % 12],
                     (int)s_status.note / 12 - 1, s_status.playing ? "*" : "");
        }
        else if((int32_t)(s_hit_display_until - now_ms_()) > 0)
        {
            snprintf(text, sizeof(text), "V:%03u", s_status.velocity);
        }
        else
        {
            snprintf(text, sizeof(text), "%s",
                     s_status.enabled ? "ARMED" : "PAUSE");
        }
        poom_arduboy_set_cursor(94, 2);
        (void)poom_arduboy_print(text);
    }
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 41, WHITE);
    snprintf(text, sizeof(text), "BLE: %s",
             s_status.connected ? "CONNECTED" : "PAIRING");
    line_(14, text);

    if(s_start_failed || s_status.error)
    {
        line_(24, "MIDI / IMU error");
        line_(34, "A: retry setup");
        action = "A:RETRY";
    }
    else if(s_calibrating)
    {
        line_(24, "Hold as you play");
        line_(34, s_status.sensor_wait ? "Waiting for IMU" : "Keep still...");
        poom_arduboy_draw_rect(4, 44, 120, 6, WHITE);
        poom_arduboy_fill_rect(
            5, 45, (int16_t)(118U * s_status.calibration_percent / 100U), 4,
            WHITE);
        action = "";
    }
    else if(s_help)
    {
        if(!s_status.connected)
        {
            line_(24, "BLE MIDI: poom-midi");
            line_(34, "Connect in music app");
            line_(44, "Choose an instrument");
        }
        else if(melody_())
        {
            line_(24, "Tilt to select note");
            line_(34, "Hold A to play");
            line_(44, "Release A to stop");
        }
        else
        {
            line_(24, "A: arm / pause");
            line_(34, "Strike down to play");
            line_(44, "Harder = louder");
        }
        action = "A:BACK";
    }
    else
    {
        const uint8_t total = row_count_();
        if(s_selected >= total)
            s_selected = total - 1;
        if(s_selected < s_scroll)
            s_scroll = s_selected;
        if(s_selected >= s_scroll + VISIBLE_ROWS)
            s_scroll = s_selected - VISIBLE_ROWS + 1;
        for(uint8_t visible = 0; visible < VISIBLE_ROWS; ++visible)
        {
            uint8_t index = s_scroll + visible;
            if(index >= total)
                break;
            row_label_(row_(index), text, sizeof(text));
            const int y = ROW_Y + visible * ROW_STEP;
            line_(y, text);
            if(index == s_selected)
                poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, 9, INVERT);
        }
        /* Tiny scroll indicators stay outside the text area. */
        if(s_scroll > 0)
            poom_arduboy_fill_rect(124, 24, 2, 2, WHITE);
        if(s_scroll + VISIBLE_ROWS < total)
            poom_arduboy_fill_rect(124, 48, 2, 2, WHITE);
        if(row_(s_selected) == ROW_CALIBRATE)
            action = "A:CAL";
        else if(row_(s_selected) == ROW_HELP)
            action = "A:VIEW";
        else if(s_status.sensor_wait)
            action = "A:WAIT";
        else if(melody_())
            action = s_status.playing ? "A:RELEASE" : "A:HOLD";
        else
            action = s_status.enabled ? "A:PAUSE" : "A:ARM";
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(action);
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print("B:BACK");
    poom_arduboy_display();
}

static void exit_(void)
{
    s_active = false;
    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", on_button_, USER);
        s_buttons_subscribed = false;
    }
    if(s_status_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(POOM_MIDI_STATUS_TOPIC, on_status_,
                                       USER);
        s_status_subscribed = false;
    }
    poom_motion_midi_stop();
    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

static uint8_t adjust_(uint8_t value, int direction, uint8_t max)
{
    int next = value + direction;
    return (uint8_t)(next < 0 ? 0 : (next > max ? max : next));
}

static void adjust_row_(int direction)
{
    switch(row_(s_selected))
    {
        case ROW_MODE:
            s_config.mode = adjust_(s_config.mode, direction, POOM_MIDI_MELODY);
            s_scroll      = 0;
            break;
        case ROW_SOUND:
            s_config.drum = adjust_(s_config.drum, direction, 5);
            break;
        case ROW_SENSITIVITY:
            s_config.sensitivity = adjust_(s_config.sensitivity, direction, 2);
            break;
        case ROW_RESPONSE:
            s_config.response = adjust_(s_config.response, direction, 2);
            break;
        case ROW_SCALE:
            s_config.scale =
                adjust_(s_config.scale, direction, POOM_MIDI_MINOR);
            break;
        case ROW_TONIC:
            s_config.tonic = adjust_(s_config.tonic, direction, 11);
            break;
        case ROW_OCTAVE:
            s_config.octave = adjust_(s_config.octave, direction, 8);
            break;
        case ROW_INTENSITY:
            s_config.intensity = adjust_(s_config.intensity, direction, 2);
            break;
        case ROW_STYLE:
            s_config.moving_note = direction > 0;
            break;
        default:
            return;
    }
    poom_motion_midi_set_config(&s_config);
}

static void on_button_(const poom_sbus_msg_t* msg, void* user)
{
    (void)user;
    if(!s_active || !msg || msg->len < sizeof(button_event_msg_t))
        return;
    button_event_msg_t event;
    memcpy(&event, msg->data, sizeof(event));
    /* Gate uses physical down/up, not the delayed single-click event. */
    if(event.button == BUTTON_A && event.event == BUTTON_PRESS_UP)
    {
        s_a_down = false;
        poom_motion_midi_gate(false);
        return;
    }
    if(event.button == BUTTON_B && event.event == BUTTON_SINGLE_CLICK)
    {
        exit_();
        return;
    }
    /* Stop on navigation down, but keep the existing single-click navigation
     * convention. In particular, B's trailing click must not reach ZEN. */
    if(event.button != BUTTON_A && event.event == BUTTON_PRESS_DOWN)
    {
        poom_motion_midi_pause();
        s_status.playing = s_status.enabled = false;
        render_();
        return;
    }
    if((event.button == BUTTON_A && event.event != BUTTON_PRESS_DOWN) ||
       (event.button != BUTTON_A && event.event != BUTTON_SINGLE_CLICK))
        return;
    if(event.button == BUTTON_A)
    {
        if(s_a_down)
            return;
        s_a_down = true;
        if(s_start_failed || s_status.error)
        {
            if(s_start_failed)
                s_start_failed = !poom_motion_midi_start();
            else
                poom_motion_midi_calibrate();
            s_status.error               = false;
            s_status.calibrated          = false;
            s_status.calibration_percent = 0;
            s_calibrating                = true;
            s_calibration_pending        = true;
        }
        else if(s_calibrating)
        {
            return;
        }
        else if(s_help)
        {
            s_help = false;
        }
        else if(row_(s_selected) == ROW_HELP)
        {
            poom_motion_midi_pause();
            s_help = true;
        }
        else if(row_(s_selected) == ROW_CALIBRATE)
        {
            poom_motion_midi_calibrate();
            s_status.calibrated          = false;
            s_status.calibration_percent = 0;
            s_calibrating                = true;
            s_calibration_pending        = true;
        }
        else if(melody_())
        {
            poom_motion_midi_gate(true);
        }
        else
        {
            poom_motion_midi_toggle_drums();
        }
    }
    else if(!s_help && !s_calibrating && !s_start_failed && !s_status.error)
    {
        poom_motion_midi_pause();
        s_status.playing = s_status.enabled = false;
        if(event.button == BUTTON_UP && s_selected > 0)
            s_selected--;
        else if(event.button == BUTTON_DOWN && s_selected + 1 < row_count_())
            s_selected++;
        else if(event.button == BUTTON_LEFT)
            adjust_row_(-1);
        else if(event.button == BUTTON_RIGHT)
            adjust_row_(1);
    }
    render_();
}

static void on_status_(const poom_sbus_msg_t* msg, void* user)
{
    (void)msg;
    (void)user;
    if(!s_active)
        return;
    poom_midi_status_t status;
    poom_motion_midi_get_status(&status);
    /* Ignore a queued pre-calibration snapshot until the worker acknowledges
     * the request; it must not dismiss the progress screen prematurely. */
    if(s_calibration_pending && status.calibrated && !status.error)
        return;
    s_calibration_pending = false;
    const bool changed    = memcmp(&status, &s_status, sizeof(status)) != 0;
    s_status              = status;
    if(s_status.calibrated)
        s_calibrating = false;
    else if(!s_status.error)
        s_calibrating = true;
    if(s_status.hit_count != s_last_hit)
    {
        s_last_hit          = s_status.hit_count;
        s_hit_display_until = now_ms_() + 700U;
    }
    bool expired =
        s_hit_display_until && (int32_t)(s_hit_display_until - now_ms_()) <= 0;
    if(expired)
        s_hit_display_until = 0;
    if(changed || expired)
        render_();
}

void menu_midi_init(void)
{
    s_active   = true;
    s_selected = s_scroll = 0;
    s_help = s_a_down = s_start_failed = false;
    s_calibration_pending              = false;
    s_calibrating                      = true;
    s_last_hit = s_hit_display_until = 0;
    memset(&s_status, 0, sizeof(s_status));
    poom_motion_midi_get_config(&s_config);
    s_buttons_subscribed =
        poom_sbus_subscribe_cb("input/button", on_button_, USER);
    s_status_subscribed =
        poom_sbus_subscribe_cb(POOM_MIDI_STATUS_TOPIC, on_status_, USER);
    if(!s_buttons_subscribed || !s_status_subscribed)
    {
        exit_();
        return;
    }
    render_();
    s_start_failed = !poom_motion_midi_start();
    if(s_start_failed)
        render_();
}
