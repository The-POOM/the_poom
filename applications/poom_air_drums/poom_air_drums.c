// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/* poom_air_drums.c
 *
 * Air Drums application over BLE MIDI
 * - Reads IMU motion and triggers drum notes on MIDI channel 10
 * - Uses button topic to select drum note mapping
 *
 * Notes:
 * - BLE MIDI transport stays in ble_midi component
 * - This component owns the gesture-to-note logic
 */

#include "poom_air_drums.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_midi.h"
#include "poom_imu_stream.h"
#include "poom_sbus.h"

/**
 * @file poom_air_drums.c
 * @brief Air Drums application logic over BLE MIDI.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */

#if POOM_AIR_DRUMS_LOG_ENABLED
    static const char *POOM_AIR_DRUMS_TAG = "poom_air_drums";

    #define POOM_AIR_DRUMS_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\\n", POOM_AIR_DRUMS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_AIR_DRUMS_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\\n", POOM_AIR_DRUMS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_AIR_DRUMS_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\\n", POOM_AIR_DRUMS_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_AIR_DRUMS_DEBUG_LOG_ENABLED
        #define POOM_AIR_DRUMS_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\\n", POOM_AIR_DRUMS_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_AIR_DRUMS_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_AIR_DRUMS_PRINTF_E(...) do { } while (0)
    #define POOM_AIR_DRUMS_PRINTF_W(...) do { } while (0)
    #define POOM_AIR_DRUMS_PRINTF_I(...) do { } while (0)
    #define POOM_AIR_DRUMS_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants
 * ========================= */
#define ARRAY_LEN(x) (sizeof(x) / sizeof((x)[0]))

/* BLE MIDI */
#define MIDI_PORT_INDEX                     0U
#define MIDI_NOTE_ON_CH10_STATUS            0x99U
#define MIDI_NOTE_OFF_CH10_STATUS           0x89U
#define MIDI_NOTE_OFF_VALUE                 0x00U
#define MIDI_MESSAGE_SIZE                   3U
#define MIDI_VELOCITY_MIN                   25U
#define MIDI_VELOCITY_MAX                   127U

/* Buttons */
#define BUTTON_EVENT_SINGLE_CLICK           4U
#define BUTTON_EVENT_DOUBLE_CLICK           5U
#define BUTTON_TOPIC                        "input/button"
#define BUTTON_SUBSCRIBER_ID                "poom_air_drums"

/* Drum notes */
#define NOTE_KICK                           36U
#define NOTE_SNARE                          38U
#define NOTE_HH_CLOSED                      42U
#define NOTE_HH_OPEN                        46U
#define NOTE_CRASH                          49U
#define NOTE_LOW_TOM                        45U

/* Hit detector */
#define G_BASELINE                          1.0f
#define ACC_MG_TO_G                         1000.0f
#define GYRO_MDPS_TO_DPS                    1000.0f
#define GYRO_DYNAMIC_SCALE_DPS              700.0f
#define MOTION_NORM_MIN                     0.06f
#define MOTION_NORM_MAX                     2.2f
#define NORMALIZED_MIN                      0.0f
#define NORMALIZED_MAX                      1.0f
#define MIDI_HIT_THRESHOLD_ON               12U
#define MIDI_HIT_MIN_INTERVAL_MS            80U
#define MIDI_HIT_NOTE_LEN_MS                35U

/* Task */
#define POOM_AIR_DRUMS_TASK_DELAY_MS        10U
#define POOM_AIR_DRUMS_START_DELAY_MS       500U
#define POOM_AIR_DRUMS_TASK_STACK_SIZE      2048U
#define POOM_AIR_DRUMS_TASK_PRIORITY        (tskIDLE_PRIORITY + 2)
#define POOM_AIR_DRUMS_TASK_NAME            "poom_air_drums"

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} poom_air_drums_button_event_t;

/* =========================
 * Local state
 * ========================= */
static const uint8_t k_button_note_map[] = {
    NOTE_KICK, NOTE_SNARE, NOTE_HH_CLOSED, NOTE_HH_OPEN, NOTE_CRASH, NOTE_LOW_TOM
};

static uint8_t s_selected_note = NOTE_CRASH;
static uint8_t s_active_note = NOTE_KICK;
static bool s_hit_active = false;
static bool s_buttons_subscribed = false;
static bool s_started = false;
static TickType_t s_last_hit_tick = 0;
static TickType_t s_note_on_tick = 0;
static TaskHandle_t s_poom_air_drums_task = NULL;

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Clamps a floating-point value to the provided range.
 *
 * @param value Input value.
 * @param min_value Lower bound.
 * @param max_value Upper bound.
 * @return Clamped value.
 */
static inline float poom_air_drums_clampf_(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

/**
 * @brief Checks whether a button event is valid for note selection.
 *
 * @param event Button event ID.
 * @return true if event is single or double click.
 */
static inline bool poom_air_drums_is_note_select_event_(uint8_t event)
{
    return (event == BUTTON_EVENT_SINGLE_CLICK) || (event == BUTTON_EVENT_DOUBLE_CLICK);
}

/**
 * @brief Sends MIDI NOTE ON on channel 10.
 *
 * @param note MIDI note.
 * @param velocity MIDI velocity.
 */
static inline void poom_air_drums_midi_note_on_ch10_(uint8_t note, uint8_t velocity)
{
    uint8_t msg[MIDI_MESSAGE_SIZE] = {MIDI_NOTE_ON_CH10_STATUS, note, velocity};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

/**
 * @brief Sends MIDI NOTE OFF on channel 10.
 *
 * @param note MIDI note.
 */
static inline void poom_air_drums_midi_note_off_ch10_(uint8_t note)
{
    uint8_t msg[MIDI_MESSAGE_SIZE] = {MIDI_NOTE_OFF_CH10_STATUS, note, MIDI_NOTE_OFF_VALUE};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

/**
 * @brief Handles button events used to select the active drum note.
 *
 * @param msg Incoming SBUS message.
 * @param user User context (unused).
 */
static void poom_air_drums_button_event_cb_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    if (msg->len < sizeof(poom_air_drums_button_event_t))
    {
        POOM_AIR_DRUMS_PRINTF_W("input/button payload too short: len=%u", (unsigned)msg->len);
        return;
    }

    poom_air_drums_button_event_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (poom_air_drums_is_note_select_event_(ev.event) && (ev.button < ARRAY_LEN(k_button_note_map)))
    {
        s_selected_note = k_button_note_map[ev.button];
    }

    POOM_AIR_DRUMS_PRINTF_D("[%s] b=%u e=%u t=%ums",
                            msg->topic,
                            ev.button,
                            ev.event,
                            (unsigned)ev.ts_ms);
}

/**
 * @brief BLE MIDI RX callback for diagnostics.
 *
 * @param blemidi_port BLE MIDI logical port.
 * @param timestamp BLE MIDI timestamp.
 * @param midi_status MIDI status byte.
 * @param remaining_message Remaining MIDI payload bytes.
 * @param len Remaining payload length.
 * @param continued_sysex_pos Continued sysex position.
 */
static void poom_air_drums_midi_rx_cb_(uint8_t blemidi_port,
                                       uint16_t timestamp,
                                       uint8_t midi_status,
                                       uint8_t *remaining_message,
                                       size_t len,
                                       size_t continued_sysex_pos)
{
    (void)continued_sysex_pos;
    POOM_AIR_DRUMS_PRINTF_I("RX MIDI port=%u ts=%u status=0x%02X",
                            (unsigned)blemidi_port,
                            (unsigned)timestamp,
                            (unsigned)midi_status);

    for (size_t i = 0; i < len; i++)
    {
        POOM_AIR_DRUMS_PRINTF_D("RX data[%u]=0x%02X", (unsigned)i, (unsigned)remaining_message[i]);
    }
}

/**
 * @brief Subscribes to button topic if not already subscribed.
 */
static void poom_air_drums_buttons_subscribe_(void)
{
    if (!s_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb(BUTTON_TOPIC, poom_air_drums_button_event_cb_, (void *)BUTTON_SUBSCRIBER_ID))
        {
            s_buttons_subscribed = true;
        }
        else
        {
            POOM_AIR_DRUMS_PRINTF_W("failed to subscribe to %s", BUTTON_TOPIC);
        }
    }
}

/**
 * @brief Unsubscribes from button topic if currently subscribed.
 */
static void poom_air_drums_buttons_unsubscribe_(void)
{
    if (s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb(BUTTON_TOPIC, poom_air_drums_button_event_cb_, (void *)BUTTON_SUBSCRIBER_ID);
        s_buttons_subscribed = false;
    }
}

/**
 * @brief Executes one Air Drums processing step.
 *
 * Reads IMU data, computes motion intensity, and emits NOTE ON/OFF events
 * according to configured thresholds and timings.
 *
 * @param trigger_pressed Hit detection enable flag.
 */
static void poom_air_drums_midi_step_(bool trigger_pressed)
{
    poom_imu_data_t d = {0};
    if (!poom_imu_stream_read_data(&d))
    {
        return;
    }

    float ax = d.acceleration_mg[0] / ACC_MG_TO_G;
    float ay = d.acceleration_mg[1] / ACC_MG_TO_G;
    float az = d.acceleration_mg[2] / ACC_MG_TO_G;
    float gx = d.angular_rate_mdps[0] / GYRO_MDPS_TO_DPS;
    float gy = d.angular_rate_mdps[1] / GYRO_MDPS_TO_DPS;
    float gz = d.angular_rate_mdps[2] / GYRO_MDPS_TO_DPS;

    float a_total = sqrtf(ax * ax + ay * ay + az * az);
    float a_dynamic = fabsf(a_total - G_BASELINE);
    float gyro_dynamic = sqrtf(gx * gx + gy * gy + gz * gz) / GYRO_DYNAMIC_SCALE_DPS;
    float motion = a_dynamic + gyro_dynamic;

    float norm = (motion - MOTION_NORM_MIN) / (MOTION_NORM_MAX - MOTION_NORM_MIN);
    uint8_t velocity = (uint8_t)(poom_air_drums_clampf_(norm, NORMALIZED_MIN, NORMALIZED_MAX) *
                                 (float)MIDI_VELOCITY_MAX);
    TickType_t now = xTaskGetTickCount();

    if (s_hit_active &&
        (((now - s_note_on_tick) >= pdMS_TO_TICKS(MIDI_HIT_NOTE_LEN_MS)) || !trigger_pressed))
    {
        poom_air_drums_midi_note_off_ch10_(s_active_note);
        s_hit_active = false;
    }

    if (!trigger_pressed)
    {
        return;
    }

    if (!s_hit_active &&
        velocity >= MIDI_HIT_THRESHOLD_ON &&
        ((now - s_last_hit_tick) >= pdMS_TO_TICKS(MIDI_HIT_MIN_INTERVAL_MS)))
    {
        uint8_t midi_vel = velocity;
        if (midi_vel < MIDI_VELOCITY_MIN)
        {
            midi_vel = MIDI_VELOCITY_MIN;
        }
        if (midi_vel > MIDI_VELOCITY_MAX)
        {
            midi_vel = MIDI_VELOCITY_MAX;
        }

        poom_air_drums_midi_note_on_ch10_(s_selected_note, midi_vel);
        s_active_note = s_selected_note;
        s_hit_active = true;
        s_note_on_tick = now;
        s_last_hit_tick = now;

        POOM_AIR_DRUMS_PRINTF_I("HIT: motion=%.2f vel=%u note=%u",
                                motion,
                                (unsigned)midi_vel,
                                (unsigned)s_selected_note);
    }
}

/**
 * @brief Main Air Drums FreeRTOS task loop.
 *
 * @param arg Task argument (unused).
 */
static void poom_air_drums_task_(void *arg)
{
    (void)arg;
    for (;;)
    {
        bool trigger = true; /* TODO: bind to enable button/gesture */
        poom_air_drums_midi_step_(trigger);
        vTaskDelay(pdMS_TO_TICKS(POOM_AIR_DRUMS_TASK_DELAY_MS));
    }
}

/**
 * @brief Creates the Air Drums task if it is not running.
 */
static void poom_air_drums_task_start_(void)
{
    if (s_poom_air_drums_task != NULL)
    {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(POOM_AIR_DRUMS_START_DELAY_MS));
    if (xTaskCreate(poom_air_drums_task_,
                    POOM_AIR_DRUMS_TASK_NAME,
                    POOM_AIR_DRUMS_TASK_STACK_SIZE,
                    NULL,
                    POOM_AIR_DRUMS_TASK_PRIORITY,
                    &s_poom_air_drums_task) == pdPASS)
    {
        POOM_AIR_DRUMS_PRINTF_I("poom_air_drums task created");
    }
    else
    {
        POOM_AIR_DRUMS_PRINTF_E("failed to create poom_air_drums task");
    }
}

/* =========================
 * Public API
 * ========================= */
/**
 * @brief Starts the Air Drums application.
 *
 * Initializes BLE MIDI and IMU, subscribes to button events, and starts
 * the periodic processing task.
 */
void poom_air_drums_start(void)
{
    if (s_started)
    {
        POOM_AIR_DRUMS_PRINTF_W("poom_air_drums already started");
        return;
    }
    s_started = true;

    int status = blemidi_init((void *)poom_air_drums_midi_rx_cb_);
    poom_imu_stream_init();

    if (status < 0)
    {
        POOM_AIR_DRUMS_PRINTF_E("BLE MIDI init failed: %d", status);
    }
    else
    {
        POOM_AIR_DRUMS_PRINTF_I("BLE MIDI initialized");
    }

    poom_air_drums_buttons_subscribe_();
    poom_air_drums_task_start_();
}

/**
 * @brief Stops the Air Drums application.
 *
 * Deletes task, sends NOTE OFF if needed, and unsubscribes from button events.
 */
void poom_air_drums_stop(void)
{
    if (!s_started)
    {
        return;
    }

    if (s_poom_air_drums_task != NULL)
    {
        vTaskDelete(s_poom_air_drums_task);
        s_poom_air_drums_task = NULL;
        POOM_AIR_DRUMS_PRINTF_I("poom_air_drums task deleted");
    }

    if (s_hit_active)
    {
        poom_air_drums_midi_note_off_ch10_(s_active_note);
        s_hit_active = false;
    }

    poom_air_drums_buttons_unsubscribe_();
    s_started = false;
}
