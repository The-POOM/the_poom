// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_http_load_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "poom_http_load_test.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_HTTP_LOAD_OLED_COL (6)
#define MENU_HTTP_LOAD_BOX_Y (10)
#define MENU_HTTP_LOAD_BOX_H (50)
#define MENU_HTTP_LOAD_REFRESH_MS (250U)
#define MENU_HTTP_LOAD_STATUS_STACK (3072U)
#define MENU_HTTP_LOAD_STATUS_PRIO (4U)

#define MENU_HTTP_LOAD_WIFI_SSID_MAX_LEN (64U)
#define MENU_HTTP_LOAD_WIFI_PASS_MAX_LEN (128U)
#define MENU_HTTP_LOAD_HOST_MAX_LEN (128U)
#define MENU_HTTP_LOAD_PORT_MAX_LEN (16U)
#define MENU_HTTP_LOAD_PATH_MAX_LEN (128U)
#define MENU_HTTP_LOAD_DEFAULT_WORKERS (8U)
#define MENU_HTTP_LOAD_MAX_WORKERS (16U)

#define MENU_HTTP_LOAD_KEY_HOST "ld_host"
#define MENU_HTTP_LOAD_KEY_PORT "ld_port"
#define MENU_HTTP_LOAD_KEY_PATH "ld_path"
#define MENU_HTTP_LOAD_KEY_WORKERS "ld_workers"

#ifndef BTN_A
#define BTN_A (0U)
#endif

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_http_load_button_msg_t;

static bool s_menu_http_load_active = false;
static bool s_menu_http_load_running = false;
static bool s_menu_http_load_buttons_subscribed = false;
static bool s_menu_http_load_exit_requested = false;
static TaskHandle_t s_menu_http_load_status_task = NULL;
static char s_menu_http_load_sbus_user[] = "menu_http_load_test";
static char s_menu_http_load_status[22] = "Press A to run";

static esp_err_t menu_http_load_test_exit_(void);
static void menu_http_load_test_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);

/**
 * @brief Draws the current menu state.
 *
 * @param[in] wifi_ready Parameter passed to the helper.
 * @param[in] running Parameter passed to the helper.
 * @param[in] status_text Parameter passed to the helper.
 * @return esp_err_t
 */
static esp_err_t menu_http_load_test_draw_(bool wifi_ready, bool running, const char* status_text)
{
    char line_wifi[22];
    char line_test[22];
    char line_status[22];

    (void)snprintf(line_wifi, sizeof(line_wifi), "WiFi:%s", wifi_ready ? "READY" : "NOIP");
    (void)snprintf(line_test, sizeof(line_test), "Load:%s", running ? "RUNNING" : "PAUSED");
    (void)snprintf(line_status, sizeof(line_status), "%.18s", (status_text != NULL) ? status_text : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(22, 2);
    (void)poom_arduboy_print(F("HTTP LOAD"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 40, WHITE);

    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line_wifi);

    const int16_t y_load = 26;
    poom_arduboy_set_cursor(4, y_load);
    (void)poom_arduboy_print(line_test);
    if(running)
    {
        poom_arduboy_fill_rect(1, (int16_t)(y_load - 1), ARDUBOY_WIDTH - 2, 9, INVERT);
    }

    poom_arduboy_set_cursor(4, 38);
    (void)poom_arduboy_print(line_status);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:TOGGLE"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] key Parameter passed to the helper.
 * @param[in] out_value Parameter passed to the helper.
 * @param[in] out_len Parameter passed to the helper.
 * @return esp_err_t
 */
static esp_err_t menu_http_load_test_read_str_(const char* key, char* out_value, size_t out_len)
{
    size_t value_len = out_len;
    esp_err_t status;

    if((key == NULL) || (out_value == NULL) || (out_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    status = poom_secrets_get_str(key, out_value, &value_len);
    if(status != ESP_OK)
    {
        return status;
    }

    return ESP_OK;
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return esp_err_t
 */
static esp_err_t menu_http_load_test_start_from_secrets_(void)
{
    char wifi_ssid[MENU_HTTP_LOAD_WIFI_SSID_MAX_LEN];
    char wifi_pass[MENU_HTTP_LOAD_WIFI_PASS_MAX_LEN];
    char host[MENU_HTTP_LOAD_HOST_MAX_LEN];
    char port[MENU_HTTP_LOAD_PORT_MAX_LEN];
    char path[MENU_HTTP_LOAD_PATH_MAX_LEN];
    size_t wifi_ssid_len;
    size_t wifi_pass_len;
    uint32_t workers = MENU_HTTP_LOAD_DEFAULT_WORKERS;
    esp_err_t status;
    poom_http_load_test_config_t cfg;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "NVS error");
        return status;
    }

    wifi_ssid_len = sizeof(wifi_ssid);
    status = poom_secrets_get_wifi_ssid(wifi_ssid, &wifi_ssid_len);
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Set WiFi in CLI");
        return status;
    }

    wifi_pass_len = sizeof(wifi_pass);
    status = poom_secrets_get_wifi_pass(wifi_pass, &wifi_pass_len);
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Set WiFi in CLI");
        return status;
    }

    status = menu_http_load_test_read_str_(MENU_HTTP_LOAD_KEY_HOST, host, sizeof(host));
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Set target CLI");
        return status;
    }

    status = menu_http_load_test_read_str_(MENU_HTTP_LOAD_KEY_PORT, port, sizeof(port));
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Set target CLI");
        return status;
    }

    status = menu_http_load_test_read_str_(MENU_HTTP_LOAD_KEY_PATH, path, sizeof(path));
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Set target CLI");
        return status;
    }

    status = poom_secrets_get_u32(MENU_HTTP_LOAD_KEY_WORKERS, &workers);
    if((status != ESP_OK) || (workers == 0U) || (workers > MENU_HTTP_LOAD_MAX_WORKERS))
    {
        workers = MENU_HTTP_LOAD_DEFAULT_WORKERS;
    }

    cfg.ssid = wifi_ssid;
    cfg.password = wifi_pass;
    cfg.host = host;
    cfg.port = port;
    cfg.path = path;
    cfg.worker_count = (uint8_t)workers;

    status = poom_http_load_test_start(&cfg);
    if(status != ESP_OK)
    {
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Start failed");
        return status;
    }

    s_menu_http_load_running = true;
    (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Running");
    return ESP_OK;
}

/**
 * @brief Toggles the current runtime state.
 *
 * @return void
 */
static void menu_http_load_test_toggle_(void)
{
    if(s_menu_http_load_running)
    {
        poom_http_load_test_stop();
        s_menu_http_load_running = false;
        (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Paused");
    }
    else
    {
        (void)menu_http_load_test_start_from_secrets_();
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_http_load_test_status_task_(void* task_arg)
{
    bool wifi_ready;

    (void)task_arg;

    while(s_menu_http_load_active)
    {
        if(s_menu_http_load_exit_requested)
        {
            (void)menu_http_load_test_exit_();
            break;
        }

        wifi_ready = poom_wifi_ctrl_sta_has_ip();
        (void)menu_http_load_test_draw_(wifi_ready, s_menu_http_load_running, s_menu_http_load_status);
        vTaskDelay(pdMS_TO_TICKS(MENU_HTTP_LOAD_REFRESH_MS));
    }

    s_menu_http_load_status_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_http_load_test_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_http_load_active = false;
    s_menu_http_load_exit_requested = false;

    poom_http_load_test_stop();
    s_menu_http_load_running = false;

    if(s_menu_http_load_status_task != NULL)
    {
        if(s_menu_http_load_status_task != current_task)
        {
            TaskHandle_t status_task = s_menu_http_load_status_task;
            s_menu_http_load_status_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_http_load_status_task = NULL;
        }
    }

    if(s_menu_http_load_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_http_load_test_button_cb_, s_menu_http_load_sbus_user);
        s_menu_http_load_buttons_subscribed = false;
    }


    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_http_load_test_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_http_load_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if(button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if(button_msg.button == BTN_B)
    {
        if(s_menu_http_load_status_task == NULL)
        {
            (void)menu_http_load_test_exit_();
        }
        else
        {
            s_menu_http_load_exit_requested = true;
        }
        return;
    }

    if(button_msg.button == BTN_A)
    {
        menu_http_load_test_toggle_();
    }
}

void menu_http_load_test_show(void)
{
    s_menu_http_load_active = true;
    s_menu_http_load_running = false;
    s_menu_http_load_exit_requested = false;
    (void)snprintf(s_menu_http_load_status, sizeof(s_menu_http_load_status), "Press A to run");

    if(!s_menu_http_load_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_http_load_test_button_cb_, s_menu_http_load_sbus_user))
        {
            s_menu_http_load_buttons_subscribed = true;
        }
        else
        {
            s_menu_http_load_active = false;
            (void)menu_http_load_test_draw_(false, false, "Button sub error");

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_http_load_status_task == NULL)
    {
        (void)xTaskCreate(menu_http_load_test_status_task_,
                          "menu_http_load",
                          MENU_HTTP_LOAD_STATUS_STACK,
                          NULL,
                          MENU_HTTP_LOAD_STATUS_PRIO,
                          &s_menu_http_load_status_task);
    }
}
