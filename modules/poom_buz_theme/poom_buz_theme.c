// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buzzer.h"
#include "poom_buz_theme.h"

#ifndef POOM_BUZ_THEME_ENABLE_LOG
#define POOM_BUZ_THEME_ENABLE_LOG 0
#endif

#ifndef POOM_BUZ_THEME_DEBUG_LOG_ENABLED
#define POOM_BUZ_THEME_DEBUG_LOG_ENABLED 0
#endif

#if POOM_BUZ_THEME_ENABLE_LOG
    static const char *POOM_BUZ_THEME_TAG = "poom_buz_theme";

    #define POOM_BUZ_THEME_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_BUZ_THEME_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_BUZ_THEME_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_BUZ_THEME_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_BUZ_THEME_DEBUG_LOG_ENABLED
        #define POOM_BUZ_THEME_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_BUZ_THEME_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_BUZ_THEME_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_BUZ_THEME_PRINTF_E(...) do { } while (0)
    #define POOM_BUZ_THEME_PRINTF_I(...) do { } while (0)
    #define POOM_BUZ_THEME_PRINTF_D(...) do { } while (0)
#endif

#define NOTE_B0  31
#define NOTE_C1  33
#define NOTE_CS1 35
#define NOTE_D1  37
#define NOTE_DS1 39
#define NOTE_E1  41
#define NOTE_F1  44
#define NOTE_FS1 46
#define NOTE_G1  49
#define NOTE_GS1 52
#define NOTE_A1  55
#define NOTE_AS1 58
#define NOTE_B1  62
#define NOTE_C2  65
#define NOTE_CS2 69
#define NOTE_D2  73
#define NOTE_DS2 78
#define NOTE_E2  82
#define NOTE_F2  87
#define NOTE_FS2 93
#define NOTE_G2  98
#define NOTE_GS2 104
#define NOTE_A2  110
#define NOTE_AS2 117
#define NOTE_B2  123
#define NOTE_C3  131
#define NOTE_CS3 139
#define NOTE_D3  147
#define NOTE_DS3 156
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_FS3 185
#define NOTE_G3  196
#define NOTE_GS3 208
#define NOTE_A3  220
#define NOTE_AS3 233
#define NOTE_B3  247
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_CS6 1109
#define NOTE_D6  1175
#define NOTE_DS6 1245
#define NOTE_E6  1319
#define NOTE_F6  1397
#define NOTE_FS6 1480
#define NOTE_G6  1568
#define NOTE_GS6 1661
#define NOTE_A6  1760
#define NOTE_AS6 1865
#define NOTE_B6  1976
#define NOTE_C7  2093
#define NOTE_CS7 2217
#define NOTE_D7  2349
#define NOTE_DS7 2489
#define NOTE_E7  2637
#define NOTE_F7  2794
#define NOTE_FS7 2960
#define NOTE_G7  3136
#define NOTE_GS7 3322
#define NOTE_A7  3520
#define NOTE_AS7 3729
#define NOTE_B7  3951
#define NOTE_C8  4186

static TaskHandle_t s_poom_buz_theme_melody_task = NULL;

static void poom_buz_theme_play_sequence_(const uint32_t *melody,
                                          const uint32_t *durations,
                                          int count,
                                          uint32_t pause_ms)
{
    int index = 0;
    for(index = 0; index < count; index++) {
        if(melody[index] != 0U) {
            buzzer_tone(melody[index], durations[index]);
        } else {
            vTaskDelay(pdMS_TO_TICKS(durations[index]));
        }
        if(pause_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(pause_ms));
        }
    }
}

void poom_buz_theme_mario(void)
{
    static const uint32_t melody[] = {
        NOTE_E7, NOTE_E7, 0, NOTE_E7, 0, NOTE_C7, NOTE_E7, 0,
        NOTE_G7, 0, 0, 0, NOTE_G6, 0, 0, 0,
        NOTE_C7, 0, 0, NOTE_G6, 0, 0, NOTE_E6, 0,
        0, NOTE_A6, 0, NOTE_B6, 0, NOTE_AS6, NOTE_A6, 0,
        NOTE_G6, NOTE_E7, NOTE_G7, NOTE_A7, 0, NOTE_F7, NOTE_G7, 0,
        NOTE_E7, 0, NOTE_C7, NOTE_D7, NOTE_B6, 0, 0,
        NOTE_C7, 0, 0, NOTE_G6, 0, 0, NOTE_E6, 0,
        0, NOTE_A6, 0, NOTE_B6, 0, NOTE_AS6, NOTE_A6, 0,
        NOTE_G6, NOTE_E7, NOTE_G7, NOTE_A7, 0, NOTE_F7, NOTE_G7, 0,
        NOTE_E7, 0, NOTE_C7, NOTE_D7, NOTE_B6, 0, 0
    };

    static const uint32_t durations[] = {
        125, 125, 125, 125, 125, 125, 125, 125,
        250, 125, 125, 125, 250, 125, 125, 125,
        250, 125, 125, 250, 125, 125, 250, 125,
        125, 250, 125, 250, 125, 125, 250, 125,
        250, 250, 250, 125, 250, 250, 125, 250,
        125, 250, 250, 250, 250, 125, 125,
        250, 125, 125, 250, 125, 125, 250, 125,
        125, 250, 125, 250, 125, 125, 250, 125,
        250, 250, 250, 125, 250, 250, 125, 250,
        125, 250, 250, 250, 250, 125, 125
    };

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  30U);
}

void poom_buz_theme_zelda_treasure(void)
{
    static const uint32_t melody[] = {NOTE_B4, NOTE_E5, NOTE_G5, NOTE_B5, NOTE_E6};
    static const uint32_t durations[] = {150, 150, 150, 150, 300};
    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  50U);
}

void poom_buz_theme_tetris(void)
{
    static const uint32_t melody[] = {
        NOTE_E5, NOTE_B4, NOTE_C5, NOTE_D5, NOTE_C5, NOTE_B4, NOTE_A4,
        NOTE_A4, NOTE_C5, NOTE_E5, NOTE_D5, NOTE_C5, NOTE_B4, NOTE_C5
    };
    static const uint32_t durations[] = {
        150, 150, 150, 150, 150, 150, 300, 150, 150, 150, 150, 150, 150, 300
    };

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  30U);
}

void poom_buz_theme_pacman_intro(void)
{
    static const uint32_t melody[] = {
        NOTE_B4, NOTE_B5, NOTE_FS5, NOTE_DS5, NOTE_B5, NOTE_FS5, NOTE_DS5, NOTE_C5,
        NOTE_C6, NOTE_G6, NOTE_E6, NOTE_C6, NOTE_G6, NOTE_E6
    };
    static const uint32_t durations[] = {
        125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125
    };

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  50U);
}

void poom_buz_theme_gameboy_startup(void)
{
    static const uint32_t melody[] = {NOTE_G4, NOTE_C5, NOTE_E5, NOTE_G5};
    static const uint32_t durations[] = {250, 250, 250, 500};
    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  50U);
}

void poom_buz_theme_sonic_ring(void)
{
    static const uint32_t melody[] = {NOTE_E6, NOTE_G6, NOTE_E7};
    static const uint32_t durations[] = {100, 100, 200};
    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  30U);
}

void poom_buz_theme_megaman_jump(void)
{
    static const uint32_t melody[] = {NOTE_E5, NOTE_G5, NOTE_C6};
    static const uint32_t durations[] = {100, 100, 150};
    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  20U);
}

void poom_buz_theme_snake(void)
{
    static const uint32_t melody[] = {
        NOTE_E5, NOTE_G5, NOTE_E5, NOTE_D5, NOTE_C5, 0,
        NOTE_E5, NOTE_G5, NOTE_A5, 0,
        NOTE_A4, NOTE_C5, NOTE_E5, NOTE_G5, NOTE_E5, NOTE_C5, NOTE_A4, 0,
        NOTE_B4, NOTE_D5, NOTE_FS5, NOTE_A5, NOTE_FS5, NOTE_D5, NOTE_B4, 0,
        NOTE_C5, NOTE_E5, NOTE_G5, NOTE_B5, NOTE_A5, NOTE_G5, NOTE_E5, 0,
        NOTE_D5, NOTE_F5, NOTE_A5, NOTE_C6, NOTE_B5, NOTE_A5, NOTE_F5, 0,
        NOTE_E5, NOTE_D5, NOTE_C5, NOTE_B4, NOTE_A4, 0
    };

    static const uint32_t durations[] = {
        90, 90, 90, 90, 140, 120,
        90, 90, 160, 140,
        90, 90, 90, 120, 90, 90, 140, 80,
        90, 90, 90, 120, 90, 90, 140, 80,
        90, 90, 90, 120, 90, 90, 160, 80,
        90, 90, 90, 120, 90, 90, 160, 80,
        90, 90, 100, 100, 180, 160
    };

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  25U);
}

void poom_buz_theme_snake_eat_fx(void)
{
    static const uint32_t melody[] = {NOTE_C6, NOTE_E6, NOTE_G6, 0};
    static const uint32_t durations[] = {35, 35, 55, 20};

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  10U);
}

void poom_buz_theme_snake_gameover_fx(void)
{
    static const uint32_t melody[] = {
        NOTE_E5, NOTE_DS5, NOTE_D5, NOTE_CS5, NOTE_C5,
        NOTE_B4, NOTE_AS4, NOTE_A4, NOTE_GS4, NOTE_G4,
        NOTE_FS4, NOTE_F4, NOTE_E4, 0, NOTE_E3
    };
    static const uint32_t durations[] = {
        60, 60, 60, 60, 70,
        70, 70, 80, 80, 90,
        90, 100, 140, 120, 220
    };

    poom_buz_theme_play_sequence_(melody,
                                  durations,
                                  (int)(sizeof(melody) / sizeof(melody[0])),
                                  15U);
}

static void poom_buz_theme_kill_current_melody_(void)
{
    if(s_poom_buz_theme_melody_task != NULL) {
        vTaskDelete(s_poom_buz_theme_melody_task);
        s_poom_buz_theme_melody_task = NULL;
    }

    /* Ensure PWM output is silenced even if the melody task was killed mid-tone. */
    buzzer_tone(0U, 0U);
}

void poom_buz_theme_stop(void)
{
    poom_buz_theme_kill_current_melody_();
}

static void poom_buz_theme_melody_task_(void *param)
{
    poom_buz_theme_melody_id_t id = (poom_buz_theme_melody_id_t)(uintptr_t)param;

    switch(id) {
        case POOM_BUZ_THEME_MELODY_MARIO:
            poom_buz_theme_mario();
            break;
        case POOM_BUZ_THEME_MELODY_ZELDA:
            poom_buz_theme_zelda_treasure();
            break;
        case POOM_BUZ_THEME_MELODY_TETRIS:
            poom_buz_theme_tetris();
            break;
        case POOM_BUZ_THEME_MELODY_PACMAN:
            poom_buz_theme_pacman_intro();
            break;
        case POOM_BUZ_THEME_MELODY_GAMEBOY:
            poom_buz_theme_gameboy_startup();
            break;
        case POOM_BUZ_THEME_MELODY_SONIC:
            poom_buz_theme_sonic_ring();
            break;
        case POOM_BUZ_THEME_MELODY_MEGAMAN:
            poom_buz_theme_megaman_jump();
            break;
        default:
            POOM_BUZ_THEME_PRINTF_E("invalid melody id: %d", (int)id);
            break;
    }

    s_poom_buz_theme_melody_task = NULL;
    vTaskDelete(NULL);
}

void poom_buz_theme_init_melody(poom_buz_theme_melody_id_t id)
{
    poom_buz_theme_kill_current_melody_();
    if(xTaskCreate(poom_buz_theme_melody_task_,
                   "poom_buz_theme",
                   2048,
                   (void *)(uintptr_t)id,
                   5,
                   &s_poom_buz_theme_melody_task) != pdPASS) {
        s_poom_buz_theme_melody_task = NULL;
        POOM_BUZ_THEME_PRINTF_E("failed to create melody task");
    }
}
