// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_scanner_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_scanner_core.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_SCANNER_REFRESH_MS (120U)
#define MENU_SCANNER_STACK (3328U)
#define MENU_SCANNER_PRIO (4U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (42)

#define LIST_Y0 (14)
#define ROW_STEP (8)
#define ROW_HILITE_H (8)
#define VISIBLE_ROWS (5)

#define BAR_X (82)
#define BAR_W (44)
#define BAR_H (6)

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
#endif
#ifndef BTN_UP
#define BTN_UP (4U)
#endif
#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif
#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif
#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_SCANNER_STATE_SELECT = 0,
    MENU_SCANNER_STATE_CHANNELS,
} menu_scanner_state_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
} menu_scanner_button_msg_t;

static bool s_scanner_active = false;
static bool s_scanner_buttons_subscribed = false;
static TaskHandle_t s_scanner_ui_task = NULL;
static QueueHandle_t s_scanner_btn_q = NULL;
static char s_scanner_sbus_user[] = "menu_scanner_core";

static menu_scanner_state_t s_state = MENU_SCANNER_STATE_SELECT;
static uint8_t s_selected = 0U;
static char s_status[18] = "READY";
static int s_chan_selected = 0;
static int s_chan_scroll = 0;

static void menu_scanner_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_scanner_ui_task_(void* arg);

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] selected Parameter passed to the helper.
 * @return const char*
 */
static const char* menu_scanner_mode_label_(uint8_t selected)
{
    return (selected == 0U) ? "WIFI" : "15.4";
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_draw_frame_(const char* title)
{
    char st_short[10];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(18, 2);
    (void)poom_arduboy_print(title ? title : "SCANNER");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_set_cursor(92, 2);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_scanner_render_select_(void)
{
    menu_scanner_draw_frame_("SCAN CHANNELS");

    const int16_t list_y0 = 16;
    const int16_t row_step = 10;
    const int16_t row_h = 9;

    for(int i = 0; i < 2; i++)
    {
        const int16_t y = (int16_t)(list_y0 + i * row_step);

        poom_arduboy_set_cursor(2, y);
        if(i == 0)
        {
            (void)poom_arduboy_print(F("WIFI"));
        }
        else
        {
            (void)poom_arduboy_print(F("IEEE 802.15.4"));
        }

        if((uint8_t)i == s_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, row_h, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SELECT"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] x Parameter passed to the helper.
 * @param[in] y Parameter passed to the helper.
 * @param[in] w Parameter passed to the helper.
 * @param[in] h Parameter passed to the helper.
 * @param[in] pct Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_draw_pct_bar_(int16_t x, int16_t y, uint8_t w, uint8_t h, uint8_t pct)
{
    if((w < 4U) || (h < 3U))
    {
        return;
    }

    if(pct > 100U)
    {
        pct = 100U;
    }

    poom_arduboy_draw_rect(x, y, w, h, WHITE);

    const uint8_t inner_w = (uint8_t)(w - 2U);
    const uint8_t inner_h = (uint8_t)(h - 2U);
    const uint8_t fill = (uint8_t)(((uint16_t)inner_w * (uint16_t)pct) / 100U);
    if(fill > 0U)
    {
        poom_arduboy_fill_rect((int16_t)(x + 1), (int16_t)(y + 1), fill, inner_h, WHITE);
    }
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_channels_adjust_scroll_(int count)
{
    if(count <= 0)
    {
        s_chan_selected = 0;
        s_chan_scroll = 0;
        return;
    }

    if(s_chan_selected < 0)
    {
        s_chan_selected = 0;
    }
    if(s_chan_selected > (count - 1))
    {
        s_chan_selected = count - 1;
    }

    if(s_chan_selected < s_chan_scroll)
    {
        s_chan_scroll = s_chan_selected;
    }
    if(s_chan_selected >= (s_chan_scroll + VISIBLE_ROWS))
    {
        s_chan_scroll = s_chan_selected - VISIBLE_ROWS + 1;
    }

    int max_scroll = count - VISIBLE_ROWS;
    if(max_scroll < 0)
    {
        max_scroll = 0;
    }
    if(s_chan_scroll < 0)
    {
        s_chan_scroll = 0;
    }
    if(s_chan_scroll > max_scroll)
    {
        s_chan_scroll = max_scroll;
    }
}

/**
 * @brief Internal helper for `menu_scanner_range_count`.
 *
 * @param[in] ch_min Parameter passed to the helper.
 * @param[in] ch_max Parameter passed to the helper.
 * @param[in] cap Parameter passed to the helper.
 * @return int
 */
static int menu_scanner_range_count_(uint8_t ch_min, uint8_t ch_max, int cap)
{
    if(ch_max < ch_min)
    {
        return 0;
    }

    int count = (int)(ch_max - ch_min + 1U);
    if(count > cap)
    {
        count = cap;
    }
    if(count < 0)
    {
        count = 0;
    }

    return count;
}

typedef struct
{
    uint8_t channel;
    uint32_t count;
    uint8_t pct;
} menu_scanner_chan_entry_t;

/**
 * @brief Sorts internal items for this menu module.
 *
 * @param[in] entries Parameter passed to the helper.
 * @param[in] count Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_sort_entries_desc_(menu_scanner_chan_entry_t *entries, int count)
{
    if((entries == NULL) || (count <= 1))
    {
        return;
    }

    for(int i = 0; i < (count - 1); i++)
    {
        int best = i;
        for(int j = i + 1; j < count; j++)
        {
            const bool better = (entries[j].count > entries[best].count) ||
                                ((entries[j].count == entries[best].count) && (entries[j].channel < entries[best].channel));
            if(better)
            {
                best = j;
            }
        }

        if(best != i)
        {
            const menu_scanner_chan_entry_t tmp = entries[i];
            entries[i] = entries[best];
            entries[best] = tmp;
        }
    }
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_scanner_render_channels_wifi_(void)
{
    poom_scanner_core_wifi_stats_t st;
    (void)poom_scanner_core_get_wifi_stats(&st);

    uint32_t total = 0U;
    int count = (int)st.channel_count;
    if(count < 0)
    {
        count = 0;
    }
    if(count > (int)POOM_SCANNER_CORE_WIFI_CH_COUNT)
    {
        count = (int)POOM_SCANNER_CORE_WIFI_CH_COUNT;
    }

    menu_scanner_chan_entry_t entries[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    for(int i = 0; i < count; i++)
    {
        entries[i].channel = st.channels[i];
        entries[i].count = st.packet_count[i];
        entries[i].pct = 0U;
        total += entries[i].count;
    }
    for(int i = 0; i < count; i++)
    {
        entries[i].pct = (total > 0U) ? (uint8_t)(((uint64_t)entries[i].count * 100ULL) / (uint64_t)total) : 0U;
    }
    menu_scanner_sort_entries_desc_(entries, count);

    (void)snprintf(s_status, sizeof(s_status), "T%lu", (unsigned long)total);
    menu_scanner_draw_frame_("SCAN WIFI");

    menu_scanner_channels_adjust_scroll_(count);

    for(int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_chan_scroll + row;
        if(idx >= count)
        {
            break;
        }

        const uint8_t channel = entries[idx].channel;
        const uint32_t c = entries[idx].count;
        const uint8_t pct = entries[idx].pct;

        char line[22];
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "%3u:%5lu %3u%%", (unsigned)channel, (unsigned long)c, (unsigned)pct);
        (void)snprintf(line, sizeof(line), "%.21s", tmp);

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);

        menu_scanner_draw_pct_bar_(BAR_X, (int16_t)(y + 1), BAR_W, BAR_H, pct);

        if(idx == s_chan_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:RST"));
    poom_arduboy_set_cursor(42, 56);
    (void)poom_arduboy_print(F("L/R:PG"));
    poom_arduboy_set_cursor(84, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_scanner_render_channels_ieee_(void)
{
    poom_scanner_core_ieee802154_stats_t st;
    (void)poom_scanner_core_get_ieee802154_stats(&st);

    uint32_t total = 0U;
    const int count = menu_scanner_range_count_(st.channel_min, st.channel_max, POOM_SCANNER_CORE_IEEE802154_CH_COUNT);

    menu_scanner_chan_entry_t entries[POOM_SCANNER_CORE_IEEE802154_CH_COUNT];
    for(int i = 0; i < count; i++)
    {
        entries[i].channel = (uint8_t)(st.channel_min + (uint8_t)i);
        entries[i].count = st.packet_count[i];
        entries[i].pct = 0U;
        total += entries[i].count;
    }
    for(int i = 0; i < count; i++)
    {
        entries[i].pct = (total > 0U) ? (uint8_t)(((uint64_t)entries[i].count * 100ULL) / (uint64_t)total) : 0U;
    }
    menu_scanner_sort_entries_desc_(entries, count);

    (void)snprintf(s_status, sizeof(s_status), "T%lu", (unsigned long)total);
    menu_scanner_draw_frame_("SCAN 15.4");

    menu_scanner_channels_adjust_scroll_(count);

    for(int row = 0; row < VISIBLE_ROWS; row++)
    {
        const int idx = s_chan_scroll + row;
        if(idx >= count)
        {
            break;
        }

        const uint8_t channel = entries[idx].channel;
        const uint32_t c = entries[idx].count;
        const uint8_t pct = entries[idx].pct;

        char line[22];
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "%2u:%5lu %3u%%", (unsigned)channel, (unsigned long)c, (unsigned)pct);
        (void)snprintf(line, sizeof(line), "%.21s", tmp);

        const int16_t y = (int16_t)(LIST_Y0 + row * ROW_STEP);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line);

        menu_scanner_draw_pct_bar_(BAR_X, (int16_t)(y + 1), BAR_W, BAR_H, pct);

        if(idx == s_chan_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, ROW_HILITE_H, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:RST"));
    poom_arduboy_set_cursor(42, 56);
    (void)poom_arduboy_print(F("L/R:PG"));
    poom_arduboy_set_cursor(84, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_scanner_render_(void)
{
    if(s_state == MENU_SCANNER_STATE_SELECT)
    {
        menu_scanner_render_select_();
    }
    else
    {
        if(s_selected == 0U)
        {
            menu_scanner_render_channels_wifi_();
        }
        else
        {
            menu_scanner_render_channels_ieee_();
        }
    }
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_scanner_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_scanner_active = false;

    (void)poom_scanner_core_stop();

    if(s_scanner_ui_task != NULL)
    {
        if(s_scanner_ui_task != current_task)
        {
            TaskHandle_t t = s_scanner_ui_task;
            s_scanner_ui_task = NULL;
            vTaskDelete(t);
        }
        else
        {
            s_scanner_ui_task = NULL;
        }
    }

    if(s_scanner_btn_q != NULL)
    {
        QueueHandle_t q = s_scanner_btn_q;
        s_scanner_btn_q = NULL;
        vQueueDelete(q);
    }

    if(s_scanner_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_scanner_button_cb_, s_scanner_sbus_user);
        s_scanner_buttons_subscribed = false;
    }

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_handle_select_button_(uint8_t button)
{
    if(button == BTN_UP || button == BTN_DOWN)
    {
        s_selected = (s_selected == 0U) ? 1U : 0U;
    }
    else if(button == BTN_A)
    {
        esp_err_t st;
        (void)snprintf(s_status, sizeof(s_status), "%s...", menu_scanner_mode_label_(s_selected));
        menu_scanner_render_();

        s_chan_selected = 0;
        s_chan_scroll = 0;

        if(s_selected == 0U)
        {
            st = poom_scanner_core_start_wifi(200U);
        }
        else
        {
            st = poom_scanner_core_start_ieee802154(200U);
        }

        if(st == ESP_OK)
        {
            (void)snprintf(s_status, sizeof(s_status), "%s OK", menu_scanner_mode_label_(s_selected));
            s_state = MENU_SCANNER_STATE_CHANNELS;
        }
        else
        {
            if(st == ESP_ERR_NOT_SUPPORTED)
            {
                (void)snprintf(s_status, sizeof(s_status), "NO %s", menu_scanner_mode_label_(s_selected));
            }
            else
            {
                (void)snprintf(s_status, sizeof(s_status), "ERR %d", (int)st);
            }
        }
    }
    else if(button == BTN_B)
    {
        menu_scanner_exit_();
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_handle_channels_button_(uint8_t button)
{
    if(button == BTN_A)
    {
        poom_scanner_core_reset_stats();
    }
    else if(button == BTN_UP)
    {
        s_chan_selected--;
    }
    else if(button == BTN_DOWN)
    {
        s_chan_selected++;
    }
    else if(button == BTN_LEFT)
    {
        s_chan_selected -= VISIBLE_ROWS;
    }
    else if(button == BTN_RIGHT)
    {
        s_chan_selected += VISIBLE_ROWS;
    }
    else if(button == BTN_B)
    {
        (void)poom_scanner_core_stop();
        (void)snprintf(s_status, sizeof(s_status), "READY");
        s_state = MENU_SCANNER_STATE_SELECT;
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if(!s_scanner_active)
    {
        return;
    }
    if(msg == NULL || msg->len < sizeof(button_event_msg_t))
    {
        return;
    }
    if(s_scanner_btn_q == NULL)
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));
    if(ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    menu_scanner_button_msg_t qmsg = {
        .button = ev.button,
        .event = ev.event,
    };
    (void)xQueueSend(s_scanner_btn_q, &qmsg, 0);
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_scanner_ui_task_(void* arg)
{
    (void)arg;

    while(s_scanner_active)
    {
        menu_scanner_button_msg_t msg;
        if(s_scanner_btn_q != NULL && xQueueReceive(s_scanner_btn_q, &msg, pdMS_TO_TICKS(MENU_SCANNER_REFRESH_MS)) == pdTRUE)
        {
            if(s_state == MENU_SCANNER_STATE_SELECT)
            {
                menu_scanner_handle_select_button_(msg.button);
            }
            else
            {
                menu_scanner_handle_channels_button_(msg.button);
            }
        }

        if(s_scanner_active)
        {
            menu_scanner_render_();
        }
    }

    s_scanner_ui_task = NULL;
    vTaskDelete(NULL);
}

void menu_scanner_core_show(void)
{
    s_scanner_active = true;
    s_state = MENU_SCANNER_STATE_SELECT;
    s_selected = 0U;
    (void)snprintf(s_status, sizeof(s_status), "READY");

    (void)poom_scanner_core_stop();

    if(s_scanner_btn_q == NULL)
    {
        s_scanner_btn_q = xQueueCreate(8, sizeof(menu_scanner_button_msg_t));
    }

    if(!s_scanner_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_scanner_button_cb_, s_scanner_sbus_user))
        {
            s_scanner_buttons_subscribed = true;
        }
        else
        {
            s_scanner_active = false;
            (void)snprintf(s_status, sizeof(s_status), "BTN SUB ERR");
            menu_scanner_render_select_();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    menu_scanner_render_select_();

    if(s_scanner_ui_task == NULL)
    {
        (void)xTaskCreate(menu_scanner_ui_task_, "menu_scan_core", MENU_SCANNER_STACK, NULL, MENU_SCANNER_PRIO, &s_scanner_ui_task);
    }
}
