// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "menu_air_ble.h"
#include "Arduboy2.h"
#include "poom_sbus.h"
#include "poom_secrets_store.h"
#include "poom_wii.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define MENU_AIR_BLE_REDRAW_TOPIC "menu_air_ble/redraw"
#define MENU_AIR_BLE_CFG_KEY "air_ble_cfg"
#define MENU_AIR_BLE_CFG_MAGIC (0x41424c45UL)
#define MENU_AIR_BLE_CFG_VERSION (1U)
#define MENU_AIR_BLE_VISIBLE_ROWS (4U)

#ifndef BTN_A
#define BTN_A 0
#endif

#ifndef BTN_B
#define BTN_B 1
#endif

#ifndef BTN_LEFT
#define BTN_LEFT 2
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT 3
#endif

#ifndef BTN_UP
#define BTN_UP 4
#endif

#ifndef BTN_DOWN
#define BTN_DOWN 5
#endif

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN 0
#endif

#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP 1
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} button_event_msg_t;

typedef enum
{
    MENU_AIR_BLE_SCREEN_HOME = 0,
    MENU_AIR_BLE_SCREEN_RUNNING,
    MENU_AIR_BLE_SCREEN_SETTINGS,
    MENU_AIR_BLE_SCREEN_ACTION_PICKER,
    MENU_AIR_BLE_SCREEN_ASCII_PICKER,
} menu_air_ble_screen_t;

typedef enum
{
    MENU_AIR_BLE_HOME_RUNNING = 0,
    MENU_AIR_BLE_HOME_SETTINGS,
} menu_air_ble_home_option_t;

typedef enum
{
    MENU_AIR_BLE_ACTION_NONE = 0,
    MENU_AIR_BLE_ACTION_ASCII,
    MENU_AIR_BLE_ACTION_MOUSE_LEFT,
    MENU_AIR_BLE_ACTION_MOUSE_RIGHT,
    MENU_AIR_BLE_ACTION_TOGGLE_MOTION,
} menu_air_ble_action_kind_t;

typedef struct
{
    uint8_t kind;
    char ascii;
    uint8_t reserved[2];
} menu_air_ble_binding_t;

typedef struct
{
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    menu_air_ble_binding_t bindings[6];
} menu_air_ble_config_store_t;

typedef struct
{
    menu_air_ble_action_kind_t kind;
    const char *label;
} menu_air_ble_action_option_t;

static const uint8_t k_binding_button_ids[6] = {
    BTN_A,
    BTN_B,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_UP,
    BTN_DOWN,
};

static const char *k_binding_button_labels[6] = {
    "BTN A",
    "BTN B",
    "LEFT",
    "RIGHT",
    "UP",
    "DOWN",
};

static const menu_air_ble_action_option_t k_action_options[] = {
    {MENU_AIR_BLE_ACTION_NONE, "NONE"},
    {MENU_AIR_BLE_ACTION_ASCII, "ASCII"},
    {MENU_AIR_BLE_ACTION_MOUSE_LEFT, "MOUSE LEFT"},
    {MENU_AIR_BLE_ACTION_MOUSE_RIGHT, "MOUSE RIGHT"},
    {MENU_AIR_BLE_ACTION_TOGGLE_MOTION, "TOGGLE MOVE"},
};

static const char k_ascii_options[] = "abcdefghijklmnopqrstuvwxyz0123456789";

static bool s_air_ble_initialized = false;
static bool s_air_ble_initializing = false;
static bool s_air_ble_init_ok = true;
static bool s_air_ble_handler_registered = false;
static bool s_air_ble_buttons_subscribed = false;
static bool s_air_ble_redraw_subscribed = false;
static bool s_air_ble_cfg_loaded = false;
static bool s_air_ble_cfg_persist_ready = false;

static menu_air_ble_screen_t s_air_ble_screen = MENU_AIR_BLE_SCREEN_HOME;
static uint8_t s_air_ble_home_selected = (uint8_t)MENU_AIR_BLE_HOME_RUNNING;
static uint8_t s_air_ble_settings_selected = 0U;
static uint8_t s_air_ble_action_selected = 0U;
static uint8_t s_air_ble_ascii_selected = 0U;
static uint8_t s_air_ble_edit_binding = 0U;
static uint8_t s_air_ble_button_down_mask = 0U;
static menu_air_ble_binding_t s_air_ble_bindings[6];

static void menu_air_ble_on_button_event_(const poom_sbus_msg_t *msg, void *user);
static void menu_air_ble_on_redraw_(const poom_sbus_msg_t *msg, void *user);
static void menu_air_ble_request_redraw_(void);

static void menu_air_ble_connection_handler_(bool connected)
{
    (void)connected;
    menu_air_ble_request_redraw_();
}

static int menu_air_ble_button_index_from_id_(uint8_t button_id)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)(sizeof(k_binding_button_ids) / sizeof(k_binding_button_ids[0])); ++i)
    {
        if (k_binding_button_ids[i] == button_id)
        {
            return (int)i;
        }
    }

    return -1;
}

static void menu_air_ble_binding_set_(menu_air_ble_binding_t *binding,
                                      menu_air_ble_action_kind_t kind,
                                      char ascii)
{
    if (binding == NULL)
    {
        return;
    }

    binding->kind = (uint8_t)kind;
    binding->ascii = ascii;
    binding->reserved[0] = 0U;
    binding->reserved[1] = 0U;
}

static void menu_air_ble_apply_default_config_(void)
{
    menu_air_ble_binding_set_(&s_air_ble_bindings[0], MENU_AIR_BLE_ACTION_MOUSE_LEFT, '\0');
    menu_air_ble_binding_set_(&s_air_ble_bindings[1], MENU_AIR_BLE_ACTION_MOUSE_RIGHT, '\0');
    menu_air_ble_binding_set_(&s_air_ble_bindings[2], MENU_AIR_BLE_ACTION_ASCII, 'q');
    menu_air_ble_binding_set_(&s_air_ble_bindings[3], MENU_AIR_BLE_ACTION_ASCII, 'e');
    menu_air_ble_binding_set_(&s_air_ble_bindings[4], MENU_AIR_BLE_ACTION_ASCII, 'w');
    menu_air_ble_binding_set_(&s_air_ble_bindings[5], MENU_AIR_BLE_ACTION_ASCII, 's');
}

static bool menu_air_ble_binding_is_valid_(const menu_air_ble_binding_t *binding)
{
    const uint8_t kind = (binding != NULL) ? binding->kind : 0U;
    size_t i;

    if (binding == NULL)
    {
        return false;
    }

    if (kind > (uint8_t)MENU_AIR_BLE_ACTION_TOGGLE_MOTION)
    {
        return false;
    }

    if (kind != (uint8_t)MENU_AIR_BLE_ACTION_ASCII)
    {
        return true;
    }

    for (i = 0U; i < (sizeof(k_ascii_options) / sizeof(k_ascii_options[0])); ++i)
    {
        if (k_ascii_options[i] == binding->ascii)
        {
            return true;
        }
    }

    return false;
}

static void menu_air_ble_save_config_(void)
{
    menu_air_ble_config_store_t cfg;

    if (!s_air_ble_cfg_persist_ready)
    {
        return;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = MENU_AIR_BLE_CFG_MAGIC;
    cfg.version = MENU_AIR_BLE_CFG_VERSION;
    (void)memcpy(cfg.bindings, s_air_ble_bindings, sizeof(s_air_ble_bindings));
    (void)poom_secrets_set_blob(MENU_AIR_BLE_CFG_KEY, &cfg, sizeof(cfg));
}

static void menu_air_ble_load_config_(void)
{
    menu_air_ble_config_store_t cfg;
    size_t len = sizeof(cfg);
    uint8_t i;

    if (s_air_ble_cfg_loaded)
    {
        return;
    }

    s_air_ble_cfg_loaded = true;
    menu_air_ble_apply_default_config_();

    if (poom_secrets_init() != ESP_OK)
    {
        s_air_ble_cfg_persist_ready = false;
        return;
    }

    s_air_ble_cfg_persist_ready = true;
    if (poom_secrets_get_blob(MENU_AIR_BLE_CFG_KEY, &cfg, &len) != ESP_OK)
    {
        return;
    }

    if ((len != sizeof(cfg)) || (cfg.magic != MENU_AIR_BLE_CFG_MAGIC) || (cfg.version != MENU_AIR_BLE_CFG_VERSION))
    {
        return;
    }

    for (i = 0U; i < (uint8_t)(sizeof(cfg.bindings) / sizeof(cfg.bindings[0])); ++i)
    {
        if (!menu_air_ble_binding_is_valid_(&cfg.bindings[i]))
        {
            return;
        }
    }

    (void)memcpy(s_air_ble_bindings, cfg.bindings, sizeof(s_air_ble_bindings));
}

static const char *menu_air_ble_button_label_(uint8_t binding_index)
{
    if (binding_index >= (uint8_t)(sizeof(k_binding_button_labels) / sizeof(k_binding_button_labels[0])))
    {
        return "BTN ?";
    }

    return k_binding_button_labels[binding_index];
}

static const char *menu_air_ble_ascii_option_label_(uint8_t index)
{
    static char s_label[2];

    if (index >= (uint8_t)(sizeof(k_ascii_options) / sizeof(k_ascii_options[0])))
    {
        return "?";
    }

    s_label[0] = k_ascii_options[index];
    if ((s_label[0] >= 'a') && (s_label[0] <= 'z'))
    {
        s_label[0] = (char)(s_label[0] - ('a' - 'A'));
    }
    s_label[1] = '\0';
    return s_label;
}

static int menu_air_ble_ascii_option_index_(char ascii)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)(sizeof(k_ascii_options) / sizeof(k_ascii_options[0])); ++i)
    {
        if (k_ascii_options[i] == ascii)
        {
            return (int)i;
        }
    }

    return 0;
}

static int menu_air_ble_action_option_index_(menu_air_ble_action_kind_t kind)
{
    uint8_t i;

    for (i = 0U; i < (uint8_t)(sizeof(k_action_options) / sizeof(k_action_options[0])); ++i)
    {
        if (k_action_options[i].kind == kind)
        {
            return (int)i;
        }
    }

    return 0;
}

static void menu_air_ble_binding_label_(const menu_air_ble_binding_t *binding,
                                        char *out_label,
                                        size_t out_label_len)
{
    if ((out_label == NULL) || (out_label_len == 0U))
    {
        return;
    }

    out_label[0] = '\0';
    if (binding == NULL)
    {
        return;
    }

    switch ((menu_air_ble_action_kind_t)binding->kind)
    {
        case MENU_AIR_BLE_ACTION_NONE:
            (void)snprintf(out_label, out_label_len, "NONE");
            break;

        case MENU_AIR_BLE_ACTION_ASCII:
        {
            char disp = binding->ascii;
            if ((disp >= 'a') && (disp <= 'z'))
            {
                disp = (char)(disp - ('a' - 'A'));
            }
            (void)snprintf(out_label, out_label_len, "KEY:%c", disp);
            break;
        }

        case MENU_AIR_BLE_ACTION_MOUSE_LEFT:
            (void)snprintf(out_label, out_label_len, "M LEFT");
            break;

        case MENU_AIR_BLE_ACTION_MOUSE_RIGHT:
            (void)snprintf(out_label, out_label_len, "M RIGHT");
            break;

        case MENU_AIR_BLE_ACTION_TOGGLE_MOTION:
            (void)snprintf(out_label, out_label_len, "TOGGLE");
            break;

        default:
            (void)snprintf(out_label, out_label_len, "?");
            break;
    }
}

static void menu_air_ble_draw_header_(const char *title)
{
    poom_arduboy_set_cursor(2, 2);
    (void)poom_arduboy_print(title != NULL ? title : "POOM WII");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

static void menu_air_ble_draw_footer_(const char *left, const char *right)
{
    poom_arduboy_set_cursor(0, 56);
    if (left != NULL)
    {
        (void)poom_arduboy_print(left);
    }

    if (right != NULL)
    {
        poom_arduboy_set_cursor(76, 56);
        (void)poom_arduboy_print(right);
    }
}

static void menu_air_ble_draw_home_(void)
{
    const bool running = poom_wii_is_running();
    const bool connected = poom_wii_is_connected();
    char line[22];
    const int16_t running_x = 10;
    const int16_t settings_x = 64;

    poom_arduboy_clear();
    menu_air_ble_draw_header_("POOM WII");

    if (s_air_ble_initializing)
    {
        (void)snprintf(line, sizeof(line), "Init: starting");
    }
    else if (!s_air_ble_init_ok)
    {
        (void)snprintf(line, sizeof(line), "Init: error");
    }
    else if (running)
    {
        (void)snprintf(line, sizeof(line), "Link: running");
    }
    else if (connected)
    {
        (void)snprintf(line, sizeof(line), "Link: connected");
    }
    else
    {
        (void)snprintf(line, sizeof(line), "Link: idle");
    }

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "Move:%s", poom_wii_is_motion_enabled() ? "on" : "off");
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print(line);

    poom_arduboy_set_cursor(4, 42);
    poom_arduboy_set_cursor(running_x, 42);
    (void)poom_arduboy_print("RUNNING");
    poom_arduboy_set_cursor(settings_x, 42);
    (void)poom_arduboy_print("SETTINGS");

    if (s_air_ble_home_selected == (uint8_t)MENU_AIR_BLE_HOME_RUNNING)
    {
        poom_arduboy_set_cursor((int16_t)(running_x - 10), 42);
        (void)poom_arduboy_print("[");
        poom_arduboy_set_cursor((int16_t)(running_x + 48), 42);
        (void)poom_arduboy_print("]");
    }
    else
    {
        poom_arduboy_set_cursor((int16_t)(settings_x - 10), 42);
        (void)poom_arduboy_print("[");
        poom_arduboy_set_cursor((int16_t)(settings_x + 54), 42);
        (void)poom_arduboy_print("]");
    }

    menu_air_ble_draw_footer_("A:SEL", "B:EXIT");
    poom_arduboy_display();
}

static void menu_air_ble_draw_running_(void)
{
    char line[22];

    poom_arduboy_clear();
    menu_air_ble_draw_header_("RUNNING");

    if (!s_air_ble_init_ok)
    {
        (void)snprintf(line, sizeof(line), "Init error");
    }
    else if (poom_wii_is_running())
    {
        (void)snprintf(line, sizeof(line), "State: running");
    }
    else if (poom_wii_is_connected())
    {
        (void)snprintf(line, sizeof(line), "State: connected");
    }
    else
    {
        (void)snprintf(line, sizeof(line), "State: waiting");
    }

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "Move:%s", poom_wii_is_motion_enabled() ? "on" : "off");
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print(line);

    poom_arduboy_set_cursor(4, 40);
    (void)poom_arduboy_print("Exit: LEFT+RIGHT");
    poom_arduboy_set_cursor(4, 50);
    (void)poom_arduboy_print("Buttons mapped");

    poom_arduboy_display();
}

static void menu_air_ble_draw_settings_(void)
{
    const uint8_t total = (uint8_t)(sizeof(s_air_ble_bindings) / sizeof(s_air_ble_bindings[0]));
    uint8_t selected = s_air_ble_settings_selected;
    uint8_t scroll = 0U;
    uint8_t row;

    if (selected >= total)
    {
        selected = 0U;
        s_air_ble_settings_selected = 0U;
    }

    if (selected >= MENU_AIR_BLE_VISIBLE_ROWS)
    {
        scroll = (uint8_t)(selected - MENU_AIR_BLE_VISIBLE_ROWS + 1U);
    }

    poom_arduboy_clear();
    menu_air_ble_draw_header_("SETTINGS");

    for (row = 0U; row < MENU_AIR_BLE_VISIBLE_ROWS; ++row)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        const int16_t y = (int16_t)(16 + (int16_t)row * 10);
        char line[22];
        char action[12];

        if (idx >= total)
        {
            break;
        }

        menu_air_ble_binding_label_(&s_air_ble_bindings[idx], action, sizeof(action));
        (void)snprintf(line,
                       sizeof(line),
                       "%c%-5s %-11s",
                       (idx == selected) ? '>' : ' ',
                       menu_air_ble_button_label_(idx),
                       action);

        poom_arduboy_set_cursor(0, y);
        (void)poom_arduboy_print(line);
    }

    menu_air_ble_draw_footer_("A:EDIT", "B:BACK");
    poom_arduboy_display();
}

static void menu_air_ble_draw_action_picker_(void)
{
    const uint8_t total = (uint8_t)(sizeof(k_action_options) / sizeof(k_action_options[0]));
    uint8_t selected = s_air_ble_action_selected;
    uint8_t scroll = 0U;
    uint8_t row;

    if (selected >= total)
    {
        selected = 0U;
        s_air_ble_action_selected = 0U;
    }

    if (selected >= MENU_AIR_BLE_VISIBLE_ROWS)
    {
        scroll = (uint8_t)(selected - MENU_AIR_BLE_VISIBLE_ROWS + 1U);
    }

    poom_arduboy_clear();
    menu_air_ble_draw_header_(menu_air_ble_button_label_(s_air_ble_edit_binding));

    for (row = 0U; row < MENU_AIR_BLE_VISIBLE_ROWS; ++row)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        const int16_t y = (int16_t)(16 + (int16_t)row * 10);
        char line[22];

        if (idx >= total)
        {
            break;
        }

        (void)snprintf(line,
                       sizeof(line),
                       "%c%s",
                       (idx == selected) ? '>' : ' ',
                       k_action_options[idx].label);

        poom_arduboy_set_cursor(0, y);
        (void)poom_arduboy_print(line);
    }

    menu_air_ble_draw_footer_("A:OK", "B:BACK");
    poom_arduboy_display();
}

static void menu_air_ble_draw_ascii_picker_(void)
{
    const uint8_t total = (uint8_t)(sizeof(k_ascii_options) / sizeof(k_ascii_options[0]));
    uint8_t selected = s_air_ble_ascii_selected;
    uint8_t scroll = 0U;
    uint8_t row;

    if (selected >= total)
    {
        selected = 0U;
        s_air_ble_ascii_selected = 0U;
    }

    if (selected >= MENU_AIR_BLE_VISIBLE_ROWS)
    {
        scroll = (uint8_t)(selected - MENU_AIR_BLE_VISIBLE_ROWS + 1U);
    }

    poom_arduboy_clear();
    menu_air_ble_draw_header_("SELECT ASCII");

    for (row = 0U; row < MENU_AIR_BLE_VISIBLE_ROWS; ++row)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        const int16_t y = (int16_t)(16 + (int16_t)row * 10);
        char line[22];

        if (idx >= total)
        {
            break;
        }

        (void)snprintf(line,
                       sizeof(line),
                       "%c%s",
                       (idx == selected) ? '>' : ' ',
                       menu_air_ble_ascii_option_label_(idx));

        poom_arduboy_set_cursor(0, y);
        (void)poom_arduboy_print(line);
    }

    menu_air_ble_draw_footer_("A:OK", "B:BACK");
    poom_arduboy_display();
}

static void menu_air_ble_draw_current_(void)
{
    switch (s_air_ble_screen)
    {
        case MENU_AIR_BLE_SCREEN_HOME:
            menu_air_ble_draw_home_();
            break;

        case MENU_AIR_BLE_SCREEN_RUNNING:
            menu_air_ble_draw_running_();
            break;

        case MENU_AIR_BLE_SCREEN_SETTINGS:
            menu_air_ble_draw_settings_();
            break;

        case MENU_AIR_BLE_SCREEN_ACTION_PICKER:
            menu_air_ble_draw_action_picker_();
            break;

        case MENU_AIR_BLE_SCREEN_ASCII_PICKER:
            menu_air_ble_draw_ascii_picker_();
            break;

        default:
            menu_air_ble_draw_home_();
            break;
    }
}

static void menu_air_ble_request_redraw_(void)
{
    const uint8_t token = 1U;
    (void)poom_sbus_publish(MENU_AIR_BLE_REDRAW_TOPIC, &token, sizeof(token), 0);
}

static void menu_air_ble_apply_press_(const menu_air_ble_binding_t *binding)
{
    if (binding == NULL)
    {
        return;
    }

    switch ((menu_air_ble_action_kind_t)binding->kind)
    {
        case MENU_AIR_BLE_ACTION_MOUSE_LEFT:
            poom_wii_left_button_press();
            break;

        case MENU_AIR_BLE_ACTION_MOUSE_RIGHT:
            poom_wii_right_button_press();
            break;

        case MENU_AIR_BLE_ACTION_ASCII:
            poom_wii_key_press_ascii(binding->ascii);
            break;

        default:
            break;
    }
}

static void menu_air_ble_apply_release_(const menu_air_ble_binding_t *binding)
{
    if (binding == NULL)
    {
        return;
    }

    switch ((menu_air_ble_action_kind_t)binding->kind)
    {
        case MENU_AIR_BLE_ACTION_MOUSE_LEFT:
            poom_wii_left_button_release();
            break;

        case MENU_AIR_BLE_ACTION_MOUSE_RIGHT:
            poom_wii_right_button_release();
            break;

        case MENU_AIR_BLE_ACTION_ASCII:
            poom_wii_key_release_ascii(binding->ascii);
            break;

        default:
            break;
    }
}

static void menu_air_ble_apply_click_(const menu_air_ble_binding_t *binding)
{
    if (binding == NULL)
    {
        return;
    }

    if ((menu_air_ble_action_kind_t)binding->kind == MENU_AIR_BLE_ACTION_TOGGLE_MOTION)
    {
        poom_wii_toggle_motion_enabled();
        menu_air_ble_draw_current_();
    }
}

static void menu_air_ble_enter_running_(void)
{
    if (s_air_ble_init_ok && s_air_ble_initialized)
    {
        poom_wii_start();
    }

    s_air_ble_button_down_mask = 0U;
    s_air_ble_screen = MENU_AIR_BLE_SCREEN_RUNNING;
    menu_air_ble_draw_current_();
}

static void menu_air_ble_exit_(void)
{
    s_air_ble_button_down_mask = 0U;

    if (s_air_ble_handler_registered)
    {
        poom_wii_set_connection_handler(NULL);
        s_air_ble_handler_registered = false;
    }

    if (s_air_ble_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_air_ble_on_button_event_, "menu_air_ble");
        s_air_ble_buttons_subscribed = false;
    }

    if (s_air_ble_redraw_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(MENU_AIR_BLE_REDRAW_TOPIC, menu_air_ble_on_redraw_, "menu_air_ble");
        s_air_ble_redraw_subscribed = false;
    }

    poom_wii_key_release_all();
    poom_wii_stop();

    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}

static void menu_air_ble_on_redraw_(const poom_sbus_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;
    menu_air_ble_draw_current_();
}

static void menu_air_ble_handle_home_click_(uint8_t button)
{
    if (button == BTN_LEFT)
    {
        s_air_ble_home_selected = (uint8_t)MENU_AIR_BLE_HOME_RUNNING;
    }
    else if (button == BTN_RIGHT)
    {
        s_air_ble_home_selected = (uint8_t)MENU_AIR_BLE_HOME_SETTINGS;
    }
    else if (button == BTN_A)
    {
        if (s_air_ble_home_selected == (uint8_t)MENU_AIR_BLE_HOME_RUNNING)
        {
            menu_air_ble_enter_running_();
            return;
        }

        s_air_ble_screen = MENU_AIR_BLE_SCREEN_SETTINGS;
    }
    else if (button == BTN_B)
    {
        menu_air_ble_exit_();
        return;
    }

    menu_air_ble_draw_current_();
}

static void menu_air_ble_handle_settings_click_(uint8_t button)
{
    const uint8_t total = (uint8_t)(sizeof(s_air_ble_bindings) / sizeof(s_air_ble_bindings[0]));

    if (button == BTN_UP)
    {
        if (s_air_ble_settings_selected == 0U)
        {
            s_air_ble_settings_selected = (uint8_t)(total - 1U);
        }
        else
        {
            s_air_ble_settings_selected--;
        }
    }
    else if (button == BTN_DOWN)
    {
        s_air_ble_settings_selected = (uint8_t)((s_air_ble_settings_selected + 1U) % total);
    }
    else if (button == BTN_A)
    {
        s_air_ble_edit_binding = s_air_ble_settings_selected;
        s_air_ble_action_selected =
            (uint8_t)menu_air_ble_action_option_index_(
                (menu_air_ble_action_kind_t)s_air_ble_bindings[s_air_ble_edit_binding].kind);
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_ACTION_PICKER;
    }
    else if (button == BTN_B)
    {
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_HOME;
    }

    menu_air_ble_draw_current_();
}

static void menu_air_ble_handle_action_click_(uint8_t button)
{
    const uint8_t total = (uint8_t)(sizeof(k_action_options) / sizeof(k_action_options[0]));

    if (button == BTN_UP)
    {
        if (s_air_ble_action_selected == 0U)
        {
            s_air_ble_action_selected = (uint8_t)(total - 1U);
        }
        else
        {
            s_air_ble_action_selected--;
        }
    }
    else if (button == BTN_DOWN)
    {
        s_air_ble_action_selected = (uint8_t)((s_air_ble_action_selected + 1U) % total);
    }
    else if (button == BTN_A)
    {
        const menu_air_ble_action_kind_t kind = k_action_options[s_air_ble_action_selected].kind;

        if (kind == MENU_AIR_BLE_ACTION_ASCII)
        {
            s_air_ble_ascii_selected = (uint8_t)menu_air_ble_ascii_option_index_(
                s_air_ble_bindings[s_air_ble_edit_binding].ascii);
            s_air_ble_screen = MENU_AIR_BLE_SCREEN_ASCII_PICKER;
        }
        else
        {
            menu_air_ble_binding_set_(&s_air_ble_bindings[s_air_ble_edit_binding], kind, '\0');
            menu_air_ble_save_config_();
            s_air_ble_screen = MENU_AIR_BLE_SCREEN_SETTINGS;
        }
    }
    else if (button == BTN_B)
    {
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_SETTINGS;
    }

    menu_air_ble_draw_current_();
}

static void menu_air_ble_handle_ascii_click_(uint8_t button)
{
    const uint8_t total = (uint8_t)(sizeof(k_ascii_options) / sizeof(k_ascii_options[0]));

    if (button == BTN_UP)
    {
        if (s_air_ble_ascii_selected == 0U)
        {
            s_air_ble_ascii_selected = (uint8_t)(total - 1U);
        }
        else
        {
            s_air_ble_ascii_selected--;
        }
    }
    else if (button == BTN_DOWN)
    {
        s_air_ble_ascii_selected = (uint8_t)((s_air_ble_ascii_selected + 1U) % total);
    }
    else if (button == BTN_A)
    {
        menu_air_ble_binding_set_(&s_air_ble_bindings[s_air_ble_edit_binding],
                                  MENU_AIR_BLE_ACTION_ASCII,
                                  k_ascii_options[s_air_ble_ascii_selected]);
        menu_air_ble_save_config_();
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_SETTINGS;
    }
    else if (button == BTN_B)
    {
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_ACTION_PICKER;
    }

    menu_air_ble_draw_current_();
}

static void menu_air_ble_handle_running_event_(const button_event_msg_t *ev)
{
    const uint8_t left_mask = (uint8_t)(1U << BTN_LEFT);
    const uint8_t right_mask = (uint8_t)(1U << BTN_RIGHT);
    const int binding_idx = menu_air_ble_button_index_from_id_(ev->button);
    const menu_air_ble_binding_t *binding =
        (binding_idx >= 0) ? &s_air_ble_bindings[binding_idx] : NULL;

    if (ev->event == BUTTON_PRESS_DOWN)
    {
        s_air_ble_button_down_mask = (uint8_t)(s_air_ble_button_down_mask | (uint8_t)(1U << ev->button));
        if ((s_air_ble_button_down_mask & left_mask) && (s_air_ble_button_down_mask & right_mask))
        {
            menu_air_ble_exit_();
            return;
        }

        menu_air_ble_apply_press_(binding);
        return;
    }

    if (ev->event == BUTTON_PRESS_UP)
    {
        s_air_ble_button_down_mask = (uint8_t)(s_air_ble_button_down_mask & (uint8_t)(~(uint8_t)(1U << ev->button)));
        menu_air_ble_apply_release_(binding);
        return;
    }

    if (ev->event == BUTTON_SINGLE_CLICK)
    {
        menu_air_ble_apply_click_(binding);
    }
}

static void menu_air_ble_on_button_event_(const poom_sbus_msg_t *msg, void *user)
{
    button_event_msg_t ev;

    (void)user;

    if ((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (s_air_ble_screen == MENU_AIR_BLE_SCREEN_RUNNING)
    {
        menu_air_ble_handle_running_event_(&ev);
        return;
    }

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    switch (s_air_ble_screen)
    {
        case MENU_AIR_BLE_SCREEN_HOME:
            menu_air_ble_handle_home_click_(ev.button);
            break;

        case MENU_AIR_BLE_SCREEN_SETTINGS:
            menu_air_ble_handle_settings_click_(ev.button);
            break;

        case MENU_AIR_BLE_SCREEN_ACTION_PICKER:
            menu_air_ble_handle_action_click_(ev.button);
            break;

        case MENU_AIR_BLE_SCREEN_ASCII_PICKER:
            menu_air_ble_handle_ascii_click_(ev.button);
            break;

        default:
            break;
    }
}

void menu_air_ble_display(void)
{
    menu_air_ble_load_config_();

    if (!s_air_ble_initialized)
    {
        s_air_ble_initializing = true;
        s_air_ble_screen = MENU_AIR_BLE_SCREEN_HOME;
        menu_air_ble_draw_current_();

        if (poom_wii_init() == 0U)
        {
            s_air_ble_initialized = true;
            s_air_ble_init_ok = true;
            poom_wii_stop();
        }
        else
        {
            s_air_ble_init_ok = false;
        }

        s_air_ble_initializing = false;
    }

    if (s_air_ble_initialized && !s_air_ble_handler_registered)
    {
        poom_wii_set_connection_handler(menu_air_ble_connection_handler_);
        s_air_ble_handler_registered = true;
    }

    if (!s_air_ble_buttons_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", menu_air_ble_on_button_event_, "menu_air_ble");
        s_air_ble_buttons_subscribed = true;
    }

    if (!s_air_ble_redraw_subscribed)
    {
        (void)poom_sbus_register_topic(MENU_AIR_BLE_REDRAW_TOPIC);
        (void)poom_sbus_subscribe_cb(MENU_AIR_BLE_REDRAW_TOPIC, menu_air_ble_on_redraw_, "menu_air_ble");
        s_air_ble_redraw_subscribed = true;
    }

    s_air_ble_screen = MENU_AIR_BLE_SCREEN_HOME;
    s_air_ble_home_selected = (uint8_t)MENU_AIR_BLE_HOME_RUNNING;
    s_air_ble_settings_selected = 0U;
    s_air_ble_action_selected = 0U;
    s_air_ble_ascii_selected = 0U;
    s_air_ble_edit_binding = 0U;
    s_air_ble_button_down_mask = 0U;

    if (s_air_ble_initialized)
    {
        poom_wii_stop();
    }

    menu_air_ble_request_redraw_();
}
