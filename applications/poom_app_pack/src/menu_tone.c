// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_tone.h"

#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "cJSON.h"
#include "input_events.h"
#include "poom_buz_theme.h"
#include "poom_sbus.h"
#include "sd_card.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

#define HEADER_H (11)

#define LIST_Y0 (14)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)
#define VISIBLE_ROWS (4)

#define MAX_PRESETS (7)
#define MAX_SD_TONES (24)
#define SD_TONE_NAME_MAX (28)

typedef enum
{
    MENU_TONE_STATE_SOURCE = 0,
    MENU_TONE_STATE_PRESETS,
    MENU_TONE_STATE_SD_LIST,
    MENU_TONE_STATE_INFO,
} menu_tone_state_t;

static const char *const k_preset_titles[MAX_PRESETS] = {
    "Mario Theme",
    "Zelda Item",
    "Tetris",
    "Pac-Man Intro",
    "GameBoy Start",
    "Sonic Ring",
    "Megaman Jump",
};

static const poom_buz_theme_melody_id_t k_preset_map[MAX_PRESETS] = {
    POOM_BUZ_THEME_MELODY_MARIO,
    POOM_BUZ_THEME_MELODY_ZELDA,
    POOM_BUZ_THEME_MELODY_TETRIS,
    POOM_BUZ_THEME_MELODY_PACMAN,
    POOM_BUZ_THEME_MELODY_GAMEBOY,
    POOM_BUZ_THEME_MELODY_SONIC,
    POOM_BUZ_THEME_MELODY_MEGAMAN,
};

static bool s_buttons_subscribed = false;
static menu_tone_state_t s_state = MENU_TONE_STATE_SOURCE;

static int s_source_selected = 0;

static int s_list_selected = 0;
static int s_list_scroll = 0;

static char s_sd_tones[MAX_SD_TONES][SD_TONE_NAME_MAX];
static int s_sd_tone_count = 0;

static char s_info_line0[22] = "";
static char s_info_line1[22] = "";

static void render_(void);
static void render_source_(void);
static void render_list_(const char *title, const char *const *items, int count, const char *footer_left, const char *footer_right);
static void render_sd_list_(void);
static void render_info_(void);
static void menu_tone_exit_(void);
static void on_button_any_(const poom_sbus_msg_t *msg, void *user);

/**
 * @brief Draws the menu header.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void draw_header_(const char *title)
{
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(title ? title : "TONE");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void list_adjust_scroll_(int count)
{
    if (count <= 0)
    {
        s_list_selected = 0;
        s_list_scroll = 0;
        return;
    }

    if (s_list_selected < 0)
    {
        s_list_selected = 0;
    }
    if (s_list_selected > (count - 1))
    {
        s_list_selected = count - 1;
    }

    if (s_list_selected < s_list_scroll)
    {
        s_list_scroll = s_list_selected;
    }
    if (s_list_selected >= (s_list_scroll + VISIBLE_ROWS))
    {
        s_list_scroll = s_list_selected - VISIBLE_ROWS + 1;
    }

    int max_scroll = count - VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_list_scroll < 0)
    {
        s_list_scroll = 0;
    }
    if (s_list_scroll > max_scroll)
    {
        s_list_scroll = max_scroll;
    }
}

/**
 * @brief Internal helper for `clamp_selection`.
 *
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void clamp_selection_(int count)
{
    if (count <= 0)
    {
        s_list_selected = 0;
        return;
    }
    if (s_list_selected < 0)
    {
        s_list_selected = 0;
    }
    if (s_list_selected > (count - 1))
    {
        s_list_selected = count - 1;
    }
}

/**
 * @brief Internal helper for `set_info`.
 *
 * @param[in] l0 Parameter passed to the helper.
 * @param[in] l1 Parameter passed to the helper.
 * @return void
 */
static void set_info_(const char *l0, const char *l1)
{
    (void)snprintf(s_info_line0, sizeof(s_info_line0), "%.21s", l0 ? l0 : "");
    (void)snprintf(s_info_line1, sizeof(s_info_line1), "%.21s", l1 ? l1 : "");
    s_state = MENU_TONE_STATE_INFO;
}

/**
 * @brief Internal helper for `ends_with`.
 *
 * @param[in] s Parameter passed to the helper.
 * @param[in] suffix Parameter passed to the helper.
 * @return bool
 */
static bool ends_with_(const char *s, const char *suffix)
{
    if ((s == NULL) || (suffix == NULL))
    {
        return false;
    }
    const size_t sl = strlen(s);
    const size_t su = strlen(suffix);
    if (su > sl)
    {
        return false;
    }
    return strcmp(s + (sl - su), suffix) == 0;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] a Parameter passed to the helper.
 * @param[in] b Parameter passed to the helper.
 * @return int
 */
static int cmp_str_(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

/**
 * @brief Internal helper for ltrim.
 *
 * @param[in] s Parameter passed to the helper.
 * @return char *
 */
static char *ltrim_(char *s)
{
    if (s == NULL)
    {
        return s;
    }
    while ((*s != '\0') && isspace((unsigned char)*s))
    {
        s++;
    }
    return s;
}

/**
 * @brief Internal helper for `rtrim`.
 *
 * @param[in] s Parameter passed to the helper.
 * @return void
 */
static void rtrim_(char *s)
{
    if (s == NULL)
    {
        return;
    }
    size_t n = strlen(s);
    while ((n > 0U) && isspace((unsigned char)s[n - 1U]))
    {
        s[n - 1U] = '\0';
        n--;
    }
}

/**
 * @brief Parses input data for this menu module.
 *
 * @param[in] buf Parameter passed to the helper.
 * @param[in] len Parameter passed to the helper.
 * @param[in] out_evs Parameter passed to the helper.
 * @param[in] out_count Parameter passed to the helper.
 * @param[in] out_pause Parameter passed to the helper.
 * @return bool
 */
static bool parse_tone_text_(const uint8_t *buf, size_t len, poom_buz_theme_event_t **out_evs, size_t *out_count, uint32_t *out_pause)
{
    if ((buf == NULL) || (len == 0U) || (out_evs == NULL) || (out_count == NULL) || (out_pause == NULL))
    {
        return false;
    }

    *out_evs = NULL;
    *out_count = 0U;
    *out_pause = 0U;

    char *text = (char *)calloc(1U, len + 1U);
    if (text == NULL)
    {
        return false;
    }
    (void)memcpy(text, buf, len);
    text[len] = '\0';

    poom_buz_theme_event_t *evs = (poom_buz_theme_event_t *)calloc(1024U, sizeof(*evs));
    if (evs == NULL)
    {
        free(text);
        return false;
    }

    bool in_events = false;
    size_t written = 0U;
    uint32_t pause = 0U;

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\r\n", &saveptr); line != NULL; line = strtok_r(NULL, "\r\n", &saveptr))
    {
        char *p = ltrim_(line);
        rtrim_(p);
        if ((p[0] == '\0') || (p[0] == '#'))
        {
            continue;
        }
        if (strncmp(p, "POOMTONE", 8) == 0)
        {
            continue;
        }
        if (strncmp(p, "name", 4) == 0)
        {
            continue;
        }
        if (strncmp(p, "pause", 5) == 0)
        {
            const char *q = strchr(p, ':');
            if (q == NULL)
            {
                q = strchr(p, '=');
            }
            if (q != NULL)
            {
                q++;
                while ((*q != '\0') && isspace((unsigned char)*q))
                {
                    q++;
                }
                unsigned long v = strtoul(q, NULL, 10);
                if (v > 10000UL)
                {
                    v = 10000UL;
                }
                pause = (uint32_t)v;
            }
            continue;
        }
        if (strncmp(p, "events", 6) == 0)
        {
            in_events = true;
            continue;
        }

        if (!in_events)
        {
            in_events = true;
        }

        char *end = NULL;
        unsigned long freq = strtoul(p, &end, 10);
        if ((end == NULL) || (end == p))
        {
            continue;
        }
        while ((*end != '\0') && (isspace((unsigned char)*end) || (*end == ',') || (*end == ';')))
        {
            end++;
        }
        unsigned long dur = strtoul(end, NULL, 10);
        if (dur == 0UL)
        {
            continue;
        }
        if (freq > 32767UL)
        {
            freq = 32767UL;
        }
        if (dur > 600000UL)
        {
            dur = 600000UL;
        }

        if (written < 1024U)
        {
            evs[written].freq_hz = (uint32_t)freq;
            evs[written].duration_ms = (uint32_t)dur;
            written++;
        }
    }

    free(text);

    if (written == 0U)
    {
        free(evs);
        return false;
    }

    *out_evs = evs;
    *out_count = written;
    *out_pause = pause;
    return true;
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @return void
 */
static void load_sd_tone_list_(void)
{
    (void)memset(s_sd_tones, 0, sizeof(s_sd_tones));
    s_sd_tone_count = 0;

    if (sd_card_is_not_mounted())
    {
        sd_card_begin();
        if (sd_card_mount() != ESP_OK)
        {
            set_info_("SD mount failed", "");
            return;
        }
    }

    (void)mkdir("/sdcard/tones", 0775);

    DIR *dir = opendir("/sdcard/tones");
    if (dir == NULL)
    {
        set_info_("No /tones dir", "");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (s_sd_tone_count >= MAX_SD_TONES)
        {
            break;
        }

        const char *name = ent->d_name;
        if ((name == NULL) || (name[0] == '\0') || (name[0] == '.'))
        {
            continue;
        }
        if (!ends_with_(name, ".tone"))
        {
            continue;
        }

        (void)snprintf(s_sd_tones[s_sd_tone_count], SD_TONE_NAME_MAX, "%.27s", name);
        s_sd_tone_count++;
    }

    (void)closedir(dir);

    const char *ptrs[MAX_SD_TONES];
    for (int i = 0; i < s_sd_tone_count; i++)
    {
        ptrs[i] = s_sd_tones[i];
    }
    qsort(ptrs, (size_t)s_sd_tone_count, sizeof(ptrs[0]), cmp_str_);

    char sorted[MAX_SD_TONES][SD_TONE_NAME_MAX];
    (void)memset(sorted, 0, sizeof(sorted));
    for (int i = 0; i < s_sd_tone_count; i++)
    {
        (void)snprintf(sorted[i], SD_TONE_NAME_MAX, "%s", ptrs[i]);
    }
    (void)memcpy(s_sd_tones, sorted, sizeof(s_sd_tones));

    s_list_selected = 0;
    s_list_scroll = 0;
}

/**
 * @brief Internal helper for `play_tone_file`.
 *
 * @param[in] file_name Parameter passed to the helper.
 * @return bool
 */
static bool play_tone_file_(const char *file_name)
{
    if ((file_name == NULL) || file_name[0] == '\0')
    {
        return false;
    }

    char rel_path[128];
    (void)snprintf(rel_path, sizeof(rel_path), "tones/%s", file_name);

    uint8_t *buf = NULL;
    size_t len = 0U;
    if (sd_card_read_file_to_buffer(rel_path, &buf, &len) != ESP_OK)
    {
        return false;
    }

    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while ((p < end) && isspace((unsigned char)*p))
    {
        p++;
    }

    bool ok = false;
    if ((p < end) && (*p == '{'))
    {
        cJSON *root = cJSON_ParseWithLength((const char *)buf, len);
        free(buf);

        if (root == NULL)
        {
            return false;
        }

        cJSON *events = cJSON_GetObjectItem(root, "events");
        cJSON *pause_ms = cJSON_GetObjectItem(root, "pause_ms");
        if (!cJSON_IsArray(events))
        {
            cJSON_Delete(root);
            return false;
        }

        uint32_t pause = 0U;
        if (cJSON_IsNumber(pause_ms) && (pause_ms->valuedouble > 0.0))
        {
            double v = pause_ms->valuedouble;
            if (v > 10000.0)
            {
                v = 10000.0;
            }
            pause = (uint32_t)v;
        }

        const int count = cJSON_GetArraySize(events);
        if ((count <= 0) || (count > 1024))
        {
            cJSON_Delete(root);
            return false;
        }

        poom_buz_theme_event_t *evs = (poom_buz_theme_event_t *)calloc((size_t)count, sizeof(*evs));
        if (evs == NULL)
        {
            cJSON_Delete(root);
            return false;
        }

        size_t written = 0U;
        for (int i = 0; i < count; i++)
        {
            cJSON *ev = cJSON_GetArrayItem(events, i);
            if (!cJSON_IsArray(ev))
            {
                continue;
            }
            cJSON *f = cJSON_GetArrayItem(ev, 0);
            cJSON *d = cJSON_GetArrayItem(ev, 1);
            if (!cJSON_IsNumber(d))
            {
                continue;
            }

            uint32_t freq = 0U;
            if (cJSON_IsNumber(f))
            {
                double fv = f->valuedouble;
                if (fv < 0.0)
                {
                    fv = 0.0;
                }
                if (fv > 32767.0)
                {
                    fv = 32767.0;
                }
                freq = (uint32_t)fv;
            }

            double dv = d->valuedouble;
            if (dv < 0.0)
            {
                dv = 0.0;
            }
            if (dv > 600000.0)
            {
                dv = 600000.0;
            }
            const uint32_t dur = (uint32_t)dv;
            if (dur == 0U)
            {
                continue;
            }

            evs[written].freq_hz = freq;
            evs[written].duration_ms = dur;
            written++;
        }

        cJSON_Delete(root);

        if (written == 0U)
        {
            free(evs);
            return false;
        }

        poom_buz_theme_stop();
        ok = poom_buz_theme_play_events(evs, written, pause);
        free(evs);
        return ok;
    }

    poom_buz_theme_event_t *evs = NULL;
    size_t count = 0U;
    uint32_t pause = 0U;
    ok = parse_tone_text_(buf, len, &evs, &count, &pause);
    free(buf);
    if (!ok || (evs == NULL) || (count == 0U))
    {
        free(evs);
        return false;
    }

    poom_buz_theme_stop();
    ok = poom_buz_theme_play_events(evs, count, pause);
    free(evs);
    return ok;
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void render_source_(void)
{
    poom_arduboy_clear();
    draw_header_("TONES");

    const char *const items[2] = {"Retro presets", "Saved on SD"};
    s_list_selected = s_source_selected;
    s_list_scroll = 0;
    list_adjust_scroll_(2);

    for (int row = 0; row < 2; row++)
    {
        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(8, y);
        (void)poom_arduboy_print(items[row]);
        if (row == s_source_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:OPEN"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @param[in] items Parameter passed to the helper.
 * @param[in] count Parameter passed to the helper.
 * @param[in] footer_left Parameter passed to the helper.
 * @param[in] footer_right Parameter passed to the helper.
 * @return void
 */
static void render_list_(const char *title, const char *const *items, int count, const char *footer_left, const char *footer_right)
{
    poom_arduboy_clear();
    draw_header_(title);

    list_adjust_scroll_(count);

    for (int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_list_scroll + row;
        if (idx >= count)
        {
            break;
        }

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(6, y);
        (void)poom_arduboy_print(items[idx] ? items[idx] : "");

        if (idx == s_list_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(footer_left ? footer_left : "");
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(footer_right ? footer_right : "");

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void render_sd_list_(void)
{
    static const char *ptrs[MAX_SD_TONES];
    for (int i = 0; i < s_sd_tone_count; i++)
    {
        ptrs[i] = s_sd_tones[i];
    }

    if (s_sd_tone_count == 0)
    {
        poom_arduboy_clear();
        draw_header_("TONES SD");
        poom_arduboy_set_cursor(8, 28);
        (void)poom_arduboy_print(F("No .tone files"));
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:REFR"));
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    render_list_("TONES SD", ptrs, s_sd_tone_count, "A:PLAY", "B:BACK");
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void render_info_(void)
{
    poom_arduboy_clear();
    draw_header_("TONES");

    poom_arduboy_set_cursor(4, 24);
    (void)poom_arduboy_print(s_info_line0);
    poom_arduboy_set_cursor(4, 36);
    (void)poom_arduboy_print(s_info_line1);

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Internal helper for `render`.
 *
 * @return void
 */
static void render_(void)
{
    switch (s_state)
    {
        case MENU_TONE_STATE_SOURCE:
            render_source_();
            break;
        case MENU_TONE_STATE_PRESETS:
            render_list_("TONES", k_preset_titles, MAX_PRESETS, "A:PLAY", "B:BACK");
            break;
        case MENU_TONE_STATE_SD_LIST:
            render_sd_list_();
            break;
        case MENU_TONE_STATE_INFO:
            render_info_();
            break;
        default:
            s_state = MENU_TONE_STATE_SOURCE;
            render_source_();
            break;
    }
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_tone_exit_(void)
{
    poom_buz_theme_stop();

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", on_button_any_, NULL);
        s_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void on_button_any_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    if (msg == NULL || msg->len < sizeof(button_event_msg_t))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (ev.button == BUTTON_B)
    {
        poom_buz_theme_stop();
        if (s_state == MENU_TONE_STATE_SOURCE)
        {
            menu_tone_exit_();
            return;
        }
        s_state = MENU_TONE_STATE_SOURCE;
        render_();
        return;
    }

    if (s_state == MENU_TONE_STATE_INFO)
    {
        s_state = MENU_TONE_STATE_SOURCE;
        render_();
        return;
    }

    if (s_state == MENU_TONE_STATE_SOURCE)
    {
        if (ev.button == BUTTON_UP || ev.button == BUTTON_DOWN)
        {
            s_source_selected = (s_source_selected == 0) ? 1 : 0;
        }
        else if (ev.button == BUTTON_A)
        {
            if (s_source_selected == 0)
            {
                s_state = MENU_TONE_STATE_PRESETS;
                s_list_selected = 0;
                s_list_scroll = 0;
            }
            else
            {
                load_sd_tone_list_();
                if (s_state != MENU_TONE_STATE_INFO)
                {
                    s_state = MENU_TONE_STATE_SD_LIST;
                }
            }
        }
        render_();
        return;
    }

    if (s_state == MENU_TONE_STATE_PRESETS)
    {
        if (ev.button == BUTTON_UP)
        {
            s_list_selected--;
        }
        else if (ev.button == BUTTON_DOWN)
        {
            s_list_selected++;
        }
        else if (ev.button == BUTTON_A)
        {
            clamp_selection_(MAX_PRESETS);
            poom_buz_theme_stop();
            poom_buz_theme_init_melody(k_preset_map[s_list_selected]);
        }
        render_();
        return;
    }

    if (s_state == MENU_TONE_STATE_SD_LIST)
    {
        if (s_sd_tone_count == 0)
        {
            if (ev.button == BUTTON_A)
            {
                load_sd_tone_list_();
                if (s_state != MENU_TONE_STATE_INFO)
                {
                    s_state = MENU_TONE_STATE_SD_LIST;
                }
            }
            render_();
            return;
        }

        if (ev.button == BUTTON_UP)
        {
            s_list_selected--;
        }
        else if (ev.button == BUTTON_DOWN)
        {
            s_list_selected++;
        }
        else if (ev.button == BUTTON_A)
        {
            clamp_selection_(s_sd_tone_count);
            if ((s_list_selected >= 0) && (s_list_selected < s_sd_tone_count))
            {
                const bool ok = play_tone_file_(s_sd_tones[s_list_selected]);
                if (!ok)
                {
                    set_info_("Play failed", s_sd_tones[s_list_selected]);
                }
            }
        }
        render_();
        return;
    }
}

void app_buzzer_menu(void)
{
    s_state = MENU_TONE_STATE_SOURCE;
    s_source_selected = 0;
    s_list_selected = 0;
    s_list_scroll = 0;
    s_sd_tone_count = 0;
    s_info_line0[0] = '\0';
    s_info_line1[0] = '\0';

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", on_button_any_, NULL);
        s_buttons_subscribed = true;
    }

    render_();
}
