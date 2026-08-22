// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_MIDI_HARMONY_H
#define POOM_MIDI_HARMONY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POOM_MIDI_SCALE_MAJOR = 0,
    POOM_MIDI_SCALE_MINOR,
    POOM_MIDI_SCALE_PENTATONIC_MAJOR,
    POOM_MIDI_SCALE_PENTATONIC_MINOR,
} poom_midi_scale_t;

typedef enum
{
    POOM_MIDI_PATTERN_CHORD = 0,
    POOM_MIDI_PATTERN_ARPEGGIO_UP,
    POOM_MIDI_PATTERN_ARPEGGIO_DOWN,
    POOM_MIDI_PATTERN_BASS_CHORD,
} poom_midi_pattern_t;

typedef enum
{
    POOM_MIDI_DEGREE_I = 0,
    POOM_MIDI_DEGREE_II,
    POOM_MIDI_DEGREE_III,
    POOM_MIDI_DEGREE_IV,
    POOM_MIDI_DEGREE_V,
    POOM_MIDI_DEGREE_VI,
    POOM_MIDI_DEGREE_VII,
} poom_midi_degree_t;

typedef struct
{
    poom_midi_degree_t degree;
    uint8_t duration_beats; /* 1..127 */
} poom_midi_harmony_step_t;

#define POOM_MIDI_HARMONY_STEPS_MAX (32U)

typedef struct
{
    uint16_t tempo_bpm; /* 20..300 typical */
    uint8_t key_semitone; /* 0..11, C=0 */
    poom_midi_scale_t scale;
    uint8_t octave;  /* 0..8 typical, MIDI formula uses octave+1 */
    uint8_t channel; /* 0..15 */
    uint8_t program; /* 0..127 */
    bool loop;
    poom_midi_pattern_t pattern;
    poom_midi_harmony_step_t steps[POOM_MIDI_HARMONY_STEPS_MAX];
    uint8_t steps_len;
} poom_midi_harmony_config_t;

/**
 * @brief Parses a key string into a semitone value.
 *
 * @param[in] key Input key string.
 * @param[out] out_semitone Parsed semitone value.
 * @return bool
 */
bool poom_midi_parse_key_semitone(const char *key, uint8_t *out_semitone);

/**
 * @brief Parses a scale string into a scale enum.
 *
 * @param[in] scale Input scale string.
 * @param[out] out_scale Parsed scale value.
 * @return bool
 */
bool poom_midi_parse_scale(const char *scale, poom_midi_scale_t *out_scale);

/**
 * @brief Parses a pattern string into a pattern enum.
 *
 * @param[in] pattern Input pattern string.
 * @param[out] out_pattern Parsed pattern value.
 * @return bool
 */
bool poom_midi_parse_pattern(const char *pattern, poom_midi_pattern_t *out_pattern);

/**
 * @brief Parses a degree string into a degree enum.
 *
 * @param[in] degree Input degree string.
 * @param[out] out_degree Parsed degree value.
 * @return bool
 */
bool poom_midi_parse_degree(const char *degree, poom_midi_degree_t *out_degree);

/**
 * @brief Convert (key+scale+degree) to a triad chord (3 notes).
 *
 * For MAJOR/MINOR: uses diatonic triads derived from the scale.
 * For PENTATONIC_*: uses a simple triad built by stacking scale degrees (0,2,4).
 */
void poom_midi_harmony_build_triad(const poom_midi_harmony_config_t *cfg,
                                  poom_midi_degree_t degree,
                                  uint8_t out_notes[3]);

/**
 * @brief MIDI note formula: 12*(octave+1) + semitone.
 */
static inline uint8_t poom_midi_note_from_octave_semitone(uint8_t octave, uint8_t semitone)
{
    const int note = 12 * ((int)octave + 1) + (int)semitone;
    if (note < 0)
    {
        return 0U;
    }
    if (note > 127)
    {
        return 127U;
    }
    return (uint8_t)note;
}

#ifdef __cplusplus
}
#endif

#endif /* POOM_MIDI_HARMONY_H */
