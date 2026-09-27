// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#ifndef MOTION_ENGINE_H
#define MOTION_ENGINE_H
#include "poom_motion_midi.h"
/* Gesture recognition and MIDI note state, independent of the BLE transport. */
typedef void (*motion_send_fn)(void*,
                               uint8_t status,
                               uint8_t note,
                               uint8_t velocity);
typedef struct
{
    float acc[3];
    float gyro[3];
} motion_sample_t; /* g, degrees/s */
typedef struct
{
    poom_midi_config_t config;
    poom_midi_status_t status;
    motion_send_fn send;
    void* user;
    float gravity[3], center[3], lateral[3], bias[3];
    float sum_acc[3], sum_gyro[3], previous_acc[3];
    float tilt, peak;
    uint16_t calibration_samples;
    uint8_t degree, active_note, active_channel;
    bool gate, hit_armed, peak_pending, have_sample;
    uint32_t sample_ms, hit_ms, peak_ms, quiet_ms, note_on_ms;
} motion_engine_t;
poom_midi_config_t motion_default_config(void);
void motion_engine_init(motion_engine_t*,
                        const poom_midi_config_t*,
                        motion_send_fn,
                        void*);
void motion_engine_configure(motion_engine_t*, const poom_midi_config_t*);
void motion_engine_pause(motion_engine_t*);
void motion_engine_gate(motion_engine_t*, bool pressed);
void motion_engine_toggle_drums(motion_engine_t*);
void motion_engine_calibrate(motion_engine_t*);
/* NULL sample still services note-off deadlines, disconnects and stale input.
 */
void motion_engine_step(motion_engine_t*,
                        const motion_sample_t*,
                        uint32_t now_ms,
                        bool connected);
#endif
