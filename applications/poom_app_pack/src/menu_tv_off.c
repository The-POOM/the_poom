// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_tv_off.h"

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
#include "ir_dec.h"
#include "ir_tx.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_TV_OFF_TASK_STACK_WORDS (3072U)
#define MENU_TV_OFF_TASK_PRIORITY (4U)
#define MENU_TV_OFF_BUTTON_QUEUE_DEPTH (4U)
#define MENU_TV_OFF_STATUS_HOLD_MS (900U)
#define MENU_TV_OFF_SEND_GAP_MS (110U)

#define MENU_TV_OFF_HEADER_H (11)
#define MENU_TV_OFF_BOX_Y (12)
#define MENU_TV_OFF_BOX_H (41)
#define MENU_TV_OFF_TEXT_X (6)

#define MENU_TV_OFF_CLK_HZ (1000000U)
#define MENU_TV_OFF_CARRIER_HZ (38000U)
#define MENU_TV_OFF_DUTY_CYCLE (0.33f)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_tv_off_button_msg_t;

typedef struct
{
    ir_protocol_t protocol;
    uint16_t address;
    uint8_t command;
    const char* family;
} menu_tv_off_code_t;

/*
 * ir_tx_send() expects decoded protocol values, not the full Flipper byte
 * string. NEC/NECext/Samsung command is the first command byte only; the
 * driver emits the complement byte. NECext address uses the Flipper byte
 * order as little-endian: "64 46" becomes 0x4664.
 */
static const menu_tv_off_code_t s_menu_tv_off_codes[] = {
    {IR_PROTOCOL_SAMSUNG32, 0x0007U, 0x02U, "Samsung"},
    {IR_PROTOCOL_RC5,       0x0000U, 0x0CU, "Grundig"},
    {IR_PROTOCOL_SIRC,      0x0001U, 0x15U, "Sony"},
    {IR_PROTOCOL_RC5,       0x0001U, 0x0CU, "Telefunken"},
    {IR_PROTOCOL_NEC,       0x0004U, 0x08U, "LG / Vizio"},
    {IR_PROTOCOL_RC6,       0x0000U, 0x0CU, "Philips"},
    {IR_PROTOCOL_NEC,       0x0019U, 0x18U, "Medion"},
    {IR_PROTOCOL_NEC,       0x0049U, 0x1AU, "Oppo"},
    {IR_PROTOCOL_NEC_EXT,   0x4664U, 0x5DU, "Fetch"},
    {IR_PROTOCOL_NEC_EXT,   0x7F00U, 0x0AU, "Denver"},
    {IR_PROTOCOL_NEC_EXT,   0x7F00U, 0x15U, "Plat / Elitelux"},
    {IR_PROTOCOL_NEC_EXT,   0xBF00U, 0x0DU, "Hisense"},
    {IR_PROTOCOL_NEC_EXT,   0xC7EAU, 0x17U, "TCL"},
};

static void menu_tv_off_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_tv_off_task_(void* arg);

static void menu_tv_off_draw_header_(void)
{
    poom_arduboy_set_cursor(44, 2);
    (void)poom_arduboy_print(F("TV OFF"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, MENU_TV_OFF_HEADER_H, INVERT);
}

static void menu_tv_off_line_(int16_t y, const char* text)
{
    poom_arduboy_set_cursor(MENU_TV_OFF_TEXT_X, y);
    (void)poom_arduboy_print((text != NULL) ? text : "");
}

static void menu_tv_off_draw_footer_(const char* left, const char* right)
{
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((left != NULL) ? left : "");
    if(right != NULL)
    {
        poom_arduboy_set_cursor(84, 56);
        (void)poom_arduboy_print(right);
    }
}

static void menu_tv_off_draw_progress_(uint8_t sent, const menu_tv_off_code_t* code)
{
    char line[22];
    const uint8_t total = (uint8_t)(sizeof(s_menu_tv_off_codes) / sizeof(s_menu_tv_off_codes[0]));
    uint16_t fill_width = 0U;

    if(total > 0U)
    {
        fill_width = (uint16_t)((118U * sent) / total);
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_tv_off_draw_header_();
    poom_arduboy_draw_rect(0, MENU_TV_OFF_BOX_Y, ARDUBOY_WIDTH, MENU_TV_OFF_BOX_H, WHITE);

    (void)snprintf(line, sizeof(line), "%u/%u SENT", (unsigned)sent, (unsigned)total);
    menu_tv_off_line_(14, line);
    menu_tv_off_line_(24, (code != NULL) ? ir_protocol_name(code->protocol) : "COMPLETE");
    menu_tv_off_line_(34, (code != NULL) ? code->family : " ");

    poom_arduboy_draw_rect(4, 44, 120, 6, WHITE);
    poom_arduboy_fill_rect(5, 45, (int16_t)fill_width, 4, WHITE);

    if(code != NULL)
    {
        menu_tv_off_draw_footer_("B:STOP", NULL);
    }
    else
    {
        menu_tv_off_draw_footer_("A:AGAIN", "B:BACK");
    }

    poom_arduboy_display();
}

static void menu_tv_off_draw_status_(const char* line1, const char* line2, const char* footer)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_tv_off_draw_header_();
    poom_arduboy_draw_rect(0, MENU_TV_OFF_BOX_Y, ARDUBOY_WIDTH, MENU_TV_OFF_BOX_H, WHITE);
    menu_tv_off_line_(18, line1);
    menu_tv_off_line_(30, line2);
    menu_tv_off_draw_footer_(footer, NULL);
    poom_arduboy_display();
}

static void menu_tv_off_exit_(QueueHandle_t button_queue)
{
    (void)poom_sbus_unsubscribe_cb("input/button", menu_tv_off_button_cb_, button_queue);

    if(button_queue != NULL)
    {
        vQueueDelete(button_queue);
    }

    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

static bool menu_tv_off_pop_button_(QueueHandle_t button_queue,
                                    menu_tv_off_button_msg_t* out_msg,
                                    uint32_t timeout_ms)
{
    if((button_queue == NULL) || (out_msg == NULL))
    {
        return false;
    }

    return xQueueReceive(button_queue, out_msg, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

static bool menu_tv_off_should_stop_(QueueHandle_t button_queue)
{
    menu_tv_off_button_msg_t button_msg = {0};

    while(menu_tv_off_pop_button_(button_queue, &button_msg, 0U))
    {
        if((button_msg.event == BUTTON_SINGLE_CLICK) && (button_msg.button == BUTTON_B))
        {
            return true;
        }
    }

    return false;
}

static void menu_tv_off_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_tv_off_button_msg_t button_msg;
    QueueHandle_t button_queue = (QueueHandle_t)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_event_msg_t)) || (button_queue == NULL))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    (void)xQueueSend(button_queue, &button_msg, 0);
}

#if !defined(PIN_NUM_IR_TX)

void menu_tv_off_show(void)
{
    menu_tv_off_draw_status_("IR not available", "Board has no TX", "B:BACK");
    vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_STATUS_HOLD_MS));
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

#else

static bool menu_tv_off_wait_after_done_(QueueHandle_t button_queue)
{
    while(1)
    {
        menu_tv_off_button_msg_t button_msg = {0};
        if(!menu_tv_off_pop_button_(button_queue, &button_msg, 100U))
        {
            continue;
        }

        if(button_msg.event != BUTTON_SINGLE_CLICK)
        {
            continue;
        }

        if(button_msg.button == BUTTON_A)
        {
            return true;
        }

        if(button_msg.button == BUTTON_B)
        {
            return false;
        }
    }
}

static bool menu_tv_off_send_all_(ir_tx_handle_t* transmitter, QueueHandle_t button_queue)
{
    const uint8_t total = (uint8_t)(sizeof(s_menu_tv_off_codes) / sizeof(s_menu_tv_off_codes[0]));

    menu_tv_off_draw_progress_(0U, &s_menu_tv_off_codes[0]);

    for(uint8_t i = 0U; i < total; ++i)
    {
        const menu_tv_off_code_t* code = &s_menu_tv_off_codes[i];
        menu_tv_off_draw_progress_(i, code);

        esp_err_t err = ir_tx_send(transmitter, code->protocol, code->address, code->command);
        if(err != ESP_OK)
        {
            menu_tv_off_draw_status_("SEND FAIL", ir_protocol_name(code->protocol), "B:STOP");
            vTaskDelay(pdMS_TO_TICKS(180U));
        }

        menu_tv_off_draw_progress_((uint8_t)(i + 1U), code);
        vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_SEND_GAP_MS));

        if(menu_tv_off_should_stop_(button_queue))
        {
            menu_tv_off_draw_status_("STOPPED", " ", "A:AGAIN B:BACK");
            return true;
        }
    }

    menu_tv_off_draw_progress_(total, NULL);
    return true;
}

static void menu_tv_off_task_(void* arg)
{
    ir_tx_handle_t transmitter = {0};
    ir_tx_config_t transmitter_cfg;
    QueueHandle_t button_queue = NULL;
    bool transmitter_ready = false;
    bool stay_on_done = true;

    (void)arg;

    button_queue = xQueueCreate(MENU_TV_OFF_BUTTON_QUEUE_DEPTH, sizeof(menu_tv_off_button_msg_t));
    if(button_queue == NULL)
    {
        menu_tv_off_draw_status_("No queue", "Try again", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_STATUS_HOLD_MS));
        menu_tv_off_exit_(button_queue);
        vTaskDelete(NULL);
        return;
    }

    if(!poom_sbus_subscribe_cb("input/button", menu_tv_off_button_cb_, button_queue))
    {
        menu_tv_off_draw_status_("No input", "Try again", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_STATUS_HOLD_MS));
        menu_tv_off_exit_(button_queue);
        vTaskDelete(NULL);
        return;
    }

    transmitter_cfg = ir_tx_default_config();
    transmitter_cfg.gpio = PIN_NUM_IR_TX;
    transmitter_cfg.clk_hz = MENU_TV_OFF_CLK_HZ;
    transmitter_cfg.carrier_hz = MENU_TV_OFF_CARRIER_HZ;
    transmitter_cfg.duty_cycle = MENU_TV_OFF_DUTY_CYCLE;

    if(ir_tx_init(&transmitter, &transmitter_cfg, "tv_off_tx") == ESP_OK)
    {
        transmitter_ready = true;
    }
    else
    {
        menu_tv_off_draw_status_("IR TX FAIL", " ", "B:BACK");
        vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_STATUS_HOLD_MS));
        stay_on_done = false;
    }

    while(transmitter_ready && stay_on_done)
    {
        (void)xQueueReset(button_queue);
        stay_on_done = menu_tv_off_send_all_(&transmitter, button_queue);
        if(stay_on_done)
        {
            stay_on_done = menu_tv_off_wait_after_done_(button_queue);
        }
    }

    if(transmitter_ready)
    {
        (void)ir_tx_deinit(&transmitter);
    }

    menu_tv_off_exit_(button_queue);
    vTaskDelete(NULL);
}

void menu_tv_off_show(void)
{
    if(xTaskCreate(menu_tv_off_task_,
                   "menu_tv_off",
                   MENU_TV_OFF_TASK_STACK_WORDS,
                   NULL,
                   MENU_TV_OFF_TASK_PRIORITY,
                   NULL) != pdPASS)
    {
        menu_tv_off_draw_status_("Task create", "failed", " ");
        vTaskDelay(pdMS_TO_TICKS(MENU_TV_OFF_STATUS_HOLD_MS));
        const uint8_t token = 1U;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
    }
}

#endif
