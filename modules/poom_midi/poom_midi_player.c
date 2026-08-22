// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_midi_player.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "poom_midi_ble_out.h"

#define PLAYER_STEPS_MAX POOM_MIDI_HARMONY_STEPS_MAX
#define PLAYER_ARP_NOTES 3U

typedef struct
{
    bool has_cfg;
    poom_midi_harmony_config_t cfg;

    bool playing;
    bool paused;
    uint32_t start_ms;
    uint32_t last_update_ms;

    uint8_t step_index;
    uint32_t step_start_ms;
    uint32_t step_end_ms;

    uint8_t active_notes[3];
    uint8_t active_notes_len;
    bool notes_on;

    // Arpeggio sub-scheduler within a step.
    uint8_t arp_idx;
    uint32_t arp_next_ms;
    uint32_t arp_note_off_ms;
    bool arp_note_on;
    uint8_t arp_note;
} poom_midi_player_state_t;

static poom_midi_player_state_t s_player;
static TaskHandle_t s_player_task = NULL;
static volatile bool s_player_task_stop = false;

static uint32_t now_ms_(void);

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_midi_player_task_(void *arg)
{
    (void)arg;

    while (!s_player_task_stop)
    {
        if (s_player.playing)
        {
            uint32_t t = now_ms_();
            poom_midi_player_update(t);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    s_player_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Runs the internal task for this module.
 *
 * @return void
 */
static void ensure_task_running_(void)
{
    if (s_player_task != NULL)
    {
        return;
    }
    s_player_task_stop = false;
    (void)xTaskCreate(poom_midi_player_task_, "poom_midi_player", 3072, NULL, 3, &s_player_task);
}

/**
 * @brief Internal helper for `now_ms`.
 *
 * @return uint32_t
 */
static uint32_t now_ms_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * @brief Internal helper for `ms_per_beat`.
 *
 * @param[in] tempo_bpm Parameter passed to the function.
 * @return uint32_t
 */
static uint32_t ms_per_beat_(uint16_t tempo_bpm)
{
    if (tempo_bpm == 0U)
    {
        tempo_bpm = 120U;
    }
    return (uint32_t)(60000U / (uint32_t)tempo_bpm);
}

/**
 * @brief Internal helper for `all_notes_off`.
 *
 * @return void
 */
static void all_notes_off_()
{
    if (!s_player.has_cfg)
    {
        return;
    }

    for (uint8_t i = 0; i < s_player.active_notes_len; i++)
    {
        poom_midi_ble_out_note_off(s_player.cfg.channel, s_player.active_notes[i]);
    }
    s_player.active_notes_len = 0U;
    s_player.notes_on = false;
    s_player.arp_note_on = false;

    poom_midi_ble_out_all_notes_off(s_player.cfg.channel);
}

/**
 * @brief Internal helper for `json_get_string`.
 *
 * @param[in] obj Parameter passed to the function.
 * @param[in] key Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return bool
 */
static bool json_get_string_(const cJSON *obj, const char *key, const char **out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsString(v) || (v->valuestring == NULL))
    {
        return false;
    }
    *out = v->valuestring;
    return true;
}

/**
 * @brief Internal helper for `json_get_u32`.
 *
 * @param[in] obj Parameter passed to the function.
 * @param[in] key Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return bool
 */
static bool json_get_u32_(const cJSON *obj, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (!cJSON_IsNumber(v))
    {
        return false;
    }
    if (v->valuedouble < 0.0)
    {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

/**
 * @brief Internal helper for `json_get_bool`.
 *
 * @param[in] obj Parameter passed to the function.
 * @param[in] key Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @return bool
 */
static bool json_get_bool_(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (cJSON_IsBool(v))
    {
        *out = cJSON_IsTrue(v);
        return true;
    }
    return false;
}

/**
 * @brief Internal helper for `set_err`.
 *
 * @param[in] out_err Parameter passed to the function.
 * @param[in] out_err_len Parameter passed to the function.
 * @param[in] msg Parameter passed to the function.
 * @return void
 */
static void set_err_(char *out_err, size_t out_err_len, const char *msg)
{
    if ((out_err == NULL) || (out_err_len == 0U))
    {
        return;
    }
    (void)snprintf(out_err, out_err_len, "%s", (msg != NULL) ? msg : "error");
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] json Parameter passed to the function.
 * @param[in] json_len Parameter passed to the function.
 * @param[in] out_cfg Parameter passed to the function.
 * @param[in] out_err Parameter passed to the function.
 * @param[in] out_err_len Parameter passed to the function.
 * @return bool
 */
static bool parse_cfg_from_json_(const char *json,
                                size_t json_len,
                                poom_midi_harmony_config_t *out_cfg,
                                char *out_err,
                                size_t out_err_len)
{
    (void)json_len;

    if (out_err && out_err_len)
    {
        out_err[0] = '\0';
    }

    if ((out_cfg == NULL) || (json == NULL) || (json[0] == '\0'))
    {
        set_err_(out_err, out_err_len, "empty json");
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        set_err_(out_err, out_err_len, "json parse failed");
        return false;
    }

    const char *type = NULL;
    uint32_t version = 0U;
    if (!json_get_string_(root, "type", &type) || (strcmp(type, "poom_midi_harmony") != 0))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "type must be poom_midi_harmony");
        return false;
    }
    if (!json_get_u32_(root, "version", &version) || (version != 1U))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "version must be 1");
        return false;
    }

    poom_midi_harmony_config_t cfg = {0};
    uint32_t tmp = 0U;

    if (!json_get_u32_(root, "tempo_bpm", &tmp) || (tmp < 20U) || (tmp > 300U))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "tempo_bpm 20..300");
        return false;
    }
    cfg.tempo_bpm = (uint16_t)tmp;

    const char *key = NULL;
    if (!json_get_string_(root, "key", &key) || !poom_midi_parse_key_semitone(key, &cfg.key_semitone))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "invalid key");
        return false;
    }

    const char *scale = NULL;
    if (!json_get_string_(root, "scale", &scale) || !poom_midi_parse_scale(scale, &cfg.scale))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "invalid scale");
        return false;
    }

    if (!json_get_u32_(root, "octave", &tmp) || (tmp > 8U))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "octave 0..8");
        return false;
    }
    cfg.octave = (uint8_t)tmp;

    if (!json_get_u32_(root, "channel", &tmp) || (tmp > 15U))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "channel 0..15");
        return false;
    }
    cfg.channel = (uint8_t)tmp;

    if (!json_get_u32_(root, "program", &tmp) || (tmp > 127U))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "program 0..127");
        return false;
    }
    cfg.program = (uint8_t)tmp;

    (void)json_get_bool_(root, "loop", &cfg.loop);

    const char *pattern = NULL;
    if (!json_get_string_(root, "pattern", &pattern) || !poom_midi_parse_pattern(pattern, &cfg.pattern))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "invalid pattern");
        return false;
    }

    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    if (!cJSON_IsArray(steps))
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "steps must be array");
        return false;
    }

    uint8_t step_count = 0U;
    cJSON *step = NULL;
    cJSON_ArrayForEach(step, steps)
    {
        if (step_count >= PLAYER_STEPS_MAX)
        {
            break;
        }
        const char *chord = NULL;
        uint32_t dur = 0U;
        if (!json_get_string_(step, "chord", &chord))
        {
            cJSON_Delete(root);
            set_err_(out_err, out_err_len, "step missing chord");
            return false;
        }
        if (!json_get_u32_(step, "duration_beats", &dur) || (dur == 0U) || (dur > 127U))
        {
            cJSON_Delete(root);
            set_err_(out_err, out_err_len, "duration_beats 1..127");
            return false;
        }

        poom_midi_degree_t deg;
        if (!poom_midi_parse_degree(chord, &deg))
        {
            cJSON_Delete(root);
            set_err_(out_err, out_err_len, "invalid chord degree");
            return false;
        }

        cfg.steps[step_count].degree = deg;
        cfg.steps[step_count].duration_beats = (uint8_t)dur;
        step_count++;
    }
    cfg.steps_len = step_count;
    if (cfg.steps_len == 0U)
    {
        cJSON_Delete(root);
        set_err_(out_err, out_err_len, "steps empty");
        return false;
    }

    cJSON_Delete(root);

    *out_cfg = cfg;
    return true;
}

bool poom_midi_player_validate_json(const char *json, size_t json_len, char *out_err, size_t out_err_len)
{
    poom_midi_harmony_config_t cfg = {0};
    return parse_cfg_from_json_(json, json_len, &cfg, out_err, out_err_len);
}

bool poom_midi_player_load_json(const char *json, size_t json_len, char *out_err, size_t out_err_len)
{
    poom_midi_harmony_config_t cfg = {0};
    if (!parse_cfg_from_json_(json, json_len, &cfg, out_err, out_err_len))
    {
        return false;
    }

    poom_midi_player_stop();
    s_player.cfg = cfg;
    s_player.has_cfg = true;
    return true;
}

bool poom_midi_player_get_config(poom_midi_harmony_config_t *out_cfg)
{
    if ((out_cfg == NULL) || !s_player.has_cfg)
    {
        return false;
    }
    *out_cfg = s_player.cfg;
    return true;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] now_ms Parameter passed to the function.
 * @return void
 */
static void start_step_(uint32_t now_ms)
{
    const poom_midi_harmony_step_t *st = &s_player.cfg.steps[s_player.step_index];
    const uint32_t beat_ms = ms_per_beat_(s_player.cfg.tempo_bpm);
    const uint32_t dur_ms = beat_ms * (uint32_t)st->duration_beats;

    s_player.step_start_ms = now_ms;
    s_player.step_end_ms = now_ms + dur_ms;

    uint8_t chord_notes[3] = {0};
    poom_midi_harmony_build_triad(&s_player.cfg, st->degree, chord_notes);

    memcpy(s_player.active_notes, chord_notes, sizeof(chord_notes));
    s_player.active_notes_len = 3U;
    s_player.notes_on = false;

    s_player.arp_idx = 0U;
    s_player.arp_note_on = false;
    s_player.arp_note = 0U;

    poom_midi_ble_out_program_change(s_player.cfg.channel, s_player.cfg.program);

    if (s_player.cfg.pattern == POOM_MIDI_PATTERN_CHORD)
    {
        for (uint8_t i = 0; i < 3U; i++)
        {
            poom_midi_ble_out_note_on(s_player.cfg.channel, s_player.active_notes[i], 96U);
        }
        s_player.notes_on = true;
    }
    else if ((s_player.cfg.pattern == POOM_MIDI_PATTERN_ARPEGGIO_UP) ||
             (s_player.cfg.pattern == POOM_MIDI_PATTERN_ARPEGGIO_DOWN) ||
             (s_player.cfg.pattern == POOM_MIDI_PATTERN_BASS_CHORD))
    {
        uint32_t slice_ms = dur_ms / 3U;
        if (slice_ms < 30U)
        {
            slice_ms = 30U;
        }
        s_player.arp_next_ms = now_ms;
        s_player.arp_note_off_ms = now_ms;
        (void)slice_ms;
    }
}

void poom_midi_player_play(void)
{
    if (!s_player.has_cfg)
    {
        return;
    }

    poom_midi_ble_out_init();
    ensure_task_running_();

    s_player.playing = true;
    s_player.paused = false;
    s_player.start_ms = now_ms_();
    s_player.last_update_ms = s_player.start_ms;
    s_player.step_index = 0U;
    start_step_(s_player.start_ms);
}

void poom_midi_player_stop(void)
{
    if (!s_player.playing && !s_player.paused)
    {
        s_player.playing = false;
        s_player.paused = false;
        s_player.notes_on = false;
        s_player.active_notes_len = 0U;
        s_player.arp_note_on = false;
        return;
    }

    all_notes_off_();
    s_player.playing = false;
    s_player.paused = false;
}

void poom_midi_player_pause(void)
{
    if (!s_player.playing)
    {
        return;
    }
    all_notes_off_();
    s_player.playing = false;
    s_player.paused = true;
}

void poom_midi_player_resume(void)
{
    if (!s_player.paused || !s_player.has_cfg)
    {
        return;
    }
    s_player.paused = false;
    s_player.playing = true;
    s_player.last_update_ms = now_ms_();
}

void poom_midi_player_set_loop(bool loop)
{
    if (!s_player.has_cfg)
    {
        return;
    }
    s_player.cfg.loop = loop;
}

bool poom_midi_player_is_playing(void)
{
    return s_player.playing;
}

/**
 * @brief Internal helper for `advance_step`.
 *
 * @param[in] now_ms Parameter passed to the function.
 * @return void
 */
static void advance_step_(uint32_t now_ms)
{
    all_notes_off_();

    s_player.step_index++;
    if (s_player.step_index >= s_player.cfg.steps_len)
    {
        if (s_player.cfg.loop)
        {
            s_player.step_index = 0U;
        }
        else
        {
            s_player.playing = false;
            return;
        }
    }

    start_step_(now_ms);
}

/**
 * @brief Internal helper for `update_arpeggio`.
 *
 * @param[in] now_ms Parameter passed to the function.
 * @return void
 */
static void update_arpeggio_(uint32_t now_ms)
{
    if (!s_player.playing)
    {
        return;
    }

    if ((s_player.cfg.pattern != POOM_MIDI_PATTERN_ARPEGGIO_UP) &&
        (s_player.cfg.pattern != POOM_MIDI_PATTERN_ARPEGGIO_DOWN) &&
        (s_player.cfg.pattern != POOM_MIDI_PATTERN_BASS_CHORD))
    {
        return;
    }

    const poom_midi_harmony_step_t *st = &s_player.cfg.steps[s_player.step_index];
    const uint32_t beat_ms = ms_per_beat_(s_player.cfg.tempo_bpm);
    const uint32_t dur_ms = beat_ms * (uint32_t)st->duration_beats;
    uint32_t slice_ms = dur_ms / 3U;
    if (slice_ms < 30U)
    {
        slice_ms = 30U;
    }

    if (s_player.arp_note_on && (now_ms >= s_player.arp_note_off_ms))
    {
        poom_midi_ble_out_note_off(s_player.cfg.channel, s_player.arp_note);
        s_player.arp_note_on = false;
    }

    if (now_ms < s_player.arp_next_ms)
    {
        return;
    }

    if (s_player.arp_idx >= 3U)
    {
        if ((s_player.cfg.pattern == POOM_MIDI_PATTERN_BASS_CHORD) && !s_player.notes_on)
        {
            for (uint8_t i = 0; i < 3U; i++)
            {
                poom_midi_ble_out_note_on(s_player.cfg.channel, s_player.active_notes[i], 92U);
            }
            s_player.notes_on = true;
        }
        s_player.arp_next_ms = s_player.step_end_ms + 1U;
        return;
    }

    uint8_t idx = s_player.arp_idx;
    if (s_player.cfg.pattern == POOM_MIDI_PATTERN_ARPEGGIO_DOWN)
    {
        idx = (uint8_t)(2U - idx);
    }
    if (s_player.cfg.pattern == POOM_MIDI_PATTERN_BASS_CHORD)
    {
        idx = 0U;
    }

    const uint8_t note = s_player.active_notes[idx];
    poom_midi_ble_out_note_on(s_player.cfg.channel, note, 96U);
    s_player.arp_note = note;
    s_player.arp_note_on = true;
    s_player.arp_note_off_ms = now_ms + (slice_ms / 2U);

    s_player.arp_idx++;
    s_player.arp_next_ms = now_ms + slice_ms;
}

void poom_midi_player_update(uint32_t now_ms)
{
    if (!s_player.playing || !s_player.has_cfg)
    {
        return;
    }

    s_player.last_update_ms = now_ms;

    update_arpeggio_(now_ms);

    if (now_ms >= s_player.step_end_ms)
    {
        advance_step_(now_ms);
    }
}
