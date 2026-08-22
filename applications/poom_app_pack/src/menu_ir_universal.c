// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_ir_universal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Arduboy2.h"
#include "bsp_pong.h"
#include "button_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_events.h"
#include "ir_dec.h"
#include "ir_rcv.h"
#include "ir_tx.h"
#include "nvs.h"
#include "poom_led_rainbow.h"
#include "poom_sd_browser.h"
#include "poom_secrets_store.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_IR_UNIV_TAG "menu_ir_univ"

#ifndef MENU_IR_UNIV_ENABLE_LOG
#define MENU_IR_UNIV_ENABLE_LOG (1)
#endif

#if MENU_IR_UNIV_ENABLE_LOG
#define MENU_IR_UNIV_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", MENU_IR_UNIV_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define MENU_IR_UNIV_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", MENU_IR_UNIV_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define MENU_IR_UNIV_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", MENU_IR_UNIV_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define MENU_IR_UNIV_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", MENU_IR_UNIV_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
#define MENU_IR_UNIV_PRINTF_E(...)
#define MENU_IR_UNIV_PRINTF_W(...)
#define MENU_IR_UNIV_PRINTF_I(...)
#define MENU_IR_UNIV_PRINTF_D(...)
#endif

#define MENU_IR_UNIV_TASK_STACK_WORDS (4096U)
#define MENU_IR_UNIV_TASK_PRIORITY (4U)
#define MENU_IR_UNIV_BUTTON_QUEUE_DEPTH (8U)
#define MENU_IR_UNIV_STATUS_HOLD_MS (900U)
#define MENU_IR_UNIV_ERROR_HOLD_MS (1300U)
#define MENU_IR_UNIV_RCV_TIMEOUT_MS (200U)
#define MENU_IR_UNIV_RX_BUFFER_SYMBOLS (256U)
#define MENU_IR_UNIV_RX_DUMP_SYMBOLS_MAX (80U)

#define MENU_IR_UNIV_HEADER_H (11)
#define MENU_IR_UNIV_BOX_Y (12)
#define MENU_IR_UNIV_BOX_H (40)
#define MENU_IR_UNIV_TEXT_X (6)
#define MENU_IR_UNIV_ROW0_Y (16)
#define MENU_IR_UNIV_ROW_STEP (10)

#define MENU_IR_UNIV_NEC_CLK_HZ (1000000U)
#define MENU_IR_UNIV_NEC_CARRIER_HZ (38000U)
#define MENU_IR_UNIV_NEC_DUTY_CYCLE (0.33f)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#ifndef BUTTON_PRESS_DOWN
#define BUTTON_PRESS_DOWN (0U)
#endif

#ifndef BUTTON_PRESS_UP
#define BUTTON_PRESS_UP (1U)
#endif

typedef enum
{
    MENU_IR_UNIV_SCREEN_MAIN = 0,
    MENU_IR_UNIV_SCREEN_LEARN_WAIT_IR,
    MENU_IR_UNIV_SCREEN_LEARN_SELECT_BUTTON,
    MENU_IR_UNIV_SCREEN_EMULATOR,
    MENU_IR_UNIV_SCREEN_SD_COMMANDS,
} menu_ir_univ_screen_t;

typedef enum
{
    MENU_IR_UNIV_PROTO_NONE = IR_PROTOCOL_NONE,
    MENU_IR_UNIV_PROTO_NEC = IR_PROTOCOL_NEC,
    MENU_IR_UNIV_PROTO_NEC_EXT = IR_PROTOCOL_NEC_EXT,
    MENU_IR_UNIV_PROTO_SAMSUNG32 = IR_PROTOCOL_SAMSUNG32,
    MENU_IR_UNIV_PROTO_SIRC = IR_PROTOCOL_SIRC,
    MENU_IR_UNIV_PROTO_SIRC15 = IR_PROTOCOL_SIRC15,
    MENU_IR_UNIV_PROTO_SIRC20 = IR_PROTOCOL_SIRC20,
    MENU_IR_UNIV_PROTO_RC5 = IR_PROTOCOL_RC5,
    MENU_IR_UNIV_PROTO_RC5X = IR_PROTOCOL_RC5X,
    MENU_IR_UNIV_PROTO_RC6 = IR_PROTOCOL_RC6,
    MENU_IR_UNIV_PROTO_RCA = IR_PROTOCOL_RCA,
    MENU_IR_UNIV_PROTO_PIONEER = IR_PROTOCOL_PIONEER,
    MENU_IR_UNIV_PROTO_KASEIKYO = IR_PROTOCOL_KASEIKYO,
    MENU_IR_UNIV_PROTO_NEC42 = IR_PROTOCOL_NEC42,
    MENU_IR_UNIV_PROTO_NEC42_EXT = IR_PROTOCOL_NEC42_EXT,
} menu_ir_univ_proto_t;

typedef struct
{
    uint8_t protocol;
    uint8_t flags;
    uint16_t reserved;
    uint32_t address;
    uint32_t command;
} menu_ir_univ_code_t;

typedef struct
{
    uint8_t protocol;
    uint16_t address;
    uint8_t command;
    uint8_t reserved;
} menu_ir_univ_code_legacy_t;

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_ir_univ_button_msg_t;

typedef struct
{
    menu_ir_univ_screen_t screen;
    uint8_t main_selected;
    uint8_t assign_selected;
    uint8_t emulator_mapped_count;
    uint8_t sd_selected;

    bool left_down;
    bool right_down;

    menu_ir_univ_code_t pending_code;
} menu_ir_univ_state_t;

static TaskHandle_t s_menu_ir_univ_task = NULL;
static QueueHandle_t s_menu_ir_univ_button_queue = NULL;
static bool s_menu_ir_univ_buttons_subscribed = false;
static char s_menu_ir_univ_sbus_user[] = "menu_ir_univ";
static bool s_menu_ir_univ_restore_rainbow = false;
static bool s_menu_ir_univ_led_owned = false;

#define MENU_IR_UNIV_SD_PATH_MAX (256U)
#define MENU_IR_UNIV_SD_MAX_COMMANDS (48U)
#define MENU_IR_UNIV_SD_NAME_MAX (20U)
#define MENU_IR_UNIV_SD_TITLE_MAX (21U)

typedef struct
{
    char name[MENU_IR_UNIV_SD_NAME_MAX + 1U];
    menu_ir_univ_proto_t proto;
    uint32_t address;
    uint32_t command;
} menu_ir_univ_sd_cmd_t;

static char s_menu_ir_univ_sd_selected_path[MENU_IR_UNIV_SD_PATH_MAX] = {0};
static bool s_menu_ir_univ_sd_path_pending = false;
static bool s_menu_ir_univ_sd_bad_ext_pending = false;
static char s_menu_ir_univ_sd_start_dir[MENU_IR_UNIV_SD_PATH_MAX] = "/sdcard";
static char s_menu_ir_univ_sd_title[MENU_IR_UNIV_SD_TITLE_MAX + 1U] = {0};
static menu_ir_univ_sd_cmd_t* s_menu_ir_univ_sd_cmds = NULL;
static uint8_t s_menu_ir_univ_sd_cmd_count = 0U;

static void menu_ir_univ_draw_sd_commands_(const menu_ir_univ_state_t* s);
static bool menu_ir_univ_sd_load_file_(const char* abs_path);
static void menu_ir_univ_sd_exit_cb_(void* user_ctx);
static void menu_ir_univ_sd_file_selected_cb_(const char* abs_path, void* user_ctx);
static void menu_ir_univ_launch_sd_browser_and_handover_(void);
static void menu_ir_univ_sd_set_title_from_text_(const char* text);
static void menu_ir_univ_sd_set_title_from_comment_(const char* line);
static void menu_ir_univ_sd_set_title_from_path_(const char* abs_path);
static void menu_ir_univ_draw_sd_header_(void);

static void menu_ir_univ_draw_main_(uint8_t selected);
static void menu_ir_univ_draw_status_(const char* line1, const char* line2, const char* footer);
static void menu_ir_univ_draw_assign_list_(uint8_t selected);
static void menu_ir_univ_draw_emulator_(uint8_t mapped_count);
static void menu_ir_univ_exit_(void);
static void menu_ir_univ_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_ir_univ_task_(void* arg);
static bool menu_ir_univ_pop_button_msg_(menu_ir_univ_button_msg_t* out_msg, uint32_t timeout_ms);
static bool menu_ir_univ_is_exit_chord_active_(const menu_ir_univ_state_t* s);

static void menu_ir_univ_handover_exit_to_sd_browser_(void);

static const uint8_t s_menu_ir_univ_assign_buttons[] = {
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_A,
    BUTTON_B,
};

/**
 * @brief Returns the display label for the current state.
 *
 * @param[in] idx Parameter passed to the helper.
 * @return const char*
 */
static const char* menu_ir_univ_assign_label_(uint8_t idx)
{
    switch(idx)
    {
        case 0U: return "UP";
        case 1U: return "DOWN";
        case 2U: return "LEFT";
        case 3U: return "RIGHT";
        case 4U: return "A";
        case 5U: return "B";
        default: return "?";
    }
}

/**
 * @brief Internal helper for `menu_ir_univ_button_key`.
 *
 * @param[in] button Parameter passed to the helper.
 * @return const char*
 */
static const char* menu_ir_univ_button_key_(uint8_t button)
{
    switch(button)
    {
        case BUTTON_A: return "ir_a";
        case BUTTON_B: return "ir_b";
        case BUTTON_LEFT: return "ir_l";
        case BUTTON_RIGHT: return "ir_r";
        case BUTTON_UP: return "ir_u";
        case BUTTON_DOWN: return "ir_d";
        default: return NULL;
    }
}

/**
 * @brief Draws the menu header.
 *
 * @param[in] title Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_header_(const char* title)
{
    poom_arduboy_set_cursor(22, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_IR_UNIV_HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] line1 Parameter passed to the helper.
 * @param[in] line2 Parameter passed to the helper.
 * @param[in] footer Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_status_(const char* line1, const char* line2, const char* footer)
{
    char line1_buf[22];
    char line2_buf[22];

    (void)snprintf(line1_buf, sizeof(line1_buf), "%.*s", 21, (line1 != NULL) ? line1 : "");
    (void)snprintf(line2_buf, sizeof(line2_buf), "%.*s", 21, (line2 != NULL) ? line2 : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ir_univ_draw_header_(F("IR UNIVERSAL"));

    poom_arduboy_draw_rect(0, MENU_IR_UNIV_BOX_Y, ARDUBOY_WIDTH, MENU_IR_UNIV_BOX_H, WHITE);

    poom_arduboy_set_cursor(MENU_IR_UNIV_TEXT_X, MENU_IR_UNIV_ROW0_Y);
    (void)poom_arduboy_print(line1_buf);

    poom_arduboy_set_cursor(MENU_IR_UNIV_TEXT_X, (int16_t)(MENU_IR_UNIV_ROW0_Y + MENU_IR_UNIV_ROW_STEP));
    (void)poom_arduboy_print(line2_buf);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((footer != NULL) ? footer : "");

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] selected Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_main_(uint8_t selected)
{
    const int16_t list_y0 = 16;
    const int16_t row_step = 10;
    const int16_t row_h = 9;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ir_univ_draw_header_(F("IR UNIVERSAL"));

    poom_arduboy_draw_rect(0, MENU_IR_UNIV_BOX_Y, ARDUBOY_WIDTH, MENU_IR_UNIV_BOX_H, WHITE);

    poom_arduboy_set_cursor(2, list_y0);
    (void)poom_arduboy_print(F("LEARN"));

    poom_arduboy_set_cursor(2, (int16_t)(list_y0 + row_step));
    (void)poom_arduboy_print(F("EMULATOR"));

    poom_arduboy_set_cursor(2, (int16_t)(list_y0 + (int16_t)row_step * 2));
    (void)poom_arduboy_print(F("SD EMULATOR"));

    poom_arduboy_fill_rect(
        0,
        (int16_t)(list_y0 + (int16_t)row_step * (int16_t)selected - 1),
        ARDUBOY_WIDTH,
        row_h,
        INVERT);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SELECT"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] s Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_sd_commands_(const menu_ir_univ_state_t* s)
{
    const uint8_t visible_rows = 3U;
    const int16_t list_y0 = (int16_t)(MENU_IR_UNIV_ROW0_Y + 2);
    const int16_t row_step = 9;
    const int16_t row_h = 8;
    uint8_t scroll = 0U;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ir_univ_draw_sd_header_();
    poom_arduboy_draw_rect(0, MENU_IR_UNIV_BOX_Y, ARDUBOY_WIDTH, MENU_IR_UNIV_BOX_H, WHITE);

    if((s_menu_ir_univ_sd_cmd_count == 0U) || (s == NULL))
    {
        poom_arduboy_set_cursor(MENU_IR_UNIV_TEXT_X, MENU_IR_UNIV_ROW0_Y);
        (void)poom_arduboy_print(F("No commands"));
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("B:BACK"));
        poom_arduboy_display();
        return;
    }

    if(s_menu_ir_univ_sd_cmd_count > visible_rows)
    {
        if(s->sd_selected >= visible_rows)
        {
            scroll = (uint8_t)(s->sd_selected - visible_rows + 1U);
        }

        const uint8_t max_scroll = (uint8_t)(s_menu_ir_univ_sd_cmd_count - visible_rows);
        if(scroll > max_scroll)
        {
            scroll = max_scroll;
        }
    }

    for(uint8_t row = 0U; row < visible_rows; row++)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        if(idx >= s_menu_ir_univ_sd_cmd_count)
        {
            break;
        }

        const int16_t y = (int16_t)(list_y0 + (int16_t)row * row_step);
        const menu_ir_univ_sd_cmd_t* cmd = &s_menu_ir_univ_sd_cmds[idx];

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(cmd->name);

        if(idx == s->sd_selected)
        {
            poom_arduboy_fill_rect(1, y, (int16_t)(ARDUBOY_WIDTH - 2), row_h, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEND"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));
    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_ir_univ_draw_sd_header_(void)
{
    const char* title = (s_menu_ir_univ_sd_title[0] != '\0') ? s_menu_ir_univ_sd_title : "IR SD";
    const size_t title_len = strlen(title);
    const int16_t text_w = (int16_t)(title_len * 6U);
    int16_t x = 2;

    if(text_w < (int16_t)(ARDUBOY_WIDTH - 4))
    {
        x = (int16_t)((ARDUBOY_WIDTH - text_w) / 2);
    }

    poom_arduboy_set_cursor(x, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_IR_UNIV_HEADER_H, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] selected Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_assign_list_(uint8_t selected)
{
    const int16_t list_y0 = 16;
    const int16_t row_step = 12;
    const int16_t row_h = 11;
    const uint8_t visible_rows = 3U;
    const uint8_t count = (uint8_t)(sizeof(s_menu_ir_univ_assign_buttons) / sizeof(s_menu_ir_univ_assign_buttons[0]));
    uint8_t scroll = 0U;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    menu_ir_univ_draw_header_(F("BIND"));

    poom_arduboy_draw_rect(0, MENU_IR_UNIV_BOX_Y, ARDUBOY_WIDTH, MENU_IR_UNIV_BOX_H, WHITE);

    if(count > visible_rows)
    {
        if(selected >= visible_rows)
        {
            scroll = (uint8_t)(selected - visible_rows + 1U);
        }

        const uint8_t max_scroll = (uint8_t)(count - visible_rows);
        if(scroll > max_scroll)
        {
            scroll = max_scroll;
        }
    }

    for(uint8_t row = 0U; row < visible_rows; row++)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        if(idx >= count)
        {
            break;
        }

        const int16_t y = (int16_t)(list_y0 + (int16_t)row * row_step);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(menu_ir_univ_assign_label_(idx));

        if(idx == selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, row_h, INVERT);
        }
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SAVE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] mapped_count Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_draw_emulator_(uint8_t mapped_count)
{
    char line2[22];

    (void)snprintf(line2, sizeof(line2), "Mapped: %u/6", (unsigned)mapped_count);
    menu_ir_univ_draw_status_("EMULATOR", line2, "B:BACK");
}

/**
 * @brief Internal helper for `menu_ir_univ_pop_button_msg`.
 *
 * @param[in] out_msg Parameter passed to the helper.
 * @param[in] timeout_ms Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_pop_button_msg_(menu_ir_univ_button_msg_t* out_msg, uint32_t timeout_ms)
{
    if((s_menu_ir_univ_button_queue == NULL) || (out_msg == NULL))
    {
        return false;
    }

    return xQueueReceive(s_menu_ir_univ_button_queue, out_msg, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @param[in] s Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_is_exit_chord_active_(const menu_ir_univ_state_t* s)
{
    if(s == NULL)
    {
        return false;
    }

    return s->left_down && s->right_down;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_ir_univ_exit_(void)
{
    if(s_menu_ir_univ_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ir_univ_button_cb_, s_menu_ir_univ_sbus_user);
        s_menu_ir_univ_buttons_subscribed = false;
    }

    if(s_menu_ir_univ_button_queue != NULL)
    {
        vQueueDelete(s_menu_ir_univ_button_queue);
        s_menu_ir_univ_button_queue = NULL;
    }

    if(s_menu_ir_univ_led_owned)
    {
        poom_led_rainbow_init();
        if(s_menu_ir_univ_restore_rainbow)
        {
            (void)poom_led_rainbow_start();
        }
        s_menu_ir_univ_restore_rainbow = false;
        s_menu_ir_univ_led_owned = false;
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_ir_univ_handover_exit_to_sd_browser_(void)
{
    if(s_menu_ir_univ_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_ir_univ_button_cb_, s_menu_ir_univ_sbus_user);
        s_menu_ir_univ_buttons_subscribed = false;
    }

    if(s_menu_ir_univ_button_queue != NULL)
    {
        vQueueDelete(s_menu_ir_univ_button_queue);
        s_menu_ir_univ_button_queue = NULL;
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_ir_univ_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_event_msg_t)) || (s_menu_ir_univ_button_queue == NULL))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    (void)xQueueSend(s_menu_ir_univ_button_queue, &button_msg, 0);
}

#if !defined(PIN_NUM_IR_TX) || !defined(PIN_NUM_IR_RX)

void menu_ir_universal_show(void)
{
    menu_ir_univ_draw_status_("IR not available", "Board has no IR", "B:BACK");
    vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

#else

/**
 * @brief Internal helper for `menu_ir_univ_sd_has_ir_ext`.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_sd_has_ir_ext_(const char* abs_path)
{
    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return false;
    }

    const char* dot = strrchr(abs_path, '.');
    if(dot == NULL)
    {
        return false;
    }

    return strcasecmp(dot, ".ir") == 0;
}

/**
 * @brief Internal helper for `menu_ir_univ_sd_filter_ir`.
 *
 * @param[in] name Parameter passed to the helper.
 * @param[in] is_directory Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_sd_filter_ir_(const char* name, bool is_directory, void* user_ctx)
{
    (void)user_ctx;

    if(is_directory)
    {
        return true;
    }

    return menu_ir_univ_sd_has_ir_ext_(name);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_set_start_dir_from_abs_path_(const char* abs_path)
{
    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return;
    }

    const char* slash = strrchr(abs_path, '/');
    if((slash == NULL) || (slash == abs_path))
    {
        return;
    }

    const size_t dir_len = (size_t)(slash - abs_path);
    if(dir_len >= sizeof(s_menu_ir_univ_sd_start_dir))
    {
        return;
    }

    (void)snprintf(s_menu_ir_univ_sd_start_dir, sizeof(s_menu_ir_univ_sd_start_dir), "%.*s", (int)dir_len, abs_path);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_exit_cb_(void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);
    menu_ir_universal_show();
}

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_file_selected_cb_(const char* abs_path, void* user_ctx)
{
    (void)user_ctx;
    (void)poom_sd_browser_set_exit_callback(NULL, NULL);

    s_menu_ir_univ_sd_selected_path[0] = '\0';
    s_menu_ir_univ_sd_bad_ext_pending = false;
    if((abs_path != NULL) && (abs_path[0] != '\0'))
    {
        menu_ir_univ_sd_set_start_dir_from_abs_path_(abs_path);

        if(menu_ir_univ_sd_has_ir_ext_(abs_path))
        {
            (void)snprintf(s_menu_ir_univ_sd_selected_path, sizeof(s_menu_ir_univ_sd_selected_path), "%s", abs_path);
            s_menu_ir_univ_sd_path_pending = true;
        }
        else
        {
            s_menu_ir_univ_sd_bad_ext_pending = true;
        }
    }

    menu_ir_universal_show();
}

/**
 * @brief Internal helper for `menu_ir_univ_launch_sd_browser_and_handover`.
 *
 * @return void
 */
static void menu_ir_univ_launch_sd_browser_and_handover_(void)
{
    poom_sd_browser_config_t cfg = {
        .start_dir = s_menu_ir_univ_sd_start_dir,
        .header = "IR",
        .filter = menu_ir_univ_sd_filter_ir_,
        .filter_ctx = NULL,
        .on_file_selected = menu_ir_univ_sd_file_selected_cb_,
        .on_file_selected_ctx = NULL,
    };

    (void)poom_sd_browser_set_exit_callback(menu_ir_univ_sd_exit_cb_, NULL);
    (void)poom_sd_browser_start_ex(&cfg);
}

/**
 * @brief Parses input data for this menu module.
 *
 * @param[in] s Parameter passed to the helper.
 * @param[in] out_byte Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_sd_parse_hex_byte_(const char* s, uint8_t* out_byte)
{
    if((s == NULL) || (out_byte == NULL))
    {
        return false;
    }

    unsigned int v = 0U;
    if(sscanf(s, " %2x", &v) != 1)
    {
        return false;
    }
    if(v > 0xFFU)
    {
        return false;
    }
    *out_byte = (uint8_t)v;
    return true;
}

/**
 * @brief Parses input data for this menu module.
 *
 * @param[in] s Parameter passed to the helper.
 * @param[in] out_bytes Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_sd_parse_hex4_(const char* s, uint8_t out_bytes[4])
{
    const char* p = s;

    if((p == NULL) || (out_bytes == NULL))
    {
        return false;
    }

    for(int i = 0; i < 4; i++)
    {
        while((*p == ' ') || (*p == '\t'))
        {
            p++;
        }
        if(*p == '\0')
        {
            return false;
        }
        if(!menu_ir_univ_sd_parse_hex_byte_(p, &out_bytes[i]))
        {
            return false;
        }
        while((*p != '\0') && (*p != ' ') && (*p != '\t') && (*p != '\r') && (*p != '\n'))
        {
            p++;
        }
    }

    return true;
}

/**
 * @brief Internal helper for `menu_ir_univ_sd_set_title_from_text`.
 *
 * @param[in] text Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_set_title_from_text_(const char* text)
{
    const char* start = text;
    size_t len;

    if(text == NULL)
    {
        return;
    }

    while((*start == ' ') || (*start == '\t'))
    {
        start++;
    }

    len = strlen(start);
    while((len > 0U) && ((start[len - 1U] == ' ') || (start[len - 1U] == '\t')))
    {
        len--;
    }

    if(len == 0U)
    {
        return;
    }

    (void)snprintf(s_menu_ir_univ_sd_title, sizeof(s_menu_ir_univ_sd_title), "%.*s", (int)len, start);

    for(size_t i = 0U; s_menu_ir_univ_sd_title[i] != '\0'; i++)
    {
        if(s_menu_ir_univ_sd_title[i] == '_')
        {
            s_menu_ir_univ_sd_title[i] = ' ';
        }
    }
}

/**
 * @brief Internal helper for `menu_ir_univ_sd_set_title_from_comment`.
 *
 * @param[in] line Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_set_title_from_comment_(const char* line)
{
    const char* text;

    if((line == NULL) || (line[0] != '#') || (s_menu_ir_univ_sd_title[0] != '\0'))
    {
        return;
    }

    text = line + 1;
    while((*text == ' ') || (*text == '\t'))
    {
        text++;
    }

    menu_ir_univ_sd_set_title_from_text_(text);
}

/**
 * @brief Internal helper for `menu_ir_univ_sd_set_title_from_path`.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_sd_set_title_from_path_(const char* abs_path)
{
    const char* base;
    const char* dot;
    char title[MENU_IR_UNIV_SD_TITLE_MAX + 1U];
    size_t len;

    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return;
    }

    base = strrchr(abs_path, '/');
    base = (base != NULL) ? (base + 1) : abs_path;
    if(base[0] == '\0')
    {
        return;
    }

    dot = strrchr(base, '.');
    len = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
    if(len == 0U)
    {
        return;
    }

    (void)snprintf(title, sizeof(title), "%.*s", (int)len, base);
    menu_ir_univ_sd_set_title_from_text_(title);
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @param[in] abs_path Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_sd_load_file_(const char* abs_path)
{
    FILE* f;
    char line[96];

    s_menu_ir_univ_sd_cmd_count = 0U;
    s_menu_ir_univ_sd_title[0] = '\0';
    if (s_menu_ir_univ_sd_cmds != NULL)
    {
        (void)memset(s_menu_ir_univ_sd_cmds, 0, MENU_IR_UNIV_SD_MAX_COMMANDS * sizeof(s_menu_ir_univ_sd_cmds[0]));
    }

    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return false;
    }

    f = fopen(abs_path, "r");
    if(f == NULL)
    {
        return false;
    }

    char cur_name[MENU_IR_UNIV_SD_NAME_MAX + 1U] = {0};
    char cur_type[16] = {0};
    char cur_proto[16] = {0};
    uint8_t cur_addr[4] = {0};
    uint8_t cur_cmd[4] = {0};
    bool have_name = false;
    bool have_type = false;
    bool have_proto = false;
    bool have_addr = false;
    bool have_cmd = false;

    while(fgets(line, (int)sizeof(line), f) != NULL)
    {
        size_t n = strlen(line);
        while((n > 0U) && ((line[n - 1U] == '\r') || (line[n - 1U] == '\n')))
        {
            line[n - 1U] = '\0';
            n--;
        }

        if(line[0] == '#')
        {
            menu_ir_univ_sd_set_title_from_comment_(line);
            continue;
        }

        if((strncmp(line, "name:", 5) == 0) || (strncmp(line, "name: ", 6) == 0))
        {
            const char* v = strchr(line, ':');
            v = (v != NULL) ? (v + 1) : "";
            while((*v == ' ') || (*v == '\t'))
            {
                v++;
            }
            (void)snprintf(cur_name, sizeof(cur_name), "%.*s", (int)MENU_IR_UNIV_SD_NAME_MAX, v);
            have_name = (cur_name[0] != '\0');
        }
        else if((strncmp(line, "type:", 5) == 0) || (strncmp(line, "type: ", 6) == 0))
        {
            const char* v = strchr(line, ':');
            v = (v != NULL) ? (v + 1) : "";
            while((*v == ' ') || (*v == '\t'))
            {
                v++;
            }
            (void)snprintf(cur_type, sizeof(cur_type), "%.*s", 15, v);
            have_type = (cur_type[0] != '\0');
        }
        else if((strncmp(line, "protocol:", 9) == 0) || (strncmp(line, "protocol: ", 10) == 0))
        {
            const char* v = strchr(line, ':');
            v = (v != NULL) ? (v + 1) : "";
            while((*v == ' ') || (*v == '\t'))
            {
                v++;
            }
            (void)snprintf(cur_proto, sizeof(cur_proto), "%.*s", 15, v);
            have_proto = (cur_proto[0] != '\0');
        }
        else if((strncmp(line, "address:", 8) == 0) || (strncmp(line, "address: ", 9) == 0))
        {
            const char* v = strchr(line, ':');
            v = (v != NULL) ? (v + 1) : "";
            have_addr = menu_ir_univ_sd_parse_hex4_(v, cur_addr);
        }
        else if((strncmp(line, "command:", 8) == 0) || (strncmp(line, "command: ", 9) == 0))
        {
            const char* v = strchr(line, ':');
            v = (v != NULL) ? (v + 1) : "";
            have_cmd = menu_ir_univ_sd_parse_hex4_(v, cur_cmd);
        }

        if(have_name && have_type && have_proto && have_addr && have_cmd)
        {
            if(strcasecmp(cur_type, "parsed") == 0)
            {
                menu_ir_univ_sd_cmd_t* out = NULL;
                menu_ir_univ_proto_t proto = MENU_IR_UNIV_PROTO_NONE;

                (void)ir_protocol_parse_name(cur_proto, (ir_protocol_t*)&proto);

                if((proto != MENU_IR_UNIV_PROTO_NONE) &&
                   (s_menu_ir_univ_sd_cmds != NULL) &&
                   (s_menu_ir_univ_sd_cmd_count < MENU_IR_UNIV_SD_MAX_COMMANDS))
                {
                    out = &s_menu_ir_univ_sd_cmds[s_menu_ir_univ_sd_cmd_count];
                    (void)snprintf(out->name, sizeof(out->name), "%s", cur_name);
                    out->proto = proto;
                    out->address = (uint32_t)cur_addr[0] |
                        ((uint32_t)cur_addr[1] << 8) |
                        ((uint32_t)cur_addr[2] << 16) |
                        ((uint32_t)cur_addr[3] << 24);
                    out->command = (uint32_t)cur_cmd[0] |
                        ((uint32_t)cur_cmd[1] << 8) |
                        ((uint32_t)cur_cmd[2] << 16) |
                        ((uint32_t)cur_cmd[3] << 24);
                    s_menu_ir_univ_sd_cmd_count++;
                }
            }

            have_name = false;
            have_type = false;
            have_proto = false;
            have_addr = false;
            have_cmd = false;
            cur_name[0] = '\0';
            cur_type[0] = '\0';
            cur_proto[0] = '\0';
            (void)memset(cur_addr, 0, sizeof(cur_addr));
            (void)memset(cur_cmd, 0, sizeof(cur_cmd));
        }
    }

    (void)fclose(f);

    if(s_menu_ir_univ_sd_title[0] == '\0')
    {
        menu_ir_univ_sd_set_title_from_path_(abs_path);
    }

    return s_menu_ir_univ_sd_cmd_count > 0U;
}

typedef enum
{
    MENU_IR_UNIV_LOAD_OK = 0,
    MENU_IR_UNIV_LOAD_NO_CODE,
    MENU_IR_UNIV_LOAD_STORE_ERR,
    MENU_IR_UNIV_LOAD_BAD_DATA,
} menu_ir_univ_load_result_t;

/**
 * @brief Loads internal data used by this menu module.
 *
 * @param[in] button Parameter passed to the helper.
 * @param[in] out_code Parameter passed to the helper.
 * @return menu_ir_univ_load_result_t
 */
static menu_ir_univ_load_result_t menu_ir_univ_load_code_for_button_(uint8_t button, menu_ir_univ_code_t* out_code)
{
    esp_err_t err;
    const char* key;
    size_t blob_len = 0U;
    uint8_t raw[8] = {0};
    menu_ir_univ_code_t code = {0};
    menu_ir_univ_code_legacy_t legacy = {0};

    if(out_code == NULL)
    {
        return MENU_IR_UNIV_LOAD_STORE_ERR;
    }

    key = menu_ir_univ_button_key_(button);
    if(key == NULL)
    {
        return MENU_IR_UNIV_LOAD_NO_CODE;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        MENU_IR_UNIV_PRINTF_E("poom_secrets_init failed: %s", esp_err_to_name(err));
        return MENU_IR_UNIV_LOAD_STORE_ERR;
    }

    err = poom_secrets_get_blob(key, NULL, &blob_len);
    if(err == ESP_ERR_NVS_NOT_FOUND)
    {
        return MENU_IR_UNIV_LOAD_NO_CODE;
    }
    if(err != ESP_OK)
    {
        MENU_IR_UNIV_PRINTF_E("poom_secrets_get_blob len failed (%s): %s", key, esp_err_to_name(err));
        return MENU_IR_UNIV_LOAD_STORE_ERR;
    }

    if(blob_len == 4U)
    {
        size_t tmp_len = sizeof(raw);
        err = poom_secrets_get_blob(key, raw, &tmp_len);
        if((err != ESP_OK) || (tmp_len != 4U))
        {
            MENU_IR_UNIV_PRINTF_E("poom_secrets_get_blob raw failed (%s): %s", key, esp_err_to_name(err));
            return MENU_IR_UNIV_LOAD_BAD_DATA;
        }

        code.protocol = raw[0];
        code.address = (uint32_t)raw[1];
        code.command = raw[2];
        code.flags = 0U;
        code.reserved = raw[3];
    }
    else if(blob_len == sizeof(legacy))
    {
        size_t tmp_len = sizeof(legacy);
        err = poom_secrets_get_blob(key, &legacy, &tmp_len);
        if((err != ESP_OK) || (tmp_len != sizeof(legacy)))
        {
            MENU_IR_UNIV_PRINTF_E("poom_secrets_get_blob legacy failed (%s): %s", key, esp_err_to_name(err));
            return MENU_IR_UNIV_LOAD_BAD_DATA;
        }

        code.protocol = legacy.protocol;
        code.flags = 0U;
        code.reserved = legacy.reserved;
        code.address = legacy.address;
        code.command = legacy.command;
    }
    else if(blob_len == sizeof(menu_ir_univ_code_t))
    {
        size_t tmp_len = sizeof(code);
        err = poom_secrets_get_blob(key, &code, &tmp_len);
        if((err != ESP_OK) || (tmp_len != sizeof(code)))
        {
            MENU_IR_UNIV_PRINTF_E("poom_secrets_get_blob struct failed (%s): %s", key, esp_err_to_name(err));
            return MENU_IR_UNIV_LOAD_BAD_DATA;
        }
    }
    else
    {
        MENU_IR_UNIV_PRINTF_W("Invalid blob len (%s): %u", key, (unsigned)blob_len);
        return MENU_IR_UNIV_LOAD_BAD_DATA;
    }

    if(!ir_protocol_is_supported((ir_protocol_t)code.protocol))
    {
        MENU_IR_UNIV_PRINTF_W("Invalid protocol (%s): %u", key, (unsigned)code.protocol);
        return MENU_IR_UNIV_LOAD_BAD_DATA;
    }

    *out_code = code;
    return MENU_IR_UNIV_LOAD_OK;
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @param[in] button Parameter passed to the helper.
 * @param[in] code Parameter passed to the helper.
 * @return bool
 */
static bool menu_ir_univ_save_code_for_button_(uint8_t button, const menu_ir_univ_code_t* code)
{
    esp_err_t err;
    const char* key;

    if(code == NULL)
    {
        return false;
    }

    key = menu_ir_univ_button_key_(button);
    if(key == NULL)
    {
        return false;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return false;
    }

    err = poom_secrets_set_blob(key, code, sizeof(*code));
    return err == ESP_OK;
}

/**
 * @brief Internal helper for `menu_ir_univ_count_mapped`.
 *
 * @return uint8_t
 */
static uint8_t menu_ir_univ_count_mapped_(void)
{
    uint8_t count = 0U;

    for(uint8_t i = 0U; i < (uint8_t)(sizeof(s_menu_ir_univ_assign_buttons) / sizeof(s_menu_ir_univ_assign_buttons[0])); i++)
    {
        menu_ir_univ_code_t tmp;
        if(menu_ir_univ_load_code_for_button_(s_menu_ir_univ_assign_buttons[i], &tmp) == MENU_IR_UNIV_LOAD_OK)
        {
            count++;
        }
    }

    return count;
}

typedef enum
{
    MENU_IR_UNIV_SEND_OK = 0,
    MENU_IR_UNIV_SEND_NO_CODE,
    MENU_IR_UNIV_SEND_STORE_ERR,
    MENU_IR_UNIV_SEND_BAD_DATA,
    MENU_IR_UNIV_SEND_TX_INIT_FAILED,
    MENU_IR_UNIV_SEND_FAILED,
} menu_ir_univ_send_result_t;

/**
 * @brief Internal helper for `menu_ir_univ_send_for_button`.
 *
 * @param[in] transmitter Parameter passed to the helper.
 * @param[in] transmitter_ready Parameter passed to the helper.
 * @param[in] transmitter_cfg Parameter passed to the helper.
 * @param[in] button Parameter passed to the helper.
 * @return menu_ir_univ_send_result_t
 */
static menu_ir_univ_send_result_t menu_ir_univ_send_for_button_(ir_tx_handle_t* transmitter,
                                                                bool* transmitter_ready,
                                                                const ir_tx_config_t* transmitter_cfg,
                                                                uint8_t button)
{
    menu_ir_univ_code_t code;
    menu_ir_univ_load_result_t load_result;

    if((transmitter == NULL) || (transmitter_ready == NULL) || (transmitter_cfg == NULL))
    {
        return MENU_IR_UNIV_SEND_FAILED;
    }

    load_result = menu_ir_univ_load_code_for_button_(button, &code);
    if(load_result != MENU_IR_UNIV_LOAD_OK)
    {
        if(load_result == MENU_IR_UNIV_LOAD_NO_CODE)
        {
            return MENU_IR_UNIV_SEND_NO_CODE;
        }
        if(load_result == MENU_IR_UNIV_LOAD_STORE_ERR)
        {
            return MENU_IR_UNIV_SEND_STORE_ERR;
        }
        if(load_result == MENU_IR_UNIV_LOAD_BAD_DATA)
        {
            return MENU_IR_UNIV_SEND_BAD_DATA;
        }
        return MENU_IR_UNIV_SEND_FAILED;
    }

    if(!(*transmitter_ready))
    {
        esp_err_t err = ir_tx_init(transmitter, transmitter_cfg, "ir_univ_tx");
        if(err != ESP_OK)
        {
            MENU_IR_UNIV_PRINTF_E("IR TX init failed (gpio=%d): %s",
                                  transmitter_cfg->gpio,
                                  esp_err_to_name(err));
            return MENU_IR_UNIV_SEND_TX_INIT_FAILED;
        }

        *transmitter_ready = true;
    }

    return (ir_tx_send(transmitter, (ir_protocol_t)code.protocol, code.address, code.command) == ESP_OK)
        ? MENU_IR_UNIV_SEND_OK
        : MENU_IR_UNIV_SEND_FAILED;
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_ir_univ_task_(void* arg)
{
    menu_ir_univ_state_t s = {0};
    ir_rcv_handle_t receiver = {0};
    ir_tx_handle_t transmitter = {0};
    ir_decoder_context_t decoder_ctx = {0};
    bool receiver_ready = false;
    bool transmitter_ready = false;
    ir_rcv_config_t receiver_cfg;
    ir_tx_config_t transmitter_cfg;

    (void)arg;

    s.screen = MENU_IR_UNIV_SCREEN_MAIN;
    s.main_selected = 0U;
    s.assign_selected = 0U;
    s.emulator_mapped_count = 0U;
    s.sd_selected = 0U;
    s.left_down = false;
    s.right_down = false;
    (void)memset(&s.pending_code, 0, sizeof(s.pending_code));
    ir_decoder_context_reset(&decoder_ctx);

    free(s_menu_ir_univ_sd_cmds);
    s_menu_ir_univ_sd_cmds = (menu_ir_univ_sd_cmd_t*)calloc(MENU_IR_UNIV_SD_MAX_COMMANDS, sizeof(*s_menu_ir_univ_sd_cmds));
    if(s_menu_ir_univ_sd_cmds == NULL)
    {
        menu_ir_univ_draw_status_("No RAM", "IR SD cache", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
        menu_ir_univ_exit_();
        s_menu_ir_univ_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_menu_ir_univ_button_queue = xQueueCreate(MENU_IR_UNIV_BUTTON_QUEUE_DEPTH, sizeof(menu_ir_univ_button_msg_t));
    if(s_menu_ir_univ_button_queue == NULL)
    {
        menu_ir_univ_draw_status_("No queue", "Try again", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
        free(s_menu_ir_univ_sd_cmds);
        s_menu_ir_univ_sd_cmds = NULL;
        menu_ir_univ_exit_();
        s_menu_ir_univ_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if(!s_menu_ir_univ_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_ir_univ_button_cb_, s_menu_ir_univ_sbus_user))
        {
            s_menu_ir_univ_buttons_subscribed = true;
        }
        else
        {
            menu_ir_univ_draw_status_("No input", "Try again", " ");
            vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
            free(s_menu_ir_univ_sd_cmds);
            s_menu_ir_univ_sd_cmds = NULL;
            menu_ir_univ_exit_();
            s_menu_ir_univ_task = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    receiver_cfg = ir_rcv_default_config();
    receiver_cfg.gpio = PIN_NUM_IR_RX;
    receiver_cfg.clk_hz = MENU_IR_UNIV_NEC_CLK_HZ;
    receiver_cfg.buffer_symbols = MENU_IR_UNIV_RX_BUFFER_SYMBOLS;

    transmitter_cfg = ir_tx_default_config();
    transmitter_cfg.gpio = PIN_NUM_IR_TX;
    transmitter_cfg.clk_hz = MENU_IR_UNIV_NEC_CLK_HZ;
    transmitter_cfg.carrier_hz = MENU_IR_UNIV_NEC_CARRIER_HZ;
    transmitter_cfg.duty_cycle = MENU_IR_UNIV_NEC_DUTY_CYCLE;

    if(s_menu_ir_univ_sd_path_pending)
    {
        s_menu_ir_univ_sd_path_pending = false;
        if(menu_ir_univ_sd_load_file_(s_menu_ir_univ_sd_selected_path))
        {
            s.screen = MENU_IR_UNIV_SCREEN_SD_COMMANDS;
            s.sd_selected = 0U;
            menu_ir_univ_draw_sd_commands_(&s);
        }
        else
        {
            s.screen = MENU_IR_UNIV_SCREEN_MAIN;
            menu_ir_univ_draw_status_("BAD IR FILE", " ", "B:BACK");
            vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
            menu_ir_univ_draw_main_(s.main_selected);
        }
    }
    else if(s_menu_ir_univ_sd_bad_ext_pending)
    {
        s_menu_ir_univ_sd_bad_ext_pending = false;
        s.screen = MENU_IR_UNIV_SCREEN_MAIN;
        menu_ir_univ_draw_status_("Select .IR", "file only", "B:BACK");
        vTaskDelay(pdMS_TO_TICKS(700U));

        menu_ir_univ_handover_exit_to_sd_browser_();
        s_menu_ir_univ_task = NULL;
        menu_ir_univ_launch_sd_browser_and_handover_();
        vTaskDelete(NULL);
    }
    else
    {
        menu_ir_univ_draw_main_(s.main_selected);
    }

    while(1)
    {
        menu_ir_univ_button_msg_t button_msg = {0};
        bool got_button = menu_ir_univ_pop_button_msg_(&button_msg, 50U);

        if(got_button)
        {
            if(button_msg.event == BUTTON_PRESS_DOWN)
            {
                if(button_msg.button == BUTTON_LEFT)
                {
                    s.left_down = true;
                }
                else if(button_msg.button == BUTTON_RIGHT)
                {
                    s.right_down = true;
                }
            }
            else if(button_msg.event == BUTTON_PRESS_UP)
            {
                if(button_msg.button == BUTTON_LEFT)
                {
                    s.left_down = false;
                }
                else if(button_msg.button == BUTTON_RIGHT)
                {
                    s.right_down = false;
                }
            }

            if(menu_ir_univ_is_exit_chord_active_(&s))
            {
                if(s.screen == MENU_IR_UNIV_SCREEN_MAIN)
                {
                    break;
                }

                if(receiver_ready)
                {
                    (void)ir_rcv_deinit(&receiver);
                    receiver_ready = false;
                }

                if(transmitter_ready)
                {
                    (void)ir_tx_deinit(&transmitter);
                    transmitter_ready = false;
                }

                s.screen = MENU_IR_UNIV_SCREEN_MAIN;
                s.left_down = false;
                s.right_down = false;
                (void)xQueueReset(s_menu_ir_univ_button_queue);
                menu_ir_univ_draw_main_(s.main_selected);
                continue;
            }
        }

        if(got_button && (button_msg.event == BUTTON_SINGLE_CLICK) && (button_msg.button == BUTTON_B))
        {
            if(s.screen == MENU_IR_UNIV_SCREEN_MAIN)
            {
                break;
            }

            if(s.screen == MENU_IR_UNIV_SCREEN_LEARN_SELECT_BUTTON)
            {
                s.screen = MENU_IR_UNIV_SCREEN_LEARN_WAIT_IR;
                menu_ir_univ_draw_status_("LEARN", "Press remote", "B:BACK");
                continue;
            }

            if(s.screen == MENU_IR_UNIV_SCREEN_SD_COMMANDS)
            {
                if(receiver_ready)
                {
                    (void)ir_rcv_deinit(&receiver);
                    receiver_ready = false;
                }

                if(transmitter_ready)
                {
                    (void)ir_tx_deinit(&transmitter);
                    transmitter_ready = false;
                }

                menu_ir_univ_handover_exit_to_sd_browser_();
                s_menu_ir_univ_task = NULL;
                menu_ir_univ_launch_sd_browser_and_handover_();
                vTaskDelete(NULL);
            }

            if(receiver_ready)
            {
                (void)ir_rcv_deinit(&receiver);
                receiver_ready = false;
            }

            if(transmitter_ready)
            {
                (void)ir_tx_deinit(&transmitter);
                transmitter_ready = false;
            }

            s.screen = MENU_IR_UNIV_SCREEN_MAIN;
            s.left_down = false;
            s.right_down = false;
            (void)xQueueReset(s_menu_ir_univ_button_queue);
            menu_ir_univ_draw_main_(s.main_selected);
            continue;
        }

        if(s.screen == MENU_IR_UNIV_SCREEN_MAIN)
        {
            if(got_button && (button_msg.event == BUTTON_SINGLE_CLICK))
            {
                if(button_msg.button == BUTTON_UP)
                {
                    if(s.main_selected > 0U)
                    {
                        s.main_selected--;
                    }
                    menu_ir_univ_draw_main_(s.main_selected);
                }
                else if(button_msg.button == BUTTON_DOWN)
                {
                    if(s.main_selected < 2U)
                    {
                        s.main_selected++;
                    }
                    menu_ir_univ_draw_main_(s.main_selected);
                }
                else if(button_msg.button == BUTTON_A)
                {
                    if(s.main_selected == 0U)
                    {
                        s.screen = MENU_IR_UNIV_SCREEN_LEARN_WAIT_IR;
                        menu_ir_univ_draw_status_("LEARN", "Press remote", "B:BACK");
                    }
                    else if(s.main_selected == 1U)
                    {
                        s.screen = MENU_IR_UNIV_SCREEN_EMULATOR;
                        s.emulator_mapped_count = menu_ir_univ_count_mapped_();
                        menu_ir_univ_draw_emulator_(s.emulator_mapped_count);
                    }
                    else
                    {
                        if(receiver_ready)
                        {
                            (void)ir_rcv_deinit(&receiver);
                            receiver_ready = false;
                        }
                        if(transmitter_ready)
                        {
                            (void)ir_tx_deinit(&transmitter);
                            transmitter_ready = false;
                        }

                        menu_ir_univ_handover_exit_to_sd_browser_();
                        free(s_menu_ir_univ_sd_cmds);
                        s_menu_ir_univ_sd_cmds = NULL;
                        s_menu_ir_univ_sd_cmd_count = 0U;
                        s_menu_ir_univ_task = NULL;
                        menu_ir_univ_launch_sd_browser_and_handover_();
                        vTaskDelete(NULL);
                    }
                }
            }
        }
        else if(s.screen == MENU_IR_UNIV_SCREEN_LEARN_WAIT_IR)
        {
            if(!receiver_ready)
            {
                if(ir_rcv_init(&receiver, &receiver_cfg, "ir_univ_rx") == ESP_OK)
                {
                    receiver_ready = true;
                }
                else
                {
                    MENU_IR_UNIV_PRINTF_E("IR RX init failed (gpio=%d)", receiver_cfg.gpio);
                    menu_ir_univ_draw_status_("IR RX init", "failed", "B:BACK");
                    vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
                    s.screen = MENU_IR_UNIV_SCREEN_MAIN;
                    menu_ir_univ_draw_main_(s.main_selected);
                    continue;
                }
            }

            if(ir_rcv_start(&receiver, &receiver_cfg) == ESP_OK)
            {
                rmt_rx_done_event_data_t rx = {0};
                if(ir_rcv_wait(&receiver, &rx, MENU_IR_UNIV_RCV_TIMEOUT_MS))
                {
                    ir_decoded_frame_t frame = {0};
                    MENU_IR_UNIV_PRINTF_I("RX done: symbols=%u", (unsigned)rx.num_symbols);
                    ir_rcv_dump(&receiver, &rx, MENU_IR_UNIV_RX_DUMP_SYMBOLS_MAX);
                    if(ir_decode_any_ex(rx.received_symbols,
                                        rx.num_symbols,
                                        receiver_cfg.clk_hz,
                                        &decoder_ctx,
                                        &frame))
                    {
                        MENU_IR_UNIV_PRINTF_I("Decoded: %s addr=0x%08lX cmd=0x%08lX repeat=%u",
                                              ir_protocol_name(frame.protocol),
                                              (unsigned long)frame.address,
                                              (unsigned long)frame.command,
                                              (unsigned)frame.repeat);
                        s.pending_code.protocol = (uint8_t)frame.protocol;
                        s.pending_code.flags = frame.repeat ? 1U : 0U;
                        s.pending_code.reserved = 0U;
                        s.pending_code.address = frame.address;
                        s.pending_code.command = frame.command;
                        s.screen = MENU_IR_UNIV_SCREEN_LEARN_SELECT_BUTTON;
                        s.assign_selected = 0U;
                        menu_ir_univ_draw_assign_list_(s.assign_selected);
                    }
                    else
                    {
                        MENU_IR_UNIV_PRINTF_W("Decode failed (unsupported)");
                        menu_ir_univ_draw_status_("Unsupported", "Try again", "B:BACK");
                        vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_STATUS_HOLD_MS));
                        menu_ir_univ_draw_status_("LEARN", "Press remote", "B:BACK");
                    }

                    (void)ir_rcv_deinit(&receiver);
                    receiver_ready = false;
                }
                else
                {
                    MENU_IR_UNIV_PRINTF_D("RX wait timeout");
                }
            }
            else
            {
                MENU_IR_UNIV_PRINTF_W("ir_rcv_start failed (gpio=%d)", receiver_cfg.gpio);
            }
        }
        else if(s.screen == MENU_IR_UNIV_SCREEN_LEARN_SELECT_BUTTON)
        {
            if(got_button && (button_msg.event == BUTTON_SINGLE_CLICK))
            {
                const uint8_t assign_count = (uint8_t)(sizeof(s_menu_ir_univ_assign_buttons) / sizeof(s_menu_ir_univ_assign_buttons[0]));

                if(button_msg.button == BUTTON_UP)
                {
                    if(s.assign_selected == 0U)
                    {
                        s.assign_selected = (uint8_t)(assign_count - 1U);
                    }
                    else
                    {
                        s.assign_selected--;
                    }
                    menu_ir_univ_draw_assign_list_(s.assign_selected);
                }
                else if(button_msg.button == BUTTON_DOWN)
                {
                    s.assign_selected++;
                    if(s.assign_selected >= assign_count)
                    {
                        s.assign_selected = 0U;
                    }
                    menu_ir_univ_draw_assign_list_(s.assign_selected);
                }
                else if(button_msg.button == BUTTON_A)
                {
                    uint8_t target_button = s_menu_ir_univ_assign_buttons[s.assign_selected];
                    if(menu_ir_univ_save_code_for_button_(target_button, &s.pending_code))
                    {
                        menu_ir_univ_draw_status_("SAVED", " ", "B:BACK");
                    }
                    else
                    {
                        menu_ir_univ_draw_status_("SAVE FAIL", " ", "B:BACK");
                    }

                    vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_STATUS_HOLD_MS));
                    s.screen = MENU_IR_UNIV_SCREEN_LEARN_WAIT_IR;
                    menu_ir_univ_draw_status_("LEARN", "Press remote", "B:BACK");
                }
            }
        }
        else if(s.screen == MENU_IR_UNIV_SCREEN_EMULATOR)
        {
            if(got_button && (button_msg.event == BUTTON_SINGLE_CLICK))
            {
                menu_ir_univ_send_result_t send_result =
                    menu_ir_univ_send_for_button_(&transmitter, &transmitter_ready, &transmitter_cfg, button_msg.button);

                if(send_result == MENU_IR_UNIV_SEND_OK)
                {
                    menu_ir_univ_draw_status_("SENT", " ", "B:BACK");
                }
                else if(send_result == MENU_IR_UNIV_SEND_NO_CODE)
                {
                    menu_ir_univ_draw_status_("NOT SET", "Use LEARN", "B:BACK");
                }
                else if(send_result == MENU_IR_UNIV_SEND_STORE_ERR)
                {
                    menu_ir_univ_draw_status_("STORE ERR", " ", "B:BACK");
                }
                else if(send_result == MENU_IR_UNIV_SEND_BAD_DATA)
                {
                    menu_ir_univ_draw_status_("BAD CODE", "Relearn", "B:BACK");
                }
                else if(send_result == MENU_IR_UNIV_SEND_TX_INIT_FAILED)
                {
                    menu_ir_univ_draw_status_("IR TX FAIL", " ", "B:BACK");
                }
                else
                {
                    menu_ir_univ_draw_status_("SEND FAIL", " ", "B:BACK");
                }

                vTaskDelay(pdMS_TO_TICKS(140U));
                menu_ir_univ_draw_emulator_(s.emulator_mapped_count);
            }
        }
        else if(s.screen == MENU_IR_UNIV_SCREEN_SD_COMMANDS)
        {
            if(got_button && (button_msg.event == BUTTON_SINGLE_CLICK))
            {
                if(button_msg.button == BUTTON_UP)
                {
                    if(s_menu_ir_univ_sd_cmd_count > 0U)
                    {
                        if(s.sd_selected == 0U)
                        {
                            s.sd_selected = (uint8_t)(s_menu_ir_univ_sd_cmd_count - 1U);
                        }
                        else
                        {
                            s.sd_selected--;
                        }
                        menu_ir_univ_draw_sd_commands_(&s);
                    }
                }
                else if(button_msg.button == BUTTON_DOWN)
                {
                    if(s_menu_ir_univ_sd_cmd_count > 0U)
                    {
                        s.sd_selected++;
                        if(s.sd_selected >= s_menu_ir_univ_sd_cmd_count)
                        {
                            s.sd_selected = 0U;
                        }
                        menu_ir_univ_draw_sd_commands_(&s);
                    }
                }
                else if(button_msg.button == BUTTON_A)
                {
                    if(s_menu_ir_univ_sd_cmd_count > 0U)
                    {
                        const menu_ir_univ_sd_cmd_t* cmd = &s_menu_ir_univ_sd_cmds[s.sd_selected];

                        if(!transmitter_ready)
                        {
                            esp_err_t err = ir_tx_init(&transmitter, &transmitter_cfg, "ir_sd_tx");
                            if(err != ESP_OK)
                            {
                                menu_ir_univ_draw_status_("IR TX FAIL", " ", "B:BACK");
                                vTaskDelay(pdMS_TO_TICKS(140U));
                                menu_ir_univ_draw_sd_commands_(&s);
                                continue;
                            }
                            transmitter_ready = true;
                        }

                        esp_err_t send_err =
                            ir_tx_send(&transmitter, (ir_protocol_t)cmd->proto, cmd->address, cmd->command);

                        if(send_err == ESP_OK)
                        {
                            menu_ir_univ_draw_status_("SENT", " ", "B:BACK");
                        }
                        else
                        {
                            menu_ir_univ_draw_status_("SEND FAIL", " ", "B:BACK");
                        }

                        vTaskDelay(pdMS_TO_TICKS(140U));
                        menu_ir_univ_draw_sd_commands_(&s);
                    }
                }
            }
        }
    }

    if(receiver_ready)
    {
        (void)ir_rcv_deinit(&receiver);
    }

    if(transmitter_ready)
    {
        (void)ir_tx_deinit(&transmitter);
    }

    free(s_menu_ir_univ_sd_cmds);
    s_menu_ir_univ_sd_cmds = NULL;
    s_menu_ir_univ_sd_cmd_count = 0U;

    menu_ir_univ_exit_();
    s_menu_ir_univ_task = NULL;
    vTaskDelete(NULL);
}

void menu_ir_universal_show(void)
{
    if(s_menu_ir_univ_task != NULL)
    {
        return;
    }

    if(!s_menu_ir_univ_led_owned)
    {
        s_menu_ir_univ_restore_rainbow = poom_led_rainbow_deinit();
        s_menu_ir_univ_led_owned = true;
    }

    if(xTaskCreate(menu_ir_univ_task_,
                   "menu_ir_univ",
                   MENU_IR_UNIV_TASK_STACK_WORDS,
                   NULL,
                   MENU_IR_UNIV_TASK_PRIORITY,
                   &s_menu_ir_univ_task) != pdPASS)
    {
        s_menu_ir_univ_task = NULL;
        if(s_menu_ir_univ_led_owned)
        {
            poom_led_rainbow_init();
            if(s_menu_ir_univ_restore_rainbow)
            {
                (void)poom_led_rainbow_start();
            }
            s_menu_ir_univ_restore_rainbow = false;
            s_menu_ir_univ_led_owned = false;
        }
        menu_ir_univ_draw_status_("Task create", "failed", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_IR_UNIV_ERROR_HOLD_MS));
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
    }
}

#endif
