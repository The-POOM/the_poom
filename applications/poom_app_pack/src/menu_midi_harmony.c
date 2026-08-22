// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_midi_harmony.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "input_events.h"
#include "i2c.h"
#include "poom_sbus.h"
#include "sd_card.h"

#include "ble_midi.h"
#include "poom_midi_player.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED "ble/midi/connected"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

#define BOX_Y (12)
#define BOX_H (40)

#define ROW0_Y (24)
#define ROW_STEP (10)
#define ROW_H (9)

#define POOM_MIDI_HARMONY_DIR "/sdcard/harmonies"
#define POOM_MIDI_HARMONY_FILES_MAX (12U)
#define POOM_MIDI_HARMONY_FILE_NAME_MAX (28U)

static bool s_active = false;
static bool s_buttons_subscribed = false;
static bool s_ble_subscribed = false;
static bool s_ble_connected = false;
static uint8_t s_selected_row = 0U;
static char s_files[POOM_MIDI_HARMONY_FILES_MAX][POOM_MIDI_HARMONY_FILE_NAME_MAX];
static uint8_t s_files_len = 0U;
static uint8_t s_file_index = 0U;
static bool s_loop = true;
static char s_last_error[22] = "-";

static void menu_midi_harmony_on_button_(const poom_sbus_msg_t *msg, void *user);
static void menu_midi_harmony_on_ble_(const poom_sbus_msg_t *msg, void *user);

/**
 * @brief Internal helper for `menu_midi_harmony_is_json`.
 *
 * @param[in] name Parameter passed to the helper.
 * @return bool
 */
static bool menu_midi_harmony_is_json_(const char *name)
{
    const size_t len = (name != NULL) ? strlen(name) : 0U;
    if (len < 6U)
    {
        return false;
    }
    return (strcmp(name + (len - 5U), ".json") == 0);
}

/**
 * @brief Sorts internal items for this menu module.
 *
 * @return void
 */
static void menu_midi_harmony_sort_files_(void)
{
    for (uint8_t i = 0U; i < s_files_len; i++)
    {
        for (uint8_t j = (uint8_t)(i + 1U); j < s_files_len; j++)
        {
            if (strcmp(s_files[i], s_files[j]) > 0)
            {
                char tmp[POOM_MIDI_HARMONY_FILE_NAME_MAX];
                memcpy(tmp, s_files[i], sizeof(tmp));
                memcpy(s_files[i], s_files[j], sizeof(s_files[i]));
                memcpy(s_files[j], tmp, sizeof(s_files[j]));
            }
        }
    }
}

/**
 * @brief Internal helper for `menu_midi_harmony_scan_files`.
 *
 * @return void
 */
static void menu_midi_harmony_scan_files_(void)
{
    DIR *dir;
    struct dirent *entry;

    s_files_len = 0U;
    s_file_index = 0U;

    dir = opendir(POOM_MIDI_HARMONY_DIR);
    if (dir == NULL)
    {
        (void)snprintf(s_last_error, sizeof(s_last_error), "NODIR");
        return;
    }

    while (((entry = readdir(dir)) != NULL) && (s_files_len < POOM_MIDI_HARMONY_FILES_MAX))
    {
        const char *name = entry->d_name;
        if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0))
        {
            continue;
        }
        if (!menu_midi_harmony_is_json_(name))
        {
            continue;
        }
        if (strlen(name) >= POOM_MIDI_HARMONY_FILE_NAME_MAX)
        {
            continue;
        }

        (void)strncpy(s_files[s_files_len], name, POOM_MIDI_HARMONY_FILE_NAME_MAX - 1U);
        s_files[s_files_len][POOM_MIDI_HARMONY_FILE_NAME_MAX - 1U] = '\0';
        s_files_len++;
    }

    (void)closedir(dir);

    if (s_files_len == 0U)
    {
        (void)snprintf(s_last_error, sizeof(s_last_error), "EMPTY");
        return;
    }

    menu_midi_harmony_sort_files_();
    (void)snprintf(s_last_error, sizeof(s_last_error), "-");
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_midi_harmony_render_(void)
{
    char line_ble[22];
    char line1[22];
    char line2[22];
    char line3[22];
    char err_line[22];
    const char *file_name = (s_files_len > 0U) ? s_files[s_file_index] : NULL;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(20, 2);
    (void)poom_arduboy_print(F("MIDI HARMONY"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(4, 14);
    (void)snprintf(line_ble, sizeof(line_ble), "BLE: %s", s_ble_connected ? "CONNECTED" : "PAIRING");
    (void)poom_arduboy_print(line_ble);

    (void)snprintf(line1, sizeof(line1), "Play:%s", poom_midi_player_is_playing() ? "ON" : "OFF");
    (void)snprintf(line2, sizeof(line2), "Loop:%s", s_loop ? "ON" : "OFF");
    if (file_name != NULL)
    {
        char display_name[POOM_MIDI_HARMONY_FILE_NAME_MAX];
        (void)snprintf(display_name, sizeof(display_name), "%s", file_name);
        char *dot = strrchr(display_name, '.');
        if (dot != NULL)
        {
            *dot = '\0';
        }
        (void)snprintf(line3, sizeof(line3), "Mel:%.16s", display_name);
    }
    else
    {
        (void)snprintf(line3, sizeof(line3), "Mel:<none>");
    }

    poom_arduboy_set_cursor(4, ROW0_Y);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(4, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(4, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(line3);

    if (s_selected_row > 2U)
    {
        s_selected_row = 2U;
    }

    {
        const int16_t sel_y = (int16_t)(ROW0_Y + (int16_t)s_selected_row * ROW_STEP);
        poom_arduboy_fill_rect(1, (int16_t)(sel_y - 1), (int16_t)(ARDUBOY_WIDTH - 2), ROW_H, INVERT);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    if ((s_last_error[0] != '-') || (s_last_error[1] != '\0'))
    {
        (void)snprintf(err_line, sizeof(err_line), "Err:%.12s", s_last_error);
        poom_arduboy_set_cursor(52, 56);
        (void)poom_arduboy_print(err_line);
    }

    i2c_lock();
    poom_arduboy_display();
    i2c_unlock();
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @return bool
 */
static bool menu_midi_harmony_load_selected_file_(void)
{
    char path[64];
    if (s_files_len == 0U)
    {
        (void)snprintf(s_last_error, sizeof(s_last_error), "EMPTY");
        return false;
    }
    (void)snprintf(path, sizeof(path), "%s/%s", POOM_MIDI_HARMONY_DIR, s_files[s_file_index]);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        (void)snprintf(s_last_error, sizeof(s_last_error), "NOFILE");
        return false;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1U, f);
    fclose(f);
    buf[n] = '\0';

    char err[32] = {0};
    if (!poom_midi_player_load_json(buf, n, err, sizeof(err)))
    {
        (void)snprintf(s_last_error, sizeof(s_last_error), "BADJSON");
        return false;
    }

    (void)snprintf(s_last_error, sizeof(s_last_error), "-");
    return true;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_midi_harmony_exit_(void)
{
    s_active = false;

    poom_midi_player_stop();

    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_midi_harmony_on_button_, "menu_midi_harmony");
        s_buttons_subscribed = false;
    }

    if (s_ble_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED, menu_midi_harmony_on_ble_, "menu_midi_harmony");
        s_ble_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Internal helper for `menu_midi_harmony_on_ble`.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_midi_harmony_on_ble_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    if (!s_active)
    {
        return;
    }
    if ((msg == NULL) || (msg->len < 1U))
    {
        return;
    }
    const bool connected = (msg->data[0] != 0U);
    if (connected == s_ble_connected)
    {
        return;
    }
    s_ble_connected = connected;
    menu_midi_harmony_render_();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user Parameter passed to the helper.
 * @return void
 */
static void menu_midi_harmony_on_button_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    if (!s_active)
    {
        return;
    }
    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
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
        menu_midi_harmony_exit_();
        return;
    }

    if (ev.button == BUTTON_UP)
    {
        if (s_selected_row > 0U)
        {
            s_selected_row--;
        }
    }
    else if (ev.button == BUTTON_DOWN)
    {
        if (s_selected_row < 2U)
        {
            s_selected_row++;
        }
    }
    else if ((ev.button == BUTTON_LEFT) || (ev.button == BUTTON_RIGHT))
    {
        const bool inc = (ev.button == BUTTON_RIGHT);

        if (s_selected_row == 0U)
        {
            if (inc)
            {
                if (!menu_midi_harmony_load_selected_file_())
                {
                    menu_midi_harmony_render_();
                    return;
                }
                poom_midi_player_set_loop(s_loop);
                poom_midi_player_play();
            }
            else
            {
                poom_midi_player_stop();
            }
        }
        else if (s_selected_row == 1U)
        {
            s_loop = !s_loop;
            poom_midi_player_set_loop(s_loop);
        }
        else if (s_selected_row == 2U)
        {
            if (s_files_len == 0U)
            {
            }
            else if (inc)
            {
                s_file_index = (uint8_t)((s_file_index + 1U) % s_files_len);
            }
            else
            {
                s_file_index = (uint8_t)((s_file_index == 0U) ? (s_files_len - 1U) : (s_file_index - 1U));
            }
        }
    }

    menu_midi_harmony_render_();
}

void menu_midi_harmony_init(void)
{
    s_active = true;
    s_selected_row = 0U;
    s_ble_connected = blemidi_is_connected();
    s_loop = true;
    s_last_error[0] = '-';
    s_last_error[1] = '\0';

    if (!s_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_midi_harmony_on_button_, "menu_midi_harmony");
        s_buttons_subscribed = true;
    }
    if (!s_ble_subscribed)
    {
        (void)poom_sbus_subscribe_cb(POOM_MIDI_BLE_SBUS_TOPIC_CONNECTED, menu_midi_harmony_on_ble_, "menu_midi_harmony");
        s_ble_subscribed = true;
    }

    sd_card_begin();
    if (!sd_card_is_mounted())
    {
        (void)sd_card_mount();
    }
    (void)mkdir(POOM_MIDI_HARMONY_DIR, 0775);
    menu_midi_harmony_scan_files_();

    menu_midi_harmony_render_();
}
