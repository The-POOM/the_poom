// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_web_midi.h"

#include <string.h>

#include "poom_midi_player.h"

bool poom_web_midi_load_harmony_json(const char *json,
                                    size_t json_len,
                                    bool auto_play,
                                    char *out_err,
                                    size_t out_err_len)
{
    if (out_err && out_err_len)
    {
        out_err[0] = '\0';
    }

    const bool ok = poom_midi_player_load_json(json, json_len, out_err, out_err_len);
    if (!ok)
    {
        return false;
    }

    if (auto_play)
    {
        poom_midi_player_play();
    }
    return true;
}

void poom_web_midi_stop(void)
{
    poom_midi_player_stop();
}
