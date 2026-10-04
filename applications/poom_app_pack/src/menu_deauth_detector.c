// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_deauth_detector.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "bsp_pong.h"
#include "buzzer.h"
#include "poom_led_rainbow.h"
#include "poom_secrets_store.h"
#include "ws2812.h"
#include "poom_sbus.h"
#include "poom_wifi_deauth_detector.h"
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_DEAUTH_DET_OLED_COL            (6)
#define MENU_DEAUTH_DET_BOX_Y               (12)
#define MENU_DEAUTH_DET_BOX_H               (40)
#define MENU_DEAUTH_DET_REFRESH_MS          (250U)
#define MENU_DEAUTH_DET_STACK               (3072U)
#define MENU_DEAUTH_DET_PRIO                (4U)
#define MENU_DEAUTH_DET_TEXT_MAX_CHARS      (15U)

#define MENU_DEAUTH_DET_ALERT_HOLD_MS       (2000U)
#define MENU_DEAUTH_DET_BEEP_INTERVAL_MS    (5000U)
#define MENU_DEAUTH_DET_BEEP_COUNT          (3U)
#define MENU_DEAUTH_DET_BEEP_DURATION_MS    (100U)
#define MENU_DEAUTH_DET_BEEP_GAP_MS         (100U)
#define MENU_DEAUTH_DET_LED_BRIGHTNESS      (21U) /* Previous 32 reduced by one third. */
#define MENU_DEAUTH_DET_INPUT_UP            (1UL << 0)
#define MENU_DEAUTH_DET_INPUT_DOWN          (1UL << 1)
#define MENU_DEAUTH_DET_INPUT_LEFT          (1UL << 2)
#define MENU_DEAUTH_DET_INPUT_RIGHT         (1UL << 3)
#define MENU_DEAUTH_DET_INPUT_ACCEPT        (1UL << 4)
#define MENU_DEAUTH_DET_INPUT_BACK          (1UL << 5)
#define MENU_DEAUTH_DET_INPUT_RESET         (1UL << 6)

#define MENU_DEAUTH_DET_HEADER_H            (11)
#define MENU_DEAUTH_DET_TEXT_X              (4)
#define MENU_DEAUTH_DET_ROW0_Y              (16)
#define MENU_DEAUTH_DET_ROW_STEP            (8)

#ifndef BTN_A
#define BTN_A (0U)
#endif

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif

#ifndef BTN_UP
#define BTN_UP (4U)
#endif

#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#ifndef BUTTON_LONG_PRESS_START
#define BUTTON_LONG_PRESS_START (6U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_deauth_detector_button_msg_t;

typedef enum
{
    MENU_DEAUTH_DET_VIEW_OVERVIEW = 0,
    MENU_DEAUTH_DET_VIEW_CORRELATION,
    MENU_DEAUTH_DET_VIEW_REASON,
    MENU_DEAUTH_DET_VIEW_SETTINGS,
    MENU_DEAUTH_DET_VIEW_COUNT,
} menu_deauth_det_view_t;

typedef enum
{
    MENU_DEAUTH_DET_SCREEN_HOME = 0,
    MENU_DEAUTH_DET_SCREEN_CONFIG,
    MENU_DEAUTH_DET_SCREEN_MONITOR,
} menu_deauth_det_screen_t;

static menu_deauth_det_screen_t s_menu_deauth_detector_screen = MENU_DEAUTH_DET_SCREEN_HOME;
static uint8_t s_menu_deauth_detector_selection = 0U;
static bool s_alerts_initialized = false;
static bool s_menu_deauth_detector_active = false;
static bool s_menu_deauth_detector_buttons_subscribed = false;
static TaskHandle_t s_menu_deauth_detector_task = NULL;
static char s_menu_deauth_detector_sbus_user[] = "menu_deauth_detector";
static char s_menu_deauth_detector_status_override[22] = {0};
static uint8_t s_menu_deauth_detector_status_hold_cycles = 0;
static poom_wifi_deauth_detector_stats_t s_menu_deauth_detector_stats;
static poom_wifi_deauth_detector_report_t s_menu_deauth_detector_report;
static menu_deauth_det_view_t s_menu_deauth_detector_view = MENU_DEAUTH_DET_VIEW_OVERVIEW;

static bool s_alert_sound = false;
static bool s_alert_led = true;
static bool s_alert_strip_ready = false;
static bool s_alert_rainbow_was_running = false;
static ws2812_strip_t s_alert_strip;
static bool s_alert_activity = false;
static uint32_t s_alert_last_deauth_total;
static uint32_t s_alert_last_disassoc_total;
static TickType_t s_alert_last_detection;
static TickType_t s_alert_last_beep;
static bool s_alert_has_beeped = false;

static esp_err_t menu_deauth_detector_exit_(void);
static void menu_deauth_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);
static void menu_deauth_detector_status_hold_(const char *text, uint8_t hold_cycles);
static bool menu_deauth_detector_channel_attacked_(uint8_t channel);

/* Start/reset baselines match the detector's cleared counters. No packet history. */
static void menu_deauth_detector_alerts_reset_(void)
{
    s_alert_activity = false;
    s_alert_last_deauth_total = 0U;
    s_alert_last_disassoc_total = 0U;
    s_alert_last_detection = xTaskGetTickCount();
    s_alert_has_beeped = false;
}

/* Use the same counters as the channel overview, including isolated frames and
 * traffic arriving between report windows. Historical counts never retrigger. */
static void menu_deauth_detector_alerts_update_(void)
{
    const TickType_t now = xTaskGetTickCount();
    const bool running = poom_wifi_deauth_detector_is_running();
    const bool detected = running &&
        ((s_menu_deauth_detector_stats.deauth_total != s_alert_last_deauth_total) ||
         (s_menu_deauth_detector_stats.disassoc_total != s_alert_last_disassoc_total));
    s_alert_last_deauth_total = s_menu_deauth_detector_stats.deauth_total;
    s_alert_last_disassoc_total = s_menu_deauth_detector_stats.disassoc_total;

    if(detected)
    {
        s_alert_activity = true;
        s_alert_last_detection = now;
    }
    else if(!running ||
            ((TickType_t)(now - s_alert_last_detection) >= pdMS_TO_TICKS(MENU_DEAUTH_DET_ALERT_HOLD_MS)))
    {
        s_alert_activity = false;
    }

    if(s_alert_strip_ready)
    {
        const uint8_t yellow = (s_alert_led && s_alert_activity) ? 255U : 0U;
        ws2812_fill(&s_alert_strip, yellow, yellow, 0U, 0U);
        if(ws2812_show(&s_alert_strip) != ESP_OK)
        {
            menu_deauth_detector_status_hold_("LED error", 8U);
        }
    }

    if(s_alert_sound && detected &&
       (!s_alert_has_beeped ||
        ((TickType_t)(now - s_alert_last_beep) >= pdMS_TO_TICKS(MENU_DEAUTH_DET_BEEP_INTERVAL_MS))))
    {
        s_alert_last_beep = now;
        s_alert_has_beeped = true;
        for(uint8_t beep = 0U; beep < MENU_DEAUTH_DET_BEEP_COUNT; ++beep)
        {
            buzzer_tone(2200U, MENU_DEAUTH_DET_BEEP_DURATION_MS);
            if((beep + 1U) < MENU_DEAUTH_DET_BEEP_COUNT)
            {
                vTaskDelay(pdMS_TO_TICKS(MENU_DEAUTH_DET_BEEP_GAP_MS));
            }
        }
    }
}

static void menu_deauth_detector_load_settings_(void)
{
    uint32_t settings = 2U; /* LED enabled, sound disabled by default. */
    if(poom_secrets_init() == ESP_OK)
    {
        (void)poom_secrets_get_u32("deauth_alerts", &settings);
    }
    s_alert_sound = (settings & 1U) != 0U;
    s_alert_led = (settings & 2U) != 0U;
}

static void menu_deauth_detector_alerts_init_(void)
{
    s_alerts_initialized = true;
    menu_deauth_detector_alerts_reset_();
    buzzer_init(PIN_NUM_BUZZER);
    s_alert_rainbow_was_running = poom_led_rainbow_deinit();
    s_alert_strip_ready = (ws2812_init(&s_alert_strip, PIN_NUM_WS2812, PIN_NUM_LEDS,
                                     false, 10000000) == ESP_OK);
    if(s_alert_strip_ready)
    {
        ws2812_set_brightness(&s_alert_strip, MENU_DEAUTH_DET_LED_BRIGHTNESS);
        ws2812_clear(&s_alert_strip);
        (void)ws2812_show(&s_alert_strip);
    }
}

static void menu_deauth_detector_alerts_release_(void)
{
    if(!s_alerts_initialized)
    {
        return;
    }
    s_alerts_initialized = false;
    buzzer_tone(0U, 0U);
    if(s_alert_strip_ready)
    {
        ws2812_clear(&s_alert_strip);
        (void)ws2812_show(&s_alert_strip);
        ws2812_deinit(&s_alert_strip);
        s_alert_strip_ready = false;
    }
    poom_led_rainbow_init();
    if(s_alert_rainbow_was_running)
    {
        (void)poom_led_rainbow_start();
    }
    s_alert_rainbow_was_running = false;
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] out Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @param[in] mac Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_detector_format_mac3_(char *out, size_t out_len, const uint8_t mac[6])
{
    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    if(mac == NULL)
    {
        (void)snprintf(out, out_len, "--:--:--");
        return;
    }

    (void)snprintf(out, out_len, "%02X:%02X:%02X", (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

/**
 * @brief Internal helper for `menu_deauth_detector_next_view`.
 *
 * @param[in] delta Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_detector_next_view_(int delta)
{
    int v = (int)s_menu_deauth_detector_view + delta;
    if(v < 0)
    {
        v = (int)MENU_DEAUTH_DET_VIEW_COUNT - 1;
    }
    if(v >= (int)MENU_DEAUTH_DET_VIEW_COUNT)
    {
        v = 0;
    }
    s_menu_deauth_detector_view = (menu_deauth_det_view_t)v;
}

/**
 * @brief Internal helper for `menu_deauth_detector_cap_u`.
 *
 * @param[in] value Parameter passed to the helper.
 * @param[in] cap Parameter passed to the helper.
 * @return unsigned
 */
static unsigned menu_deauth_detector_cap_u_(unsigned value, unsigned cap)
{
    return (value > cap) ? cap : value;
}

/**
 * @brief Summarizes the most active attacked channels without temporary arrays.
 *
 * @param[out] out_primary Most active channel or `0`.
 * @param[out] out_second Second most active channel or `0`.
 * @param[out] out_third Third most active channel or `0`.
 * @param[out] out_fourth Fourth most active channel or `0`.
 * @param[out] out_total Total attacked channel count.
 * @return void
 */
static void menu_deauth_detector_summarize_attacked_channels_(uint8_t *out_primary,
                                                              uint8_t *out_second,
                                                              uint8_t *out_third,
                                                              uint8_t *out_fourth,
                                                              uint8_t *out_total)
{
    uint8_t channel;
    uint8_t primary = 0U;
    uint8_t second = 0U;
    uint8_t third = 0U;
    uint8_t fourth = 0U;
    uint8_t total = 0U;
    uint32_t primary_hits = 0U;
    uint32_t second_hits = 0U;
    uint32_t third_hits = 0U;
    uint32_t fourth_hits = 0U;

    for(channel = 1U; channel <= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT; channel++)
    {
        const uint8_t idx = (uint8_t)(channel - 1U);
        const uint32_t hits = s_menu_deauth_detector_stats.deauth_by_channel[idx] +
                              s_menu_deauth_detector_stats.disassoc_by_channel[idx];

        if(hits == 0U)
        {
            continue;
        }

        total++;

        if(hits > primary_hits)
        {
            fourth = third;
            fourth_hits = third_hits;
            third = second;
            third_hits = second_hits;
            second = primary;
            second_hits = primary_hits;
            primary = channel;
            primary_hits = hits;
        }
        else if(hits > second_hits)
        {
            fourth = third;
            fourth_hits = third_hits;
            third = second;
            third_hits = second_hits;
            second = channel;
            second_hits = hits;
        }
        else if(hits > third_hits)
        {
            fourth = third;
            fourth_hits = third_hits;
            third = channel;
            third_hits = hits;
        }
        else if(hits > fourth_hits)
        {
            fourth = channel;
            fourth_hits = hits;
        }
    }

    if(out_primary != NULL)
    {
        *out_primary = primary;
    }
    if(out_second != NULL)
    {
        *out_second = second;
    }
    if(out_third != NULL)
    {
        *out_third = third;
    }
    if(out_fourth != NULL)
    {
        *out_fourth = fourth;
    }
    if(out_total != NULL)
    {
        *out_total = total;
    }
}

/**
 * @brief Internal helper for `menu_deauth_detector_channel_attacked`.
 *
 * @param[in] channel Parameter passed to the helper.
 * @return bool
 */
static bool menu_deauth_detector_channel_attacked_(uint8_t channel)
{
    uint8_t idx;

    if((channel < 1U) || (channel > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT))
    {
        return false;
    }

    idx = (uint8_t)(channel - 1U);
    return (s_menu_deauth_detector_stats.deauth_by_channel[idx] != 0U) ||
           (s_menu_deauth_detector_stats.disassoc_by_channel[idx] != 0U);
}

/**
 * @brief Internal helper for `menu_deauth_detector_next_attacked_channel`.
 *
 * @param[in] current_channel Parameter passed to the helper.
 * @param[in] direction Parameter passed to the helper.
 * @return uint8_t
 */
static uint8_t menu_deauth_detector_next_attacked_channel_(uint8_t current_channel, int direction)
{
    uint8_t step;
    uint8_t ch;
    bool any_attacked = false;

    for(ch = 1U; ch <= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT; ch++)
    {
        if(menu_deauth_detector_channel_attacked_(ch))
        {
            any_attacked = true;
            break;
        }
    }

    if(!any_attacked)
    {
        if(direction >= 0)
        {
            return (current_channel >= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT) ? 1U : (uint8_t)(current_channel + 1U);
        }
        return (current_channel <= 1U) ? POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT : (uint8_t)(current_channel - 1U);
    }

    for(step = 1U; step <= POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT; step++)
    {
        if(direction >= 0)
        {
            ch = (uint8_t)(current_channel + step);
            if(ch > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT)
            {
                ch = (uint8_t)(ch - POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT);
            }
        }
        else
        {
            if(current_channel > step)
            {
                ch = (uint8_t)(current_channel - step);
            }
            else
            {
                ch = (uint8_t)(POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT - (step - current_channel));
            }
        }

        if(menu_deauth_detector_channel_attacked_(ch))
        {
            return ch;
        }
    }

    return current_channel;
}

/**
 * @brief Internal helper for `menu_deauth_detector_lock_attacked_channel`.
 *
 * @param[in] direction Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_detector_lock_attacked_channel_(int direction)
{
    uint8_t current_ch = s_menu_deauth_detector_stats.current_channel;
    uint8_t next_ch;

    if((current_ch < 1U) || (current_ch > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT))
    {
        current_ch = 1U;
    }

    next_ch = menu_deauth_detector_next_attacked_channel_(current_ch, direction);
    if(poom_wifi_deauth_detector_set_fixed_channel(next_ch) == ESP_OK)
    {
        char msg[22];
        (void)snprintf(msg, sizeof(msg), "Lock CH%02u", (unsigned)next_ch);
        menu_deauth_detector_status_hold_(msg, 6U);
    }
    else
    {
        menu_deauth_detector_status_hold_("Lock failed", 6U);
    }
}

/**
 * @brief Internal helper for `menu_deauth_detector_status_hold`.
 *
 * @param[in] text Parameter passed to the helper.
 * @param[in] hold_cycles Parameter passed to the helper.
 * @return void
 */
static void menu_deauth_detector_status_hold_(const char *text, uint8_t hold_cycles)
{
    if(text == NULL)
    {
        return;
    }

    (void)snprintf(s_menu_deauth_detector_status_override, sizeof(s_menu_deauth_detector_status_override), "%.18s", text);
    s_menu_deauth_detector_status_hold_cycles = hold_cycles;
}

/**
 * @brief Internal helper for `menu_deauth_detector_enable_scan_all`.
 *
 * @return void
 */
static void menu_deauth_detector_enable_scan_all_(void)
{
    if(poom_wifi_deauth_detector_set_channel_hopping(true) == ESP_OK)
    {
        menu_deauth_detector_status_hold_("Scan all (HOP)", 6U);
    }
    else
    {
        menu_deauth_detector_status_hold_("HOP failed", 6U);
    }
}

/* All navigation and peripheral changes run in the existing menu task. */
static void menu_deauth_detector_handle_input_(uint32_t input)
{
    if((input & MENU_DEAUTH_DET_INPUT_BACK) != 0U)
    {
        if(s_menu_deauth_detector_screen == MENU_DEAUTH_DET_SCREEN_HOME)
        {
            (void)menu_deauth_detector_exit_();
        }
        else
        {
            if(s_menu_deauth_detector_screen == MENU_DEAUTH_DET_SCREEN_MONITOR)
            {
                (void)poom_wifi_deauth_detector_stop();
                menu_deauth_detector_alerts_release_();
            }
            s_menu_deauth_detector_screen = MENU_DEAUTH_DET_SCREEN_HOME;
            s_menu_deauth_detector_selection = 0U;
            s_menu_deauth_detector_status_hold_cycles = 0U;
        }
        return;
    }

    if(s_menu_deauth_detector_screen != MENU_DEAUTH_DET_SCREEN_MONITOR)
    {
        if((input & (MENU_DEAUTH_DET_INPUT_UP | MENU_DEAUTH_DET_INPUT_DOWN)) != 0U)
        {
            s_menu_deauth_detector_selection ^= 1U;
        }
        if(s_menu_deauth_detector_screen == MENU_DEAUTH_DET_SCREEN_HOME)
        {
            if((input & MENU_DEAUTH_DET_INPUT_ACCEPT) != 0U)
            {
                if(s_menu_deauth_detector_selection == 1U)
                {
                    s_menu_deauth_detector_screen = MENU_DEAUTH_DET_SCREEN_CONFIG;
                    s_menu_deauth_detector_selection = 0U;
                    s_menu_deauth_detector_status_hold_cycles = 0U;
                }
                else if(poom_wifi_deauth_detector_start() == ESP_OK)
                {
                    menu_deauth_detector_alerts_init_();
                    s_menu_deauth_detector_screen = MENU_DEAUTH_DET_SCREEN_MONITOR;
                    s_menu_deauth_detector_view = MENU_DEAUTH_DET_VIEW_OVERVIEW;
                    menu_deauth_detector_status_hold_(s_alert_led && !s_alert_strip_ready ?
                                                      "LED unavailable" : "Running", 8U);
                }
                else
                {
                    menu_deauth_detector_status_hold_("Start failed", 8U);
                }
            }
        }
        else if((input & (MENU_DEAUTH_DET_INPUT_ACCEPT | MENU_DEAUTH_DET_INPUT_LEFT |
                          MENU_DEAUTH_DET_INPUT_RIGHT)) != 0U)
        {
            if(s_menu_deauth_detector_selection == 0U)
            {
                s_alert_sound = !s_alert_sound;
            }
            else
            {
                s_alert_led = !s_alert_led;
            }
            const uint32_t settings = (s_alert_sound ? 1U : 0U) | (s_alert_led ? 2U : 0U);
            const bool saved = (poom_secrets_init() == ESP_OK) &&
                               (poom_secrets_set_u32("deauth_alerts", settings) == ESP_OK);
            menu_deauth_detector_status_hold_(saved ? "Saved" : "Save failed", 8U);
        }
        return;
    }

    if((input & MENU_DEAUTH_DET_INPUT_RESET) != 0U)
    {
        (void)poom_wifi_deauth_detector_reset_stats();
        menu_deauth_detector_alerts_reset_();
        menu_deauth_detector_status_hold_("Reset", 4U);
    }
    if((input & MENU_DEAUTH_DET_INPUT_LEFT) != 0U)
    {
        menu_deauth_detector_enable_scan_all_();
    }
    if((input & MENU_DEAUTH_DET_INPUT_RIGHT) != 0U)
    {
        menu_deauth_detector_lock_attacked_channel_(1);
    }
    if((input & MENU_DEAUTH_DET_INPUT_UP) != 0U)
    {
        menu_deauth_detector_next_view_(-1);
    }
    if((input & MENU_DEAUTH_DET_INPUT_DOWN) != 0U)
    {
        menu_deauth_detector_next_view_(1);
    }
}

/**
 * @brief Draws the menu header.
 *
 * @return void
 */
static void menu_deauth_detector_draw_header_(void)
{
    poom_arduboy_set_cursor(37, 2);
    (void)poom_arduboy_print(F("DEAUTH DET"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_DEAUTH_DET_HEADER_H, INVERT);
}

/* Match the highlighted list rows and A/B navigation of the other app menus. */
static void menu_deauth_detector_draw_options_(void)
{
    const bool configuring = (s_menu_deauth_detector_screen == MENU_DEAUTH_DET_SCREEN_CONFIG);
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    if(configuring)
    {
        poom_arduboy_set_cursor(40, 2);
        (void)poom_arduboy_print(F("SETTINGS"));
        poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_DEAUTH_DET_HEADER_H, INVERT);
    }
    else
    {
        menu_deauth_detector_draw_header_();
    }
    for(uint8_t row = 0U; row < 2U; ++row)
    {
        const int16_t y = (int16_t)(MENU_DEAUTH_DET_ROW0_Y + row * 10);
        poom_arduboy_set_cursor(MENU_DEAUTH_DET_TEXT_X, y);
        if(configuring)
        {
            (void)poom_arduboy_print(row == 0U ? "Sound" : "LED");
            poom_arduboy_set_cursor(96, y);
            const bool enabled = (row == 0U) ? s_alert_sound : s_alert_led;
            (void)poom_arduboy_print(enabled ? "ON" : "OFF");
        }
        else
        {
            (void)poom_arduboy_print(row == 0U ? "Start" : "Settings");
        }
        if(row == s_menu_deauth_detector_selection)
        {
            poom_arduboy_fill_rect(1, (int16_t)(y - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
        }
    }
    if(s_menu_deauth_detector_status_hold_cycles != 0U)
    {
        poom_arduboy_set_cursor(4, 42);
        (void)poom_arduboy_print(s_menu_deauth_detector_status_override);
    }
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(configuring ? "A:CHANGE" : "A:SELECT");
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(configuring ? "B:BACK" : "B:EXIT");
    poom_arduboy_display();
}

/**
 * @brief Draws deauth detector dashboard on OLED.
 * @return esp_err_t
 */
static esp_err_t menu_deauth_detector_draw_(void)
{
    char line1[22];
    char line2[22];
    char line3[22];
    char line4[22];
    uint8_t ch;
    uint8_t index;
    uint32_t ch_deauth;

    if(s_menu_deauth_detector_screen != MENU_DEAUTH_DET_SCREEN_MONITOR)
    {
        menu_deauth_detector_draw_options_();
        return ESP_OK;
    }

    ch = s_menu_deauth_detector_stats.current_channel;
    if((ch < 1U) || (ch > POOM_WIFI_DEAUTH_DETECTOR_CHANNEL_COUNT))
    {
        ch = 1U;
    }
    index = (uint8_t)(ch - 1U);
    ch_deauth = s_menu_deauth_detector_stats.deauth_by_channel[index];

    (void)memset(line1, 0, sizeof(line1));
    (void)memset(line2, 0, sizeof(line2));
    (void)memset(line3, 0, sizeof(line3));
    (void)memset(line4, 0, sizeof(line4));

    if(s_menu_deauth_detector_view == MENU_DEAUTH_DET_VIEW_OVERVIEW)
    {
        uint8_t primary_channel = 0U;
        uint8_t second_channel = 0U;
        uint8_t third_channel = 0U;
        uint8_t fourth_channel = 0U;
        uint8_t attacked_count = 0U;

        menu_deauth_detector_summarize_attacked_channels_(&primary_channel,
                                                          &second_channel,
                                                          &third_channel,
                                                          &fourth_channel,
                                                          &attacked_count);

        (void)snprintf(line1, sizeof(line1), "ATTACKED CHANNELS");

        if(attacked_count == 0U)
        {
            (void)snprintf(line2, sizeof(line2), "NO ACTIVITY");
            (void)snprintf(line3, sizeof(line3), " ");
        }
        else
        {
            (void)snprintf(line2, sizeof(line2), "PRIMARY: %02u", (unsigned)primary_channel);

            if(attacked_count == 1U)
            {
                (void)snprintf(line3, sizeof(line3), "ONLY ONE ACTIVE");
            }
            else if(attacked_count == 2U)
            {
                (void)snprintf(line3, sizeof(line3), "OTHER: %02u", (unsigned)second_channel);
            }
            else if(attacked_count == 3U)
            {
                (void)snprintf(line3,
                               sizeof(line3),
                               "OTHERS:%02u %02u",
                               (unsigned)second_channel,
                               (unsigned)third_channel);
            }
            else
            {
                (void)snprintf(line3,
                               sizeof(line3),
                               "OTHERS:%02u %02u %02u",
                               (unsigned)second_channel,
                               (unsigned)third_channel,
                               (unsigned)fourth_channel);
            }
        }

        if(s_menu_deauth_detector_status_hold_cycles != 0U)
        {
            (void)snprintf(line4, sizeof(line4), "%.18s", s_menu_deauth_detector_status_override);
        }
        else if(s_alert_activity)
        {
            (void)snprintf(line4, sizeof(line4), "ACTIVITY DETECTED");
        }
        else if(attacked_count > 4U)
        {
            (void)snprintf(line4, sizeof(line4), "+%u MORE", (unsigned)(attacked_count - 4U));
        }
        else if(attacked_count != 0U)
        {
            (void)snprintf(line4, sizeof(line4), "UP/DOWN FOR DETAIL");
        }
        else
        {
            (void)snprintf(line4, sizeof(line4), "WAITING TRAFFIC");
        }
    }
    else if(s_menu_deauth_detector_view == MENU_DEAUTH_DET_VIEW_CORRELATION)
    {
        char mac_src[12];
        char mac_dst[12];
        char mac_bss[12];
        unsigned pps;
        unsigned reason;

        menu_deauth_detector_format_mac3_(mac_src, sizeof(mac_src), s_menu_deauth_detector_report.top_src);
        menu_deauth_detector_format_mac3_(mac_dst, sizeof(mac_dst), s_menu_deauth_detector_report.top_dst);
        menu_deauth_detector_format_mac3_(mac_bss, sizeof(mac_bss), s_menu_deauth_detector_report.top_bssid);

        (void)snprintf(line1, sizeof(line1), "SRC:%.8s", s_menu_deauth_detector_report.top_valid ? mac_src : "--:--:--");
        (void)snprintf(line2, sizeof(line2), "DST:%.8s", s_menu_deauth_detector_report.top_valid ? mac_dst : "--:--:--");
        (void)snprintf(line3, sizeof(line3), "BSS:%.8s", s_menu_deauth_detector_report.top_valid ? mac_bss : "--:--:--");
        if(s_menu_deauth_detector_report.top_valid)
        {
            pps = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.top_pps, 9999U);
            reason = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.top_reason, 999U);
            (void)snprintf(line4,
                           sizeof(line4),
                           "CH:%02u P:%04u R:%03u",
                           (unsigned)s_menu_deauth_detector_report.top_channel,
                           pps,
                           reason);
        }
        else
        {
            (void)snprintf(line4, sizeof(line4), "CH:-- P:---- R:---");
        }
    }
    else if(s_menu_deauth_detector_view == MENU_DEAUTH_DET_VIEW_REASON)
    {
        unsigned reason;
        unsigned reason_count;
        unsigned bad_len;
        unsigned prot;
        unsigned weird;
        unsigned total_deauth;
        unsigned ch_deauth_cap;

        reason = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.top_reason, 999U);
        reason_count = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.top_reason_count, 9999U);
        bad_len = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.bad_len, 9999U);
        prot = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.protected_frames, 9999U);
        weird = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_report.weird_ds, 9999U);
        total_deauth = menu_deauth_detector_cap_u_((unsigned)s_menu_deauth_detector_stats.deauth_total, 999999U);
        ch_deauth_cap = menu_deauth_detector_cap_u_((unsigned)ch_deauth, 999999U);

        (void)snprintf(line1, sizeof(line1), "R:%03u C:%04u", reason, reason_count);
        (void)snprintf(line2, sizeof(line2), "Bad:%04u Prot:%04u", bad_len, prot);
        (void)snprintf(line3, sizeof(line3), "WDS:%04u CH:%02u", weird, (unsigned)ch);
        (void)snprintf(line4, sizeof(line4), "TD:%06u CHD:%06u", total_deauth, ch_deauth_cap);
    }
    else if(s_menu_deauth_detector_view == MENU_DEAUTH_DET_VIEW_SETTINGS)
    {
        (void)snprintf(line1, sizeof(line1), "WIN:%us", (unsigned)(POOM_WIFI_DEAUTH_DETECTOR_PPS_WINDOW_MS / 1000U));
        (void)snprintf(line2, sizeof(line2), "TH:%u/%u pps",
                       (unsigned)POOM_WIFI_DEAUTH_DETECTOR_TH_MED_PPS,
                       (unsigned)POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_PPS);
        (void)snprintf(line3, sizeof(line3), "BC:%02u/%02u dps",
                       (unsigned)POOM_WIFI_DEAUTH_DETECTOR_TH_MED_BCAST_PPS,
                       (unsigned)POOM_WIFI_DEAUTH_DETECTOR_TH_HIGH_BCAST_PPS);
        (void)snprintf(line4, sizeof(line4), "LongA:Reset");
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_deauth_detector_draw_header_();
    poom_arduboy_set_cursor(108, 2);
    {
        char page_buf[8];
        (void)snprintf(page_buf, sizeof(page_buf), "%u/%u",
                       (unsigned)((uint8_t)s_menu_deauth_detector_view + 1U),
                       (unsigned)MENU_DEAUTH_DET_VIEW_COUNT);
        (void)poom_arduboy_print(page_buf);
    }

    poom_arduboy_draw_rect(0, MENU_DEAUTH_DET_BOX_Y, ARDUBOY_WIDTH, MENU_DEAUTH_DET_BOX_H, WHITE);

    poom_arduboy_set_cursor(MENU_DEAUTH_DET_TEXT_X, MENU_DEAUTH_DET_ROW0_Y);
    (void)poom_arduboy_print(line1);
    poom_arduboy_set_cursor(MENU_DEAUTH_DET_TEXT_X, (int16_t)(MENU_DEAUTH_DET_ROW0_Y + MENU_DEAUTH_DET_ROW_STEP));
    (void)poom_arduboy_print(line2);
    poom_arduboy_set_cursor(MENU_DEAUTH_DET_TEXT_X, (int16_t)(MENU_DEAUTH_DET_ROW0_Y + 2 * MENU_DEAUTH_DET_ROW_STEP));
    (void)poom_arduboy_print(line3);
    poom_arduboy_set_cursor(MENU_DEAUTH_DET_TEXT_X, (int16_t)(MENU_DEAUTH_DET_ROW0_Y + 3 * MENU_DEAUTH_DET_ROW_STEP));
    (void)poom_arduboy_print(line4);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("L:SCAN"));
    poom_arduboy_set_cursor(42, 56);
    (void)poom_arduboy_print(F("R:LOCK"));
    poom_arduboy_set_cursor(88, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Detector menu task loop.
 * @param[in,out] task_arg Unused task argument.
 * @return void
 */
static void menu_deauth_detector_task_(void *task_arg)
{
    (void)task_arg;

    while(s_menu_deauth_detector_active)
    {
        uint32_t input = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &input, 0U);
        menu_deauth_detector_handle_input_(input);
        if(!s_menu_deauth_detector_active)
        {
            break;
        }
        if(s_menu_deauth_detector_screen == MENU_DEAUTH_DET_SCREEN_MONITOR)
        {
            (void)poom_wifi_deauth_detector_get_stats(&s_menu_deauth_detector_stats);
            (void)poom_wifi_deauth_detector_get_report(&s_menu_deauth_detector_report);
            menu_deauth_detector_alerts_update_();
        }
        (void)menu_deauth_detector_draw_();
        if(s_menu_deauth_detector_status_hold_cycles != 0U)
        {
            s_menu_deauth_detector_status_hold_cycles--;
        }
        vTaskDelay(pdMS_TO_TICKS(MENU_DEAUTH_DET_REFRESH_MS));
    }

    s_menu_deauth_detector_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Exits detector menu and restores submenu context.
 * @return esp_err_t
 */
static esp_err_t menu_deauth_detector_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_deauth_detector_active = false;

    if(s_menu_deauth_detector_task != NULL)
    {
        if(s_menu_deauth_detector_task != current_task)
        {
            TaskHandle_t status_task = s_menu_deauth_detector_task;
            s_menu_deauth_detector_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_deauth_detector_task = NULL;
        }
    }

    if(s_menu_deauth_detector_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_deauth_detector_button_cb_, s_menu_deauth_detector_sbus_user);
        s_menu_deauth_detector_buttons_subscribed = false;
    }

    if(s_alerts_initialized)
    {
        (void)poom_wifi_deauth_detector_stop();
        menu_deauth_detector_alerts_release_();
    }
    {
        const uint8_t token = 1;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }

    return ESP_OK;
}

/**
 * @brief Handles detector menu button events.
 * @param[in] msg SBUS payload.
 * @param[in] user_ctx User context (unused).
 * @return void
 */
static void menu_deauth_detector_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    menu_deauth_detector_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    if(!s_menu_deauth_detector_active || (s_menu_deauth_detector_task == NULL))
    {
        return;
    }
    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    uint32_t input = 0U;
    if((button_msg.button == BTN_A) && (button_msg.event == BUTTON_LONG_PRESS_START))
    {
        input = MENU_DEAUTH_DET_INPUT_RESET;
    }
    else if(button_msg.event == BUTTON_SINGLE_CLICK)
    {
        switch(button_msg.button)
        {
            case BTN_A: input = MENU_DEAUTH_DET_INPUT_ACCEPT; break;
            case BTN_B: input = MENU_DEAUTH_DET_INPUT_BACK; break;
            case BTN_UP: input = MENU_DEAUTH_DET_INPUT_UP; break;
            case BTN_DOWN: input = MENU_DEAUTH_DET_INPUT_DOWN; break;
            case BTN_LEFT: input = MENU_DEAUTH_DET_INPUT_LEFT; break;
            case BTN_RIGHT: input = MENU_DEAUTH_DET_INPUT_RIGHT; break;
            default: break;
        }
    }
    if(input != 0U)
    {
        (void)xTaskNotify(s_menu_deauth_detector_task, input, eSetBits);
    }

}

/**
 * @brief Opens deauth detector menu.
 * @return void
 */
void app_deauth_detector(void)
{
    if(s_menu_deauth_detector_active)
    {
        return;
    }
    menu_deauth_detector_load_settings_();
    s_menu_deauth_detector_screen = MENU_DEAUTH_DET_SCREEN_HOME;
    s_menu_deauth_detector_selection = 0U;
    s_menu_deauth_detector_active = true;
    (void)memset(&s_menu_deauth_detector_stats, 0, sizeof(s_menu_deauth_detector_stats));
    (void)memset(&s_menu_deauth_detector_report, 0, sizeof(s_menu_deauth_detector_report));
    s_menu_deauth_detector_view = MENU_DEAUTH_DET_VIEW_OVERVIEW;
    s_menu_deauth_detector_status_hold_cycles = 0;
    (void)memset(s_menu_deauth_detector_status_override, 0, sizeof(s_menu_deauth_detector_status_override));
    if(!s_menu_deauth_detector_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_deauth_detector_button_cb_, s_menu_deauth_detector_sbus_user))
        {
            s_menu_deauth_detector_buttons_subscribed = true;
        }
        else
        {
            (void)menu_deauth_detector_exit_();
            return;
        }
    }

    (void)menu_deauth_detector_draw_();

    if(s_menu_deauth_detector_task == NULL)
    {
        if(xTaskCreate(menu_deauth_detector_task_,
                        "menu_deauth_det",
                        MENU_DEAUTH_DET_STACK,
                        NULL,
                        MENU_DEAUTH_DET_PRIO,
                        &s_menu_deauth_detector_task) != pdPASS)
        {
            (void)menu_deauth_detector_exit_();
        }
    }
}
