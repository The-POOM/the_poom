// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/* poom_air_drums.c
 *
 * Air Drums application over BLE MIDI
 * - Reads IMU motion and triggers drum notes on MIDI channel 10
 * - Exposes configurable note/threshold globals for menu-driven control
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
#define MIDI_NOTE_ON_CH1_STATUS             0x90U
#define MIDI_NOTE_OFF_CH1_STATUS            0x80U
#define MIDI_NOTE_OFF_VALUE                 0x00U
#define MIDI_MESSAGE_SIZE                   3U
#define MIDI_VELOCITY_MIN                   25U
#define MIDI_VELOCITY_MAX                   127U

/* Buttons */
/* Drum notes (defaults only; actual note is controlled by g_midi_note) */
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
#define MIDI_HIT_THRESHOLD_ON_DEFAULT       12U
#define MIDI_HIT_MIN_INTERVAL_MS            80U
#define MIDI_HIT_NOTE_LEN_MS                35U
#define MIDI_HIT_ARM_SAMPLES               3U
#define MIDI_CALIB_SAMPLES                 50U
#define MOTION_BASELINE_ALPHA              0.01f
#define MOTION_BASELINE_QUIET_MARGIN       0.04f
#define MOTION_RISE_MIN                    0.015f

/* Melody mode */
#define MELODY_GYRO_ON_DPS                  140.0f
#define MELODY_GYRO_OFF_DPS                 70.0f
#define MELODY_GYRO_VELOCITY_DPS_MAX        550.0f
#define MELODY_OCTAVE_STEP_DPS              220.0f
#define MELODY_OCTAVE_REARM_DPS             80.0f
#define MELODY_OCTAVE_COOLDOWN_MS           250U
#define MELODY_OCTAVE_MIN                   (-4)
#define MELODY_OCTAVE_MAX                   (4)
#define MELODY_DEGREE_STEP_DPS              180.0f
#define MELODY_DEGREE_REARM_DPS             70.0f
#define MELODY_DEGREE_COOLDOWN_MS           180U
#define MELODY_DEGREE_MIN                   (0)
#define MELODY_DEGREE_MAX                   (4)

/* Task */
#define POOM_AIR_DRUMS_TASK_DELAY_MS        10U
#define POOM_AIR_DRUMS_START_DELAY_MS       500U
#define POOM_AIR_DRUMS_TASK_STACK_SIZE      2048U
#define POOM_AIR_DRUMS_TASK_PRIORITY        (tskIDLE_PRIORITY + 2)
#define POOM_AIR_DRUMS_TASK_NAME            "poom_air_drums"

/* =========================
 * Local state
 * ========================= */
uint8_t g_midi_note = NOTE_CRASH;
uint8_t g_hit_threshold = MIDI_HIT_THRESHOLD_ON_DEFAULT;
uint8_t g_midi_mode = POOM_MIDI_MODE_DRUM;
uint8_t g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;

static uint8_t s_active_note = NOTE_KICK;
static bool s_hit_active = false;
static bool s_started = false;
static volatile bool s_stop_requested = false;
static TickType_t s_last_hit_tick = 0;
static TickType_t s_note_on_tick = 0;
static TaskHandle_t s_poom_air_drums_task = NULL;
static uint8_t s_arm_count = 0U;
static float s_motion_baseline = 0.0f;
static float s_calib_motion_sum = 0.0f;
static uint16_t s_calib_count = 0U;
static bool s_calibrated = false;
static float s_prev_motion_hp = 0.0f;
static bool s_melody_note_active = false;
static uint8_t s_melody_active_note = 60U;
static int8_t s_melody_octave = 0;
static bool s_melody_octave_armed = true;
static TickType_t s_melody_last_octave_step_tick = 0;
static int8_t s_melody_degree = 0;
static bool s_melody_degree_armed = true;
static TickType_t s_melody_last_degree_step_tick = 0;
static uint8_t s_prev_mode = POOM_MIDI_MODE_DRUM;

static const int8_t k_pentatonic_major_intervals_[5] = {0, 2, 4, 7, 9};
static const int8_t k_pentatonic_minor_intervals_[5] = {0, 3, 5, 7, 10};

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

static inline void poom_air_drums_midi_note_on_ch1_(uint8_t note, uint8_t velocity)
{
    uint8_t msg[MIDI_MESSAGE_SIZE] = {MIDI_NOTE_ON_CH1_STATUS, note, velocity};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

static inline void poom_air_drums_midi_note_off_ch1_(uint8_t note)
{
    uint8_t msg[MIDI_MESSAGE_SIZE] = {MIDI_NOTE_OFF_CH1_STATUS, note, MIDI_NOTE_OFF_VALUE};
    (void)blemidi_send_message(MIDI_PORT_INDEX, msg, sizeof(msg));
}

static inline uint8_t poom_air_drums_clamp_u8_(int value, int min_value, int max_value)
{
    if (value < min_value)
    {
        return (uint8_t)min_value;
    }
    if (value > max_value)
    {
        return (uint8_t)max_value;
    }
    return (uint8_t)value;
}

static void poom_air_drums_melody_reset_(void)
{
    s_melody_note_active = false;
    s_melody_active_note = 60U;
    s_melody_octave = 0;
    s_melody_octave_armed = true;
    s_melody_last_octave_step_tick = 0;
    s_melody_degree = 0;
    s_melody_degree_armed = true;
    s_melody_last_degree_step_tick = 0;
}

static inline uint8_t poom_air_drums_scale_interval_(uint8_t scale, uint8_t degree)
{
    const uint8_t idx = (degree > 4U) ? 4U : degree;
    if (scale == POOM_MIDI_SCALE_PENTATONIC_MINOR)
    {
        return (uint8_t)k_pentatonic_minor_intervals_[idx];
    }
    return (uint8_t)k_pentatonic_major_intervals_[idx];
}

static uint8_t poom_air_drums_melody_note_(void)
{
    const int tonic = (int)g_midi_note;
    const int interval = (int)poom_air_drums_scale_interval_(g_midi_scale, (uint8_t)s_melody_degree);
    const int note = tonic + interval + ((int)s_melody_octave * 12);
    return poom_air_drums_clamp_u8_(note, 0, 127);
}

static uint8_t poom_air_drums_melody_velocity_(float gyro_dps_norm)
{
    float v = gyro_dps_norm / MELODY_GYRO_VELOCITY_DPS_MAX;
    v = poom_air_drums_clampf_(v, 0.0f, 1.0f);
    uint8_t vel = (uint8_t)(v * (float)MIDI_VELOCITY_MAX);
    if (vel < MIDI_VELOCITY_MIN)
    {
        vel = MIDI_VELOCITY_MIN;
    }
    if (vel > MIDI_VELOCITY_MAX)
    {
        vel = MIDI_VELOCITY_MAX;
    }
    return vel;
}

static void poom_air_drums_melody_step_(float gx_dps, float gy_dps, float gz_dps)
{
    TickType_t now = xTaskGetTickCount();

    const float gyro_norm = sqrtf(gx_dps * gx_dps + gy_dps * gy_dps + gz_dps * gz_dps);

    if (!s_melody_octave_armed)
    {
        if (fabsf(gy_dps) <= MELODY_OCTAVE_REARM_DPS)
        {
            s_melody_octave_armed = true;
        }
    }
    else
    {
        if ((now - s_melody_last_octave_step_tick) >= pdMS_TO_TICKS(MELODY_OCTAVE_COOLDOWN_MS))
        {
            if (gy_dps >= MELODY_OCTAVE_STEP_DPS)
            {
                if (s_melody_octave < MELODY_OCTAVE_MAX)
                {
                    s_melody_octave++;
                }
                s_melody_octave_armed = false;
                s_melody_last_octave_step_tick = now;
            }
            else if (gy_dps <= -MELODY_OCTAVE_STEP_DPS)
            {
                if (s_melody_octave > MELODY_OCTAVE_MIN)
                {
                    s_melody_octave--;
                }
                s_melody_octave_armed = false;
                s_melody_last_octave_step_tick = now;
            }
        }
    }

    // Scale degree change driven by X axis (roll). Require rearm + cooldown.
    if (!s_melody_degree_armed)
    {
        if (fabsf(gx_dps) <= MELODY_DEGREE_REARM_DPS)
        {
            s_melody_degree_armed = true;
        }
    }
    else
    {
        if ((now - s_melody_last_degree_step_tick) >= pdMS_TO_TICKS(MELODY_DEGREE_COOLDOWN_MS))
        {
            if (gx_dps >= MELODY_DEGREE_STEP_DPS)
            {
                if (s_melody_degree < MELODY_DEGREE_MAX)
                {
                    s_melody_degree++;
                }
                s_melody_degree_armed = false;
                s_melody_last_degree_step_tick = now;
            }
            else if (gx_dps <= -MELODY_DEGREE_STEP_DPS)
            {
                if (s_melody_degree > MELODY_DEGREE_MIN)
                {
                    s_melody_degree--;
                }
                s_melody_degree_armed = false;
                s_melody_last_degree_step_tick = now;
            }
        }
    }

    const bool want_note_on = (gyro_norm >= MELODY_GYRO_ON_DPS) && blemidi_is_connected();
    const bool want_note_off = (gyro_norm <= MELODY_GYRO_OFF_DPS) || !blemidi_is_connected();
    const uint8_t note = poom_air_drums_melody_note_();

    if (s_melody_note_active)
    {
        if (want_note_off)
        {
            poom_air_drums_midi_note_off_ch1_(s_melody_active_note);
            s_melody_note_active = false;
        }
        else if (note != s_melody_active_note)
        {
            poom_air_drums_midi_note_off_ch1_(s_melody_active_note);
            s_melody_active_note = note;
            poom_air_drums_midi_note_on_ch1_(note, MIDI_VELOCITY_MIN);
        }
    }
    else
    {
        if (want_note_on)
        {
            s_melody_active_note = note;
            const uint8_t vel = poom_air_drums_melody_velocity_(gyro_norm);
            poom_air_drums_midi_note_on_ch1_(note, vel);
            s_melody_note_active = true;
        }
    }
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

    if (g_midi_mode > POOM_MIDI_MODE_MELODY)
    {
        g_midi_mode = POOM_MIDI_MODE_DRUM;
    }
    if (g_midi_scale > POOM_MIDI_SCALE_PENTATONIC_MINOR)
    {
        g_midi_scale = POOM_MIDI_SCALE_PENTATONIC_MAJOR;
    }

    float ax = d.acceleration_mg[0] / ACC_MG_TO_G;
    float ay = d.acceleration_mg[1] / ACC_MG_TO_G;
    float az = d.acceleration_mg[2] / ACC_MG_TO_G;
    float gx = d.angular_rate_mdps[0] / GYRO_MDPS_TO_DPS;
    float gy = d.angular_rate_mdps[1] / GYRO_MDPS_TO_DPS;
    float gz = d.angular_rate_mdps[2] / GYRO_MDPS_TO_DPS;

    if (g_midi_mode != s_prev_mode)
    {
        if (s_hit_active)
        {
            poom_air_drums_midi_note_off_ch10_(s_active_note);
            s_hit_active = false;
        }
        if (s_melody_note_active)
        {
            poom_air_drums_midi_note_off_ch1_(s_melody_active_note);
            s_melody_note_active = false;
        }
        s_prev_mode = g_midi_mode;
    }

    if (g_midi_mode == POOM_MIDI_MODE_MELODY)
    {
        poom_air_drums_melody_step_(gx, gy, gz);
        return;
    }

    float a_total = sqrtf(ax * ax + ay * ay + az * az);
    float a_dynamic = fabsf(a_total - G_BASELINE);
    float gyro_dynamic = sqrtf(gx * gx + gy * gy + gz * gz) / GYRO_DYNAMIC_SCALE_DPS;
    float motion_raw = a_dynamic + gyro_dynamic;

    if (!s_calibrated)
    {
        s_calib_motion_sum += motion_raw;
        s_calib_count++;
        if (s_calib_count >= MIDI_CALIB_SAMPLES)
        {
            s_motion_baseline = s_calib_motion_sum / (float)s_calib_count;
            s_calibrated = true;
            s_prev_motion_hp = 0.0f;
            POOM_AIR_DRUMS_PRINTF_I("calibrated baseline=%.3f (%u samples)",
                                    (double)s_motion_baseline,
                                    (unsigned)s_calib_count);
        }
        return;
    }

    if (!s_hit_active)
    {
        if (motion_raw <= (s_motion_baseline + MOTION_BASELINE_QUIET_MARGIN))
        {
            s_motion_baseline = ((1.0f - MOTION_BASELINE_ALPHA) * s_motion_baseline) +
                                (MOTION_BASELINE_ALPHA * motion_raw);
        }
    }

    float motion = motion_raw - s_motion_baseline;
    if (motion < 0.0f)
    {
        motion = 0.0f;
    }
    float motion_rise = motion - s_prev_motion_hp;
    s_prev_motion_hp = motion;

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

    if (velocity >= g_hit_threshold)
    {
        if (motion_rise >= MOTION_RISE_MIN)
        {
            if (s_arm_count < MIDI_HIT_ARM_SAMPLES)
            {
                s_arm_count++;
            }
        }
        else
        {
            s_arm_count = 0U;
        }
    }
    else
    {
        s_arm_count = 0U;
    }

    if (!s_hit_active &&
        (s_arm_count >= MIDI_HIT_ARM_SAMPLES) &&
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

        const uint8_t note = g_midi_note;
        s_active_note = note;
        poom_air_drums_midi_note_on_ch10_(note, midi_vel);
        s_hit_active = true;
        s_note_on_tick = now;
        s_last_hit_tick = now;
        s_arm_count = 0U;

        POOM_AIR_DRUMS_PRINTF_I("HIT: motion=%.2f vel=%u note=%u",
                                motion,
                                (unsigned)midi_vel,
                                (unsigned)note);
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
        if (s_stop_requested)
        {
            break;
        }
        bool trigger = true; /* TODO: bind to enable button/gesture */
        poom_air_drums_midi_step_(trigger);
        vTaskDelay(pdMS_TO_TICKS(POOM_AIR_DRUMS_TASK_DELAY_MS));
    }

    s_poom_air_drums_task = NULL;
    vTaskDelete(NULL);
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
    s_stop_requested = false;

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

    s_arm_count = 0U;
    s_motion_baseline = 0.0f;
    s_calib_motion_sum = 0.0f;
    s_calib_count = 0U;
    s_calibrated = false;
    poom_air_drums_melody_reset_();
    s_prev_mode = g_midi_mode;

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

    s_stop_requested = true;

    if (s_hit_active)
    {
        poom_air_drums_midi_note_off_ch10_(s_active_note);
        s_hit_active = false;
    }

    if (s_melody_note_active)
    {
        poom_air_drums_midi_note_off_ch1_(s_melody_active_note);
        s_melody_note_active = false;
    }

    s_arm_count = 0U;
    s_calibrated = false;
    s_calib_motion_sum = 0.0f;
    s_calib_count = 0U;
    s_started = false;

    // Shut down BLE MIDI so it doesn't remain advertising/connected after exiting the menu.
    blemidi_deinit();
}
