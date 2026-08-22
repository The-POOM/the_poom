// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_wii.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "poom_ble_hid.h"
#include "poom_imu_stream.h"



#if POOM_WII_LOG_ENABLED
#define POOM_WII_TAG "poom_wii"
#define POOM_WII_LOGI(fmt, ...) printf("[I] [%s] %s:%d: " fmt "\n", POOM_WII_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_WII_LOGW(fmt, ...) printf("[W] [%s] %s:%d: " fmt "\n", POOM_WII_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_WII_LOGE(fmt, ...) printf("[E] [%s] %s:%d: " fmt "\n", POOM_WII_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define POOM_WII_LOGI(...) do { } while (0)
#define POOM_WII_LOGW(...) do { } while (0)
#define POOM_WII_LOGE(...) do { } while (0)
#endif

#define POOM_WII_PI_F (3.14159265358979323846f)
#define POOM_WII_DEG_TO_RAD_F (POOM_WII_PI_F / 180.0f)
#define POOM_WII_RAD_TO_DEG_F (180.0f / POOM_WII_PI_F)
#define POOM_WII_CLAMP_I8_MAX (127)
#define POOM_WII_CLAMP_I8_MIN (-127)
#define POOM_WII_MIN_DT_S (0.001f)
#define POOM_WII_MAX_DT_S (0.050f)
#define POOM_WII_BTN_LEFT_MASK (0x01U)
#define POOM_WII_BTN_RIGHT_MASK (0x02U)

#ifndef POOM_WII_AXIS_SWAP_XY
#define POOM_WII_AXIS_SWAP_XY 1
#endif

#ifndef POOM_WII_AXIS_INVERT_X
#define POOM_WII_AXIS_INVERT_X 0
#endif

#ifndef POOM_WII_AXIS_INVERT_Y
#define POOM_WII_AXIS_INVERT_Y 0
#endif

#ifndef POOM_WII_AXIS_INVERT_Z
#define POOM_WII_AXIS_INVERT_Z 0
#endif

#ifndef POOM_WII_GYRO_AXIS_X
#define POOM_WII_GYRO_AXIS_X 0
#endif

#ifndef POOM_WII_GYRO_AXIS_Y
#define POOM_WII_GYRO_AXIS_Y 1
#endif

#ifndef POOM_WII_GYRO_SIGN_X
#define POOM_WII_GYRO_SIGN_X (1.0f)
#endif

#ifndef POOM_WII_GYRO_SIGN_Y
#define POOM_WII_GYRO_SIGN_Y (-1.0f)
#endif

#ifndef POOM_WII_TILT_SIGN_X
#define POOM_WII_TILT_SIGN_X (1.0f)
#endif

#ifndef POOM_WII_TILT_SIGN_Y
#define POOM_WII_TILT_SIGN_Y (-1.0f)
#endif

static poom_wii_config_t s_cfg;
static bool s_initialized = false;
static bool s_enabled = false;
static bool s_hid_connected = false;
static bool s_calibrated = false;
static bool s_motion_enabled = true;
static TaskHandle_t s_poom_wii_task = NULL;
static poom_wii_connection_handler_t s_connection_handler = NULL;

static float s_gyro_bias_dps[3] = {0.0f, 0.0f, 0.0f};
static float s_roll_rad = 0.0f;
static float s_pitch_rad = 0.0f;
static float s_dx_filter = 0.0f;
static float s_dy_filter = 0.0f;
static float s_dx_residual = 0.0f;
static float s_dy_residual = 0.0f;
static uint8_t s_mouse_buttons = 0U;
static char s_active_key_ascii = '\0';
static bool s_active_key_valid = false;

/**
 * @brief Emit current BLE connection state to external handler.
 * @return esp_err_t
 */
static void poom_wii_emit_connection_state_(void)
{
    if (s_connection_handler != NULL)
    {
        s_connection_handler(s_hid_connected);
    }
}

/**
 * @brief Send mouse button report using zero movement.
 * @return esp_err_t
 */
static void poom_wii_send_buttons_report_(void)
{
    if (s_enabled && s_hid_connected)
    {
        poom_ble_hid_send_mouse_move(0, 0, s_mouse_buttons);
    }
}

/**
 * @brief Release all active keyboard keys and clear local key state.
 * @return esp_err_t
 */
static void poom_wii_release_all_keys_(void)
{
    poom_ble_hid_key_release_all();
    s_active_key_ascii = '\0';
    s_active_key_valid = false;
}

/**
 * @brief Clamp a float value into an inclusive range.
 * @param[in] v Input value.
 * @param[in] lo Lower bound.
 * @param[in] hi Upper bound.
 * @return esp_err_t
 */
static float poom_wii_clampf_(float v, float lo, float hi)
{
    if (v < lo)
    {
        return lo;
    }

    if (v > hi)
    {
        return hi;
    }

    return v;
}

/**
 * @brief Clamp integer to signed 8-bit mouse report limits.
 * @param[in] v Input value.
 * @return esp_err_t
 */
static int8_t poom_wii_clamp_i8_(int v)
{
    if (v > POOM_WII_CLAMP_I8_MAX)
    {
        return POOM_WII_CLAMP_I8_MAX;
    }

    if (v < POOM_WII_CLAMP_I8_MIN)
    {
        return POOM_WII_CLAMP_I8_MIN;
    }

    return (int8_t)v;
}

/**
 * @brief Remap IMU axes according to compile-time board orientation settings.
 * @param[in] in_x Input X value.
 * @param[in] in_y Input Y value.
 * @param[in] in_z Input Z value.
 * @param[out] out_x Output X value.
 * @param[out] out_y Output Y value.
 * @param[out] out_z Output Z value.
 * @return esp_err_t
 */
static void poom_wii_map_axes_(float in_x,
                                      float in_y,
                                      float in_z,
                                      float *out_x,
                                      float *out_y,
                                      float *out_z)
{
    float x = in_x;
    float y = in_y;
    float z = in_z;

#if POOM_WII_AXIS_SWAP_XY
    {
        float t = x;
        x = y;
        y = t;
    }
#endif

#if POOM_WII_AXIS_INVERT_X
    x = -x;
#endif

#if POOM_WII_AXIS_INVERT_Y
    y = -y;
#endif

#if POOM_WII_AXIS_INVERT_Z
    z = -z;
#endif

    if (out_x != NULL)
    {
        *out_x = x;
    }

    if (out_y != NULL)
    {
        *out_y = y;
    }

    if (out_z != NULL)
    {
        *out_z = z;
    }
}

/**
 * @brief Select one component from XYZ values using an index.
 * @param[in] x X value.
 * @param[in] y Y value.
 * @param[in] z Z value.
 * @param[in] axis Axis selector (0:x, 1:y, 2:z).
 * @return esp_err_t
 */
static float poom_wii_axis_pick_(float x, float y, float z, int axis)
{
    switch (axis)
    {
        case 0:
            return x;
        case 1:
            return y;
        default:
            return z;
    }
}

/**
 * @brief Normalize runtime configuration to safe defaults.
 * @param[in,out] cfg Configuration pointer.
 * @return esp_err_t
 */
static void poom_wii_normalize_config_(poom_wii_config_t *cfg)
{
    if (cfg == NULL)
    {
        return;
    }

    if ((cfg->complementary_alpha <= 0.0f) || (cfg->complementary_alpha >= 1.0f))
    {
        cfg->complementary_alpha = 0.98f;
    }

    if ((cfg->smooth_beta < 0.0f) || (cfg->smooth_beta >= 1.0f))
    {
        cfg->smooth_beta = 0.35f;
    }

    if (cfg->gyro_deadzone_dps < 0.0f)
    {
        cfg->gyro_deadzone_dps = 0.0f;
    }

    if (cfg->task_period_ms == 0U)
    {
        cfg->task_period_ms = 10U;
    }

    if (cfg->calibration_samples == 0U)
    {
        cfg->calibration_samples = 250U;
    }

    if (cfg->calibration_sample_delay_ms == 0U)
    {
        cfg->calibration_sample_delay_ms = 2U;
    }
}

/**
 * @brief Reset dynamic motion and filter state.
 * @return esp_err_t
 */
static void poom_wii_reset_motion_state_(void)
{
    s_roll_rad = 0.0f;
    s_pitch_rad = 0.0f;
    s_dx_filter = 0.0f;
    s_dy_filter = 0.0f;
    s_dx_residual = 0.0f;
    s_dy_residual = 0.0f;
}

/**
 * @brief Calibrate gyroscope bias from multiple IMU samples.
 * @return esp_err_t
 */
static bool poom_wii_calibrate_gyro_bias_(void)
{
    uint32_t collected = 0U;
    uint32_t attempts = 0U;
    uint32_t max_attempts = (uint32_t)s_cfg.calibration_samples * 20U;

    float sx = 0.0f;
    float sy = 0.0f;
    float sz = 0.0f;

    poom_imu_data_t sample;
    memset(&sample, 0, sizeof(sample));

    while ((collected < (uint32_t)s_cfg.calibration_samples) && (attempts < max_attempts))
    {
        attempts++;

        if (poom_imu_stream_read_data(&sample))
        {
            float gx = sample.angular_rate_mdps[0] / 1000.0f;
            float gy = sample.angular_rate_mdps[1] / 1000.0f;
            float gz = sample.angular_rate_mdps[2] / 1000.0f;

            poom_wii_map_axes_(gx, gy, gz, &gx, &gy, &gz);
            sx += gx;
            sy += gy;
            sz += gz;
            collected++;
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.calibration_sample_delay_ms));
    }

    if (collected == 0U)
    {
        POOM_WII_LOGE("Gyroscope calibration failed: no samples");
        return false;
    }

    s_gyro_bias_dps[0] = sx / (float)collected;
    s_gyro_bias_dps[1] = sy / (float)collected;
    s_gyro_bias_dps[2] = sz / (float)collected;

    POOM_WII_LOGI("Gyroscope bias calibrated: bx=%.3f by=%.3f bz=%.3f (%lu samples)",
                        s_gyro_bias_dps[0],
                        s_gyro_bias_dps[1],
                        s_gyro_bias_dps[2],
                        (unsigned long)collected);

    return true;
}

/**
 * @brief Process one air mouse motion update step.
 * @param[in] dt_s Delta time in seconds.
 * @return esp_err_t
 */
static void poom_wii_process_step_(float dt_s)
{
    poom_imu_data_t imu_data;

    if (!poom_imu_stream_read_data(&imu_data))
    {
        return;
    }

    if (!s_motion_enabled)
    {
        return;
    }


    float gx = imu_data.angular_rate_mdps[0] / 1000.0f;
    float gy = imu_data.angular_rate_mdps[1] / 1000.0f;
    float gz = imu_data.angular_rate_mdps[2] / 1000.0f;

    poom_wii_map_axes_(gx, gy, gz, &gx, &gy, &gz);

    gx -= s_gyro_bias_dps[0];
    gy -= s_gyro_bias_dps[1];
    gz -= s_gyro_bias_dps[2];

    if (fabsf(gx) < s_cfg.gyro_deadzone_dps)
    {
        gx = 0.0f;
    }

    if (fabsf(gy) < s_cfg.gyro_deadzone_dps)
    {
        gy = 0.0f;
    }

    if (fabsf(gz) < s_cfg.gyro_deadzone_dps)
    {
        gz = 0.0f;
    }

    float gyro_x = POOM_WII_GYRO_SIGN_X *
                   poom_wii_axis_pick_(gx, gy, gz, POOM_WII_GYRO_AXIS_X);
    float gyro_y = POOM_WII_GYRO_SIGN_Y *
                   poom_wii_axis_pick_(gx, gy, gz, POOM_WII_GYRO_AXIS_Y);

    float dx = gyro_x * s_cfg.gain_x * dt_s;
    float dy = gyro_y * s_cfg.gain_y * dt_s;

    s_dx_filter = (s_cfg.smooth_beta * s_dx_filter) +
                  ((1.0f - s_cfg.smooth_beta) * dx);
    s_dy_filter = (s_cfg.smooth_beta * s_dy_filter) +
                  ((1.0f - s_cfg.smooth_beta) * dy);

    s_dx_residual += s_dx_filter;
    s_dy_residual += s_dy_filter;

    int ix = (int)s_dx_residual;
    int iy = (int)s_dy_residual;

    int8_t mx = poom_wii_clamp_i8_(ix);
    int8_t my = poom_wii_clamp_i8_(iy);

    s_dx_residual -= (float)mx;
    s_dy_residual -= (float)my;

    if ((mx != 0) || (my != 0))
    {
        poom_ble_hid_send_mouse_move((int)mx, (int)my, s_mouse_buttons);
    }
}

/**
 * @brief FreeRTOS task loop for IMU processing and BLE mouse reporting.
 * @param[in] pv Unused task parameter.
 * @return esp_err_t
 */
static void poom_wii_task_(void *pv)
{
    TickType_t last_tick = xTaskGetTickCount();
    bool was_connected = false;

    (void)pv;

    while (s_enabled)
    {
        float dt_s;
        TickType_t now_tick;

        if (!s_hid_connected)
        {
            if (was_connected)
            {
                poom_wii_reset_motion_state_();
                last_tick = xTaskGetTickCount();
            }

            was_connected = false;
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        if (!was_connected)
        {
            last_tick = xTaskGetTickCount();
            was_connected = true;
        }

        now_tick = xTaskGetTickCount();
        dt_s = ((float)(now_tick - last_tick) * (float)portTICK_PERIOD_MS) / 1000.0f;
        dt_s = poom_wii_clampf_(dt_s, POOM_WII_MIN_DT_S, POOM_WII_MAX_DT_S);
        last_tick = now_tick;

        poom_wii_process_step_(dt_s);

        vTaskDelay(pdMS_TO_TICKS(s_cfg.task_period_ms));
    }

    s_poom_wii_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Create processing task when module is enabled and calibrated.
 * @return esp_err_t
 */
static void poom_wii_start_task_(void)
{
    if (!s_initialized || !s_enabled || !s_calibrated)
    {
        return;
    }

    if (s_poom_wii_task != NULL)
    {
        return;
    }

    if (xTaskCreate(poom_wii_task_,
                    "poom_wii",
                    3072,
                    NULL,
                    tskIDLE_PRIORITY + 2,
                    &s_poom_wii_task) != pdPASS)
    {
        s_poom_wii_task = NULL;
        POOM_WII_LOGE("Failed to create air mouse task");
        return;
    }

    POOM_WII_LOGI("Air mouse task started");
}

/**
 * @brief Delete processing task and clear motion state.
 * @return esp_err_t
 */
static void poom_wii_stop_task_(void)
{
    if (s_poom_wii_task != NULL)
    {
        vTaskDelete(s_poom_wii_task);
        s_poom_wii_task = NULL;
        POOM_WII_LOGI("Air mouse task stopped");
    }

    poom_wii_reset_motion_state_();
}

/**
 * @brief Handle BLE HID connection changes from the HID module.
 * @param[in] connected Current BLE link state.
 * @return esp_err_t
 */
static void poom_wii_hid_connection_cb_(bool connected)
{
    s_hid_connected = connected;

    if (connected && s_enabled)
    {
        poom_wii_start_task_();
        poom_wii_send_buttons_report_();
    }
    else if (!connected)
    {
        s_mouse_buttons = 0U;
        poom_wii_release_all_keys_();
    }

    poom_wii_emit_connection_state_();
}

/**
 * @brief Fill configuration with default values.
 * @param[out] out_cfg Pointer to output configuration structure.
 * @return esp_err_t
 */
void poom_wii_get_default_config(poom_wii_config_t *out_cfg)
{
    if (out_cfg == NULL)
    {
        return;
    }

    out_cfg->complementary_alpha = 0.98f;
    out_cfg->smooth_beta = 0.35f;
    out_cfg->gyro_deadzone_dps = 2.0f;
    out_cfg->gain_x = 10.0f;
    out_cfg->gain_y = 10.0f;
    out_cfg->tilt_gain_x = 0.0f;
    out_cfg->tilt_gain_y = 0.0f;
    out_cfg->task_period_ms = 10U;
    out_cfg->calibration_samples = 250U;
    out_cfg->calibration_sample_delay_ms = 2U;
}

/**
 * @brief Initialize the air mouse module with custom configuration.
 * @param[in] config Pointer to input configuration structure.
 * @return esp_err_t
 */
uint8_t poom_wii_init_with_config(const poom_wii_config_t *config)
{
    if (config == NULL)
    {
        POOM_WII_LOGE("Initialization config is NULL");
        return 1U;
    }

    if (s_initialized)
    {
        poom_wii_stop();
    }

    s_cfg = *config;
    poom_wii_normalize_config_(&s_cfg);

    poom_imu_stream_init();
    s_calibrated = poom_wii_calibrate_gyro_bias_();
    if (!s_calibrated)
    {
        return 2U;
    }

    poom_wii_reset_motion_state_();

    s_enabled = true;
    s_motion_enabled = true;
    s_initialized = true;

    poom_ble_hid_set_connection_callback(poom_wii_hid_connection_cb_);
    poom_ble_hid_start();

    POOM_WII_LOGI("Air mouse initialized");
    return 0U;
}

/**
 * @brief Initialize the air mouse module with default configuration.
 * @return esp_err_t
 */
uint8_t poom_wii_init(void)
{
    poom_wii_config_t cfg;

    poom_wii_get_default_config(&cfg);
    return poom_wii_init_with_config(&cfg);
}

/**
 * @brief Enable and run air mouse processing.
 * @return esp_err_t
 */
void poom_wii_start(void)
{
    if (!s_initialized)
    {
        POOM_WII_LOGW("poom_wii_start called before initialization");
        return;
    }

    s_enabled = true;
    poom_ble_hid_set_connection_callback(poom_wii_hid_connection_cb_);
    poom_ble_hid_start();
    poom_wii_start_task_();
}

/**
 * @brief Disable and stop air mouse processing.
 * @return esp_err_t
 */
void poom_wii_stop(void)
{
    s_mouse_buttons = 0U;
    poom_wii_send_buttons_report_();
    poom_wii_release_all_keys_();

    s_enabled = false;
    poom_wii_stop_task_();
    poom_ble_hid_stop();
    s_hid_connected = false;
    poom_wii_emit_connection_state_();
}

/**
 * @brief Press and hold the left mouse button.
 * @return esp_err_t
 */
void poom_wii_left_button_press(void)
{
    s_mouse_buttons |= POOM_WII_BTN_LEFT_MASK;
    poom_wii_send_buttons_report_();
}

/**
 * @brief Release the left mouse button.
 * @return esp_err_t
 */
void poom_wii_left_button_release(void)
{
    s_mouse_buttons = (uint8_t)(s_mouse_buttons & (uint8_t)(~POOM_WII_BTN_LEFT_MASK));
    poom_wii_send_buttons_report_();
}

/**
 * @brief Press and hold the right mouse button.
 * @return esp_err_t
 */
void poom_wii_right_button_press(void)
{
    s_mouse_buttons |= POOM_WII_BTN_RIGHT_MASK;
    poom_wii_send_buttons_report_();
}

/**
 * @brief Release the right mouse button.
 * @return esp_err_t
 */
void poom_wii_right_button_release(void)
{
    s_mouse_buttons = (uint8_t)(s_mouse_buttons & (uint8_t)(~POOM_WII_BTN_RIGHT_MASK));
    poom_wii_send_buttons_report_();
}

/**
 * @brief Press and hold an ASCII keyboard key.
 * @param[in] key ASCII key to press.
 * @return esp_err_t
 */
void poom_wii_key_press_ascii(char key)
{
    if (!s_enabled || !s_hid_connected)
    {
        return;
    }

    if (s_active_key_valid && (s_active_key_ascii == key))
    {
        return;
    }

    if (s_active_key_valid)
    {
        poom_wii_release_all_keys_();
    }

    poom_ble_hid_key_press_ascii(key);
    s_active_key_ascii = key;
    s_active_key_valid = true;
}

/**
 * @brief Release a specific ASCII keyboard key if currently active.
 * @param[in] key ASCII key to release.
 * @return esp_err_t
 */
void poom_wii_key_release_ascii(char key)
{
    if (!s_active_key_valid)
    {
        return;
    }

    if (s_active_key_ascii != key)
    {
        return;
    }

    poom_wii_release_all_keys_();
}

/**
 * @brief Release all currently active keyboard keys.
 * @return esp_err_t
 */
void poom_wii_key_release_all(void)
{
    poom_wii_release_all_keys_();
}

/**
 * @brief Enable or disable IMU-driven mouse movement while keeping BLE active.
 * @param[in] enabled True to send movement, false to pause movement reports.
 * @return esp_err_t
 */
void poom_wii_set_motion_enabled(bool enabled)
{
    if (s_motion_enabled == enabled)
    {
        return;
    }

    s_motion_enabled = enabled;
    poom_wii_reset_motion_state_();
}

/**
 * @brief Toggle IMU-driven mouse movement on/off while keeping BLE active.
 * @return esp_err_t
 */
void poom_wii_toggle_motion_enabled(void)
{
    poom_wii_set_motion_enabled(!s_motion_enabled);
}

/**
 * @brief Returns current IMU-driven mouse movement state.
 * @return esp_err_t
 */
bool poom_wii_is_motion_enabled(void)
{
    return s_motion_enabled;
}

/**
 * @brief Check whether air mouse is currently running.
 * @return esp_err_t
 */
bool poom_wii_is_running(void)
{
    return (s_enabled && s_hid_connected && (s_poom_wii_task != NULL));
}

/**
 * @brief Check whether BLE HID link is connected.
 * @return esp_err_t
 */
bool poom_wii_is_connected(void)
{
    return s_hid_connected;
}

/**
 * @brief Register a BLE connection state handler.
 * @param[in] handler Callback pointer, NULL to clear.
 * @return esp_err_t
 */
void poom_wii_set_connection_handler(poom_wii_connection_handler_t handler)
{
    s_connection_handler = handler;
    poom_wii_emit_connection_state_();
}
