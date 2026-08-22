// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_lua.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "sd_card.h"
#include "poom_sbus.h"
#include "poom_lua_input.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

void poom_lua_bindings_register(lua_State* L);
void poom_lua_bindings_cleanup(void);

#define POOM_LUA_TASK_STACK_WORDS (8192U) /* ~32KB */
#define POOM_LUA_TASK_PRIO (3U)
#define POOM_LUA_ERR_MAX (192U)
#define POOM_LUA_BTN_QUEUE_LEN (12U)

typedef struct
{
    char* path;
    poom_lua_done_cb_t done_cb;
    void* done_ctx;
} poom_lua_task_ctx_t;

static TaskHandle_t s_lua_task = NULL;
static bool s_lua_running = false;
static volatile bool s_lua_stop_requested = false;
static bool s_lua_buttons_subscribed = false;
static QueueHandle_t s_lua_button_q = NULL;
static char s_lua_sbus_user[] = "poom_lua";

#ifndef BTN_B
#define BTN_B (1U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

/**
 * @brief Handles button events for this module.
 *
 * @param[in] msg Parameter passed to the function.
 * @param[in] user_ctx Parameter passed to the function.
 * @return void
 */
static void poom_lua_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(poom_lua_button_event_t)))
    {
        return;
    }

    poom_lua_button_event_t ev;
    (void)memcpy(&ev, msg->data, sizeof(ev));

    if((ev.button == BTN_B) && (ev.event == BUTTON_SINGLE_CLICK))
    {
        s_lua_stop_requested = true;
    }

    if(s_lua_button_q == NULL)
    {
        return;
    }

    if(xQueueSend(s_lua_button_q, &ev, 0) != pdTRUE)
    {
        poom_lua_button_event_t dropped;
        (void)xQueueReceive(s_lua_button_q, &dropped, 0);
        (void)xQueueSend(s_lua_button_q, &ev, 0);
    }
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @return esp_err_t
 */
static esp_err_t poom_lua_buttons_start_(void)
{
    if(s_lua_button_q == NULL)
    {
        s_lua_button_q = xQueueCreate(POOM_LUA_BTN_QUEUE_LEN, sizeof(poom_lua_button_event_t));
        if(s_lua_button_q == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if(!s_lua_buttons_subscribed)
    {
        if(!poom_sbus_subscribe_cb("input/button", poom_lua_button_cb_, s_lua_sbus_user))
        {
            vQueueDelete(s_lua_button_q);
            s_lua_button_q = NULL;
            return ESP_FAIL;
        }
        s_lua_buttons_subscribed = true;
    }

    return ESP_OK;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @return void
 */
static void poom_lua_buttons_stop_(void)
{
    if(s_lua_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", poom_lua_button_cb_, s_lua_sbus_user);
        s_lua_buttons_subscribed = false;
    }

    if(s_lua_button_q != NULL)
    {
        vQueueDelete(s_lua_button_q);
        s_lua_button_q = NULL;
    }
}

bool poom_lua_buttons_poll(poom_lua_button_event_t* out_event, uint32_t timeout_ms)
{
    TickType_t to_ticks;

    if((out_event == NULL) || (s_lua_button_q == NULL))
    {
        return false;
    }

    to_ticks = (timeout_ms == 0U) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive(s_lua_button_q, out_event, to_ticks) == pdTRUE);
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] L Parameter passed to the function.
 * @param[in] ar Parameter passed to the function.
 * @return void
 */
static void poom_lua_stop_hook_(lua_State* L, lua_Debug* ar)
{
    (void)ar;

    if(!s_lua_stop_requested)
    {
        return;
    }

    lua_pushstring(L, "stopped");
    lua_error(L);
}

/**
 * @brief Internal helper for `poom_lua_ensure_sd_mounted`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_lua_ensure_sd_mounted_(void)
{
    esp_err_t err;

    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_lua_run_file`.
 *
 * @param[in] abs_path Parameter passed to the function.
 * @param[in] out_err Parameter passed to the function.
 * @param[in] out_err_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_lua_run_file_(const char* abs_path, char* out_err, size_t out_err_len)
{
    lua_State* L;
    int st;

    if((out_err != NULL) && (out_err_len > 0U))
    {
        out_err[0] = '\0';
    }

    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    L = luaL_newstate();
    if(L == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    luaL_openlibs(L);
    poom_lua_bindings_register(L);

    lua_sethook(L, poom_lua_stop_hook_, LUA_MASKCOUNT, 10000);

    st = luaL_loadfile(L, abs_path);
    if(st != LUA_OK)
    {
        const char* msg = lua_tostring(L, -1);
        if((out_err != NULL) && (out_err_len > 0U))
        {
            (void)snprintf(out_err, out_err_len, "load:%.160s", (msg != NULL) ? msg : "error");
        }
        lua_close(L);
        return ESP_FAIL;
    }

    st = lua_pcall(L, 0, LUA_MULTRET, 0);
    if(st != LUA_OK)
    {
        const char* msg = lua_tostring(L, -1);
        if((out_err != NULL) && (out_err_len > 0U))
        {
            (void)snprintf(out_err, out_err_len, "run:%.160s", (msg != NULL) ? msg : "error");
        }
        lua_close(L);
        return ESP_FAIL;
    }

    lua_close(L);
    return ESP_OK;
}

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_lua_task_(void* arg)
{
    poom_lua_task_ctx_t* ctx = (poom_lua_task_ctx_t*)arg;
    esp_err_t err;
    char err_buf[POOM_LUA_ERR_MAX];

    err_buf[0] = '\0';
    s_lua_stop_requested = false;
    (void)poom_lua_buttons_start_();
    err = poom_lua_ensure_sd_mounted_();
    if(err == ESP_OK)
    {
        err = poom_lua_run_file_((ctx != NULL) ? ctx->path : NULL, err_buf, sizeof(err_buf));
    }
    else
    {
        (void)snprintf(err_buf, sizeof(err_buf), "sd:%d", (int)err);
    }

    poom_lua_bindings_cleanup();

    if((ctx != NULL) && (ctx->done_cb != NULL))
    {
        ctx->done_cb(err, (err_buf[0] != '\0') ? err_buf : NULL, ctx->done_ctx);
    }

    poom_lua_buttons_stop_();
    free((ctx != NULL) ? ctx->path : NULL);
    free(ctx);

    s_lua_running = false;
    s_lua_task = NULL;
    vTaskDelete(NULL);
}

bool poom_lua_is_running(void)
{
    return s_lua_running;
}

esp_err_t poom_lua_request_stop(void)
{
    if(!s_lua_running || (s_lua_task == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_lua_stop_requested = true;
    return ESP_OK;
}

esp_err_t poom_lua_run_file_async(const char* abs_path, poom_lua_done_cb_t cb, void* user_ctx)
{
    poom_lua_task_ctx_t* ctx;
    size_t len;

    if((abs_path == NULL) || (abs_path[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_lua_running || (s_lua_task != NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    len = strlen(abs_path);
    ctx = (poom_lua_task_ctx_t*)calloc(1U, sizeof(*ctx));
    if(ctx == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ctx->path = (char*)calloc(len + 1U, 1U);
    if(ctx->path == NULL)
    {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    (void)memcpy(ctx->path, abs_path, len);
    ctx->path[len] = '\0';
    ctx->done_cb = cb;
    ctx->done_ctx = user_ctx;

    s_lua_running = true;
    s_lua_stop_requested = false;
    if(xTaskCreate(poom_lua_task_, "poom_lua", POOM_LUA_TASK_STACK_WORDS, ctx, POOM_LUA_TASK_PRIO, &s_lua_task) != pdPASS)
    {
        s_lua_running = false;
        s_lua_stop_requested = false;
        s_lua_task = NULL;
        free(ctx->path);
        free(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}
