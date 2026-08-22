// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_web_http_server.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "bsp_pong.h"
#if POOM_WEB_ENABLE_TONE_PAGE
#include "buzzer.h"
#include "poom_buz_theme.h"
#endif
#include "poom_web_ir_session.h"
#include "poom_web_log.h"
#if POOM_WEB_ENABLE_MIDI_PAGE
#include "poom_midi_player.h"
#include "poom_web_midi.h"
#endif
#include "sd_card.h"

#define POOM_CLI_WEB_HTTP_SERVER_STACK_SIZE (4096U)
#define POOM_CLI_WEB_HTTP_SERVER_MAX_URI_HANDLERS (24U)
#define POOM_CLI_WEB_HTTP_SERVER_MAX_OPEN_SOCKETS (3U)
#define POOM_CLI_WEB_HTTP_SERVER_MAX_RESP_HEADERS (4U)
#define POOM_CLI_WEB_HTTP_SERVER_BACKLOG_CONN (2U)
#define POOM_CLI_WEB_HTTP_SERVER_RECV_TIMEOUT_S (5U)
#define POOM_CLI_WEB_HTTP_SERVER_SEND_TIMEOUT_S (5U)
#define POOM_CLI_WEB_COMMAND_MAX_BODY (512U)
// When WebSocket is disabled/unavailable, POST /command returns this buffer.
// Keep it reasonably small but large enough for multi-line `help` output.
#define POOM_CLI_WEB_HTTP_FALLBACK_TEXT_MAX_LEN (4096U)
#define POOM_CLI_WEB_FILES_ROOT_DIR "/sdcard"
#define POOM_CLI_WEB_FILES_NAME_MAX_LEN (96U)
#define POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN (192U)
#define POOM_CLI_WEB_FILES_PATH_MAX_LEN (320U)
#define POOM_CLI_WEB_FILES_IO_CHUNK (1024U)
#define POOM_CLI_WEB_FILES_CONTENT_DISPOSITION_MAX_LEN (256U)
#define POOM_CLI_WEB_FILES_VIEW_MAX_BYTES (64U * 1024U)

#if POOM_WEB_ENABLE_TONE_PAGE
#define POOM_CLI_WEB_TONE_DIR "/sdcard/tones"
#define POOM_CLI_WEB_TONE_MAX_BODY (12U * 1024U)
#define POOM_CLI_WEB_TONE_MAX_EVENTS (256U)
#endif

#if POOM_WEB_ENABLE_MIDI_PAGE
#define POOM_CLI_WEB_MIDI_MAX_BODY (8U * 1024U)
#define POOM_CLI_WEB_MIDI_HARMONY_DIR "/sdcard/harmonies"
#endif
#define POOM_CLI_WEB_WS_MAX_RX_FRAME (512U)
#define POOM_CLI_WEB_WS_TX_SLOT_COUNT (8U)
#define POOM_CLI_WEB_WS_TX_TEXT_MAX_LEN (256U)
#define POOM_CLI_WEB_HTTP_CTRL_PORT_BASE (ESP_HTTPD_DEF_CTRL_PORT)
#define POOM_CLI_WEB_HTTP_CTRL_PORT_SPAN (16U)

typedef struct {
    httpd_handle_t server;
    int socket_fd;
    size_t text_len;
    bool in_use;
    char text[POOM_CLI_WEB_WS_TX_TEXT_MAX_LEN];
} poom_web_ws_async_send_ctx_t;

static const char* POOM_CLI_WEB_HTTP_TAG = "poom_web_http";
static httpd_handle_t s_poom_cli_web_http_server = NULL;
static int s_poom_cli_web_ws_client_fd = -1;
static uint16_t s_poom_cli_web_http_ctrl_port = POOM_CLI_WEB_HTTP_CTRL_PORT_BASE;
static poom_web_command_cb_t s_poom_cli_web_command_cb = NULL;
static void* s_poom_cli_web_command_user_ctx = NULL;
#if !CONFIG_HTTPD_WS_SUPPORT
static char* s_poom_cli_web_fallback_text = NULL;
static size_t s_poom_cli_web_fallback_text_len = 0U;
static bool s_poom_cli_web_fallback_text_pending = false;
#endif
static portMUX_TYPE s_poom_cli_web_ws_send_mux = portMUX_INITIALIZER_UNLOCKED;
static poom_web_ws_async_send_ctx_t s_poom_cli_web_ws_send_slots[POOM_CLI_WEB_WS_TX_SLOT_COUNT] = {0};

/**
 * @brief Internal helper for `poom_cli_web_http_log_heap`.
 *
 * @param[in] stage Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_http_log_heap_(const char* stage)
{
    POOM_CLI_WEB_PRINTF_I(POOM_CLI_WEB_HTTP_TAG,
                          "HEAP %s: free=%u min=%u internal=%u largest=%u",
                          (stage != NULL) ? stage : "?",
                          (unsigned)esp_get_free_heap_size(),
                          (unsigned)esp_get_minimum_free_heap_size(),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

#if !CONFIG_HTTPD_WS_SUPPORT

/**
 * @brief Internal helper for `poom_cli_web_fallback_free`.
 *
 * @return void
 */
static void poom_cli_web_fallback_free_(void)
{
    free(s_poom_cli_web_fallback_text);
    s_poom_cli_web_fallback_text = NULL;
    s_poom_cli_web_fallback_text_len = 0U;
    s_poom_cli_web_fallback_text_pending = false;
}

/**
 * @brief Internal helper for `poom_cli_web_fallback_alloc`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_fallback_alloc_(void)
{
    poom_cli_web_fallback_free_();

    s_poom_cli_web_fallback_text = malloc(POOM_CLI_WEB_HTTP_FALLBACK_TEXT_MAX_LEN);
    if(s_poom_cli_web_fallback_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_poom_cli_web_fallback_text[0] = '\0';
    s_poom_cli_web_fallback_text_len = 0U;
    s_poom_cli_web_fallback_text_pending = false;
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_fallback_append`.
 *
 * @param[in] text Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_fallback_append_(const char* text)
{
    size_t remain;
    size_t add;

    if(text == NULL) {
        return;
    }

    if(s_poom_cli_web_fallback_text == NULL) {
        return;
    }

    if(s_poom_cli_web_fallback_text_len >= (POOM_CLI_WEB_HTTP_FALLBACK_TEXT_MAX_LEN - 1U)) {
        s_poom_cli_web_fallback_text_pending = true;
        return;
    }

    remain = (POOM_CLI_WEB_HTTP_FALLBACK_TEXT_MAX_LEN - 1U) - s_poom_cli_web_fallback_text_len;
    add = strlen(text);
    if(add > remain) {
        add = remain;
    }

    if(add > 0U) {
        memcpy(&s_poom_cli_web_fallback_text[s_poom_cli_web_fallback_text_len], text, add);
        s_poom_cli_web_fallback_text_len += add;
        s_poom_cli_web_fallback_text[s_poom_cli_web_fallback_text_len] = '\0';
        s_poom_cli_web_fallback_text_pending = true;
    }
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_ws_send_slot_acquire`.
 *
 * @return poom_web_ws_async_send_ctx_t*
 */
static poom_web_ws_async_send_ctx_t* poom_cli_web_ws_send_slot_acquire_(void)
{
    poom_web_ws_async_send_ctx_t* slot = NULL;

    taskENTER_CRITICAL(&s_poom_cli_web_ws_send_mux);
    for(size_t i = 0U; i < POOM_CLI_WEB_WS_TX_SLOT_COUNT; i++) {
        if(!s_poom_cli_web_ws_send_slots[i].in_use) {
            s_poom_cli_web_ws_send_slots[i].in_use = true;
            slot = &s_poom_cli_web_ws_send_slots[i];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_poom_cli_web_ws_send_mux);

    if(slot != NULL) {
        slot->server = NULL;
        slot->socket_fd = -1;
        slot->text_len = 0U;
        slot->text[0] = '\0';
    }

    return slot;
}

/**
 * @brief Internal helper for `poom_cli_web_ws_send_slot_release`.
 *
 * @param[in] slot Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_ws_send_slot_release_(poom_web_ws_async_send_ctx_t* slot)
{
    if(slot == NULL) {
        return;
    }

    slot->server = NULL;
    slot->socket_fd = -1;
    slot->text_len = 0U;
    slot->text[0] = '\0';

    taskENTER_CRITICAL(&s_poom_cli_web_ws_send_mux);
    slot->in_use = false;
    taskEXIT_CRITICAL(&s_poom_cli_web_ws_send_mux);
}

static esp_err_t poom_cli_web_http_read_body_(httpd_req_t* req, size_t max_len, char** out_body, size_t* out_len);
static esp_err_t poom_cli_web_get_query_value_(
    httpd_req_t* req,
    const char* key,
    char* out_value,
    size_t out_value_len);
static bool poom_cli_web_is_valid_file_name_(const char* file_name);

extern const uint8_t poom_cli_web_index_html_start[] asm("_binary_poom_cli_web_index_html_start");
extern const uint8_t poom_cli_web_index_html_end[] asm("_binary_poom_cli_web_index_html_end");
extern const uint8_t poom_cli_web_app_css_start[] asm("_binary_poom_cli_web_app_css_start");
extern const uint8_t poom_cli_web_app_css_end[] asm("_binary_poom_cli_web_app_css_end");
extern const uint8_t poom_cli_web_app_js_start[] asm("_binary_poom_cli_web_app_js_start");
extern const uint8_t poom_cli_web_app_js_end[] asm("_binary_poom_cli_web_app_js_end");
extern const uint8_t poom_cli_web_ir_html_start[] asm("_binary_poom_cli_web_ir_html_start");
extern const uint8_t poom_cli_web_ir_html_end[] asm("_binary_poom_cli_web_ir_html_end");
extern const uint8_t poom_cli_web_ir_css_start[] asm("_binary_poom_cli_web_ir_css_start");
extern const uint8_t poom_cli_web_ir_css_end[] asm("_binary_poom_cli_web_ir_css_end");
extern const uint8_t poom_cli_web_ir_js_start[] asm("_binary_poom_cli_web_ir_js_start");
extern const uint8_t poom_cli_web_ir_js_end[] asm("_binary_poom_cli_web_ir_js_end");
#if POOM_WEB_ENABLE_TONE_PAGE
extern const uint8_t poom_cli_web_tone_html_start[] asm("_binary_poom_cli_web_tone_html_start");
extern const uint8_t poom_cli_web_tone_html_end[] asm("_binary_poom_cli_web_tone_html_end");
extern const uint8_t poom_cli_web_tone_css_start[] asm("_binary_poom_cli_web_tone_css_start");
extern const uint8_t poom_cli_web_tone_css_end[] asm("_binary_poom_cli_web_tone_css_end");
extern const uint8_t poom_cli_web_tone_js_start[] asm("_binary_poom_cli_web_tone_js_start");
extern const uint8_t poom_cli_web_tone_js_end[] asm("_binary_poom_cli_web_tone_js_end");
#endif
#if POOM_WEB_ENABLE_MIDI_PAGE
extern const uint8_t poom_cli_web_midi_html_start[] asm("_binary_poom_cli_web_midi_html_start");
extern const uint8_t poom_cli_web_midi_html_end[] asm("_binary_poom_cli_web_midi_html_end");
extern const uint8_t poom_cli_web_midi_css_start[] asm("_binary_poom_cli_web_midi_css_start");
extern const uint8_t poom_cli_web_midi_css_end[] asm("_binary_poom_cli_web_midi_css_end");
extern const uint8_t poom_cli_web_midi_js_start[] asm("_binary_poom_cli_web_midi_js_start");
extern const uint8_t poom_cli_web_midi_js_end[] asm("_binary_poom_cli_web_midi_js_end");
#endif

#if CONFIG_HTTPD_WS_SUPPORT

/**
 * @brief Internal helper for `poom_cli_web_ws_send_async_work`.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_ws_send_async_work_(void* arg) {
    poom_web_ws_async_send_ctx_t* ctx = (poom_web_ws_async_send_ctx_t*)arg;
    httpd_ws_frame_t frame = {0};

    if((ctx == NULL) || (ctx->server == NULL) || (ctx->socket_fd < 0) || (ctx->text_len == 0U)) {
        poom_cli_web_ws_send_slot_release_(ctx);
        return;
    }

    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)ctx->text;
    frame.len = ctx->text_len;

    if(httpd_ws_send_frame_async(ctx->server, ctx->socket_fd, &frame) != ESP_OK) {
        POOM_CLI_WEB_PRINTF_W(POOM_CLI_WEB_HTTP_TAG, "Failed to send WS frame");
    }

    poom_cli_web_ws_send_slot_release_(ctx);
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_http_index_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_index_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_index_html_start,
                           (ssize_t)(poom_cli_web_index_html_end - poom_cli_web_index_html_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_css_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_css_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_app_css_start,
                           (ssize_t)(poom_cli_web_app_css_end - poom_cli_web_app_css_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_js_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_js_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_app_js_start,
                           (ssize_t)(poom_cli_web_app_js_end - poom_cli_web_app_js_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_page_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_page_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_ir_html_start,
                           (ssize_t)(poom_cli_web_ir_html_end - poom_cli_web_ir_html_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_css_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_css_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_ir_css_start,
                           (ssize_t)(poom_cli_web_ir_css_end - poom_cli_web_ir_css_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_js_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_js_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_ir_js_start,
                           (ssize_t)(poom_cli_web_ir_js_end - poom_cli_web_ir_js_start));
}

#if POOM_WEB_ENABLE_TONE_PAGE

/**
 * @brief Internal helper for `poom_cli_web_http_tone_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_tone_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_tone_html_start,
                           (ssize_t)(poom_cli_web_tone_html_end - poom_cli_web_tone_html_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_tone_css_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_tone_css_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_tone_css_start,
                           (ssize_t)(poom_cli_web_tone_css_end - poom_cli_web_tone_css_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_tone_js_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_tone_js_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_tone_js_start,
                           (ssize_t)(poom_cli_web_tone_js_end - poom_cli_web_tone_js_start));
}
#endif

#if POOM_WEB_ENABLE_MIDI_PAGE

/**
 * @brief Internal helper for `poom_cli_web_http_midi_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_midi_html_start,
                           (ssize_t)(poom_cli_web_midi_html_end - poom_cli_web_midi_html_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_midi_css_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_css_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_midi_css_start,
                           (ssize_t)(poom_cli_web_midi_css_end - poom_cli_web_midi_css_start));
}

/**
 * @brief Internal helper for `poom_cli_web_http_midi_js_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_js_handler_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req,
                           (const char*)poom_cli_web_midi_js_start,
                           (ssize_t)(poom_cli_web_midi_js_end - poom_cli_web_midi_js_start));
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_http_capabilities_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_capabilities_handler_(httpd_req_t* req) {
    char response[96];

    httpd_resp_set_type(req, "application/json");
    (void)snprintf(response,
                   sizeof(response),
                   "{\"ws\":%s,\"tone\":%s,\"midi\":%s}",
#if CONFIG_HTTPD_WS_SUPPORT
                   "true",
#else
                   "false",
#endif
#if POOM_WEB_ENABLE_TONE_PAGE
                   "true",
#else
                   "false",
#endif
#if POOM_WEB_ENABLE_MIDI_PAGE
                   "true");
#else
                   "false");
#endif
    return httpd_resp_sendstr(req, response);
}

#if POOM_WEB_ENABLE_MIDI_PAGE

/**
 * @brief Internal helper for `poom_cli_web_http_midi_harmony_post_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_harmony_post_handler_(httpd_req_t* req) {
    char* payload = NULL;
    size_t payload_len = 0U;
    esp_err_t err;
    char parse_err[64] = {0};

    err = poom_cli_web_http_read_body_(req, POOM_CLI_WEB_MIDI_MAX_BODY, &payload, &payload_len);
    if(err != ESP_OK) {
        if((size_t)req->content_len > POOM_CLI_WEB_MIDI_MAX_BODY) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            return httpd_resp_sendstr(req, "body too large");
        }
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid body");
    }

    const bool ok = poom_web_midi_load_harmony_json(payload, payload_len, true, parse_err, sizeof(parse_err));
    free(payload);

    if(!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char resp[120];
        (void)snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", (parse_err[0] != '\0') ? parse_err : "parse");
        return httpd_resp_sendstr(req, resp);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] s Parameter passed to the function.
 * @param[in] suffix Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_str_ends_with_(const char* s, const char* suffix) {
    size_t sl;
    size_t sufl;

    if((s == NULL) || (suffix == NULL)) {
        return false;
    }
    sl = strlen(s);
    sufl = strlen(suffix);
    if(sufl == 0U) {
        return true;
    }
    if(sl < sufl) {
        return false;
    }
    return (strcmp(s + (sl - sufl), suffix) == 0);
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_harmony_save_post_handler_(httpd_req_t* req) {
    char name_raw[POOM_CLI_WEB_FILES_NAME_MAX_LEN] = {0};
    char file_name[POOM_CLI_WEB_FILES_NAME_MAX_LEN] = {0};
    char abs_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char rel_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char* payload = NULL;
    size_t payload_len = 0U;
    esp_err_t err;
    char parse_err[64] = {0};
    FILE* f;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_cli_web_get_query_value_(req, "name", name_raw, sizeof(name_raw));
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_name\"}");
    }
    if(!poom_cli_web_is_valid_file_name_(name_raw)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_name\"}");
    }

    if(poom_cli_web_str_ends_with_(name_raw, ".json")) {
        (void)strncpy(file_name, name_raw, sizeof(file_name) - 1U);
        file_name[sizeof(file_name) - 1U] = '\0';
    } else {
        size_t nlen = strlen(name_raw);
        if(nlen + 5U >= sizeof(file_name)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"name_too_long\"}");
        }
        memcpy(file_name, name_raw, nlen);
        memcpy(file_name + nlen, ".json", 6U); /* includes NUL */
    }

    err = poom_cli_web_http_read_body_(req, POOM_CLI_WEB_MIDI_MAX_BODY, &payload, &payload_len);
    if(err != ESP_OK) {
        if((size_t)req->content_len > POOM_CLI_WEB_MIDI_MAX_BODY) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"body_too_large\"}");
        }
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_body\"}");
    }

    if(!poom_midi_player_validate_json(payload, payload_len, parse_err, sizeof(parse_err))) {
        free(payload);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char resp[160];
        (void)snprintf(resp,
                       sizeof(resp),
                       "{\"ok\":false,\"error\":\"%s\"}",
                       (parse_err[0] != '\0') ? parse_err : "parse");
        return httpd_resp_sendstr(req, resp);
    }

    if(sd_card_is_not_mounted()) {
        sd_card_begin();
        err = sd_card_mount();
        if(err != ESP_OK) {
            free(payload);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd_mount_failed\"}");
        }
    }

    if(mkdir(POOM_CLI_WEB_MIDI_HARMONY_DIR, 0775) != 0) {
        if(errno != EEXIST) {
            free(payload);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"mkdir_failed\"}");
        }
    }

    (void)snprintf(abs_path, sizeof(abs_path), "%s/%s", POOM_CLI_WEB_MIDI_HARMONY_DIR, file_name);
    (void)snprintf(rel_path, sizeof(rel_path), "%s/%s", "harmonies", file_name);

    f = fopen(abs_path, "wb");
    if(f == NULL) {
        free(payload);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"cannot_write\"}");
    }

    if(fwrite(payload, 1U, payload_len, f) != payload_len) {
        (void)fclose(f);
        (void)remove(abs_path);
        free(payload);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write_failed\"}");
    }

    (void)fclose(f);
    free(payload);

    httpd_resp_set_type(req, "application/json");
    char resp[240];
    (void)snprintf(resp, sizeof(resp), "{\"ok\":true,\"path\":\"%s\"}", rel_path);
    return httpd_resp_sendstr(req, resp);
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_midi_harmony_stop_post_handler_(httpd_req_t* req) {
    (void)req;
    poom_web_midi_stop();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_is_valid_file_name`.
 *
 * @param[in] file_name Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_is_valid_file_name_(const char* file_name) {
    size_t i;

    if(file_name == NULL) {
        return false;
    }
    if((file_name[0] == '\0') || (strlen(file_name) >= POOM_CLI_WEB_FILES_NAME_MAX_LEN)) {
        return false;
    }
    if(strstr(file_name, "..") != NULL) {
        return false;
    }
    if((strchr(file_name, '/') != NULL) || (strchr(file_name, '\\') != NULL)) {
        return false;
    }
    if(file_name[0] == '.') {
        return false;
    }

    for(i = 0U; file_name[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)file_name[i];
        if((ch == '_') || (ch == '-') || (ch == '.') || (ch == ' ')) {
            continue;
        }
        if(isalnum((int)ch) == 0) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Internal helper for `poom_cli_web_is_valid_relative_path`.
 *
 * @param[in] relative_path Parameter passed to the function.
 * @param[in] allow_empty Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_is_valid_relative_path_(const char* relative_path, bool allow_empty) {
    size_t i;
    size_t segment_len = 0U;

    if(relative_path == NULL) {
        return false;
    }
    if(relative_path[0] == '\0') {
        return allow_empty;
    }
    if(strlen(relative_path) >= POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN) {
        return false;
    }
    if((relative_path[0] == '/') || (relative_path[0] == '\\')) {
        return false;
    }
    if(strstr(relative_path, "..") != NULL) {
        return false;
    }

    for(i = 0U; relative_path[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)relative_path[i];

        if(ch == '\\') {
            return false;
        }

        if(ch == '/') {
            if(segment_len == 0U) {
                return false;
            }
            segment_len = 0U;
            continue;
        }

        if((ch == '_') || (ch == '-') || (ch == '.') || (ch == ' ')) {
            segment_len++;
            continue;
        }

        if(isalnum((int)ch) == 0) {
            return false;
        }
        segment_len++;
    }

    if(segment_len == 0U) {
        return false;
    }
    return true;
}

/**
 * @brief Internal helper for `poom_cli_web_hex_to_int`.
 *
 * @param[in] ch Parameter passed to the function.
 * @return int
 */
static int poom_cli_web_hex_to_int_(char ch) {
    if((ch >= '0') && (ch <= '9')) {
        return (int)(ch - '0');
    }
    if((ch >= 'a') && (ch <= 'f')) {
        return (int)(ch - 'a' + 10);
    }
    if((ch >= 'A') && (ch <= 'F')) {
        return (int)(ch - 'A' + 10);
    }
    return -1;
}

/**
 * @brief Internal helper for `poom_cli_web_url_decode_inplace`.
 *
 * @param[in] text Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_url_decode_inplace_(char* text) {
    size_t read_idx;
    size_t write_idx;

    if(text == NULL) {
        return;
    }

    read_idx = 0U;
    write_idx = 0U;
    while(text[read_idx] != '\0') {
        if((text[read_idx] == '%') &&
           (text[read_idx + 1U] != '\0') &&
           (text[read_idx + 2U] != '\0')) {
            int hi = poom_cli_web_hex_to_int_(text[read_idx + 1U]);
            int lo = poom_cli_web_hex_to_int_(text[read_idx + 2U]);
            if((hi >= 0) && (lo >= 0)) {
                text[write_idx++] = (char)((hi << 4) | lo);
                read_idx += 3U;
                continue;
            }
        }

        if(text[read_idx] == '+') {
            text[write_idx++] = ' ';
            read_idx++;
            continue;
        }

        text[write_idx++] = text[read_idx++];
    }

    text[write_idx] = '\0';
}

/**
 * @brief Internal helper for `poom_cli_web_http_read_body`.
 *
 * @param[in] req Parameter passed to the function.
 * @param[in] max_len Parameter passed to the function.
 * @param[in] out_body Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_read_body_(httpd_req_t* req, size_t max_len, char** out_body, size_t* out_len) {
    size_t length;
    char* payload;
    int received;

    if((req == NULL) || (out_body == NULL) || (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    *out_len = 0U;

    length = (size_t)req->content_len;
    if((length == 0U) || (length > max_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    payload = (char*)calloc(1U, length + 1U);
    if(payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    received = httpd_req_recv(req, payload, length);
    if(received <= 0) {
        free(payload);
        return ESP_FAIL;
    }

    payload[received] = '\0';
    *out_body = payload;
    *out_len = (size_t)received;
    return ESP_OK;
}

#if POOM_WEB_ENABLE_TONE_PAGE

/**
 * @brief Internal helper for `poom_cli_web_tone_sanitize_name`.
 *
 * @param[in] in Parameter passed to the function.
 * @param[in] out Parameter passed to the function.
 * @param[in] out_len Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_tone_sanitize_name_(const char* in, char* out, size_t out_len) {
    size_t w = 0U;

    if((out == NULL) || (out_len == 0U)) {
        return false;
    }

    out[0] = '\0';
    if(in == NULL) {
        return false;
    }

    for(size_t i = 0U; in[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)in[i];
        if((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n')) {
            continue;
        }
        if((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (ch == '_') ||
           (ch == '-')) {
            if(w + 1U < out_len) {
                out[w++] = (char)ch;
            }
        }
        if(w + 1U >= out_len) {
            break;
        }
    }

    out[w] = '\0';
    return out[0] != '\0';
}

/**
 * @brief Parses input data for this module.
 *
 * @param[in] json Parameter passed to the function.
 * @param[in] json_len Parameter passed to the function.
 * @param[in] out_events Parameter passed to the function.
 * @param[in] out_count Parameter passed to the function.
 * @param[in] out_pause_ms Parameter passed to the function.
 * @param[in] out_name Parameter passed to the function.
 * @param[in] out_name_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_tone_parse_json_(const char* json,
                                              size_t json_len,
                                              poom_buz_theme_event_t** out_events,
                                              size_t* out_count,
                                              uint32_t* out_pause_ms,
                                              char* out_name,
                                              size_t out_name_len) {
    cJSON* root = NULL;
    cJSON* events = NULL;
    cJSON* pause_ms = NULL;
    cJSON* name = NULL;
    poom_buz_theme_event_t* out = NULL;
    size_t count = 0U;

    if((json == NULL) || (out_events == NULL) || (out_count == NULL) || (out_pause_ms == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_events = NULL;
    *out_count = 0U;
    *out_pause_ms = 0U;
    if((out_name != NULL) && (out_name_len > 0U)) {
        out_name[0] = '\0';
    }

    root = cJSON_ParseWithLength(json, json_len);
    if(root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    events = cJSON_GetObjectItem(root, "events");
    if(!cJSON_IsArray(events)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    pause_ms = cJSON_GetObjectItem(root, "pause_ms");
    if(cJSON_IsNumber(pause_ms) && (pause_ms->valuedouble > 0.0)) {
        double v = pause_ms->valuedouble;
        if(v > 10000.0) {
            v = 10000.0;
        }
        *out_pause_ms = (uint32_t)v;
    }

    name = cJSON_GetObjectItem(root, "name");
    if(cJSON_IsString(name) && (name->valuestring != NULL) && (out_name != NULL) && (out_name_len > 0U)) {
        (void)snprintf(out_name, out_name_len, "%s", name->valuestring);
    }

    count = (size_t)cJSON_GetArraySize(events);
    if((count == 0U) || (count > POOM_CLI_WEB_TONE_MAX_EVENTS)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    out = (poom_buz_theme_event_t*)calloc(count, sizeof(*out));
    if(out == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0U;
    for(size_t i = 0U; i < count; i++) {
        cJSON* ev = cJSON_GetArrayItem(events, (int)i);
        uint32_t freq = 0U;
        uint32_t dur = 0U;

        if(cJSON_IsArray(ev)) {
            cJSON* f = cJSON_GetArrayItem(ev, 0);
            cJSON* d = cJSON_GetArrayItem(ev, 1);
            if(cJSON_IsNumber(f)) {
                double fv = f->valuedouble;
                if(fv < 0.0) {
                    fv = 0.0;
                }
                if(fv > 32767.0) {
                    fv = 32767.0;
                }
                freq = (uint32_t)fv;
            }
            if(cJSON_IsNumber(d)) {
                double dv = d->valuedouble;
                if(dv < 0.0) {
                    dv = 0.0;
                }
                if(dv > 600000.0) {
                    dv = 600000.0;
                }
                dur = (uint32_t)dv;
            }
        } else if(cJSON_IsObject(ev)) {
            cJSON* f = cJSON_GetObjectItem(ev, "freq_hz");
            cJSON* d = cJSON_GetObjectItem(ev, "duration_ms");
            if(cJSON_IsNumber(f)) {
                double fv = f->valuedouble;
                if(fv < 0.0) {
                    fv = 0.0;
                }
                if(fv > 32767.0) {
                    fv = 32767.0;
                }
                freq = (uint32_t)fv;
            }
            if(cJSON_IsNumber(d)) {
                double dv = d->valuedouble;
                if(dv < 0.0) {
                    dv = 0.0;
                }
                if(dv > 600000.0) {
                    dv = 600000.0;
                }
                dur = (uint32_t)dv;
            }
        } else {
            continue;
        }

        if(dur == 0U) {
            continue;
        }

        out[written].freq_hz = freq;
        out[written].duration_ms = dur;
        written++;
    }

    cJSON_Delete(root);

    if(written == 0U) {
        free(out);
        return ESP_ERR_INVALID_ARG;
    }

    *out_events = out;
    *out_count = written;
    return ESP_OK;
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_get_query_value`.
 *
 * @param[in] req Parameter passed to the function.
 * @param[in] key Parameter passed to the function.
 * @param[in] out_value Parameter passed to the function.
 * @param[in] out_value_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_get_query_value_(
    httpd_req_t* req,
    const char* key,
    char* out_value,
    size_t out_value_len) {
    size_t query_len;
    char query[512] = {0};

    if((req == NULL) || (key == NULL) || (out_value == NULL) || (out_value_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    query_len = httpd_req_get_url_query_len(req);
    if((query_len == 0U) || (query_len >= sizeof(query))) {
        return ESP_ERR_NOT_FOUND;
    }

    if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_FAIL;
    }

    if(httpd_query_key_value(query, key, out_value, out_value_len) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    poom_cli_web_url_decode_inplace_(out_value);
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_get_relative_dir_path`.
 *
 * @param[in] req Parameter passed to the function.
 * @param[in] out_relative_dir Parameter passed to the function.
 * @param[in] out_relative_dir_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_get_relative_dir_path_(
    httpd_req_t* req,
    char* out_relative_dir,
    size_t out_relative_dir_len) {
    esp_err_t err;

    if((req == NULL) || (out_relative_dir == NULL) || (out_relative_dir_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_cli_web_get_query_value_(req, "dir", out_relative_dir, out_relative_dir_len);
    if(err == ESP_ERR_NOT_FOUND) {
        out_relative_dir[0] = '\0';
        return ESP_OK;
    }
    if(err != ESP_OK) {
        return err;
    }

    if(!poom_cli_web_is_valid_relative_path_(out_relative_dir, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_get_relative_file_path`.
 *
 * @param[in] req Parameter passed to the function.
 * @param[in] out_relative_path Parameter passed to the function.
 * @param[in] out_relative_path_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_get_relative_file_path_(
    httpd_req_t* req,
    char* out_relative_path,
    size_t out_relative_path_len) {
    esp_err_t err;

    if((req == NULL) || (out_relative_path == NULL) || (out_relative_path_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_cli_web_get_query_value_(req, "path", out_relative_path, out_relative_path_len);
    if(err == ESP_OK) {
        if(!poom_cli_web_is_valid_relative_path_(out_relative_path, false)) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    err = poom_cli_web_get_query_value_(req, "name", out_relative_path, out_relative_path_len);
    if(err != ESP_OK) {
        return err;
    }
    if(!poom_cli_web_is_valid_file_name_(out_relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_build_storage_path`.
 *
 * @param[in] relative_path Parameter passed to the function.
 * @param[in] out_path Parameter passed to the function.
 * @param[in] out_path_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_build_storage_path_(
    const char* relative_path,
    char* out_path,
    size_t out_path_len) {
    int written;

    if((relative_path == NULL) || (out_path == NULL) || (out_path_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(!poom_cli_web_is_valid_relative_path_(relative_path, true)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(relative_path[0] == '\0') {
        written = snprintf(out_path, out_path_len, "%s", POOM_CLI_WEB_FILES_ROOT_DIR);
    } else {
        written = snprintf(out_path, out_path_len, "%s/%s", POOM_CLI_WEB_FILES_ROOT_DIR, relative_path);
    }
    if((written < 0) || ((size_t)written >= out_path_len)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_build_relative_child_path`.
 *
 * @param[in] relative_dir Parameter passed to the function.
 * @param[in] child_name Parameter passed to the function.
 * @param[in] out_relative_path Parameter passed to the function.
 * @param[in] out_relative_path_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_build_relative_child_path_(
    const char* relative_dir,
    const char* child_name,
    char* out_relative_path,
    size_t out_relative_path_len) {
    int written;

    if((relative_dir == NULL) || (child_name == NULL) || (out_relative_path == NULL) ||
       (out_relative_path_len == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!poom_cli_web_is_valid_relative_path_(relative_dir, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!poom_cli_web_is_valid_file_name_(child_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    if(relative_dir[0] == '\0') {
        written = snprintf(out_relative_path, out_relative_path_len, "%s", child_name);
    } else {
        written = snprintf(out_relative_path, out_relative_path_len, "%s/%s", relative_dir, child_name);
    }
    if((written < 0) || ((size_t)written >= out_relative_path_len)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Internal helper for `poom_cli_web_ensure_sd_mounted`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_ensure_sd_mounted_(void)
{
    if(sd_card_is_mounted()) {
        return ESP_OK;
    }

    sd_card_begin();
    return sd_card_mount();
}

/**
 * @brief Internal helper for `poom_cli_web_http_files_list_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_files_list_handler_(httpd_req_t* req) {
    DIR* dir;
    struct dirent* entry;
    struct stat directory_stat;
    bool first = true;
    char relative_dir[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char directory_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char header[256];
    int header_len;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"sd_not_mounted\"}");
    }

    if(poom_cli_web_get_relative_dir_path_(req, relative_dir, sizeof(relative_dir)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_dir\"}");
    }
    if(poom_cli_web_build_storage_path_(relative_dir, directory_path, sizeof(directory_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_dir\"}");
    }
    if(stat(directory_path, &directory_stat) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"dir_not_found\"}");
    }
    if(!S_ISDIR(directory_stat.st_mode)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_dir\"}");
    }

    dir = opendir(directory_path);
    if(dir == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"sd_not_mounted\"}");
    }

    httpd_resp_set_type(req, "application/json");
    header_len = snprintf(header, sizeof(header), "{\"dir\":\"%s\",\"entries\":[", relative_dir);
    if((header_len < 0) || ((size_t)header_len >= sizeof(header))) {
        (void)closedir(dir);
        return ESP_FAIL;
    }
    if(httpd_resp_sendstr_chunk(req, header) != ESP_OK) {
        (void)closedir(dir);
        return ESP_FAIL;
    }

    while((entry = readdir(dir)) != NULL) {
        struct stat st;
        int n;
        char relative_entry_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
        char absolute_entry_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
        char item[320];
        const char* name = entry->d_name;
        bool is_dir;

        if((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
            continue;
        }
        if(!poom_cli_web_is_valid_file_name_(name)) {
            continue;
        }

        if(poom_cli_web_build_relative_child_path_(
               relative_dir,
               name,
               relative_entry_path,
               sizeof(relative_entry_path)) != ESP_OK) {
            continue;
        }
        if(poom_cli_web_build_storage_path_(
               relative_entry_path,
               absolute_entry_path,
               sizeof(absolute_entry_path)) != ESP_OK) {
            continue;
        }
        if(stat(absolute_entry_path, &st) != 0) {
            continue;
        }
        is_dir = S_ISDIR(st.st_mode);
        if((!is_dir) && (!S_ISREG(st.st_mode))) {
            continue;
        }

        n = snprintf(item,
                     sizeof(item),
                     "%s{\"name\":\"%s\",\"path\":\"%s\",\"is_dir\":%s,\"size\":%ld}",
                     first ? "" : ",",
                     name,
                     relative_entry_path,
                     is_dir ? "true" : "false",
                     is_dir ? 0L : (long)st.st_size);
        if((n < 0) || ((size_t)n >= sizeof(item))) {
            continue;
        }

        if(httpd_resp_sendstr_chunk(req, item) != ESP_OK) {
            (void)closedir(dir);
            return ESP_FAIL;
        }
        first = false;
    }

    (void)closedir(dir);
    if(httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_files_download_handler_(httpd_req_t* req) {
    char relative_file_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char file_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char chunk[POOM_CLI_WEB_FILES_IO_CHUNK];
    struct stat st;
    const char* file_name;
    FILE* file;
    size_t bytes_read;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "sd not mounted");
    }

    if(poom_cli_web_get_relative_file_path_(req, relative_file_path, sizeof(relative_file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing or invalid path");
    }
    if(poom_cli_web_build_storage_path_(relative_file_path, file_path, sizeof(file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid path");
    }
    if((stat(file_path, &st) != 0) || (!S_ISREG(st.st_mode))) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    file = fopen(file_path, "rb");
    if(file == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    {
        const char* basename_ptr = strrchr(relative_file_path, '/');
        char disp[POOM_CLI_WEB_FILES_CONTENT_DISPOSITION_MAX_LEN];
        file_name = (basename_ptr == NULL) ? relative_file_path : (basename_ptr + 1);
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", file_name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    while((bytes_read = fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
        if(httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            (void)fclose(file);
            return ESP_FAIL;
        }
    }

    (void)fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/**
 * @brief Internal helper for `poom_cli_web_is_viewable_text_file`.
 *
 * @param[in] relative_path Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_is_viewable_text_file_(const char* relative_path)
{
    const char* ext;

    if((relative_path == NULL) || (relative_path[0] == '\0')) {
        return false;
    }

    ext = strrchr(relative_path, '.');
    if((ext == NULL) || (ext[1] == '\0')) {
        return false;
    }

    if((strcasecmp(ext, ".txt") == 0) ||
       (strcasecmp(ext, ".csv") == 0) ||
       (strcasecmp(ext, ".ir") == 0) ||
       (strcasecmp(ext, ".nfc") == 0) ||
       (strcasecmp(ext, ".tone") == 0) ||
       (strcasecmp(ext, ".json") == 0)) {
        return true;
    }

    return false;
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_open_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_open_handler_(httpd_req_t* req)
{
    char relative_file_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char file_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char token[32] = {0};
    size_t command_count = 0U;
    struct stat st;
    esp_err_t err;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sd_not_mounted\"}");
    }

    if(poom_cli_web_get_relative_file_path_(req, relative_file_path, sizeof(relative_file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_or_invalid_path\"}");
    }

    if(!poom_web_ir_is_supported_path(relative_file_path)) {
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unsupported_file\"}");
    }

    if(poom_cli_web_build_storage_path_(relative_file_path, file_path, sizeof(file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_path\"}");
    }

    if((stat(file_path, &st) != 0) || (!S_ISREG(st.st_mode))) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"file_not_found\"}");
    }

    err = poom_web_ir_session_open(file_path, token, sizeof(token), NULL, 0U, &command_count);
    if(err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_status(req, "501 Not Implemented");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ir_not_supported\"}");
    }
    if(err == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_mem\"}");
    }
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_ir_file\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    {
        char header[320];
        int header_len = snprintf(header,
                                  sizeof(header),
                                  "{\"ok\":true,\"token\":\"%s\",\"path\":\"%s\",\"commands\":[",
                                  token,
                                  relative_file_path);
        if((header_len < 0) || ((size_t)header_len >= sizeof(header))) {
            (void)poom_web_ir_session_close(token);
            return ESP_FAIL;
        }

        if(httpd_resp_sendstr_chunk(req, header) != ESP_OK) {
            (void)poom_web_ir_session_close(token);
            return ESP_FAIL;
        }
    }

    for(size_t i = 0U; i < command_count; i++) {
        poom_web_ir_command_t command;
        char item[128];
        int item_len;

        err = poom_web_ir_session_get_command(token, i, &command);
        if(err != ESP_OK) {
            (void)poom_web_ir_session_close(token);
            return ESP_FAIL;
        }

        item_len = snprintf(item,
                            sizeof(item),
                            "%s{\"index\":%lu,\"name\":\"%s\"}",
                            (i == 0U) ? "" : ",",
                            (unsigned long)i,
                            command.name);
        if((item_len < 0) || ((size_t)item_len >= sizeof(item))) {
            (void)poom_web_ir_session_close(token);
            return ESP_FAIL;
        }

        if(httpd_resp_sendstr_chunk(req, item) != ESP_OK) {
            (void)poom_web_ir_session_close(token);
            return ESP_FAIL;
        }
    }

    if(httpd_resp_sendstr_chunk(req, "]}") != ESP_OK) {
        (void)poom_web_ir_session_close(token);
        return ESP_FAIL;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_send_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_send_handler_(httpd_req_t* req)
{
    char token[32] = {0};
    char index_text[16] = {0};
    char* endptr = NULL;
    unsigned long index_value = 0UL;
    esp_err_t err;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_get_query_value_(req, "token", token, sizeof(token)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_token\"}");
    }

    if(poom_cli_web_get_query_value_(req, "index", index_text, sizeof(index_text)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_index\"}");
    }

    index_value = strtoul(index_text, &endptr, 10);
    if((index_text[0] == '\0') || (endptr == NULL) || (*endptr != '\0')) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_index\"}");
    }

    err = poom_web_ir_session_send(token, (size_t)index_value);
    if(err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_set_status(req, "501 Not Implemented");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ir_not_supported\"}");
    }
    if(err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"session_not_found\"}");
    }
    if(err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_index\"}");
    }
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"send_failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/**
 * @brief Internal helper for `poom_cli_web_http_ir_close_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ir_close_handler_(httpd_req_t* req)
{
    char token[32] = {0};
    esp_err_t err;
    bool released = false;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = poom_cli_web_get_query_value_(req, "token", token, sizeof(token));
    if((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_token\"}");
    }

    if(err == ESP_OK) {
        err = poom_web_ir_session_close(token);
        if(err == ESP_OK) {
            released = true;
        } else if((err != ESP_ERR_NOT_FOUND) && (err != ESP_ERR_INVALID_ARG)) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"close_failed\"}");
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, released ? "{\"ok\":true,\"released\":true}" : "{\"ok\":true,\"released\":false}");
}

/**
 * @brief Internal helper for `poom_cli_web_http_files_view_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_files_view_handler_(httpd_req_t* req)
{
    char relative_file_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char file_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char chunk[POOM_CLI_WEB_FILES_IO_CHUNK];
    struct stat st;
    FILE* file;
    size_t bytes_read;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "sd not mounted");
    }

    if(poom_cli_web_get_relative_file_path_(req, relative_file_path, sizeof(relative_file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing or invalid path");
    }

    if(!poom_cli_web_is_viewable_text_file_(relative_file_path)) {
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        return httpd_resp_sendstr(req, "only .txt, .csv, .ir, .nfc, .tone and .json are supported for view");
    }

    if(poom_cli_web_build_storage_path_(relative_file_path, file_path, sizeof(file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid path");
    }

    if((stat(file_path, &st) != 0) || (!S_ISREG(st.st_mode))) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    if((size_t)st.st_size > (size_t)POOM_CLI_WEB_FILES_VIEW_MAX_BYTES) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "file too large to view");
    }

    file = fopen(file_path, "rb");
    if(file == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while((bytes_read = fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
        if(httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            (void)fclose(file);
            return ESP_FAIL;
        }
    }

    (void)fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/**
 * @brief Loads internal data used by this module.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_files_upload_handler_(httpd_req_t* req) {
    char relative_file_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char file_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    char chunk[POOM_CLI_WEB_FILES_IO_CHUNK];
    FILE* file;
    int remaining;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "sd not mounted");
    }

    if(poom_cli_web_get_relative_file_path_(req, relative_file_path, sizeof(relative_file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing or invalid path");
    }
    if(poom_cli_web_build_storage_path_(relative_file_path, file_path, sizeof(file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid path");
    }

    file = fopen(file_path, "wb");
    if(file == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "cannot write file");
    }

    remaining = req->content_len;
    while(remaining > 0) {
        int recv_len = httpd_req_recv(req, chunk, (remaining > (int)sizeof(chunk)) ? (int)sizeof(chunk) : remaining);
        if(recv_len <= 0) {
            (void)fclose(file);
            (void)remove(file_path);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "upload failed");
        }
        if(fwrite(chunk, 1U, (size_t)recv_len, file) != (size_t)recv_len) {
            (void)fclose(file);
            (void)remove(file_path);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "write failed");
        }
        remaining -= recv_len;
    }

    (void)fclose(file);
    return httpd_resp_sendstr(req, "OK");
}

/**
 * @brief Internal helper for `poom_cli_web_http_files_delete_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_files_delete_handler_(httpd_req_t* req) {
    char relative_file_path[POOM_CLI_WEB_FILES_REL_PATH_MAX_LEN] = {0};
    char file_path[POOM_CLI_WEB_FILES_PATH_MAX_LEN] = {0};
    struct stat st;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(poom_cli_web_ensure_sd_mounted_() != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "sd not mounted");
    }

    if(poom_cli_web_get_relative_file_path_(req, relative_file_path, sizeof(relative_file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing or invalid path");
    }
    if(poom_cli_web_build_storage_path_(relative_file_path, file_path, sizeof(file_path)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid path");
    }
    if((stat(file_path, &st) != 0) || (!S_ISREG(st.st_mode))) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    if(remove(file_path) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "file not found");
    }

    return httpd_resp_sendstr(req, "OK");
}

/**
 * @brief Internal helper for `poom_cli_web_trim_command`.
 *
 * @param[in] command_text Parameter passed to the function.
 * @return void
 */
static void poom_cli_web_trim_command_(char* command_text) {
    size_t len;

    if(command_text == NULL) {
        return;
    }

    len = strlen(command_text);
    while((len > 0U) &&
          ((command_text[len - 1U] == '\n') || (command_text[len - 1U] == '\r'))) {
        command_text[len - 1U] = '\0';
        len--;
    }
}

#if CONFIG_HTTPD_WS_SUPPORT

/**
 * @brief Internal helper for `poom_cli_web_http_ws_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_ws_handler_(httpd_req_t* req) {
    httpd_ws_frame_t frame = {0};
    uint8_t* payload = NULL;
    esp_err_t err;

    if(req->method == HTTP_GET) {
        s_poom_cli_web_ws_client_fd = httpd_req_to_sockfd(req);
        POOM_CLI_WEB_PRINTF_I(POOM_CLI_WEB_HTTP_TAG, "WebSocket connected (fd=%d)", s_poom_cli_web_ws_client_fd);
        poom_cli_web_http_log_heap_("ws:connect");
        return ESP_OK;
    }

    poom_cli_web_http_log_heap_("ws:before_recv");

    err = httpd_ws_recv_frame(req, &frame, 0);
    if(err != ESP_OK) {
        POOM_CLI_WEB_PRINTF_E(POOM_CLI_WEB_HTTP_TAG, "WS recv length failed: %s", esp_err_to_name(err));
        return err;
    }

    if(frame.len > POOM_CLI_WEB_WS_MAX_RX_FRAME) {
        POOM_CLI_WEB_PRINTF_W(POOM_CLI_WEB_HTTP_TAG, "WS frame too large: %u", (unsigned)frame.len);
        return ESP_ERR_INVALID_SIZE;
    }

    if(frame.len > 0U) {
        payload = (uint8_t*)calloc(1U, frame.len + 1U);
        if(payload == NULL) {
            poom_cli_web_http_log_heap_("ws:alloc_fail");
            return ESP_ERR_NO_MEM;
        }

        frame.payload = payload;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if(err != ESP_OK) {
            free(payload);
            POOM_CLI_WEB_PRINTF_E(POOM_CLI_WEB_HTTP_TAG, "WS recv payload failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if((frame.type == HTTPD_WS_TYPE_TEXT) && (payload != NULL) && (s_poom_cli_web_command_cb != NULL)) {
        poom_cli_web_trim_command_((char*)payload);
        if(((char*)payload)[0] != '\0') {
            poom_cli_web_http_log_heap_("ws:before_cb");
            s_poom_cli_web_command_cb((const char*)payload, s_poom_cli_web_command_user_ctx);
            poom_cli_web_http_log_heap_("ws:after_cb");
        }
    }

    if(frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        if(fd == s_poom_cli_web_ws_client_fd) {
            s_poom_cli_web_ws_client_fd = -1;
            POOM_CLI_WEB_PRINTF_I(POOM_CLI_WEB_HTTP_TAG, "WebSocket disconnected");
        }
    }

    free(payload);
    poom_cli_web_http_log_heap_("ws:done");
    return ESP_OK;
}
#endif

#if POOM_WEB_ENABLE_TONE_PAGE

/**
 * @brief Internal helper for `poom_cli_web_tone_path_exists`.
 *
 * @param[in] abs_path Parameter passed to the function.
 * @return bool
 */
static bool poom_cli_web_tone_path_exists_(const char* abs_path) {
    struct stat st;
    return (abs_path != NULL) && (stat(abs_path, &st) == 0);
}

/**
 * @brief Internal helper for `poom_cli_web_tone_build_unique_paths`.
 *
 * @param[in] base_name Parameter passed to the function.
 * @param[in] out_rel Parameter passed to the function.
 * @param[in] out_rel_len Parameter passed to the function.
 * @param[in] out_abs Parameter passed to the function.
 * @param[in] out_abs_len Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_tone_build_unique_paths_(const char* base_name,
                                                       char* out_rel,
                                                       size_t out_rel_len,
                                                       char* out_abs,
                                                       size_t out_abs_len) {
    if((base_name == NULL) || (out_rel == NULL) || (out_abs == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    for(int i = 0; i < 1000; i++) {
        if(i == 0) {
            (void)snprintf(out_rel, out_rel_len, "tones/%s.tone", base_name);
        } else {
            (void)snprintf(out_rel, out_rel_len, "tones/%s_%d.tone", base_name, i);
        }
        (void)snprintf(out_abs, out_abs_len, "%s/%s", SD_CARD_PATH, out_rel);

        if(!poom_cli_web_tone_path_exists_(out_abs)) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

/**
 * @brief Internal helper for `poom_cli_web_http_tone_play_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_tone_play_handler_(httpd_req_t* req) {
    char* payload = NULL;
    size_t payload_len = 0U;
    poom_buz_theme_event_t* events = NULL;
    size_t count = 0U;
    uint32_t pause_ms = 0U;
    esp_err_t err;

    err = poom_cli_web_http_read_body_(req, POOM_CLI_WEB_TONE_MAX_BODY, &payload, &payload_len);
    if(err != ESP_OK) {
        if((size_t)req->content_len > POOM_CLI_WEB_TONE_MAX_BODY) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            return httpd_resp_sendstr(req, "tone body too large");
        }
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid body");
    }

    err = poom_cli_web_tone_parse_json_(payload, payload_len, &events, &count, &pause_ms, NULL, 0U);
    free(payload);
    payload = NULL;

    if(err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid tone json");
    }

    buzzer_init(PIN_NUM_BUZZER);
    const bool ok = poom_buz_theme_play_events_take_ownership(events, count, pause_ms);
    events = NULL;

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/**
 * @brief Saves internal data used by this module.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_tone_save_handler_(httpd_req_t* req) {
    char* payload = NULL;
    size_t payload_len = 0U;
    poom_buz_theme_event_t* events = NULL;
    size_t count = 0U;
    uint32_t pause_ms = 0U;
    char name_raw[48];
    char name_safe[32];
    char rel_path[128];
    char abs_path[192];
    esp_err_t err;

    err = poom_cli_web_http_read_body_(req, POOM_CLI_WEB_TONE_MAX_BODY, &payload, &payload_len);
    if(err != ESP_OK) {
        if((size_t)req->content_len > POOM_CLI_WEB_TONE_MAX_BODY) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            return httpd_resp_sendstr(req, "tone body too large");
        }
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid body");
    }

    name_raw[0] = '\0';
    err = poom_cli_web_tone_parse_json_(payload, payload_len, &events, &count, &pause_ms, name_raw, sizeof(name_raw));
    if(err != ESP_OK) {
        free(payload);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid tone json");
    }
    if((events == NULL) || (count == 0U)) {
        free(payload);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid tone json");
    }

    free(payload);
    payload = NULL;

    if(sd_card_is_not_mounted()) {
        sd_card_begin();
        err = sd_card_mount();
        if(err != ESP_OK) {
            free(events);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "sd mount failed");
        }
    }

    if(mkdir(POOM_CLI_WEB_TONE_DIR, 0775) != 0) {
        if(errno != EEXIST) {
            free(events);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "mkdir failed");
        }
    }

    name_safe[0] = '\0';
    if(!poom_cli_web_tone_sanitize_name_(name_raw, name_safe, sizeof(name_safe))) {
        (void)snprintf(name_safe, sizeof(name_safe), "tone");
    }

    err = poom_cli_web_tone_build_unique_paths_(name_safe, rel_path, sizeof(rel_path), abs_path, sizeof(abs_path));
    if(err != ESP_OK) {
        free(events);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "path error");
    }

    FILE* f = fopen(abs_path, "w");
    if(f == NULL) {
        free(events);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "open failed");
    }

    bool ok_write = true;
    if(fprintf(f, "POOMTONE1\nname:%s\npause_ms:%u\nevents:\n", name_safe, (unsigned)pause_ms) < 0) {
        ok_write = false;
    }
    for(size_t i = 0U; ok_write && (i < count); i++) {
        if(fprintf(f, "%u %u\n", (unsigned)events[i].freq_hz, (unsigned)events[i].duration_ms) < 0) {
            ok_write = false;
        }
    }
    (void)fclose(f);
    free(events);

    if(!ok_write) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "write failed");
    }

    char response[256];
    (void)snprintf(response, sizeof(response), "{\"ok\":true,\"path\":\"%s\"}", rel_path);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}
#endif

/**
 * @brief Internal helper for `poom_cli_web_http_command_handler`.
 *
 * @param[in] req Parameter passed to the function.
 * @return esp_err_t
 */
static esp_err_t poom_cli_web_http_command_handler_(httpd_req_t* req) {
    char* payload = NULL;
    size_t payload_len = 0U;
    esp_err_t err;

    if(req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    poom_cli_web_http_log_heap_("cmd:before_read");

    err = poom_cli_web_http_read_body_(req, POOM_CLI_WEB_COMMAND_MAX_BODY, &payload, &payload_len);
    if(err != ESP_OK) {
        if((size_t)req->content_len > POOM_CLI_WEB_COMMAND_MAX_BODY) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            return httpd_resp_sendstr(req, "command too large");
        }
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid command");
    }

    (void)payload_len;
    poom_cli_web_trim_command_(payload);
    if(payload[0] == '\0') {
        free(payload);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "empty command");
    }
#if !CONFIG_HTTPD_WS_SUPPORT
    if(s_poom_cli_web_fallback_text == NULL) {
        free(payload);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "fallback unavailable");
    }
    s_poom_cli_web_fallback_text_pending = false;
    s_poom_cli_web_fallback_text[0] = '\0';
    s_poom_cli_web_fallback_text_len = 0U;
#endif

    if(s_poom_cli_web_command_cb != NULL) {
        poom_cli_web_http_log_heap_("cmd:before_cb");
        s_poom_cli_web_command_cb((const char*)payload, s_poom_cli_web_command_user_ctx);
        poom_cli_web_http_log_heap_("cmd:after_cb");
    }

    free(payload);
    poom_cli_web_http_log_heap_("cmd:done");
#if CONFIG_HTTPD_WS_SUPPORT
    return httpd_resp_sendstr(req, "OK");
#else
    if(s_poom_cli_web_fallback_text_pending) {
        return httpd_resp_sendstr(req, s_poom_cli_web_fallback_text);
    }
    return httpd_resp_sendstr(req, "OK");
#endif
}

esp_err_t poom_web_http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_index_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t css_uri = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_css_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t js_uri = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_js_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_page_uri = {
        .uri = "/ir",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_ir_page_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_css_uri = {
        .uri = "/ir.css",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_ir_css_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_js_uri = {
        .uri = "/ir.js",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_ir_js_handler_,
        .user_ctx = NULL,
    };
#if POOM_WEB_ENABLE_TONE_PAGE
    httpd_uri_t tone_uri = {
        .uri = "/tone",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_tone_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t tone_css_uri = {
        .uri = "/tone.css",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_tone_css_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t tone_js_uri = {
        .uri = "/tone.js",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_tone_js_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t tone_play_uri = {
        .uri = "/tone/play",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_tone_play_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t tone_save_uri = {
        .uri = "/tone/save",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_tone_save_handler_,
        .user_ctx = NULL,
    };
#endif
#if POOM_WEB_ENABLE_MIDI_PAGE
    httpd_uri_t midi_uri = {
        .uri = "/midi",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_midi_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t midi_css_uri = {
        .uri = "/midi.css",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_midi_css_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t midi_js_uri = {
        .uri = "/midi.js",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_midi_js_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t midi_harmony_uri = {
        .uri = "/api/midi_harmony",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_midi_harmony_post_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t midi_harmony_save_uri = {
        .uri = "/api/midi_harmony/save",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_midi_harmony_save_post_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t midi_harmony_stop_uri = {
        .uri = "/api/midi_harmony/stop",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_midi_harmony_stop_post_handler_,
        .user_ctx = NULL,
    };
#endif
    httpd_uri_t command_uri = {
        .uri = "/command",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_command_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t capabilities_uri = {
        .uri = "/capabilities",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_capabilities_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t files_list_uri = {
        .uri = "/files/list",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_files_list_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t files_download_uri = {
        .uri = "/files/download",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_files_download_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t files_view_uri = {
        .uri = "/files/view",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_files_view_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_open_uri = {
        .uri = "/api/ir/open",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_ir_open_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_send_uri = {
        .uri = "/api/ir/send",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_ir_send_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t ir_close_uri = {
        .uri = "/api/ir/close",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_ir_close_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t files_upload_uri = {
        .uri = "/files/upload",
        .method = HTTP_POST,
        .handler = poom_cli_web_http_files_upload_handler_,
        .user_ctx = NULL,
    };
    httpd_uri_t files_delete_uri = {
        .uri = "/files/delete",
        .method = HTTP_DELETE,
        .handler = poom_cli_web_http_files_delete_handler_,
        .user_ctx = NULL,
    };
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = poom_cli_web_http_ws_handler_,
        .user_ctx = NULL,
        .is_websocket = true,
    };
#endif

    if(s_poom_cli_web_http_server != NULL) {
        return ESP_OK;
    }

    poom_cli_web_http_log_heap_("http:start_entry");

#if !CONFIG_HTTPD_WS_SUPPORT
    err = poom_cli_web_fallback_alloc_();
    if(err != ESP_OK) {
        POOM_CLI_WEB_PRINTF_E(POOM_CLI_WEB_HTTP_TAG, "Fallback alloc failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    config.stack_size = POOM_CLI_WEB_HTTP_SERVER_STACK_SIZE;
    config.max_open_sockets = POOM_CLI_WEB_HTTP_SERVER_MAX_OPEN_SOCKETS;
    config.max_uri_handlers = POOM_CLI_WEB_HTTP_SERVER_MAX_URI_HANDLERS;
    config.max_resp_headers = POOM_CLI_WEB_HTTP_SERVER_MAX_RESP_HEADERS;
    config.backlog_conn = POOM_CLI_WEB_HTTP_SERVER_BACKLOG_CONN;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = POOM_CLI_WEB_HTTP_SERVER_RECV_TIMEOUT_S;
    config.send_wait_timeout = POOM_CLI_WEB_HTTP_SERVER_SEND_TIMEOUT_S;
    config.ctrl_port = s_poom_cli_web_http_ctrl_port;

    if(httpd_start(&s_poom_cli_web_http_server, &config) != ESP_OK) {
        poom_cli_web_http_log_heap_("http:start_fail");
        s_poom_cli_web_http_server = NULL;
#if !CONFIG_HTTPD_WS_SUPPORT
        poom_cli_web_fallback_free_();
#endif
        s_poom_cli_web_http_ctrl_port++;
        if(s_poom_cli_web_http_ctrl_port >= (POOM_CLI_WEB_HTTP_CTRL_PORT_BASE + POOM_CLI_WEB_HTTP_CTRL_PORT_SPAN)) {
            s_poom_cli_web_http_ctrl_port = POOM_CLI_WEB_HTTP_CTRL_PORT_BASE;
        }
        return ESP_FAIL;
    }

    s_poom_cli_web_http_ctrl_port++;
    if(s_poom_cli_web_http_ctrl_port >= (POOM_CLI_WEB_HTTP_CTRL_PORT_BASE + POOM_CLI_WEB_HTTP_CTRL_PORT_SPAN)) {
        s_poom_cli_web_http_ctrl_port = POOM_CLI_WEB_HTTP_CTRL_PORT_BASE;
    }

    poom_cli_web_http_log_heap_("http:start_ok");

    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &index_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &css_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &js_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_page_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_css_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_js_uri);
#if POOM_WEB_ENABLE_TONE_PAGE
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &tone_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &tone_css_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &tone_js_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &tone_play_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &tone_save_uri);
#endif
#if POOM_WEB_ENABLE_MIDI_PAGE
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_css_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_js_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_harmony_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_harmony_save_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &midi_harmony_stop_uri);
#endif
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &command_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &capabilities_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &files_list_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &files_download_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &files_view_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_open_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_send_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ir_close_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &files_upload_uri);
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &files_delete_uri);
#if CONFIG_HTTPD_WS_SUPPORT
    (void)httpd_register_uri_handler(s_poom_cli_web_http_server, &ws_uri);
#else
    POOM_CLI_WEB_PRINTF_W(POOM_CLI_WEB_HTTP_TAG, "WebSocket support disabled; using POST /command fallback");
#endif
    POOM_CLI_WEB_PRINTF_I(POOM_CLI_WEB_HTTP_TAG,
                          "HTTP server started on port %d (ctrl=%u)",
                          config.server_port,
                          (unsigned)config.ctrl_port);
    return ESP_OK;
}

esp_err_t poom_web_http_server_stop(void) {
    esp_err_t err;

    if(s_poom_cli_web_http_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    poom_cli_web_http_log_heap_("http:stop_entry");

    err = httpd_stop(s_poom_cli_web_http_server);
    s_poom_cli_web_http_server = NULL;
    s_poom_cli_web_ws_client_fd = -1;
#if !CONFIG_HTTPD_WS_SUPPORT
    poom_cli_web_fallback_free_();
#endif
    poom_web_ir_session_force_reset();
    poom_cli_web_http_log_heap_("http:stop_done");
    return err;
}

esp_err_t poom_web_http_server_send_text(const char* text) {
#if CONFIG_HTTPD_WS_SUPPORT
    const char* cursor;

    if(text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_poom_cli_web_http_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if(s_poom_cli_web_ws_client_fd < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    cursor = text;
    while(*cursor != '\0') {
        poom_web_ws_async_send_ctx_t* ctx;
        esp_err_t err;
        size_t chunk_len = strlen(cursor);

        if(chunk_len >= POOM_CLI_WEB_WS_TX_TEXT_MAX_LEN) {
            chunk_len = POOM_CLI_WEB_WS_TX_TEXT_MAX_LEN - 1U;
        }

        ctx = poom_cli_web_ws_send_slot_acquire_();
        if(ctx == NULL) {
            return ESP_ERR_NO_MEM;
        }

        memcpy(ctx->text, cursor, chunk_len);
        ctx->text[chunk_len] = '\0';
        ctx->text_len = chunk_len;
        ctx->server = s_poom_cli_web_http_server;
        ctx->socket_fd = s_poom_cli_web_ws_client_fd;

        err = httpd_queue_work(s_poom_cli_web_http_server, poom_cli_web_ws_send_async_work_, ctx);
        if(err != ESP_OK) {
            poom_cli_web_ws_send_slot_release_(ctx);
            return err;
        }

        cursor += chunk_len;
    }

    return ESP_OK;
#else
    if(text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    poom_cli_web_fallback_append_(text);
    return ESP_OK;
#endif
}

esp_err_t poom_web_http_server_set_command_cb(poom_web_command_cb_t cb, void* user_ctx) {
    s_poom_cli_web_command_cb = cb;
    s_poom_cli_web_command_user_ctx = user_ctx;
    return ESP_OK;
}
