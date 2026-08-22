// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_midi_harmony.h"

#include <ctype.h>
#include <string.h>

static const char *k_keys_[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

bool poom_midi_parse_key_semitone(const char *key, uint8_t *out_semitone)
{
    if ((key == NULL) || (out_semitone == NULL))
    {
        return false;
    }

    for (uint8_t i = 0; i < 12U; i++)
    {
        if (strcmp(key, k_keys_[i]) == 0)
        {
            *out_semitone = i;
            return true;
        }
    }
    return false;
}

/**
 * @brief Internal helper for `streq`.
 *
 * @param[in] a Parameter passed to the function.
 * @param[in] b Parameter passed to the function.
 * @return bool
 */
static bool streq_(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL))
    {
        return false;
    }
    return strcmp(a, b) == 0;
}

bool poom_midi_parse_scale(const char *scale, poom_midi_scale_t *out_scale)
{
    if ((scale == NULL) || (out_scale == NULL))
    {
        return false;
    }

    if (streq_(scale, "major"))
    {
        *out_scale = POOM_MIDI_SCALE_MAJOR;
        return true;
    }
    if (streq_(scale, "minor"))
    {
        *out_scale = POOM_MIDI_SCALE_MINOR;
        return true;
    }
    if (streq_(scale, "pentatonic_major"))
    {
        *out_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
        return true;
    }
    if (streq_(scale, "pentatonic_minor"))
    {
        *out_scale = POOM_MIDI_SCALE_PENTATONIC_MINOR;
        return true;
    }
    return false;
}

bool poom_midi_parse_pattern(const char *pattern, poom_midi_pattern_t *out_pattern)
{
    if ((pattern == NULL) || (out_pattern == NULL))
    {
        return false;
    }

    if (streq_(pattern, "chord"))
    {
        *out_pattern = POOM_MIDI_PATTERN_CHORD;
        return true;
    }
    if (streq_(pattern, "arpeggio_up"))
    {
        *out_pattern = POOM_MIDI_PATTERN_ARPEGGIO_UP;
        return true;
    }
    if (streq_(pattern, "arpeggio_down"))
    {
        *out_pattern = POOM_MIDI_PATTERN_ARPEGGIO_DOWN;
        return true;
    }
    if (streq_(pattern, "bass_chord"))
    {
        *out_pattern = POOM_MIDI_PATTERN_BASS_CHORD;
        return true;
    }
    return false;
}

/**
 * @brief Internal helper for `is_roman_char`.
 *
 * @param[in] c Parameter passed to the function.
 * @return bool
 */
static bool is_roman_char_(char c)
{
    return (c == 'I') || (c == 'V') || (c == 'i') || (c == 'v');
}

bool poom_midi_parse_degree(const char *degree, poom_midi_degree_t *out_degree)
{
    if ((degree == NULL) || (out_degree == NULL))
    {
        return false;
    }

    if (streq_(degree, "I") || streq_(degree, "i"))
    {
        *out_degree = POOM_MIDI_DEGREE_I;
        return true;
    }
    if (streq_(degree, "II") || streq_(degree, "ii"))
    {
        *out_degree = POOM_MIDI_DEGREE_II;
        return true;
    }
    if (streq_(degree, "III") || streq_(degree, "iii"))
    {
        *out_degree = POOM_MIDI_DEGREE_III;
        return true;
    }
    if (streq_(degree, "IV") || streq_(degree, "iv"))
    {
        *out_degree = POOM_MIDI_DEGREE_IV;
        return true;
    }
    if (streq_(degree, "V") || streq_(degree, "v"))
    {
        *out_degree = POOM_MIDI_DEGREE_V;
        return true;
    }
    if (streq_(degree, "VI") || streq_(degree, "vi"))
    {
        *out_degree = POOM_MIDI_DEGREE_VI;
        return true;
    }
    if (streq_(degree, "VII") || streq_(degree, "vii"))
    {
        *out_degree = POOM_MIDI_DEGREE_VII;
        return true;
    }

    for (size_t i = 0; degree[i] != '\0'; i++)
    {
        if (!is_roman_char_(degree[i]))
        {
            return false;
        }
    }
    return false;
}

/**
 * @brief Internal helper for `clamp_note`.
 *
 * @param[in] note Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t clamp_note_(int note)
{
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

/**
 * @brief Internal helper for `diatonic_scale_semitone`.
 *
 * @param[in] scale Parameter passed to the function.
 * @param[in] degree_idx Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t diatonic_scale_semitone_(poom_midi_scale_t scale, uint8_t degree_idx)
{
    static const uint8_t major_scale[7] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t minor_scale[7] = {0, 2, 3, 5, 7, 8, 10}; // natural minor

    if (degree_idx > 6U)
    {
        degree_idx = 6U;
    }

    if (scale == POOM_MIDI_SCALE_MINOR)
    {
        return minor_scale[degree_idx];
    }
    return major_scale[degree_idx];
}

/**
 * @brief Internal helper for `pentatonic_scale_semitone`.
 *
 * @param[in] scale Parameter passed to the function.
 * @param[in] degree_idx Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t pentatonic_scale_semitone_(poom_midi_scale_t scale, uint8_t degree_idx)
{
    static const uint8_t pmaj[5] = {0, 2, 4, 7, 9};
    static const uint8_t pmin[5] = {0, 3, 5, 7, 10};
    if (degree_idx > 4U)
    {
        degree_idx = 4U;
    }
    if (scale == POOM_MIDI_SCALE_PENTATONIC_MINOR)
    {
        return pmin[degree_idx];
    }
    return pmaj[degree_idx];
}

void poom_midi_harmony_build_triad(const poom_midi_harmony_config_t *cfg,
                                  poom_midi_degree_t degree,
                                  uint8_t out_notes[3])
{
    if ((cfg == NULL) || (out_notes == NULL))
    {
        return;
    }

    const uint8_t root_degree = (uint8_t)degree; // 0..6
    const uint8_t root_octave = cfg->octave;

    if ((cfg->scale == POOM_MIDI_SCALE_PENTATONIC_MAJOR) || (cfg->scale == POOM_MIDI_SCALE_PENTATONIC_MINOR))
    {
        const uint8_t d0 = (root_degree > 4U) ? 4U : root_degree;
        const uint8_t d1 = (uint8_t)((d0 + 2U) % 5U);
        const uint8_t d2 = (uint8_t)((d0 + 4U) % 5U);

        const int n0 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)pentatonic_scale_semitone_(cfg->scale, d0);
        const int n1 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)pentatonic_scale_semitone_(cfg->scale, d1);
        const int n2 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)pentatonic_scale_semitone_(cfg->scale, d2);

        out_notes[0] = clamp_note_(n0);
        out_notes[1] = clamp_note_(n1);
        out_notes[2] = clamp_note_(n2);
        return;
    }

    const uint8_t d0 = root_degree;
    const uint8_t d1 = (uint8_t)((root_degree + 2U) % 7U);
    const uint8_t d2 = (uint8_t)((root_degree + 4U) % 7U);

    const int n0 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)diatonic_scale_semitone_(cfg->scale, d0);
    const int n1 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)diatonic_scale_semitone_(cfg->scale, d1);
    const int n2 = 12 * ((int)root_octave + 1) + (int)cfg->key_semitone + (int)diatonic_scale_semitone_(cfg->scale, d2);

    out_notes[0] = clamp_note_(n0);
    out_notes[1] = clamp_note_(n1);
    out_notes[2] = clamp_note_(n2);
}
