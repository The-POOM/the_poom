// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#ifndef POOM_MOTION_MIDI_H
#define POOM_MOTION_MIDI_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define POOM_MIDI_STATUS_TOPIC "poom/midi/status"
typedef enum
{
    POOM_MIDI_DRUM,
    POOM_MIDI_MELODY
} poom_midi_mode_t;
typedef enum
{
    POOM_MIDI_PENTA_MAJOR,
    POOM_MIDI_PENTA_MINOR,
    POOM_MIDI_MAJOR,
    POOM_MIDI_MINOR
} poom_midi_scale_t;
typedef struct
{
    poom_midi_mode_t mode;
    uint8_t drum;        /* Kick, snare, closed/open hi-hat, tom, crash. */
    uint8_t sensitivity; /* Low, normal, high. */
    uint8_t response;    /* Soft, normal, strong velocity response. */
    poom_midi_scale_t scale;
    uint8_t tonic;     /* C=0 .. B=11. */
    uint8_t octave;    /* Display octave 0..8. */
    uint8_t intensity; /* Soft, normal, strong melody velocity. */
    bool moving_note;  /* False: latch pitch at A down; true: follow tilt. */
} poom_midi_config_t;
typedef struct
{
    bool connected, calibrated, enabled, playing, error;
    bool sensor_wait; /* Interrupted motion input; the calibrated center is
                         retained. */
    uint8_t calibration_percent;
    uint8_t note; /* Sounding pitch, or selected pitch when silent. */
    uint8_t velocity;
    uint32_t hit_count;
} poom_midi_status_t;
/* Menu calls are serialized through the worker, which owns all MIDI output. */
bool poom_motion_midi_start(void);
void poom_motion_midi_stop(void);
void poom_motion_midi_get_config(poom_midi_config_t* out);
void poom_motion_midi_get_status(poom_midi_status_t* out);
void poom_motion_midi_set_config(const poom_midi_config_t* config);
void poom_motion_midi_pause(void);
void poom_motion_midi_gate(bool pressed);
void poom_motion_midi_toggle_drums(void);
void poom_motion_midi_calibrate(void);
#ifdef __cplusplus
}
#endif
#endif
