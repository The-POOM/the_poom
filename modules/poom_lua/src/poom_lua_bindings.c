// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Arduboy2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buzzer.h"
#include "bsp_pong.h"
#include "esp_err.h"
#include "poom_led_rainbow.h"
#include "ws2812.h"

#include "poom_lua.h"
#include "poom_lua_input.h"

#include "lua.h"
#include "lauxlib.h"

void poom_lua_bindings_nfc_register(lua_State* L);
void poom_lua_bindings_nfc_cleanup(void);

static bool s_buzzer_inited = false;
static ws2812_strip_t s_led_strip;
static bool s_led_inited = false;
static bool s_led_rainbow_was_running = false;

/**
 * @brief Internal helper for `poom_lua_app_log`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_app_log_(lua_State* L)
{
    const char* msg = luaL_checkstring(L, 1);
    printf("[lua] %s\n", (msg != NULL) ? msg : "");
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_app_sleep`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_app_sleep_(lua_State* L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if(ms < 0)
    {
        ms = 0;
    }
    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_app_beep`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_app_beep_(lua_State* L)
{
    lua_Integer freq = luaL_checkinteger(L, 1);
    lua_Integer ms = luaL_checkinteger(L, 2);

    if(freq < 0)
    {
        freq = 0;
    }
    if(ms < 0)
    {
        ms = 0;
    }

    if(!s_buzzer_inited)
    {
        buzzer_init(PIN_NUM_BUZZER);
        s_buzzer_inited = true;
    }

    buzzer_tone((uint32_t)freq, (uint32_t)ms);
    return 0;
}

/**
 * @brief Releases internal state before leaving this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_app_exit_(lua_State* L)
{
    (void)poom_lua_request_stop();
    lua_pushstring(L, "stopped");
    return lua_error(L);
}

static const luaL_Reg k_app_lib[] = {
    {"log", poom_lua_app_log_},
    {"sleep", poom_lua_app_sleep_},
    {"beep", poom_lua_app_beep_},
    {"exit", poom_lua_app_exit_},
    {NULL, NULL},
};

/**
 * @brief Clears the internal state used by this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_clear_(lua_State* L)
{
    (void)L;
    poom_arduboy_clear();
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_display`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_display_(lua_State* L)
{
    (void)L;
    poom_arduboy_display();
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_set_text_size`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_set_text_size_(lua_State* L)
{
    lua_Integer size = luaL_checkinteger(L, 1);
    if(size < 0)
    {
        size = 0;
    }
    if(size > 3)
    {
        size = 3;
    }
    poom_arduboy_set_text_size((uint8_t)size);
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_set_cursor`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_set_cursor_(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    poom_arduboy_set_cursor((int16_t)x, (int16_t)y);
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_print`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_print_(lua_State* L)
{
    const char* s;
    size_t len = 0U;

    luaL_tolstring(L, 1, &len);
    s = lua_tostring(L, -1);
    (void)poom_arduboy_print((s != NULL) ? s : "");
    lua_pop(L, 1);
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_color_arg`.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] idx Parameter passed to the function.
 * @param[in] default_color Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t poom_lua_oled_color_arg_(lua_State* L, int idx, uint8_t default_color)
{
    if(lua_isnoneornil(L, idx))
    {
        return default_color;
    }

    lua_Integer v = luaL_checkinteger(L, idx);
    if(v < 0)
    {
        v = 0;
    }
    if(v > 2)
    {
        v = 2;
    }
    return (uint8_t)v;
}

/**
 * @brief Internal helper for `poom_lua_oled_fill_rect`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_fill_rect_(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer w = luaL_checkinteger(L, 3);
    lua_Integer h = luaL_checkinteger(L, 4);
    uint8_t color = poom_lua_oled_color_arg_(L, 5, WHITE);

    poom_arduboy_fill_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, color);
    return 0;
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_draw_rect_(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer w = luaL_checkinteger(L, 3);
    lua_Integer h = luaL_checkinteger(L, 4);
    uint8_t color = poom_lua_oled_color_arg_(L, 5, WHITE);

    poom_arduboy_draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, color);
    return 0;
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_draw_hline_(lua_State* L)
{
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    lua_Integer w = luaL_checkinteger(L, 3);
    uint8_t color = poom_lua_oled_color_arg_(L, 4, WHITE);

    poom_arduboy_draw_fast_hline((int16_t)x, (int16_t)y, (uint8_t)w, color);
    return 0;
}

/**
 * @brief Internal helper for `poom_lua_oled_width`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_width_(lua_State* L)
{
    lua_pushinteger(L, ARDUBOY_WIDTH);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_oled_height`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_oled_height_(lua_State* L)
{
    lua_pushinteger(L, ARDUBOY_HEIGHT);
    return 1;
}

static const luaL_Reg k_oled_lib[] = {
    {"clear", poom_lua_oled_clear_},
    {"display", poom_lua_oled_display_},
    {"textSize", poom_lua_oled_set_text_size_},
    {"setCursor", poom_lua_oled_set_cursor_},
    {"print", poom_lua_oled_print_},
    {"fillRect", poom_lua_oled_fill_rect_},
    {"drawRect", poom_lua_oled_draw_rect_},
    {"drawHLine", poom_lua_oled_draw_hline_},
    {"width", poom_lua_oled_width_},
    {"height", poom_lua_oled_height_},
    {NULL, NULL},
};

// ============================================================
// LED (WS2812)
// ============================================================

#define POOM_LUA_LED_RESOLUTION_HZ (10 * 1000 * 1000)
#define POOM_LUA_LED_IS_RGBW (false)
#define POOM_LUA_LED_DEFAULT_BRIGHTNESS (32U)

/**
 * @brief Internal helper for `poom_lua_led_restore_rainbow`.
 *
 * @return void
 */
static void poom_lua_led_restore_rainbow_(void)
{
    if (!s_led_inited)
    {
        return;
    }

    ws2812_clear(&s_led_strip);
    (void)ws2812_show(&s_led_strip);
    ws2812_deinit(&s_led_strip);
    s_led_inited = false;

    poom_led_rainbow_init();
    if (s_led_rainbow_was_running)
    {
        (void)poom_led_rainbow_start();
    }
    s_led_rainbow_was_running = false;
}

/**
 * @brief Internal helper for `poom_lua_led_ensure_inited`.
 *
 * @return bool
 */
static bool poom_lua_led_ensure_inited_(void)
{
    if (s_led_inited)
    {
        return true;
    }

    s_led_rainbow_was_running = poom_led_rainbow_deinit();

    if (ws2812_init(&s_led_strip, PIN_NUM_WS2812, PIN_NUM_LEDS, POOM_LUA_LED_IS_RGBW, POOM_LUA_LED_RESOLUTION_HZ) != ESP_OK)
    {
        poom_led_rainbow_init();
        if (s_led_rainbow_was_running)
        {
            (void)poom_led_rainbow_start();
        }
        s_led_rainbow_was_running = false;
        return false;
    }

    ws2812_set_brightness(&s_led_strip, POOM_LUA_LED_DEFAULT_BRIGHTNESS);
    ws2812_clear(&s_led_strip);
    (void)ws2812_show(&s_led_strip);
    s_led_inited = true;
    return true;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_init_(lua_State* L)
{
    lua_Integer brightness = luaL_optinteger(L, 1, (lua_Integer)POOM_LUA_LED_DEFAULT_BRIGHTNESS);
    if (brightness < 0)
    {
        brightness = 0;
    }
    if (brightness > 255)
    {
        brightness = 255;
    }

    bool ok = poom_lua_led_ensure_inited_();
    if (ok)
    {
        ws2812_set_brightness(&s_led_strip, (uint8_t)brightness);
    }

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_led_count`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_count_(lua_State* L)
{
    lua_pushinteger(L, (lua_Integer)PIN_NUM_LEDS);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_led_set_pixel`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_set_pixel_(lua_State* L)
{
    lua_Integer idx = luaL_checkinteger(L, 1);
    lua_Integer r = luaL_checkinteger(L, 2);
    lua_Integer g = luaL_checkinteger(L, 3);
    lua_Integer b = luaL_checkinteger(L, 4);
    lua_Integer w = luaL_optinteger(L, 5, 0);

    if (!poom_lua_led_ensure_inited_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    if ((idx < 0) || (idx >= (lua_Integer)s_led_strip.led_count))
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    if (r < 0)
    {
        r = 0;
    }
    if (g < 0)
    {
        g = 0;
    }
    if (b < 0)
    {
        b = 0;
    }
    if (w < 0)
    {
        w = 0;
    }
    if (r > 255)
    {
        r = 255;
    }
    if (g > 255)
    {
        g = 255;
    }
    if (b > 255)
    {
        b = 255;
    }
    if (w > 255)
    {
        w = 255;
    }

    ws2812_set_pixel(&s_led_strip, (int)idx, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_led_fill`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_fill_(lua_State* L)
{
    lua_Integer r = luaL_checkinteger(L, 1);
    lua_Integer g = luaL_checkinteger(L, 2);
    lua_Integer b = luaL_checkinteger(L, 3);
    lua_Integer w = luaL_optinteger(L, 4, 0);

    if (!poom_lua_led_ensure_inited_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    if (r < 0)
    {
        r = 0;
    }
    if (g < 0)
    {
        g = 0;
    }
    if (b < 0)
    {
        b = 0;
    }
    if (w < 0)
    {
        w = 0;
    }
    if (r > 255)
    {
        r = 255;
    }
    if (g > 255)
    {
        g = 255;
    }
    if (b > 255)
    {
        b = 255;
    }
    if (w > 255)
    {
        w = 255;
    }

    ws2812_fill(&s_led_strip, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Clears the internal state used by this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_clear_(lua_State* L)
{
    (void)L;
    if (!poom_lua_led_ensure_inited_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    ws2812_clear(&s_led_strip);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_led_brightness`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_brightness_(lua_State* L)
{
    lua_Integer br = luaL_checkinteger(L, 1);
    if (br < 0)
    {
        br = 0;
    }
    if (br > 255)
    {
        br = 255;
    }

    if (!poom_lua_led_ensure_inited_())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    ws2812_set_brightness(&s_led_strip, (uint8_t)br);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Internal helper for `poom_lua_led_show`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_show_(lua_State* L)
{
    if (!poom_lua_led_ensure_inited_())
    {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, (lua_Integer)ESP_FAIL);
        return 2;
    }

    esp_err_t err = ws2812_show(&s_led_strip);
    lua_pushboolean(L, (err == ESP_OK) ? 1 : 0);
    lua_pushinteger(L, (lua_Integer)err);
    return 2;
}

/**
 * @brief Initializes internal resources for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_led_deinit_(lua_State* L)
{
    (void)L;
    if (!s_led_inited)
    {
        lua_pushboolean(L, 1);
        return 1;
    }

    poom_lua_led_restore_rainbow_();
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg k_led_lib[] = {
    {"init", poom_lua_led_init_},
    {"deinit", poom_lua_led_deinit_},
    {"count", poom_lua_led_count_},
    {"setPixel", poom_lua_led_set_pixel_},
    {"fill", poom_lua_led_fill_},
    {"clear", poom_lua_led_clear_},
    {"brightness", poom_lua_led_brightness_},
    {"show", poom_lua_led_show_},
    {NULL, NULL},
};

void poom_lua_bindings_cleanup(void)
{
    poom_lua_led_restore_rainbow_();
    poom_lua_bindings_nfc_cleanup();
}

/**
 * @brief Internal helper for `poom_lua_buttons_poll`.
 *
 * @param[in] L Parameter passed to the function.
 * @return int
 */
static int poom_lua_buttons_poll_(lua_State* L)
{
    lua_Integer timeout_ms = luaL_optinteger(L, 1, 0);
    poom_lua_button_event_t ev;

    if(timeout_ms < 0)
    {
        timeout_ms = 0;
    }

    if(!poom_lua_buttons_poll(&ev, (uint32_t)timeout_ms))
    {
        return 0;
    }

    lua_pushinteger(L, (lua_Integer)ev.button);
    lua_pushinteger(L, (lua_Integer)ev.event);
    lua_pushinteger(L, (lua_Integer)ev.ts_ms);
    return 3;
}

static const luaL_Reg k_buttons_lib[] = {
    {"poll", poom_lua_buttons_poll_},
    {NULL, NULL},
};

void poom_lua_bindings_register(lua_State* L)
{
    if(L == NULL)
    {
        return;
    }

    luaL_newlib(L, k_app_lib);
    lua_setglobal(L, "App");

    luaL_newlib(L, k_oled_lib);
    lua_setglobal(L, "Oled");

    lua_pushinteger(L, WHITE);
    lua_setglobal(L, "WHITE");
    lua_pushinteger(L, BLACK);
    lua_setglobal(L, "BLACK");
    lua_pushinteger(L, INVERT);
    lua_setglobal(L, "INVERT");

    luaL_newlib(L, k_buttons_lib);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "A");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "B");
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "LEFT");
    lua_pushinteger(L, 3);
    lua_setfield(L, -2, "RIGHT");
    lua_pushinteger(L, 4);
    lua_setfield(L, -2, "UP");
    lua_pushinteger(L, 5);
    lua_setfield(L, -2, "DOWN");
    lua_pushinteger(L, 4);
    lua_setfield(L, -2, "SINGLE_CLICK");
    lua_setglobal(L, "Buttons");

    luaL_newlib(L, k_led_lib);
    lua_setglobal(L, "Led");
    poom_lua_bindings_nfc_register(L);
}
