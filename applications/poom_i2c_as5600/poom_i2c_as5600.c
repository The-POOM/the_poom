// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hugo Trippaers <hugo@trippaers.nl>

#include "poom_i2c_as5600.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "Sprites.h"
#include "button_driver.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_sbus.h"
#include "poom_i2c_as5600_driver.h"

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
#ifndef BTN_A
#define BTN_A (0U)
#endif

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BTN_LEFT
#define BTN_LEFT (2U)
#endif

#ifndef BTN_RIGHT
#define BTN_RIGHT (3U)
#endif

#ifndef POOM_MENU_RESUME_TOPIC
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"
#endif

#define POOM_I2C_AS5600_TASK_STACK (3584U)
#define POOM_I2C_AS5600_TASK_PRIO (4U)

// Type definitions
typedef void (*poom_i2c_as5600_exit_cb_t)(void *user_ctx);
typedef struct
{
    bool detected;
    bool validated;
    bool error_detected;
    bool magnet_detected;
    bool magnet_ml;
    bool magnet_mh;
    uint8_t agc;
    uint16_t magnitude;
    uint16_t raw_angle;
    as5600_conf_t conf;
    uint8_t zmco;
    uint16_t zpos;
    uint16_t mpos;
    uint16_t mang;
} as5600_state_t;
typedef enum
{
    Detect,
    Magnet,
    Angle,
    Filter,
    Config
} poom_i2c_as5600_page_t;
typedef struct
{
    const char *name;
    uint8_t sf;
    uint8_t hyst;
    uint8_t fth;
} poom_i2c_as5600_filter_preset_t;

// Filter presets cycled with A on the Filter page. Writes are
// volatile, so cycling through these never changes the sensor
// permanently.
static const poom_i2c_as5600_filter_preset_t s_poom_i2c_as5600_presets[] = {
    {"DEFAULT",  0U, 0U, 0U},   // 16x slow filter, no fast path, no hyst
    {"STABLE",   0U, 3U, 0U},   // as DEFAULT plus max hysteresis
    {"BALANCED", 1U, 1U, 3U},   // 8x filter, fast path at 9 LSB
    {"FAST",     3U, 0U, 7U},   // 2x filter, fast path at 10 LSB
};
#define POOM_I2C_AS5600_PRESET_COUNT \
    (sizeof(s_poom_i2c_as5600_presets) / sizeof(s_poom_i2c_as5600_presets[0]))

// CONF field decode tables (indexes are the raw datasheet codes)
static const char *const s_poom_i2c_as5600_pm_str[]   = {"NOM", "LPM1", "LPM2", "LPM3"};
static const char *const s_poom_i2c_as5600_outs_str[] = {"ANALOG", "ANLG90", "PWM", "RSVD"};
static const char *const s_poom_i2c_as5600_pwmf_str[] = {"115HZ", "230HZ", "460HZ", "920HZ"};
static const char *const s_poom_i2c_as5600_sf_str[]   = {"16X", "8X", "4X", "2X"};
static const char *const s_poom_i2c_as5600_fth_str[]  = {"OFF", "6", "7", "9", "18", "21", "24", "10"};

// Static variables
static TaskHandle_t s_poom_i2c_as5600_task = NULL;
static bool s_poom_i2c_as5600_running = false;
static bool s_poom_i2c_buttons_subscribed = false;
static bool s_poom_i2c_as5600_b_clicked = false;
static bool s_poom_i2c_as5600_exit_requested = false;
static char s_poom_i2c_as5600_sbus_user[] = "poom_i2c_as5600";
static poom_i2c_as5600_exit_cb_t s_poom_i2c_as5600_exit_cb;
static void *s_poom_i2c_as5600_exit_cb_ctx;
static as5600_state_t s_poom_i2c_as5600_state;
static poom_i2c_as5600_page_t s_poom_i2c_as5600_page = Detect;
static bool s_poom_i2c_as5600_a_clicked = false;
static int8_t s_poom_i2c_as5600_preset_idx = -1;
// Jitter tracking on the Filter page: min/max wrapped delta against a
// reference sample, reset on page entry and on preset change.
static bool s_poom_i2c_as5600_jitter_valid = false;
static uint16_t s_poom_i2c_as5600_jitter_ref = 0U;
static int16_t s_poom_i2c_as5600_jitter_min = 0;
static int16_t s_poom_i2c_as5600_jitter_max = 0;

// Function prototypes
static void poom_i2c_as5600_exit_cb(void *user_ctx);
static void poom_i2c_as5600_button_cb(const poom_sbus_msg_t *msg, void *user_ctx);
static void poom_i2c_as5600_task(void *parameters); 
static void poom_i2c_as5600_cleanup(void *parameters);
static void poom_i2c_as5600_request_exit(void);
static void poom_i2c_as5600_reset(void);
static void poom_i2c_as5600_update_state(as5600_state_t *state);
static void poom_i2c_as5600_draw_detect(void);
static void poom_i2c_as5600_draw_magnet(void);
static void poom_i2c_as5600_draw_angle(void);
static void poom_i2c_as5600_draw_filter(void);
static void poom_i2c_as5600_draw_config(void);
static void poom_i2c_as5600_apply_filter_preset(uint8_t idx);
static void poom_i2c_as5600_update_jitter(uint16_t raw_angle);
static poom_i2c_as5600_page_t next_page(poom_i2c_as5600_page_t current);
static poom_i2c_as5600_page_t previous_page(poom_i2c_as5600_page_t current);

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

    as5600_init();

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

	    poom_i2c_as5600_update_state(&s_poom_i2c_as5600_state);

	    // Switch back to detect on error
	    if (!s_poom_i2c_as5600_state.detected || !s_poom_i2c_as5600_state.validated)
	    {
	        s_poom_i2c_as5600_page = Detect;
	    }

	    // Filter page interactions: A applies the next preset,
	    // jitter accumulates while the page is shown
	    if ((s_poom_i2c_as5600_page == Filter) && s_poom_i2c_as5600_state.validated)
	    {
	        if (s_poom_i2c_as5600_a_clicked)
	        {
	            s_poom_i2c_as5600_a_clicked = false;
	            s_poom_i2c_as5600_preset_idx =
	                (int8_t)((s_poom_i2c_as5600_preset_idx + 1) % (int8_t)POOM_I2C_AS5600_PRESET_COUNT);
	            poom_i2c_as5600_apply_filter_preset((uint8_t)s_poom_i2c_as5600_preset_idx);
	            s_poom_i2c_as5600_jitter_valid = false;
	        }
	        poom_i2c_as5600_update_jitter(s_poom_i2c_as5600_state.raw_angle);
	    }
	    else
	    {
	        s_poom_i2c_as5600_a_clicked = false;
	        s_poom_i2c_as5600_jitter_valid = false;
	    }

	    switch (s_poom_i2c_as5600_page)
	    {
	        case Detect:
	        {
	            poom_i2c_as5600_draw_detect();
	            break;
	        }
	        case Magnet:
	        {
	            poom_i2c_as5600_draw_magnet();
	            break;
	        }
	        case Angle:
	        {
	            poom_i2c_as5600_draw_angle();
	            break;
	        }
	        case Filter:
	        {
	            poom_i2c_as5600_draw_filter();
	            break;
	        }
	        case Config:
	        {
	            poom_i2c_as5600_draw_config();
	            break;
	        }
	    }

		vTaskDelay(pdMS_TO_TICKS(100));
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

static poom_i2c_as5600_page_t next_page(poom_i2c_as5600_page_t current)
{
    switch (current)
    {
        case Detect:
            return Magnet;
        case Magnet:
            return Angle;
        case Angle:
            return Filter;
        case Filter:
            return Config;
        default:
            return current;
    }
}

static poom_i2c_as5600_page_t previous_page(poom_i2c_as5600_page_t current)
{
    switch (current)
    {
        case Magnet:
            return Detect;
        case Angle:
            return Magnet;
        case Filter:
            return Angle;
        case Config:
            return Filter;
        default:
            return current;
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
        else if (ev.button == BTN_A)
        {
            s_poom_i2c_as5600_a_clicked = true;
        }
        else if (ev.button == BTN_RIGHT)
        {
            s_poom_i2c_as5600_page = next_page(s_poom_i2c_as5600_page);
        }
        else if (ev.button == BTN_LEFT)
        {
            s_poom_i2c_as5600_page = previous_page(s_poom_i2c_as5600_page);
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
    s_poom_i2c_as5600_a_clicked = false;
    s_poom_i2c_as5600_page = Detect;
    s_poom_i2c_as5600_preset_idx = -1;
    s_poom_i2c_as5600_jitter_valid = false;
    memset(&s_poom_i2c_as5600_state, 0x00, sizeof(as5600_state_t));
}

static void poom_i2c_as5600_update_state(as5600_state_t *state)
{
    state->validated = false;

    state->detected = as5600_detect_presence();
    if (!state->detected) return;

    uint8_t status;
    if (!as5600_read_status(&status))
    {
        state->error_detected = true;
        return;
    };

    state->magnet_detected = status & 1 << 5;
    state->magnet_ml = status & 1 << 4;
    state->magnet_mh = status & 1 << 3;

    if (!as5600_read_agc(&state->agc))
    {
        state->error_detected = true;
        return;
    };

    if (!as5600_read_magnitude(&state->magnitude))
    {
        state->error_detected = true;
        return;
    };

    if (!as5600_read_raw_angle(&state->raw_angle))
    {
        state->error_detected = true;
        return;
    }

    if (!as5600_read_conf(&state->conf) ||
        !as5600_read_zmco(&state->zmco) ||
        !as5600_read_zpos(&state->zpos) ||
        !as5600_read_mpos(&state->mpos) ||
        !as5600_read_mang(&state->mang))
    {
        state->error_detected = true;
        return;
    }

    // For now validated means we were able to read some of the
    // key registers
    state->validated = true;
}

// Draws the detect screen indicating if a
// AS5600 is connected to the QWIIC port
static void poom_i2c_as5600_draw_detect(void)
{
    char line[22];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    // header bar
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print("AS5600 I2C");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    // Detection
    if (s_poom_i2c_as5600_state.detected)
    {
        (void)snprintf(line, sizeof(line), "I2C 0x36: PRESENT");
    } else
    {
        (void)snprintf(line, sizeof(line), "I2C 0x36: NO REPLY");
    }
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(line);

    // Validation
    if (s_poom_i2c_as5600_state.detected)
    {
        if (s_poom_i2c_as5600_state.validated)
        {
            (void)snprintf(line, sizeof(line), "AS5600  : DETECTED");
        } else
        {
            (void)snprintf(line, sizeof(line), "AS5600  : INVALID");
        }
        poom_arduboy_set_cursor(4, 30);
        (void)poom_arduboy_print(line);
    }

    // footer
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print("</>:PAGE       B:EXIT");

    poom_arduboy_display();
}

static void poom_i2c_as5600_draw_magnet(void)
{
    char line[22];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    // header bar
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print("AS5600 I2C");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    if (s_poom_i2c_as5600_state.magnet_detected)
    {
        if (s_poom_i2c_as5600_state.magnet_mh)
        {
            (void)snprintf(line, sizeof(line), "MAGNET: TOO STRONG");

        }
        else if (s_poom_i2c_as5600_state.magnet_ml)
        {
            (void)snprintf(line, sizeof(line), "MAGNET: TOO WEAK");
        }
        else
        {
            (void)snprintf(line, sizeof(line), "MAGNET: OK");
        }
    }
    else
    {
        (void)snprintf(line, sizeof(line), "MAGNET: ---");
    }
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "AGC: %04d",
        s_poom_i2c_as5600_state.agc);
    poom_arduboy_set_cursor(4, 30);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "MAG: %04d",
        s_poom_i2c_as5600_state.magnitude);
    poom_arduboy_set_cursor(4, 40);
    (void)poom_arduboy_print(line);

    // footer
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print("</>:PAGE       B:EXIT");

    poom_arduboy_display();
}

static void poom_i2c_as5600_draw_angle(void)
{
    char line[22];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    // header bar
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print("AS5600 I2C");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    (void)snprintf(line, sizeof(line), "RAW: %4d",
        s_poom_i2c_as5600_state.raw_angle);
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "ANGLE: %5.1f",
        as5600_raw_to_degrees(s_poom_i2c_as5600_state.raw_angle));
    poom_arduboy_set_cursor(4, 30);
    (void)poom_arduboy_print(line);

    // footer
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print("</>:PAGE       B:EXIT");

    poom_arduboy_display();
}

// Applies a filter preset on top of the current CONF, preserving the
// unrelated fields (PM, WD, OUTS, PWMF). Volatile write.
static void poom_i2c_as5600_apply_filter_preset(uint8_t idx)
{
    as5600_conf_t conf;
    const poom_i2c_as5600_filter_preset_t *preset = &s_poom_i2c_as5600_presets[idx];

    if (!as5600_read_conf(&conf))
    {
        POOM_I2C_AS5600_PRINTF_D("preset %s: CONF read failed", preset->name);
        return;
    }

    conf.sf = preset->sf;
    conf.hyst = preset->hyst;
    conf.fth = preset->fth;

    if (!as5600_write_conf(&conf))
    {
        POOM_I2C_AS5600_PRINTF_D("preset %s: CONF write failed", preset->name);
    }
}

// Tracks the spread of the raw angle against a reference sample using
// the wrapped 12-bit delta, so it also works across the 4095/0 seam.
static void poom_i2c_as5600_update_jitter(uint16_t raw_angle)
{
    int16_t delta;

    if (!s_poom_i2c_as5600_jitter_valid)
    {
        s_poom_i2c_as5600_jitter_ref = raw_angle;
        s_poom_i2c_as5600_jitter_min = 0;
        s_poom_i2c_as5600_jitter_max = 0;
        s_poom_i2c_as5600_jitter_valid = true;
        return;
    }

    delta = (int16_t)((((int32_t)raw_angle - (int32_t)s_poom_i2c_as5600_jitter_ref + 2048) & 4095) - 2048);
    if (delta < s_poom_i2c_as5600_jitter_min)
    {
        s_poom_i2c_as5600_jitter_min = delta;
    }
    if (delta > s_poom_i2c_as5600_jitter_max)
    {
        s_poom_i2c_as5600_jitter_max = delta;
    }
}

// Filter tuning page: A cycles SF/HYST/FTH presets (volatile), the
// jitter line shows the observed raw-angle spread since the last
// preset change. Hold the shaft still to compare presets.
static void poom_i2c_as5600_draw_filter(void)
{
    char line[22];
    const as5600_conf_t *conf = &s_poom_i2c_as5600_state.conf;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    // header bar
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print("AS5600 I2C");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    if (s_poom_i2c_as5600_preset_idx >= 0)
    {
        (void)snprintf(line, sizeof(line), "PRESET: %s",
            s_poom_i2c_as5600_presets[s_poom_i2c_as5600_preset_idx].name);
    }
    else
    {
        (void)snprintf(line, sizeof(line), "PRESET: AS-IS");
    }
    poom_arduboy_set_cursor(4, 16);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "SF:%s HYS:%u FTH:%s",
        s_poom_i2c_as5600_sf_str[conf->sf],
        (unsigned)conf->hyst,
        s_poom_i2c_as5600_fth_str[conf->fth]);
    poom_arduboy_set_cursor(4, 25);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "ANGLE: %5.1f",
        as5600_raw_to_degrees(s_poom_i2c_as5600_state.raw_angle));
    poom_arduboy_set_cursor(4, 34);
    (void)poom_arduboy_print(line);

    if (s_poom_i2c_as5600_jitter_valid)
    {
        (void)snprintf(line, sizeof(line), "JITTER: %d LSB",
            (int)(s_poom_i2c_as5600_jitter_max - s_poom_i2c_as5600_jitter_min));
    }
    else
    {
        (void)snprintf(line, sizeof(line), "JITTER: ---");
    }
    poom_arduboy_set_cursor(4, 43);
    (void)poom_arduboy_print(line);

    // footer
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print("A:SET </>:PAGE B:EXIT");

    poom_arduboy_display();
}

// Configuration page: the CONF fields not covered by the filter page
// plus the programmed position registers.
static void poom_i2c_as5600_draw_config(void)
{
    char line[22];
    const as5600_state_t *state = &s_poom_i2c_as5600_state;
    uint16_t range_raw;

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    // header bar
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print("AS5600 I2C");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    (void)snprintf(line, sizeof(line), "PM:%s  WD:%s",
        s_poom_i2c_as5600_pm_str[state->conf.pm],
        state->conf.wd ? "ON" : "OFF");
    poom_arduboy_set_cursor(4, 14);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "OUT:%s PWM:%s",
        s_poom_i2c_as5600_outs_str[state->conf.outs],
        s_poom_i2c_as5600_pwmf_str[state->conf.pwmf]);
    poom_arduboy_set_cursor(4, 22);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "ZMCO:%u ZPOS:%04u",
        (unsigned)state->zmco, (unsigned)state->zpos);
    poom_arduboy_set_cursor(4, 30);
    (void)poom_arduboy_print(line);

    (void)snprintf(line, sizeof(line), "MPOS:%04u MANG:%04u",
        (unsigned)state->mpos, (unsigned)state->mang);
    poom_arduboy_set_cursor(4, 38);
    (void)poom_arduboy_print(line);

    // Effective ANGLE range: MANG wins, else the ZPOS..MPOS span,
    // else the full turn
    if (state->mang != 0U)
    {
        range_raw = state->mang;
    }
    else
    {
        range_raw = (uint16_t)(((uint32_t)state->mpos - (uint32_t)state->zpos) & 4095U);
    }
    (void)snprintf(line, sizeof(line), "RANGE: %5.1f DEG",
        (range_raw == 0U) ? 360.0f : as5600_raw_to_degrees(range_raw));
    poom_arduboy_set_cursor(4, 46);
    (void)poom_arduboy_print(line);

    // footer
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print("</>:PAGE       B:EXIT");

    poom_arduboy_display();
}