#include "poom_i2c_as5600.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduboy2.h"
#include "Sprites.h"
#include "button_driver.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"

// Logging
#define POOM_I2C_LOGGING
#ifdef POOM_I2C_LOGGING
    static const char *POOM_I2C_AS5600_TAG = "poom_i2c_as5600";

    #define POOM_I2C_AS5600_PRINTF_D(fmt, ...) \
        printf("[D] [%s] %s:%d: " fmt "\n", POOM_I2C_AS5600_TAG, __func__, __LINE__, ##__VA_ARGS__)
#else
    #define POOM_I2C_AS5600_PRINTF_D(fmt, ...) do {} while (0)
#endif

// Defines
#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef POOM_MENU_RESUME_TOPIC
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#endif

#define POOM_I2C_AS5600_TASK_STACK (3584U)
#define POOM_I2C_AS5600_TASK_PRIO (4U)

// Type definitions
typedef void (*poom_i2c_as5600_exit_cb_t)(void *user_ctx);

// Static variables
static TaskHandle_t s_poom_i2c_as5600_task = NULL;
static bool s_poom_i2c_as5600_running = false;
static bool s_poom_i2c_buttons_subscribed = false;
static bool s_poom_i2c_as5600_b_clicked = false;
static bool s_poom_i2c_as5600_exit_requested = false;
static char s_poom_i2c_as5600_sbus_user[] = "poom_i2c_as5600";
static poom_i2c_as5600_exit_cb_t s_poom_i2c_as5600_exit_cb;
static void *s_poom_i2c_as5600_exit_cb_ctx;

// Function prototypes
static void poom_i2c_as5600_exit_cb(void *user_ctx);
static void poom_i2c_as5600_button_cb(const poom_sbus_msg_t *msg, void *user_ctx);
static void poom_i2c_as5600_task(void *parameters); 
static void poom_i2c_as5600_cleanup(void *parameters);
static void poom_i2c_as5600_request_exit(void);
static void poom_i2c_as5600_reset(void);

// Entry point
esp_err_t poom_i2c_as5600_start(void) 
{
	if (s_poom_i2c_as5600_running) {
		return ESP_ERR_INVALID_STATE;
	}

	POOM_I2C_AS5600_PRINTF_D("Entering poom_i2c_as5600_start");
    poom_i2c_as5600_reset();

	if (!poom_sbus_subscribe_cb("input/button", poom_i2c_as5600_button_cb, s_poom_i2c_as5600_sbus_user))
    {
        return ESP_FAIL;
    }
    s_poom_i2c_buttons_subscribed = true;

    if (xTaskCreate(poom_i2c_as5600_task,
                    "poom_i2c_as5600",
                    POOM_I2C_AS5600_TASK_STACK,
                    NULL,
                    POOM_I2C_AS5600_TASK_PRIO,
                    &s_poom_i2c_as5600_task) != pdPASS)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", poom_i2c_as5600_button_cb, s_poom_i2c_as5600_sbus_user);
        s_poom_i2c_buttons_subscribed = false;
        s_poom_i2c_as5600_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    POOM_I2C_AS5600_PRINTF_D("Leaving poom_i2c_as5600_start");
    s_poom_i2c_as5600_running = true;
    return ESP_OK;
}

static void poom_i2c_as5600_task(void *parameters) {
	(void)parameters;

	for (;;) {
	    // Exit on button B
		if (s_poom_i2c_as5600_b_clicked) {
			s_poom_i2c_as5600_b_clicked = false;
			break;
		}

	    // Exit when exit is requested via exit callback
	    if (s_poom_i2c_as5600_exit_requested)
	    {
	        break;
	    }

		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	poom_i2c_as5600_cleanup(parameters);
	vTaskDelete(NULL);
}

static void poom_i2c_as5600_cleanup(void *parameters) {
	(void)parameters;

    poom_i2c_as5600_exit_cb_t exit_cb = s_poom_i2c_as5600_exit_cb;
    void *exit_ctx = s_poom_i2c_as5600_exit_cb_ctx;

	POOM_I2C_AS5600_PRINTF_D("Entering poom_i2c_as5600_cleanup");
	
	if (s_poom_i2c_buttons_subscribed) {
		(void)poom_sbus_unsubscribe_cb("input/button", poom_i2c_as5600_button_cb, s_poom_i2c_as5600_sbus_user);
	}

	s_poom_i2c_as5600_running = false;

    if (exit_cb != NULL)
    {
        exit_cb(exit_ctx);
    }
}

static void poom_i2c_as5600_button_cb(const poom_sbus_msg_t *msg, void *user_ctx)
{
	button_event_msg_t ev;

    (void)user_ctx;

    POOM_I2C_AS5600_PRINTF_D("Entering poom_i2c_as5600_button_cb");

    if ((msg == NULL) || (msg->len < sizeof(ev)))
    {
        return;
    }

    (void)memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event == BUTTON_SINGLE_CLICK)
    {
        if (ev.button == BTN_B)
        {
            s_poom_i2c_as5600_b_clicked = true;
        }
    }
}

static void poom_i2c_as5600_exit_cb(void *user_ctx)
{
    const uint8_t token = 1U;

    (void)user_ctx;

    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

esp_err_t poom_i2c_as5600_stop(void)
{
    if (!s_poom_i2c_as5600_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    poom_i2c_as5600_request_exit();
    return ESP_OK;
}

static void poom_i2c_as5600_request_exit(void)
{
    s_poom_i2c_as5600_exit_requested = true;
}

bool poom_i2c_as5600_is_running(void)
{
    return s_poom_i2c_as5600_running;
}

static void poom_i2c_as5600_set_exit_callback(poom_i2c_as5600_exit_cb_t callback, void *user_ctx)
{
    s_poom_i2c_as5600_exit_cb = callback;
    s_poom_i2c_as5600_exit_cb_ctx = user_ctx;
}

/// @brief Entry point for the poom menu
///
void app_poom_i2c_as5600_menu(void)
{
    poom_i2c_as5600_set_exit_callback(poom_i2c_as5600_exit_cb, NULL);
    (void)poom_i2c_as5600_start();
}

// reset all flags and state variables to their default state
static void poom_i2c_as5600_reset(void)
{
    s_poom_i2c_as5600_task = NULL;
    s_poom_i2c_as5600_running = false;
    s_poom_i2c_buttons_subscribed = false;
    s_poom_i2c_as5600_b_clicked = false;
    s_poom_i2c_as5600_exit_requested = false;
}