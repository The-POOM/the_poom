// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WEB_MIDI_H
#define POOM_WEB_MIDI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load harmony JSON into the MIDI player and (optionally) start playback.
 */
bool poom_web_midi_load_harmony_json(const char *json, size_t json_len, bool auto_play, char *out_err, size_t out_err_len);

/**
 * @brief Stops active Web MIDI playback.
 *
 * @return void
 */
void poom_web_midi_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WEB_MIDI_H */
