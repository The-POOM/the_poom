// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_ble_keyboard.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "poom_ble_hid.h"
#include "poom_sbus.h"

#if POOM_BLE_KEYBOARD_ENABLE_LOG
    static const char *POOM_BLE_KEYBOARD_TAG = "poom_ble_keyboard";

    #define POOM_BLE_KEYBOARD_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_BLE_KEYBOARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BLE_KEYBOARD_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_BLE_KEYBOARD_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_BLE_KEYBOARD_DEBUG_LOG_ENABLED
        #define POOM_BLE_KEYBOARD_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_BLE_KEYBOARD_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_BLE_KEYBOARD_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_BLE_KEYBOARD_PRINTF_E(...) do { } while (0)
    #define POOM_BLE_KEYBOARD_PRINTF_I(...) do { } while (0)
    #define POOM_BLE_KEYBOARD_PRINTF_D(...) do { } while (0)
#endif

#define POOM_BLE_KEYBOARD_BUTTON_COUNT (6U)
#define POOM_BLE_KEYBOARD_EVENT_PRESS_DOWN (0U)
#define POOM_BLE_KEYBOARD_EVENT_PRESS_UP (1U)

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} poom_ble_keyboard_button_event_msg_t;

static bool s_is_connected = false;
static bool s_keyboard_mode = false;
static bool s_is_started = false;
static poom_ble_keyboard_connection_cb_t s_connection_cb = NULL;

/**
 * @brief Sends mapped media action for one button.
 *
 * @param[in] button Button index.
 * @param[in] active Action active state.
 * @return void
 */
static void poom_ble_keyboard_handle_media_action_(uint8_t button, bool active)
{
    switch (button)
    {
        case 0U:
            poom_ble_hid_send_play(active);
            break;
        case 1U:
            poom_ble_hid_send_pause(active);
            break;
        case 2U:
            poom_ble_hid_send_prev_track(active);
            break;
        case 3U:
            poom_ble_hid_send_next_track(active);
            break;
        case 4U:
            poom_ble_hid_send_volume_up(active);
            break;
        case 5U:
            poom_ble_hid_send_volume_down(active);
            break;
        default:
            break;
    }
}

/**
 * @brief Handles button events from sbus.
 *
 * @param[in] msg Incoming sbus message.
 * @param[in] user User context pointer.
 * @return void
 */
static void poom_ble_keyboard_on_button_event_(const poom_sbus_msg_t *msg, void *user)
{
    static const char button_chars[POOM_BLE_KEYBOARD_BUTTON_COUNT] = {'a', 'b', 'c', 'd', 'e', 'f'};
    poom_ble_keyboard_button_event_msg_t ev;

    (void)user;

    if ((msg == NULL) || (msg->len < sizeof(ev)) || (!s_is_connected))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.button >= POOM_BLE_KEYBOARD_BUTTON_COUNT)
    {
        return;
    }

    switch (ev.event)
    {
        case POOM_BLE_KEYBOARD_EVENT_PRESS_DOWN:
            if (s_keyboard_mode)
            {
                poom_ble_hid_key_press_ascii(button_chars[ev.button]);
            }
            else
            {
                poom_ble_keyboard_handle_media_action_(ev.button, true);
            }
            break;

        case POOM_BLE_KEYBOARD_EVENT_PRESS_UP:
            if (s_keyboard_mode)
            {
                poom_ble_hid_key_release_all();
            }
            else
            {
                poom_ble_keyboard_handle_media_action_(ev.button, false);
            }
            break;

        default:
            break;
    }

    POOM_BLE_KEYBOARD_PRINTF_D("topic=%s button=%u event=%u ts=%lu",
                               msg->topic,
                               (unsigned)ev.button,
                               (unsigned)ev.event,
                               (unsigned long)ev.ts_ms);
}

/**
 * @brief Handles BLE HID connection state changes.
 *
 * @param[in] connected BLE HID connection state.
 * @return void
 */
static void poom_ble_keyboard_on_connection_changed_(bool connected)
{
    s_is_connected = connected;

    if (connected)
    {
        POOM_BLE_KEYBOARD_PRINTF_I("BLE HID connected");
    }
    else
    {
        POOM_BLE_KEYBOARD_PRINTF_I("BLE HID disconnected");
    }

    if (s_connection_cb != NULL)
    {
        s_connection_cb(connected);
    }
}

void poom_ble_keyboard_set_connection_callback(poom_ble_keyboard_connection_cb_t cb)
{
    if (cb == NULL)
    {
        POOM_BLE_KEYBOARD_PRINTF_E("connection callback is NULL");
        return;
    }

    s_connection_cb = cb;
}

void poom_ble_keyboard_start(void)
{
    if (s_is_started)
    {
        return;
    }

    poom_ble_hid_set_connection_callback(poom_ble_keyboard_on_connection_changed_);
    poom_ble_hid_start();
    if (!poom_ble_hid_is_started())
    {
        POOM_BLE_KEYBOARD_PRINTF_E("poom_ble_hid_start failed");
        return;
    }
    poom_sbus_subscribe_cb("input/button", poom_ble_keyboard_on_button_event_, "poom_ble_keyboard");

    s_is_started = true;
    POOM_BLE_KEYBOARD_PRINTF_I("module started");
}

void poom_ble_keyboard_stop(void)
{
    if (!s_is_started)
    {
        return;
    }

    s_is_started = false;
    s_is_connected = false;
    s_connection_cb = NULL;

    poom_ble_hid_key_release_all();
    poom_sbus_unsubscribe_cb("input/button", poom_ble_keyboard_on_button_event_, "poom_ble_keyboard");

    poom_ble_hid_stop();

    POOM_BLE_KEYBOARD_PRINTF_I("module stopped");
}

bool poom_ble_keyboard_is_connected(void)
{
    return s_is_connected;
}

void poom_ble_keyboard_set_keyboard_mode(bool enabled)
{
    s_keyboard_mode = enabled;
    POOM_BLE_KEYBOARD_PRINTF_D("keyboard_mode=%u", (unsigned)enabled);
}
