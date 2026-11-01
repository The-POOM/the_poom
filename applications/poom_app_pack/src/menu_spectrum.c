// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/*
 * Live Wi-Fi activity instrument.
 *
 * This deliberately renders into the shared Arduboy framebuffer.  The C5
 * provides packet RSSI while hopping Wi-Fi channels, rather than an SDR FFT,
 * so the trace represents received Wi-Fi activity at the channel centres.
 */
#include "menu_spectrum.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_scanner_core.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#define SPECTRUM_TICK_MS       (120U)
#define SPECTRUM_STACK         (3328U)
#define SPECTRUM_PRIORITY      (4U)
#define SPECTRUM_WATER_ROWS    (32U)
#define SPECTRUM_MAX_PAGE_CH   (13U)
#define SPECTRUM_NOISE_DBM     (-95)
#define SPECTRUM_TOP_DBM       (-35)
#define SPECTRUM_TAG           "poom_spectrum"

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

typedef enum {
    SPECTRUM_VIEW_TRACE = 0,
    SPECTRUM_VIEW_WATER,
} spectrum_view_t;

typedef struct {
    uint8_t button;
} spectrum_button_t;

typedef struct {
    poom_scanner_core_wifi_stats_t stats;
    uint32_t last_count[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    int8_t ema[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    int8_t peak[POOM_SCANNER_CORE_WIFI_CH_COUNT];
    uint8_t water[SPECTRUM_WATER_ROWS][SPECTRUM_MAX_PAGE_CH];
    uint8_t water_head;
    uint8_t page;
    spectrum_view_t view;
    bool paused;
    bool had_stats;
    bool active;
    bool subscribed;
    bool scanner_started;
    esp_err_t startup_error;
    QueueHandle_t buttons;
    TaskHandle_t task;
} spectrum_state_t;

static spectrum_state_t *s_spectrum = NULL;
static const char s_sbus_user[] = "menu_spectrum";

static const uint8_t s_page_first[] = {0U, 13U, 21U, 33U};
static const uint8_t s_page_count[] = {13U, 8U, 12U, 5U};
static const char *const s_page_name[] = {"2G", "5G-A", "5G-B", "5G-C"};

static void spectrum_exit_(void);
static void spectrum_task_(void *arg);

static uint8_t spectrum_page_total_(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    return 4U;
#else
    return 1U;
#endif
}

static int16_t spectrum_freq_mhz_(uint8_t channel)
{
    if(channel <= 14U)
    {
        return (channel == 14U) ? 2484 : (int16_t)(2407 + 5 * channel);
    }
    return (int16_t)(5000 + 5 * channel);
}

static int16_t spectrum_y_(int8_t dbm)
{
    if(dbm < SPECTRUM_NOISE_DBM) dbm = SPECTRUM_NOISE_DBM;
    if(dbm > SPECTRUM_TOP_DBM) dbm = SPECTRUM_TOP_DBM;
    return (int16_t)(48 - ((int16_t)(dbm - SPECTRUM_NOISE_DBM) * 34) /
                           (SPECTRUM_TOP_DBM - SPECTRUM_NOISE_DBM));
}

static uint8_t spectrum_intensity_(int8_t dbm)
{
    if(dbm < -82) return 0U;
    if(dbm < -68) return 1U;
    if(dbm < -53) return 2U;
    return 3U;
}

static void spectrum_update_(void)
{
    if(s_spectrum == NULL || s_spectrum->paused ||
       !poom_scanner_core_get_wifi_stats(&s_spectrum->stats))
    {
        return;
    }

    for(uint8_t i = 0U; i < s_spectrum->stats.channel_count; ++i)
    {
        const bool sample = s_spectrum->stats.packet_count[i] != s_spectrum->last_count[i];
        int8_t value = sample ? s_spectrum->stats.rssi_max_dbm[i] : SPECTRUM_NOISE_DBM;
        if(value < SPECTRUM_NOISE_DBM) value = SPECTRUM_NOISE_DBM;
        if(!s_spectrum->had_stats)
        {
            s_spectrum->ema[i] = value;
            s_spectrum->peak[i] = value;
        }
        else if(sample)
        {
            /* EMA alpha 0.30: stable graph, but a burst still rises quickly. */
            s_spectrum->ema[i] = (int8_t)(((int16_t)s_spectrum->ema[i] * 7 +
                                            (int16_t)value * 3) / 10);
            if(value > s_spectrum->peak[i]) s_spectrum->peak[i] = value;
        }
        else
        {
            if(s_spectrum->ema[i] > SPECTRUM_NOISE_DBM) --s_spectrum->ema[i];
            if(s_spectrum->peak[i] > SPECTRUM_NOISE_DBM) --s_spectrum->peak[i];
        }
        s_spectrum->last_count[i] = s_spectrum->stats.packet_count[i];
    }
    s_spectrum->had_stats = true;

    const uint8_t count = s_page_count[s_spectrum->page];
    const uint8_t first = s_page_first[s_spectrum->page];
    uint8_t *row = s_spectrum->water[s_spectrum->water_head];
    for(uint8_t col = 0U; col < count; ++col)
    {
        const uint8_t idx = (uint8_t)(first + col);
        row[col] = (idx < s_spectrum->stats.channel_count) ?
                       spectrum_intensity_(s_spectrum->ema[idx]) : 0U;
    }
    for(uint8_t col = count; col < SPECTRUM_MAX_PAGE_CH; ++col) row[col] = 0U;
    s_spectrum->water_head = (uint8_t)((s_spectrum->water_head + 1U) % SPECTRUM_WATER_ROWS);
}

static void spectrum_header_(const char *mode)
{
    char label[20];
    poom_arduboy_clear();
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 8, WHITE);
    (void)snprintf(label, sizeof(label), "SPECTRUM %s", s_page_name[s_spectrum->page]);
    poom_arduboy_draw_tiny_text(2, 2, label, BLACK);
    poom_arduboy_draw_tiny_text(91, 2, mode, BLACK);
    poom_arduboy_draw_tiny_text(112, 2, s_spectrum->paused ? "HLD" : "RUN", BLACK);
}

static void spectrum_grid_(void)
{
    for(int16_t y = 16; y <= 48; y += 8)
    {
        for(int16_t x = 2; x < 127; x += 4)
        {
            poom_arduboy_draw_pixel(x, y, WHITE);
        }
    }
    poom_arduboy_draw_fast_hline(2, 49, 124, WHITE);
}

static void spectrum_footer_(uint8_t strongest_index)
{
    const uint8_t first = s_page_first[s_spectrum->page];
    const uint8_t count = s_page_count[s_spectrum->page];
    const uint8_t first_ch = s_spectrum->stats.channels[first];
    const uint8_t last_ch = s_spectrum->stats.channels[(uint8_t)(first + count - 1U)];
    const uint8_t peak_ch = s_spectrum->stats.channels[strongest_index];
    char left[10];
    char middle[19];
    char right[10];
    (void)snprintf(left, sizeof(left), "%d", (int)spectrum_freq_mhz_(first_ch));
    (void)snprintf(middle, sizeof(middle), "CH%u %dDBM", (unsigned)peak_ch,
                   (int)s_spectrum->ema[strongest_index]);
    (void)snprintf(right, sizeof(right), "%d", (int)spectrum_freq_mhz_(last_ch));
    poom_arduboy_draw_tiny_text(1, 57, left, WHITE);
    poom_arduboy_draw_tiny_text((int16_t)(64 - poom_arduboy_tiny_text_width(middle) / 2), 57, middle, WHITE);
    poom_arduboy_draw_tiny_text((int16_t)(127 - poom_arduboy_tiny_text_width(right)), 57, right, WHITE);
}

static void spectrum_render_trace_(void)
{
    const uint8_t first = s_page_first[s_spectrum->page];
    const uint8_t count = s_page_count[s_spectrum->page];
    const int16_t x0 = 2;
    const int16_t x1 = 125;
    int16_t previous_x = x0;
    int16_t previous_y = 48;
    uint8_t strongest = first;

    spectrum_header_("SPEC");
    spectrum_grid_();

    for(uint8_t col = 0U; col < count; ++col)
    {
        const uint8_t idx = (uint8_t)(first + col);
        const int16_t x = (count > 1U) ?
                              (int16_t)(x0 + ((int32_t)(x1 - x0) * col) / (count - 1U)) :
                              (int16_t)((x0 + x1) / 2);
        const int16_t y = spectrum_y_(s_spectrum->ema[idx]);
        const int16_t py = spectrum_y_(s_spectrum->peak[idx]);
        if(col > 0U)
        {
            poom_arduboy_draw_line(previous_x, previous_y, x, y, WHITE);
        }
        poom_arduboy_draw_dither_vline(x, y, 48, spectrum_intensity_(s_spectrum->ema[idx]), WHITE);
        poom_arduboy_draw_pixel(x, py, WHITE);
        if(s_spectrum->ema[idx] > s_spectrum->ema[strongest]) strongest = idx;
        previous_x = x;
        previous_y = y;
    }

    const uint8_t strong_col = (uint8_t)(strongest - first);
    const int16_t strong_x = (count > 1U) ?
                                 (int16_t)(x0 + ((int32_t)(x1 - x0) * strong_col) / (count - 1U)) :
                                 (int16_t)((x0 + x1) / 2);
    for(int16_t y = 10; y < 49; y += 3) poom_arduboy_draw_pixel(strong_x, y, WHITE);
    poom_arduboy_fill_triangle(strong_x, 11, (int16_t)(strong_x - 3), 8, (int16_t)(strong_x + 3), 8, WHITE);
    spectrum_footer_(strongest);
    poom_arduboy_display();
}

static bool spectrum_dither_(int16_t x, int16_t y, uint8_t level)
{
    const uint8_t phase = (uint8_t)(((uint16_t)x + (uint16_t)y * 3U) & 0x03U);
    return (level == 3U) || ((level == 2U) && ((phase & 1U) == 0U)) ||
           ((level == 1U) && (phase == 0U));
}

static void spectrum_render_water_(void)
{
    const uint8_t first = s_page_first[s_spectrum->page];
    const uint8_t count = s_page_count[s_spectrum->page];
    uint8_t strongest = first;

    spectrum_header_("WATER");
    for(uint8_t row = 0U; row < SPECTRUM_WATER_ROWS; ++row)
    {
        const uint8_t history = (uint8_t)((s_spectrum->water_head + row) % SPECTRUM_WATER_ROWS);
        const int16_t y = (int16_t)(10 + row);
        for(uint8_t col = 0U; col < count; ++col)
        {
            const int16_t xs = (int16_t)(2 + ((int32_t)123 * col) / count);
            const int16_t xe = (int16_t)(2 + ((int32_t)123 * (col + 1U)) / count);
            const uint8_t level = s_spectrum->water[history][col];
            for(int16_t x = xs; x < xe; ++x)
            {
                if(spectrum_dither_(x, y, level)) poom_arduboy_draw_pixel(x, y, WHITE);
            }
        }
    }
    poom_arduboy_draw_rect(1, 9, 125, 34, WHITE);
    for(uint8_t col = 0U; col < count; ++col)
    {
        const uint8_t idx = (uint8_t)(first + col);
        if(s_spectrum->ema[idx] > s_spectrum->ema[strongest]) strongest = idx;
    }
    spectrum_footer_(strongest);
    poom_arduboy_draw_tiny_text(2, 45, "NEW", WHITE);
    poom_arduboy_draw_tiny_text(109, 45, "HIST", WHITE);
    poom_arduboy_display();
}

static void spectrum_render_(void)
{
    if(s_spectrum == NULL) return;
    if(!s_spectrum->scanner_started)
    {
        char error[18];
        printf("[SPECTRUM] render launch state err=%d\n", (int)s_spectrum->startup_error);
        poom_arduboy_clear();
        poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 8, WHITE);
        /* Use the established menu font for the launch screen.  It makes
         * the initial frame independent of the compact graph font. */
        poom_arduboy_set_cursor(31, 1);
        (void)poom_arduboy_print("SPECTRUM");
        if(s_spectrum->startup_error == ESP_OK)
        {
            poom_arduboy_set_cursor(35, 26);
            (void)poom_arduboy_print("STARTING");
        }
        else
        {
            (void)snprintf(error, sizeof(error), "START ERR %d", (int)s_spectrum->startup_error);
            poom_arduboy_set_cursor(18, 26);
            (void)poom_arduboy_print(error);
        }
        poom_arduboy_set_cursor(82, 55);
        (void)poom_arduboy_print("B:BACK");
        poom_arduboy_display();
        printf("[SPECTRUM] launch frame sent\n");
        return;
    }
    if(s_spectrum->view == SPECTRUM_VIEW_WATER) spectrum_render_water_();
    else spectrum_render_trace_();
}

static void spectrum_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;
    if(s_spectrum == NULL || !s_spectrum->active || msg == NULL ||
       msg->len < sizeof(button_event_msg_t) || s_spectrum->buttons == NULL) return;
    button_event_msg_t event;
    memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK) return;
    const spectrum_button_t button = {.button = event.button};
    (void)xQueueSend(s_spectrum->buttons, &button, 0);
}

static void spectrum_handle_button_(uint8_t button)
{
    if(s_spectrum == NULL) return;
    if(button == BTN_B)
    {
        spectrum_exit_();
    }
    else if(button == BTN_A)
    {
        s_spectrum->paused = !s_spectrum->paused;
    }
    else if(button == BTN_LEFT || button == BTN_RIGHT)
    {
        s_spectrum->view = (s_spectrum->view == SPECTRUM_VIEW_TRACE) ?
                               SPECTRUM_VIEW_WATER : SPECTRUM_VIEW_TRACE;
    }
    else if(button == BTN_UP || button == BTN_DOWN)
    {
        const uint8_t total = spectrum_page_total_();
        if(button == BTN_UP)
            s_spectrum->page = (uint8_t)((s_spectrum->page + total - 1U) % total);
        else
            s_spectrum->page = (uint8_t)((s_spectrum->page + 1U) % total);
        memset(s_spectrum->water, 0, sizeof(s_spectrum->water));
        s_spectrum->water_head = 0U;
    }
}

static void spectrum_exit_(void)
{
    spectrum_state_t *state = s_spectrum;
    if(state == NULL)
    {
        return;
    }
    ESP_LOGI(SPECTRUM_TAG, "exit: scanner=%d subscribed=%d", state->scanner_started, state->subscribed);
    const bool was_active = state->active;
    state->active = false;
    if(state->scanner_started)
    {
        (void)poom_scanner_core_stop();
    }
    if(state->subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", spectrum_button_cb_, (void *)s_sbus_user);
        state->subscribed = false;
    }
    if(state->buttons != NULL)
    {
        vQueueDelete(state->buttons);
        state->buttons = NULL;
    }
    s_spectrum = NULL;
    free(state);
    if(was_active)
    {
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    }
}

static void spectrum_task_(void *arg)
{
    (void)arg;
    ESP_LOGI(SPECTRUM_TAG, "UI task running");
    while(s_spectrum != NULL && s_spectrum->active)
    {
        spectrum_button_t event;
        if(s_spectrum->buttons != NULL &&
           xQueueReceive(s_spectrum->buttons, &event, pdMS_TO_TICKS(SPECTRUM_TICK_MS)) == pdTRUE)
        {
            spectrum_handle_button_(event.button);
        }
        if(s_spectrum != NULL && s_spectrum->active)
        {
            spectrum_update_();
            spectrum_render_();
        }
    }
    vTaskDelete(NULL);
}

void menu_spectrum_show(void)
{
    printf("[SPECTRUM] show entered\n");
    ESP_LOGI(SPECTRUM_TAG, "open requested");
    spectrum_exit_();
    /* The live history is transient and is not ISR/DMA data.  Keep it in
     * PSRAM, release it on B, and preserve HP SRAM for Wi-Fi and OLED work. */
    s_spectrum = heap_caps_calloc(1U, sizeof(*s_spectrum),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_spectrum == NULL)
    {
        printf("[SPECTRUM] state allocation failed\n");
        ESP_LOGE(SPECTRUM_TAG, "state allocation failed: %p", (void *)s_spectrum);
        s_spectrum = NULL;
        poom_arduboy_clear();
        poom_arduboy_draw_tiny_text(35, 28, "NO MEMORY", WHITE);
        poom_arduboy_display();
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }
    printf("[SPECTRUM] state allocated\n");
    ESP_LOGI(SPECTRUM_TAG, "state=%p bytes=%u", (void *)s_spectrum,
             (unsigned)sizeof(*s_spectrum));
    for(uint8_t i = 0U; i < POOM_SCANNER_CORE_WIFI_CH_COUNT; ++i)
    {
        s_spectrum->ema[i] = SPECTRUM_NOISE_DBM;
        s_spectrum->peak[i] = SPECTRUM_NOISE_DBM;
    }
    /* Mark active before subscriptions so every early-failure path resumes
     * the detached main menu through spectrum_exit_(). */
    s_spectrum->active = true;
    s_spectrum->startup_error = ESP_OK;
    s_spectrum->buttons = xQueueCreate(8U, sizeof(spectrum_button_t));
    if(s_spectrum->buttons == NULL ||
       !poom_sbus_subscribe_cb("input/button", spectrum_button_cb_, (void *)s_sbus_user))
    {
        printf("[SPECTRUM] button setup failed\n");
        ESP_LOGE(SPECTRUM_TAG, "button setup failed: queue=%p", (void *)s_spectrum->buttons);
        spectrum_exit_();
        return;
    }
    s_spectrum->subscribed = true;

    /* Other POOM menu apps paint their first frame synchronously.  Do this
     * before starting Wi-Fi and before depending on the UI task, otherwise a
     * slow scanner start can leave the OLED blank. */
    spectrum_render_();
    printf("[SPECTRUM] stopping old scanner\n");
    /* Scanner Core is shared.  Match SCAN CHANNELS and release a former
     * scanner session before taking ownership of promiscuous Wi-Fi. */
    (void)poom_scanner_core_stop();
    s_spectrum->startup_error = poom_scanner_core_start_wifi(80U);
    printf("[SPECTRUM] scanner start result=%d\n", (int)s_spectrum->startup_error);
    ESP_LOGI(SPECTRUM_TAG, "scanner start: %s (%d)",
             esp_err_to_name(s_spectrum->startup_error), (int)s_spectrum->startup_error);
    if(s_spectrum->startup_error == ESP_OK)
    {
        s_spectrum->scanner_started = true;
    }
    spectrum_render_();

    if(xTaskCreate(spectrum_task_, "menu_spectrum", SPECTRUM_STACK, NULL,
                   SPECTRUM_PRIORITY, &s_spectrum->task) != pdPASS)
    {
        printf("[SPECTRUM] task create failed\n");
        ESP_LOGE(SPECTRUM_TAG, "UI task creation failed");
        spectrum_exit_();
    }
    else
    {
        printf("[SPECTRUM] task created\n");
        ESP_LOGI(SPECTRUM_TAG, "UI task=%p ready", (void *)s_spectrum->task);
    }
}
