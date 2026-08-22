// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_MOTION_MIDI_H
#define POOM_MOTION_MIDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file poom_motion_midi.h
 * @brief Public API for the motion-driven MIDI application.
 */

/**
 * @brief Starts the motion MIDI module.
 */
void poom_motion_midi_start(void);

/**
 * @brief Stops the motion MIDI module.
 */
void poom_motion_midi_stop(void);

/**
 * @brief Global MIDI note used for HIT -> Note On (range 0..127).
 *
 * This value is intended to be updated by the UI/menu in real time.
 */
extern uint8_t g_midi_note;

/**
 * @brief Global hit threshold used for triggering Note On (range 1..127).
 *
 * This value is intended to be updated by the UI/menu in real time.
 */
extern uint8_t g_hit_threshold;

/**
 * @brief MIDI mode selection.
 */
#define POOM_MIDI_MODE_DRUM   (0U)
#define POOM_MIDI_MODE_MELODY (1U)

/**
 * @brief Global MIDI mode (DRUM or MELODY).
 *
 * This value is intended to be updated by the UI/menu in real time.
 */
extern uint8_t g_midi_mode;

/**
 * @brief Scale selection for MELODY mode.
 */
#define POOM_MIDI_SCALE_PENTATONIC_MAJOR (0U)
#define POOM_MIDI_SCALE_PENTATONIC_MINOR (1U)

/* Short aliases used by UI/labels (same numeric values). */
#define POOM_MIDI_SCALE_MAJOR POOM_MIDI_SCALE_PENTATONIC_MAJOR
#define POOM_MIDI_SCALE_MINOR POOM_MIDI_SCALE_PENTATONIC_MINOR

/**
 * @brief Global scale selection (PENTATONIC_MAJOR / PENTATONIC_MINOR).
 *
 * Used only by MELODY mode. In DRUM mode this value has no effect.
 */
extern uint8_t g_midi_scale;

#ifdef __cplusplus
}
#endif

#endif /* POOM_MOTION_MIDI_H */
