// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_sd_browser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Arduboy2.h"
#include "poom_sd_browser_storage.h"
#include "poom_sbus.h"
#include "sd_card.h"

#if POOM_SD_BROWSER_ENABLE_LOG
    static const char* POOM_SD_BROWSER_TAG = "poom_sd_browser";

    #define POOM_SD_BROWSER_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_SD_BROWSER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_SD_BROWSER_PRINTF_W(fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", POOM_SD_BROWSER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_SD_BROWSER_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_SD_BROWSER_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_SD_BROWSER_DEBUG_LOG_ENABLED
        #define POOM_SD_BROWSER_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_SD_BROWSER_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_SD_BROWSER_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_SD_BROWSER_PRINTF_E(...) do { } while (0)
    #define POOM_SD_BROWSER_PRINTF_W(...) do { } while (0)
    #define POOM_SD_BROWSER_PRINTF_I(...) do { } while (0)
    #define POOM_SD_BROWSER_PRINTF_D(...) do { } while (0)
#endif

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

#ifndef BTN_UP
#define BTN_UP (4U)
#endif

#ifndef BTN_DOWN
#define BTN_DOWN (5U)
#endif

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

#define POOM_SD_BROWSER_MAX_ROWS (5U)
#define POOM_SD_BROWSER_PATH_LINE_MAX_LEN (21U)
#define POOM_SD_BROWSER_ITEM_LINE_MAX_LEN (21U)
#define POOM_SD_BROWSER_ITEM_NAME_VISIBLE_CHARS (12U)
#define POOM_SD_BROWSER_FILE_NAME_MAX_LEN (40U)
#define POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS (12U)
#define POOM_SD_BROWSER_SCROLLBAR_X (123)
#define POOM_SD_BROWSER_SCROLLBAR_Y (16)
#define POOM_SD_BROWSER_SCROLLBAR_W (4)
#define POOM_SD_BROWSER_SCROLLBAR_H (38)
#define POOM_SD_BROWSER_SCROLLBAR_THUMB_MIN_H (6)

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} poom_sd_browser_button_event_t;

static poom_sd_browser_storage_t s_storage;
static bool s_running = false;
static bool s_buttons_subscribed = false;
static bool s_file_details_mode = false;
static bool s_error_mode = false;
static size_t s_list_offset = 0U;
static char s_selected_file_name[POOM_SD_BROWSER_FILE_NAME_MAX_LEN + 1U] = {0};
static size_t s_selected_file_size = 0U;
static poom_sd_browser_exit_cb_t s_exit_callback = NULL;
static void* s_exit_callback_ctx = NULL;


static inline int16_t poom_sd_browser_row_y_(uint8_t row)
{
    return (int16_t)((int16_t)row * 8);
}

static inline size_t poom_sd_browser_text_width_px_(const char* s)
{
    if(s == NULL)
    {
        return 0U;
    }

    return strlen(s) * 6U;
}

static void poom_sd_browser_draw_text_row_(const char* text, int16_t x, uint8_t row)
{
    poom_arduboy_set_cursor(x, poom_sd_browser_row_y_(row));
    (void)poom_arduboy_print((text != NULL) ? text : "");
}

static void poom_sd_browser_draw_text_center_row_(const char* text, uint8_t row)
{
    const size_t w = poom_sd_browser_text_width_px_(text);
    int16_t x = 0;

    if(w < (size_t)ARDUBOY_WIDTH)
    {
        x = (int16_t)((ARDUBOY_WIDTH - (int16_t)w) / 2);
    }

    poom_sd_browser_draw_text_row_(text, x, row);
}

static void poom_sd_browser_draw_header_(const char* title)
{
    poom_sd_browser_draw_text_center_row_(title, 0);
    // Invert after drawing so the title becomes black-on-white.
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 8, INVERT);
}

/**
 * @brief Formats file name for details screen keeping extension when possible.
 *
 * @param[in] input_name Source file name.
 * @param[out] out_name Output formatted name.
 * @param[in] out_name_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_format_file_detail_name_(const char* input_name, char* out_name, size_t out_name_len)
{
    size_t input_len;
    const char* dot_ptr;
    size_t ext_len;
    size_t prefix_len;

    if((input_name == NULL) || (out_name == NULL) || (out_name_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    input_len = strlen(input_name);
    if(input_len <= POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS)
    {
        snprintf(out_name, out_name_len, "%s", input_name);
        return ESP_OK;
    }

    dot_ptr = strrchr(input_name, '.');
    if((dot_ptr != NULL) && (dot_ptr > input_name))
    {
        ext_len = strlen(dot_ptr);
        if(ext_len < (POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS - 3U))
        {
            prefix_len = POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS - 3U - ext_len;
            snprintf(out_name, out_name_len, "%.*s...%s", (int)prefix_len, input_name, dot_ptr);
            return ESP_OK;
        }
    }

    snprintf(
        out_name,
        out_name_len,
        "%.*s...",
        (int)(POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS - 3U),
        input_name);
    return ESP_OK;
}

/**
 * @brief Draws a centered status message screen.
 *
 * @param[in] line0 Top line text.
 * @param[in] line1 Bottom line text.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_status_(const char* line0, const char* line1, bool show_nav)
{
    char text0[22];
    char text1[22];

    snprintf(text0, sizeof(text0), "%s", (line0 != NULL) ? line0 : "");
    snprintf(text1, sizeof(text1), "%s", (line1 != NULL) ? line1 : "");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_sd_browser_draw_header_("SD BROWSER");
    poom_arduboy_draw_rect(0, 10, ARDUBOY_WIDTH, (int16_t)(ARDUBOY_HEIGHT - 10), WHITE);

    poom_sd_browser_draw_text_center_row_(text0, 3);
    poom_sd_browser_draw_text_center_row_(text1, 5);

    if(show_nav)
    {
        poom_sd_browser_draw_text_row_("A:Retry", 0, 7);
        poom_sd_browser_draw_text_row_("B:Back", 76, 7);
    }

    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Formats path line for OLED with tail truncation.
 *
 * @param[in] in_path Full path.
 * @param[out] out_line Output path line.
 * @param[in] out_line_len Output buffer length.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_format_path_line_(const char* in_path, char* out_line, size_t out_line_len)
{
    size_t in_len;

    if((in_path == NULL) || (out_line == NULL) || (out_line_len == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    in_len = strlen(in_path);
    if(in_len <= (POOM_SD_BROWSER_PATH_LINE_MAX_LEN - 1U))
    {
        snprintf(out_line, out_line_len, "%s", in_path);
        return ESP_OK;
    }

    snprintf(
        out_line,
        out_line_len,
        "...%s",
        &in_path[in_len - (POOM_SD_BROWSER_PATH_LINE_MAX_LEN - 4U)]);

    return ESP_OK;
}

/**
 * @brief Keeps selected item visible by adjusting list offset.
 *
 * @param[in] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_sync_offset_(const poom_sd_browser_storage_t* storage)
{
    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(storage->items_count <= POOM_SD_BROWSER_MAX_ROWS)
    {
        s_list_offset = 0U;
        return ESP_OK;
    }

    if(storage->selected_index < s_list_offset)
    {
        s_list_offset = storage->selected_index;
    }
    else if(storage->selected_index >= (s_list_offset + POOM_SD_BROWSER_MAX_ROWS))
    {
        s_list_offset = storage->selected_index - (POOM_SD_BROWSER_MAX_ROWS - 1U);
    }

    return ESP_OK;
}

/**
 * @brief Draws right-side scrollbar for directory listing.
 *
 * @param[in] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_scrollbar_(const poom_sd_browser_storage_t* storage)
{
    size_t thumb_h;
    size_t thumb_y;
    size_t max_offset;
    size_t free_track_h;
    size_t x;
    size_t y;
    size_t w;
    size_t h;

    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(storage->items_count <= POOM_SD_BROWSER_MAX_ROWS)
    {
        return ESP_OK;
    }

    x = (size_t)POOM_SD_BROWSER_SCROLLBAR_X;
    y = (size_t)POOM_SD_BROWSER_SCROLLBAR_Y;
    w = (size_t)POOM_SD_BROWSER_SCROLLBAR_W;
    h = (size_t)POOM_SD_BROWSER_SCROLLBAR_H;

    poom_arduboy_draw_rect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, WHITE);

    thumb_h = (h * POOM_SD_BROWSER_MAX_ROWS) / storage->items_count;
    if(thumb_h < POOM_SD_BROWSER_SCROLLBAR_THUMB_MIN_H)
    {
        thumb_h = POOM_SD_BROWSER_SCROLLBAR_THUMB_MIN_H;
    }
    if(thumb_h > h)
    {
        thumb_h = h;
    }

    max_offset = storage->items_count - POOM_SD_BROWSER_MAX_ROWS;
    free_track_h = h - thumb_h;
    thumb_y = y;
    if((max_offset > 0U) && (free_track_h > 0U))
    {
        thumb_y = y + ((free_track_h * s_list_offset) / max_offset);
    }

    if((w > 2U) && (thumb_h > 2U))
    {
        poom_arduboy_fill_rect((int16_t)(x + 1U), (int16_t)(thumb_y + 1U), (int16_t)(w - 2U), (int16_t)(thumb_h - 2U), WHITE);
    }

    return ESP_OK;
}

/**
 * @brief Draws current directory listing on display.
 *
 * @param[in] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_list_(const poom_sd_browser_storage_t* storage)
{
    char path_line[POOM_SD_BROWSER_PATH_LINE_MAX_LEN + 1U];
    size_t row;

    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)poom_sd_browser_sync_offset_(storage);
    (void)poom_sd_browser_format_path_line_(storage->current_path, path_line, sizeof(path_line));

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_sd_browser_draw_header_("SD BROWSER");
    poom_sd_browser_draw_text_row_(path_line, 0, 1);
    poom_arduboy_draw_fast_hline(0, 15, ARDUBOY_WIDTH, WHITE);

    if(storage->items_count == 0U)
    {
        poom_sd_browser_draw_text_center_row_("(empty)", 4);
        poom_sd_browser_draw_text_row_("B:Back", 0, 7);
        poom_arduboy_display();
        return ESP_OK;
    }

    for(row = 0U; row < POOM_SD_BROWSER_MAX_ROWS; row++)
    {
        size_t index = s_list_offset + row;

        if(index >= storage->items_count)
        {
            break;
        }

        {
            char item_line[POOM_SD_BROWSER_ITEM_LINE_MAX_LEN + 1U];
            const poom_sd_browser_storage_item_t* item = &storage->items[index];
            const char* prefix = item->is_directory ? "D:" : "F:";
            const uint8_t row_idx = (uint8_t)(2U + row);
            const int16_t y = poom_sd_browser_row_y_(row_idx);

            snprintf(item_line, sizeof(item_line), "%s%.*s", prefix, (int)POOM_SD_BROWSER_ITEM_NAME_VISIBLE_CHARS, item->name);
            poom_sd_browser_draw_text_row_(item_line, 0, row_idx);

            if(index == storage->selected_index)
            {
                // Invert only the list area so the scrollbar stays readable.
                poom_arduboy_fill_rect(0, y, (int16_t)POOM_SD_BROWSER_SCROLLBAR_X, 8, INVERT);
            }
        }
    }

    (void)poom_sd_browser_draw_scrollbar_(storage);

    poom_sd_browser_draw_text_row_("A:Open", 0, 7);
    poom_sd_browser_draw_text_row_("B:Back", 76, 7);

    poom_arduboy_display();
    return ESP_OK;
}

/**
 * @brief Draws selected file details screen.
 *
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_file_details_(void)
{
    char line_name[22];
    char line_size[22];
    const int box_x = 2;
    const int box_y = 18;
    const int box_w = 124;
    const int box_h = 34;
    const int text_x = 6;

    (void)poom_sd_browser_format_file_detail_name_(s_selected_file_name, line_name, sizeof(line_name));
    snprintf(line_size, sizeof(line_size), "SIZE: %u B", (unsigned int)s_selected_file_size);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_sd_browser_draw_header_("FILE");
    poom_arduboy_draw_rect((int16_t)box_x, (int16_t)box_y, (int16_t)box_w, (int16_t)box_h, WHITE);
    poom_sd_browser_draw_text_row_(line_name, (int16_t)text_x, 3);
    poom_sd_browser_draw_text_row_(line_size, (int16_t)text_x, 5);
    poom_sd_browser_draw_text_row_("B:Back", 0, 7);
    poom_arduboy_display();

    return ESP_OK;
}

/**
 * @brief Invokes configured exit callback.
 *
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_emit_exit_callback_(void)
{
    if(s_exit_callback != NULL)
    {
        s_exit_callback(s_exit_callback_ctx);
    }

    return ESP_OK;
}

/**
 * @brief Opens selected item as directory or file details.
 *
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_open_selected_(void)
{
    bool is_file = false;
    esp_err_t err;

    err = poom_sd_browser_storage_enter_selected(&s_storage, &is_file);
    if(err != ESP_OK)
    {
        return err;
    }

    if(is_file)
    {
        const poom_sd_browser_storage_item_t* item = poom_sd_browser_storage_get_selected(&s_storage);

        if(item == NULL)
        {
            return ESP_ERR_NOT_FOUND;
        }

        strncpy(s_selected_file_name, item->name, sizeof(s_selected_file_name) - 1U);
        s_selected_file_name[sizeof(s_selected_file_name) - 1U] = '\0';
        s_selected_file_size = item->file_size_bytes;
        s_file_details_mode = true;
        return poom_sd_browser_draw_file_details_();
    }

    s_file_details_mode = false;
    return poom_sd_browser_draw_list_(&s_storage);
}

/**
 * @brief Processes back action depending on current browser mode.
 *
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_go_back_(void)
{
    esp_err_t err;

    if(s_file_details_mode)
    {
        s_file_details_mode = false;
        return poom_sd_browser_draw_list_(&s_storage);
    }

    if(s_storage.is_root)
    {
        err = poom_sd_browser_stop();
        if(err != ESP_OK)
        {
            return err;
        }

        return poom_sd_browser_emit_exit_callback_();
    }

    err = poom_sd_browser_storage_go_parent(&s_storage);
    if(err != ESP_OK)
    {
        return err;
    }

    return poom_sd_browser_draw_list_(&s_storage);
}

/**
 * @brief Handles button event message for SD browser controls.
 *
 * @param[in] event Button event payload.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_handle_button_event_(const poom_sd_browser_button_event_t* event)
{
    esp_err_t err = ESP_OK;

    if(event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(event->event != BUTTON_SINGLE_CLICK)
    {
        return ESP_OK;
    }

    // If start failed (mount/read), keep a minimal UI where:
    // - A/RIGHT retries
    // - B/LEFT goes back to the caller via exit callback
    if(s_error_mode)
    {
        if((event->button == BTN_B) || (event->button == BTN_LEFT))
        {
            (void)poom_sd_browser_stop();
            return poom_sd_browser_emit_exit_callback_();
        }
        if((event->button == BTN_A) || (event->button == BTN_RIGHT))
        {
            (void)poom_sd_browser_stop();
            return poom_sd_browser_start();
        }
        return ESP_OK;
    }

    if(event->button == BTN_UP)
    {
        if(!s_file_details_mode)
        {
            (void)poom_sd_browser_storage_select_prev(&s_storage);
            err = poom_sd_browser_draw_list_(&s_storage);
        }
    }
    else if(event->button == BTN_DOWN)
    {
        if(!s_file_details_mode)
        {
            (void)poom_sd_browser_storage_select_next(&s_storage);
            err = poom_sd_browser_draw_list_(&s_storage);
        }
    }
    else if((event->button == BTN_A) || (event->button == BTN_RIGHT))
    {
        if(!s_file_details_mode)
        {
            err = poom_sd_browser_open_selected_();
        }
    }
    else if((event->button == BTN_B) || (event->button == BTN_LEFT))
    {
        err = poom_sd_browser_go_back_();
    }

    return err;
}

/**
 * @brief SBUS callback for button topic.
 *
 * @param[in] msg Input SBUS message.
 * @param[in] user User context pointer.
 * @return void
 */
static void poom_sd_browser_button_topic_handler_(const poom_sbus_msg_t* msg, void* user)
{
    poom_sd_browser_button_event_t event;

    (void)user;

    if((msg == NULL) || (msg->len < sizeof(event)))
    {
        return;
    }

    (void)memcpy(&event, msg->data, sizeof(event));
    if(poom_sd_browser_handle_button_event_(&event) != ESP_OK)
    {
        POOM_SD_BROWSER_PRINTF_D("button handling returned error");
    }
}

static esp_err_t poom_sd_browser_ensure_buttons_subscribed_(void)
{
    if(s_buttons_subscribed)
    {
        return ESP_OK;
    }

    if(!poom_sbus_subscribe_cb("input/button", poom_sd_browser_button_topic_handler_, "poom_sd_browser"))
    {
        POOM_SD_BROWSER_PRINTF_E("poom_sbus_subscribe_cb failed");
        return ESP_FAIL;
    }

    s_buttons_subscribed = true;
    return ESP_OK;
}

/**
 * @brief Starts SD browser and subscribes button handling.
 *
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_start(void)
{
    esp_err_t err;

    if(s_running)
    {
        return ESP_OK;
    }

    s_error_mode = false;

    (void)poom_sd_browser_draw_status_("Mounting SD...", "Please wait", false);

    sd_card_begin();
    if(sd_card_is_not_mounted())
    {
        err = sd_card_mount();
        if(err != ESP_OK)
        {
            POOM_SD_BROWSER_PRINTF_E("sd_card_mount failed: %s", esp_err_to_name(err));
            (void)poom_sd_browser_draw_status_("SD mount error", "Check card", true);
            (void)poom_sd_browser_ensure_buttons_subscribed_();
            memset(&s_storage, 0, sizeof(s_storage));
            s_error_mode = true;
            s_running = true;
            return err;
        }
    }

    err = poom_sd_browser_storage_init(&s_storage);
    if(err != ESP_OK)
    {
        POOM_SD_BROWSER_PRINTF_E("storage init failed: %s", esp_err_to_name(err));
        (void)poom_sd_browser_draw_status_("Storage error", "Init failed", true);
        (void)poom_sd_browser_ensure_buttons_subscribed_();
        memset(&s_storage, 0, sizeof(s_storage));
        s_error_mode = true;
        s_running = true;
        return err;
    }

    err = poom_sd_browser_storage_reload(&s_storage);
    if(err != ESP_OK)
    {
        POOM_SD_BROWSER_PRINTF_W("storage reload failed: %s", esp_err_to_name(err));
        (void)poom_sd_browser_storage_deinit(&s_storage);
        (void)poom_sd_browser_draw_status_("Read error", "Cannot list SD", true);
        (void)poom_sd_browser_ensure_buttons_subscribed_();
        memset(&s_storage, 0, sizeof(s_storage));
        s_error_mode = true;
        s_running = true;
        return err;
    }

    err = poom_sd_browser_ensure_buttons_subscribed_();
    if(err != ESP_OK)
    {
        (void)poom_sd_browser_storage_deinit(&s_storage);
        memset(&s_storage, 0, sizeof(s_storage));
        (void)poom_sd_browser_draw_status_("Input error", "No buttons", true);
        s_error_mode = true;
        s_running = true;
        return err;
    }

    s_running = true;
    s_file_details_mode = false;
    s_list_offset = 0U;

    POOM_SD_BROWSER_PRINTF_I("started");
    return poom_sd_browser_draw_list_(&s_storage);
}

/**
 * @brief Stops SD browser and unsubscribes button handling.
 *
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_stop(void)
{
    if(!s_running)
    {
        return ESP_OK;
    }

    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", poom_sd_browser_button_topic_handler_, "poom_sd_browser");
        s_buttons_subscribed = false;
    }

    (void)poom_sd_browser_storage_deinit(&s_storage);
    memset(&s_storage, 0, sizeof(s_storage));

    s_file_details_mode = false;
    s_error_mode = false;
    s_list_offset = 0U;
    s_running = false;

    POOM_SD_BROWSER_PRINTF_I("stopped");
    return ESP_OK;
}

/**
 * @brief Checks whether SD browser is running.
 *
 * @return bool
 */
bool poom_sd_browser_is_running(void)
{
    return s_running;
}

/**
 * @brief Registers exit callback executed when user exits at SD root.
 *
 * @param[in] callback Exit callback. NULL disables callback.
 * @param[in] user_ctx User context pointer passed to callback.
 * @return esp_err_t
 */
esp_err_t poom_sd_browser_set_exit_callback(poom_sd_browser_exit_cb_t callback, void* user_ctx)
{
    s_exit_callback = callback;
    s_exit_callback_ctx = user_ctx;
    return ESP_OK;
}
