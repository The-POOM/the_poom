// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "poom_motion_midi.h"
#include "ble_midi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motion_engine.h"
#include "poom_imu_stream.h"
#include "poom_sbus.h"
#include <string.h>

typedef enum
{
    CMD_CONFIG,
    CMD_PAUSE,
    CMD_GATE_ON,
    CMD_GATE_OFF,
    CMD_TOGGLE,
    CMD_CALIBRATE
} command_kind_t;
typedef struct
{
    command_kind_t kind;
    poom_midi_config_t config;
} command_t;
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_done;
static bool s_started, s_stop, s_emergency_pause, s_config_set;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static poom_midi_config_t s_config;
static poom_midi_status_t s_status;

static uint32_t now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void send_(void* user, uint8_t status, uint8_t note, uint8_t velocity)
{
    (void)user;
    uint8_t message[] = {status, note, velocity};
    (void)blemidi_send_message(0, message, sizeof(message));
}

void poom_motion_midi_get_config(poom_midi_config_t* out)
{
    if(!out)
        return;
    portENTER_CRITICAL(&s_lock);
    *out = s_config_set ? s_config : motion_default_config();
    portEXIT_CRITICAL(&s_lock);
}

void poom_motion_midi_get_status(poom_midi_status_t* out)
{
    if(!out)
        return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static void publish_status_(motion_engine_t* engine)
{
    portENTER_CRITICAL(&s_lock);
    s_status     = engine->status;
    s_config     = engine->config;
    s_config_set = true;
    portEXIT_CRITICAL(&s_lock);
    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MIDI_STATUS_TOPIC, &token, sizeof(token), 0);
}

static void worker_(void* arg)
{
    (void)arg;
    motion_engine_t engine;
    poom_midi_config_t config;
    poom_motion_midi_get_config(&config);
    motion_engine_init(&engine, &config, send_, NULL);
    bool ble_ok      = blemidi_init(NULL) >= 0;
    bool imu_ok      = poom_imu_stream_init_gestures();
    bool acc_pending = false, gyro_pending = false;
    uint32_t last_publish   = 0;
    uint32_t initialized_ms = now_ms_();
    for(;;)
    {
        bool stop, pause;
        portENTER_CRITICAL(&s_lock);
        stop              = s_stop;
        pause             = s_emergency_pause;
        s_emergency_pause = false;
        portEXIT_CRITICAL(&s_lock);
        if(stop)
            break;
        if(pause)
        {
            xQueueReset(s_commands);
            motion_engine_pause(&engine);
        }
        command_t command;
        while(xQueueReceive(s_commands, &command, 0) == pdTRUE)
        {
            switch(command.kind)
            {
                case CMD_CONFIG:
                    motion_engine_configure(&engine, &command.config);
                    break;
                case CMD_PAUSE:
                    motion_engine_pause(&engine);
                    break;
                case CMD_GATE_ON:
                    motion_engine_gate(&engine, true);
                    break;
                case CMD_GATE_OFF:
                    motion_engine_gate(&engine, false);
                    break;
                case CMD_TOGGLE:
                    motion_engine_toggle_drums(&engine);
                    break;
                case CMD_CALIBRATE:
                    motion_engine_calibrate(&engine);
                    if(!imu_ok || engine.status.error)
                        imu_ok = poom_imu_stream_init_gestures();
                    if(!ble_ok)
                        ble_ok = blemidi_init(NULL) >= 0;
                    acc_pending = gyro_pending = false;
                    initialized_ms             = now_ms_();
                    break;
            }
        }
        poom_imu_data_t raw = {0};
        motion_sample_t sample;
        const motion_sample_t* fresh = NULL;
        if(imu_ok && poom_imu_stream_read_data(&raw))
        {
            acc_pending |= raw.acceleration_fresh;
            gyro_pending |= raw.angular_rate_fresh;
            if(acc_pending && gyro_pending)
            {
                for(int i = 0; i < 3; ++i)
                {
                    sample.acc[i]  = raw.acceleration_mg[i] / 1000.0f;
                    sample.gyro[i] = raw.angular_rate_mdps[i] / 1000.0f;
                }
                fresh       = &sample;
                acc_pending = gyro_pending = false;
            }
        }
        const uint32_t now = now_ms_();
        motion_engine_step(&engine, fresh, now,
                           ble_ok && blemidi_is_connected());
        if(!ble_ok || !imu_ok ||
           (!engine.have_sample && now - initialized_ms > 1000U))
        {
            motion_engine_pause(&engine);
            engine.status.error = true;
        }
        /* Only the SBUS dispatcher draws; sensor processing never touches OLED.
         */
        if(now - last_publish >= 100U)
        {
            publish_status_(&engine);
            last_publish = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    motion_engine_pause(&engine);
    if(ble_ok && blemidi_is_connected())
    {
        send_(NULL, 0xB0, 123, 0);
        send_(NULL, 0xB9, 123, 0);
        /* Allow the existing transport's periodic flush to send final
         * note-offs. */
        vTaskDelay(pdMS_TO_TICKS(BLEMIDI_OUTBUFFER_FLUSH_MS + 10U));
    }
    if(ble_ok)
        blemidi_deinit();
    engine.status.connected = false;
    publish_status_(&engine);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

bool poom_motion_midi_start(void)
{
    if(s_started)
        return true;
    s_commands = xQueueCreate(16, sizeof(command_t));
    s_done     = xSemaphoreCreateBinary();
    if(!s_commands || !s_done)
    {
        if(s_commands)
            vQueueDelete(s_commands);
        if(s_done)
            vSemaphoreDelete(s_done);
        s_commands = NULL;
        s_done     = NULL;
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_stop = s_emergency_pause = false;
    portEXIT_CRITICAL(&s_lock);
    if(xTaskCreate(worker_, "motion_midi", 4096, NULL, tskIDLE_PRIORITY + 2,
                   NULL) != pdPASS)
    {
        vQueueDelete(s_commands);
        vSemaphoreDelete(s_done);
        s_commands = NULL;
        s_done     = NULL;
        return false;
    }
    s_started = true;
    return true;
}

void poom_motion_midi_stop(void)
{
    if(!s_started)
        return;
    portENTER_CRITICAL(&s_lock);
    s_stop = true;
    portEXIT_CRITICAL(&s_lock);
    /* Join before deleting commands or stopping BLE; no task can emit a late
     * note. */
    xSemaphoreTake(s_done, portMAX_DELAY);
    vQueueDelete(s_commands);
    vSemaphoreDelete(s_done);
    s_commands = NULL;
    s_done     = NULL;
    s_started  = false;
}

static void command_(command_kind_t kind, const poom_midi_config_t* config)
{
    if(!s_started)
        return;
    command_t command = {.kind = kind};
    if(config)
        command.config = *config;
    if(xQueueSend(s_commands, &command, pdMS_TO_TICKS(20U)) != pdTRUE)
    {
        /* A lost release must never leave a note held. */
        portENTER_CRITICAL(&s_lock);
        s_emergency_pause = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

void poom_motion_midi_set_config(const poom_midi_config_t* config)
{
    if(config)
        command_(CMD_CONFIG, config);
}

void poom_motion_midi_pause(void)
{
    command_(CMD_PAUSE, NULL);
}

void poom_motion_midi_gate(bool pressed)
{
    command_(pressed ? CMD_GATE_ON : CMD_GATE_OFF, NULL);
}

void poom_motion_midi_toggle_drums(void)
{
    command_(CMD_TOGGLE, NULL);
}

void poom_motion_midi_calibrate(void)
{
    command_(CMD_CALIBRATE, NULL);
}
