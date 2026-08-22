// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_ENABLED && CONFIG_OPENTHREAD_CLI

#include "menu_cli_ot.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Arduboy2.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "poom_sbus.h"
#include "soc/soc_caps.h"

#include "cli.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (2)
#define ROW0_Y (16)
#define ROW_STEP (8)

#define MENU_CLI_OT_REFRESH_MS (200U)
#define MENU_CLI_OT_UI_STACK (3072U)
#define MENU_CLI_OT_PRIO (4U)

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
} button_event_msg_t;

static const char *TAG = "menu_cli_ot";

static bool s_menu_cli_ot_active = false;
static bool s_menu_cli_ot_buttons_subscribed = false;
static bool s_menu_cli_ot_exit_requested = false;
static bool s_menu_cli_ot_started = false;
static bool s_menu_cli_ot_resume_poom_console = false;
static TaskHandle_t s_menu_cli_ot_task = NULL;
static esp_console_repl_t *s_menu_cli_ot_repl = NULL;
static char s_menu_cli_ot_sbus_user[] = "menu_cli_ot";

static void menu_cli_ot_draw_(void);
static void menu_cli_ot_task_(void *task_arg);
static void menu_cli_ot_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx);

/**
 * @brief Builds the default configuration used by this menu module.
 *
 * @return esp_openthread_radio_config_t
 */
static esp_openthread_radio_config_t menu_cli_ot_default_radio_config_(void)
{
    esp_openthread_radio_config_t config = {
        .radio_mode = RADIO_MODE_NATIVE,
    };

    return config;
}

/**
 * @brief Builds the default configuration used by this menu module.
 *
 * @return esp_openthread_host_connection_config_t
 */
static esp_openthread_host_connection_config_t menu_cli_ot_default_host_config_(void)
{
    esp_openthread_host_connection_config_t config = {
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,
    };

    return config;
}

/**
 * @brief Builds the default configuration used by this menu module.
 *
 * @return esp_openthread_port_config_t
 */
static esp_openthread_port_config_t menu_cli_ot_default_port_config_(void)
{
    esp_openthread_port_config_t config = {
        .storage_partition_name = "nvs",
        .netif_queue_size = 10,
        .task_queue_size = 10,
    };

    return config;
}

/**
 * @brief Initializes internal resources for this menu module.
 *
 * @return esp_err_t
 */
static esp_err_t menu_cli_ot_init_platform_(void)
{
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_event_loop_create_default failed");
    }

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_netif_init failed");
    }

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };
    err = esp_vfs_eventfd_register(&eventfd_config);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_vfs_eventfd_register failed");
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `menu_cli_ot_create_repl`.
 *
 * @return esp_err_t
 */
static esp_err_t menu_cli_ot_create_repl_(void)
{
    if (s_menu_cli_ot_repl != NULL)
    {
        return ESP_OK;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "poom_ot>";
    repl_config.max_cmdline_length = 256;
    repl_config.max_history_len = 10;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    return esp_console_new_repl_uart(&hw_config, &repl_config, &s_menu_cli_ot_repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &s_menu_cli_ot_repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    return esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &s_menu_cli_ot_repl);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Releases internal resources for this menu module.
 *
 * @return void
 */
static void menu_cli_ot_destroy_repl_(void)
{
    if (s_menu_cli_ot_repl == NULL)
    {
        return;
    }

    (void)s_menu_cli_ot_repl->del(s_menu_cli_ot_repl);
    s_menu_cli_ot_repl = NULL;
}

/**
 * @brief Stops the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_cli_ot_stop_stack_(void)
{
    if (!s_menu_cli_ot_started)
    {
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL)
    {
        esp_openthread_lock_acquire(portMAX_DELAY);
        (void)otThreadSetEnabled(instance, false);
        (void)otIp6SetEnabled(instance, false);
        esp_openthread_lock_release();
    }

    esp_err_t err = esp_openthread_stop();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        ESP_LOGW(TAG, "esp_openthread_stop failed: %s", esp_err_to_name(err));
    }

    s_menu_cli_ot_started = false;
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return esp_err_t
 */
static esp_err_t menu_cli_ot_start_session_(void)
{
    if (poom_console_is_active() && !poom_console_is_paused())
    {
        poom_console_pause();
        s_menu_cli_ot_resume_poom_console = true;
    }

    esp_err_t err = menu_cli_ot_init_platform_();
    if (err != ESP_OK)
    {
        goto fail;
    }

    err = menu_cli_ot_create_repl_();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OpenThread REPL create failed: %s", esp_err_to_name(err));
        goto fail;
    }

    const esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = menu_cli_ot_default_radio_config_(),
            .host_config = menu_cli_ot_default_host_config_(),
            .port_config = menu_cli_ot_default_port_config_(),
        },
    };

    err = esp_openthread_start(&config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_menu_cli_ot_started = true;

    err = esp_console_start_repl(s_menu_cli_ot_repl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
        goto fail;
    }

    return ESP_OK;

fail:
    menu_cli_ot_stop_stack_();
    menu_cli_ot_destroy_repl_();
    if (s_menu_cli_ot_resume_poom_console)
    {
        poom_console_resume();
        s_menu_cli_ot_resume_poom_console = false;
    }
    return err;
}

/**
 * @brief Stops the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_cli_ot_stop_session_(void)
{
    menu_cli_ot_destroy_repl_();
    menu_cli_ot_stop_stack_();

    if (s_menu_cli_ot_resume_poom_console)
    {
        poom_console_resume();
        s_menu_cli_ot_resume_poom_console = false;
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_cli_ot_draw_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(39, 2);
    (void)poom_arduboy_print(F("OT CLI"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
    (void)poom_arduboy_print(F("Open USB console"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
    (void)poom_arduboy_print(F("Prompt: poom_ot>"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
    (void)poom_arduboy_print(F("Type: ot help"));

    poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 3 * ROW_STEP));
    (void)poom_arduboy_print(F("B exits + stops OT"));

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_cli_ot_button_cb_(const poom_sbus_msg_t *msg, void *user_ctx)
{
    (void)user_ctx;

    if ((msg == NULL) || (msg->len < sizeof(button_event_msg_t)))
    {
        return;
    }

    button_event_msg_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if ((ev.event == BUTTON_SINGLE_CLICK) && (ev.button == BTN_B))
    {
        s_menu_cli_ot_exit_requested = true;
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_cli_ot_task_(void *task_arg)
{
    (void)task_arg;

    menu_cli_ot_draw_();
    while (s_menu_cli_ot_active)
    {
        if (s_menu_cli_ot_exit_requested)
        {
            s_menu_cli_ot_active = false;
            s_menu_cli_ot_exit_requested = false;

            if (s_menu_cli_ot_buttons_subscribed)
            {
                (void)poom_sbus_unsubscribe_cb("input/button", menu_cli_ot_button_cb_, s_menu_cli_ot_sbus_user);
                s_menu_cli_ot_buttons_subscribed = false;
            }

            menu_cli_ot_stop_session_();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_CLI_OT_REFRESH_MS));
    }

    s_menu_cli_ot_task = NULL;
    vTaskDelete(NULL);
}

void menu_cli_ot(void)
{
    if (s_menu_cli_ot_task != NULL)
    {
        return;
    }

    s_menu_cli_ot_active = true;
    s_menu_cli_ot_exit_requested = false;

    if (!s_menu_cli_ot_buttons_subscribed)
    {
        if (poom_sbus_subscribe_cb("input/button", menu_cli_ot_button_cb_, s_menu_cli_ot_sbus_user))
        {
            s_menu_cli_ot_buttons_subscribed = true;
        }
        else
        {
            s_menu_cli_ot_active = false;
            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if (menu_cli_ot_start_session_() != ESP_OK)
    {
        s_menu_cli_ot_active = false;
        if (s_menu_cli_ot_buttons_subscribed)
        {
            (void)poom_sbus_unsubscribe_cb("input/button", menu_cli_ot_button_cb_, s_menu_cli_ot_sbus_user);
            s_menu_cli_ot_buttons_subscribed = false;
        }

        const uint8_t token = 1;
        (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
        return;
    }

    (void)xTaskCreate(menu_cli_ot_task_,
                      "menu_cli_ot",
                      MENU_CLI_OT_UI_STACK,
                      NULL,
                      MENU_CLI_OT_PRIO,
                      &s_menu_cli_ot_task);
}

#endif
