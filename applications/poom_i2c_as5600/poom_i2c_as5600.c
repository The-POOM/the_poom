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
static const char *POOM_I2C_AS5600_TAG = "poom_i2c_as5600";

#define POOM_I2C_AS5600_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_I2C_AS5600_TAG, __func__, __LINE__, ##__VA_ARGS__)


// Defines
#ifndef BTN_B
#define BTN_B (1U)
#endif

#define POOM_I2C_AS5600_TASK_STACK (1024U)
#define POOM_I2C_AS5600_TASK_PRIO (4U)

// Static variables
static TaskHandle_t s_poom_i2c_as5600_task = NULL;
static bool s_poom_i2c_as5600_running = false;
static bool s_poom_i2c_buttons_subscribed = false;
static bool s_poom_i2c_as5600_b_clicked = false;
static char s_poom_i2c_as5600_sbus_user[] = "poom_i2c_as5600";

// Function prototypes
static void poom_i2c_as5600_button_cb(const poom_sbus_msg_t *msg, void *user_ctx);
static void poom_i2c_as5600_task(void *parameters); 
static void poom_i2c_as5600_cleanup(void *parameters);

// Entry point
esp_err_t poom_i2c_as5600_start(void) 
{
	if (s_poom_i2c_as5600_running) {
		return ESP_ERR_INVALID_STATE;
	}

	POOM_I2C_AS5600_PRINTF_D("Entering poom_i2c_as5600_start");

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
		if (s_poom_i2c_as5600_b_clicked) {
			s_poom_i2c_as5600_b_clicked = false;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	poom_i2c_as5600_cleanup(parameters);
	vTaskDelete(NULL);
}

static void poom_i2c_as5600_cleanup(void *parameters) {
	(void)parameters;

	POOM_I2C_AS5600_PRINTF_D("Entering poom_i2c_as5600_cleanup");
	
	if (s_poom_i2c_buttons_subscribed) {
		(void)poom_sbus_unsubscribe_cb("input/button", poom_i2c_as5600_button_cb, s_poom_i2c_as5600_sbus_user);
	}

	s_poom_i2c_as5600_running = false;
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