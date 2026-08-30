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

#define POOM_SD_BROWSER_MAX_ROWS (4U)
#define POOM_SD_BROWSER_ITEM_LINE_MAX_LEN (21U)
#define POOM_SD_BROWSER_ITEM_NAME_VISIBLE_CHARS (19U)
#define POOM_SD_BROWSER_FILE_NAME_MAX_LEN (40U)
#define POOM_SD_BROWSER_FILE_DETAIL_NAME_VISIBLE_CHARS (12U)
#define POOM_SD_BROWSER_HEADER_H (11)
#define POOM_SD_BROWSER_LIST_Y0 (14)
#define POOM_SD_BROWSER_ROW_STEP (10)
#define POOM_SD_BROWSER_ROW_HILITE_H (9)

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

static char s_start_dir[POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN] = POOM_SD_BROWSER_STORAGE_ROOT;
static poom_sd_browser_filter_cb_t s_filter_cb = NULL;
static void* s_filter_ctx = NULL;
static poom_sd_browser_file_selected_cb_t s_file_selected_cb = NULL;
static void* s_file_selected_ctx = NULL;

/**
 * @brief Internal helper for `poom_sd_browser_filter_bridge`.
 *
 * @param[in] name Parameter passed to the function.
 * @param[in] is_directory Parameter passed to the function.
 * @param[in] user_ctx Parameter passed to the function.
 * @return bool
 */
static bool poom_sd_browser_filter_bridge_(const char* name, bool is_directory, void* user_ctx)
{
    (void)user_ctx;

    if(s_filter_cb == NULL)
    {
        return true;
    }

    return s_filter_cb(name, is_directory, s_filter_ctx);
}


/**
 * @brief Internal helper for `poom_sd_browser_row_y`.
 *
 * @param[in] row Parameter passed to the function.
 * @return inline int16_t
 */
static inline int16_t poom_sd_browser_row_y_(uint8_t row)
{
    return (int16_t)((int16_t)row * 8);
}

/**
 * @brief Internal helper for `poom_sd_browser_text_width_px`.
 *
 * @param[in] s Parameter passed to the function.
 * @return inline size_t
 */
static inline size_t poom_sd_browser_text_width_px_(const char* s)
{
    if(s == NULL)
    {
        return 0U;
    }

    return strlen(s) * 6U;
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] x Parameter passed to the function.
 * @param[in] row Parameter passed to the function.
 * @return void
 */
static void poom_sd_browser_draw_text_row_(const char* text, int16_t x, uint8_t row)
{
    poom_arduboy_set_cursor(x, poom_sd_browser_row_y_(row));
    (void)poom_arduboy_print((text != NULL) ? text : "");
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] text Parameter passed to the function.
 * @param[in] row Parameter passed to the function.
 * @return void
 */
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

/**
 * @brief Draws the module header.
 *
 * @param[in] title Parameter passed to the function.
 * @return void
 */
static void poom_sd_browser_draw_header_(const char* title)
{
    const size_t w = poom_sd_browser_text_width_px_(title);
    int16_t x = 0;

    if(w < (size_t)ARDUBOY_WIDTH)
    {
        x = (int16_t)((ARDUBOY_WIDTH - (int16_t)w) / 2);
    }

    poom_arduboy_set_cursor(x, 2);
    (void)poom_arduboy_print((title != NULL) ? title : "");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, POOM_SD_BROWSER_HEADER_H, INVERT);
}

/**
 * @brief Formats internal text for display.
 *
 * @param[in] storage Parameter passed to the function.
 * @param[in] out_title Parameter passed to the function.
 * @param[in] out_title_len Parameter passed to the function.
 * @return void
 */
static void poom_sd_browser_format_header_title_(const poom_sd_browser_storage_t* storage,
                                                 char* out_title,
                                                 size_t out_title_len)
{
    const char* base_name;

    if((out_title == NULL) || (out_title_len == 0U))
    {
        return;
    }

    if((storage == NULL) ||
       (storage->current_path[0] == '\0') ||
       (strcmp(storage->current_path, POOM_SD_BROWSER_STORAGE_ROOT) == 0))
    {
        (void)snprintf(out_title, out_title_len, "SD");
        return;
    }

    base_name = strrchr(storage->current_path, '/');
    base_name = (base_name != NULL) ? (base_name + 1) : storage->current_path;
    if(base_name[0] == '\0')
    {
        (void)snprintf(out_title, out_title_len, "SD");
        return;
    }

    (void)snprintf(out_title, out_title_len, "%.21s", base_name);
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

    poom_sd_browser_draw_header_("SD");
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, (int16_t)(ARDUBOY_HEIGHT - 12), WHITE);

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
 * @brief Draws a mount-failure screen with a more specific message.
 *
 * @param[in] err Error returned by `sd_card_mount()`.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_mount_error_(esp_err_t err)
{
    if(err == ESP_ERR_NOT_SUPPORTED)
    {
        return poom_sd_browser_draw_status_("Bad filesystem", "Use SD menu", true);
    }

    if(err == ESP_ERR_NOT_FOUND)
    {
        return poom_sd_browser_draw_status_("Cannot access SD", "Check card", true);
    }

    return poom_sd_browser_draw_status_("SD mount error", "Check card", true);
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
 * @brief Draws current directory listing on display.
 *
 * @param[in] storage Storage context.
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_draw_list_(const poom_sd_browser_storage_t* storage)
{
    char header_title[22];
    size_t row;

    if(storage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)poom_sd_browser_sync_offset_(storage);
    poom_sd_browser_format_header_title_(storage, header_title, sizeof(header_title));

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_sd_browser_draw_header_(header_title);

    if(storage->items_count == 0U)
    {
        poom_arduboy_set_cursor(38, 30);
        (void)poom_arduboy_print(F("(empty)"));
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
            const int16_t y = (int16_t)(POOM_SD_BROWSER_LIST_Y0 + row * POOM_SD_BROWSER_ROW_STEP);
            const int visible_chars = item->is_directory ? (int)(POOM_SD_BROWSER_ITEM_NAME_VISIBLE_CHARS - 1U)
                                                         : (int)POOM_SD_BROWSER_ITEM_NAME_VISIBLE_CHARS;

            (void)snprintf(item_line,
                           sizeof(item_line),
                           "%.*s%s",
                           visible_chars,
                           item->name,
                           item->is_directory ? "/" : "");
            poom_arduboy_set_cursor(2, y);
            (void)poom_arduboy_print(item_line);

            if(index == storage->selected_index)
            {
                poom_arduboy_fill_rect(1, y - 1, (int16_t)(ARDUBOY_WIDTH - 2), POOM_SD_BROWSER_ROW_HILITE_H, INVERT);
            }
        }
    }

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
    if(s_file_selected_cb != NULL)
    {
        poom_sd_browser_draw_text_row_("A:Select", 0, 7);
        poom_sd_browser_draw_text_row_("B:Back", 76, 7);
    }
    else
    {
        poom_sd_browser_draw_text_row_("B:Back", 0, 7);
    }
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
 * @brief Internal helper for `poom_sd_browser_emit_file_selected`.
 *
 * @return esp_err_t
 */
static esp_err_t poom_sd_browser_emit_file_selected_(void)
{
    char abs_path[POOM_SD_BROWSER_STORAGE_MAX_PATH_LEN];

    if(s_file_selected_cb == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    abs_path[0] = '\0';
    if((s_storage.current_path[0] == '\0') || (s_selected_file_name[0] == '\0'))
    {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(abs_path, sizeof(abs_path), "%s/%s", s_storage.current_path, s_selected_file_name);
    if((written < 0) || ((size_t)written >= sizeof(abs_path)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)poom_sd_browser_stop();
    s_file_selected_cb(abs_path, s_file_selected_ctx);
    return ESP_OK;
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
        else if(s_file_selected_cb != NULL)
        {
            err = poom_sd_browser_emit_file_selected_();
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

/**
 * @brief Internal helper for `poom_sd_browser_ensure_buttons_subscribed`.
 *
 * @return esp_err_t
 */
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
    return poom_sd_browser_start_ex(NULL);
}

esp_err_t poom_sd_browser_start_ex(const poom_sd_browser_config_t* config)
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
            (void)poom_sd_browser_draw_mount_error_(err);
            (void)poom_sd_browser_ensure_buttons_subscribed_();
            memset(&s_storage, 0, sizeof(s_storage));
            s_error_mode = true;
            s_running = true;
            return err;
        }
    }

    if(config != NULL)
    {
        (void)snprintf(s_start_dir, sizeof(s_start_dir), "%s", (config->start_dir != NULL) ? config->start_dir : POOM_SD_BROWSER_STORAGE_ROOT);
        s_filter_cb = config->filter;
        s_filter_ctx = config->filter_ctx;
        s_file_selected_cb = config->on_file_selected;
        s_file_selected_ctx = config->on_file_selected_ctx;
    }
    else
    {
        (void)snprintf(s_start_dir, sizeof(s_start_dir), "%s", POOM_SD_BROWSER_STORAGE_ROOT);
        s_filter_cb = NULL;
        s_filter_ctx = NULL;
        s_file_selected_cb = NULL;
        s_file_selected_ctx = NULL;
    }

    err = poom_sd_browser_storage_init_at(&s_storage, s_start_dir);
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

    err = poom_sd_browser_storage_reload(&s_storage, poom_sd_browser_filter_bridge_, NULL);
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
