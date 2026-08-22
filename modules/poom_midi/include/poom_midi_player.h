// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_MIDI_PLAYER_H
#define POOM_MIDI_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "poom_midi_harmony.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load harmony config from JSON (minimal parser for v1).
 *
 * Returns true on success. On failure, writes a short error message to out_err.
 */
bool poom_midi_player_load_json(const char *json, size_t json_len, char *out_err, size_t out_err_len);

/**
 * @brief Validate harmony JSON (v1) without applying it.
 *
 * Returns true on success. On failure, writes a short error message to out_err.
 */
bool poom_midi_player_validate_json(const char *json, size_t json_len, char *out_err, size_t out_err_len);

bool poom_midi_player_get_config(poom_midi_harmony_config_t *out_cfg);

void poom_midi_player_play(void);
void poom_midi_player_stop(void);
void poom_midi_player_pause(void);
void poom_midi_player_resume(void);

void poom_midi_player_set_loop(bool loop);
bool poom_midi_player_is_playing(void);

/**
 * @brief Non-blocking scheduler tick.
 *
 * Caller provides `now_ms` (monotonic). This function emits MIDI events when due.
 */
void poom_midi_player_update(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* POOM_MIDI_PLAYER_H */
