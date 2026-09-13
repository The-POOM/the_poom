// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_game_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_events.h"
#include "poom_boot_policy.h"
#include "poom_game_store.h"
#include "poom_sbus.h"
#include "poom_secrets_store.h"
#include "poom_wifi_ctrl.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define GAME_STORE_REFRESH_MS          (100U)
#define GAME_STORE_TASK_STACK          (12288U)
#define GAME_STORE_TASK_PRIORITY       (4U)
#define GAME_STORE_CONNECT_TIMEOUT_MS  (15000U)
#define GAME_STORE_VISIBLE_ROWS        (4U)
#define GAME_STORE_LIST_TEXT_X         (2)
#define GAME_STORE_LIST_ARROW_X        (120)
#define GAME_STORE_LIST_TEXT_CHARS     ((GAME_STORE_LIST_ARROW_X - GAME_STORE_LIST_TEXT_X) / 6U)
#define GAME_STORE_SCREEN_TEXT_X       (4)
#define GAME_STORE_SCREEN_TEXT_CHARS   (((ARDUBOY_WIDTH) - GAME_STORE_SCREEN_TEXT_X) / 6U)
#define GAME_STORE_SCROLL_STEP_MS      (350U)
#define GAME_STORE_SCROLL_GAP_CHARS    (3U)
#define GAME_STORE_WIFI_SSID_MAX       (32U)
#define GAME_STORE_WIFI_PASS_MAX       (64U)

#ifndef BTN_A
#define BTN_A (0U)
#endif
#ifndef BTN_B
#define BTN_B (1U)
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

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} game_store_button_msg_t;

typedef enum
{
    GAME_STORE_STATE_HOME = 0,
    GAME_STORE_STATE_CONNECTING,
    GAME_STORE_STATE_CATALOG,
    GAME_STORE_STATE_CATEGORY,
    GAME_STORE_STATE_GALLERY,
    GAME_STORE_STATE_DETAIL,
    GAME_STORE_STATE_DOWNLOADING,
    GAME_STORE_STATE_READY,
    GAME_STORE_STATE_INSTALLING,
    GAME_STORE_STATE_RESULT,
} game_store_state_t;

typedef enum
{
    GAME_STORE_HOME_BROWSE = 0,
    GAME_STORE_HOME_DOWNLOAD_ALL,
    GAME_STORE_HOME_OFFLINE,
    GAME_STORE_HOME_COUNT,
} game_store_home_option_t;

typedef enum
{
    GAME_STORE_READY_INSTALL = 0,
    GAME_STORE_READY_KEEP,
    GAME_STORE_READY_COUNT,
} game_store_ready_option_t;

typedef enum
{
    GAME_STORE_ACTION_GET = 0,
    GAME_STORE_ACTION_UPDATE,
    GAME_STORE_ACTION_PLAY,
} game_store_catalog_action_t;

typedef struct
{
    const poom_game_store_game_t* game;
    size_t game_index;
    size_t game_count;
    TickType_t last_draw_tick;
    uint8_t last_percent;
} game_store_progress_t;

typedef char game_store_category_name_t[POOM_GAME_STORE_CATEGORY_MAX];

typedef struct
{
    poom_game_store_catalog_t catalog;
    char installed_game_id[POOM_GAME_STORE_ID_MAX];
    char installed_game_version[POOM_GAME_STORE_VERSION_MAX];
    char downloaded_path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    char result_title[22];
    char result_line1[22];
    char result_line2[22];
    game_store_category_name_t* categories;
    uint8_t* thumbnail_bitmap;
} game_store_buffers_t;

static bool s_active = false;
static bool s_buttons_subscribed = false;
static bool s_wifi_events_registered = false;
static volatile bool s_exit_requested = false;
static volatile bool s_action_requested = false;
static volatile bool s_cancel_requested = false;
static TaskHandle_t s_task = NULL;
static char s_sbus_user[] = "menu_poom_game_store";
static game_store_buffers_t* s_buffers = NULL;

#define s_catalog               (s_buffers->catalog)
#define s_installed_game_id     (s_buffers->installed_game_id)
#define s_installed_game_version (s_buffers->installed_game_version)
#define s_downloaded_path       (s_buffers->downloaded_path)
#define s_result_title          (s_buffers->result_title)
#define s_result_line1          (s_buffers->result_line1)
#define s_result_line2          (s_buffers->result_line2)
#define s_categories            (s_buffers->categories)
#define s_thumbnail_bitmap      (s_buffers->thumbnail_bitmap)

static game_store_state_t s_state = GAME_STORE_STATE_HOME;
static game_store_home_option_t s_home_option = GAME_STORE_HOME_BROWSE;
static game_store_ready_option_t s_ready_option = GAME_STORE_READY_INSTALL;
static size_t s_category_count = 0U;
static size_t s_selected_category = 0U;
static size_t s_category_list_start = 0U;
static size_t s_selected_game = 0U;
static TickType_t s_list_scroll_tick = 0;
static TickType_t s_screen_scroll_tick = 0;
static size_t s_thumbnail_game_index = SIZE_MAX;
static bool s_thumbnail_loaded = false;
static bool s_installed_present = false;
static bool s_installed_identity_known = false;
static bool s_selected_file_on_sd = false;
static bool s_offline_mode = false;

static void game_store_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void game_store_task_(void* task_arg);

static void game_store_format_scrolling_text_(char* out,
                                              size_t out_len,
                                              const char* text,
                                              size_t visible_chars,
                                              TickType_t start_tick)
{
    size_t text_len;

    if((out == NULL) || (out_len == 0U))
    {
        return;
    }
    out[0] = '\0';
    if((text == NULL) || (visible_chars == 0U))
    {
        return;
    }

    text_len = strlen(text);
    if(text_len <= visible_chars)
    {
        (void)snprintf(out, out_len, "%.*s", (int)visible_chars, text);
        return;
    }

    const size_t cycle_width = text_len + GAME_STORE_SCROLL_GAP_CHARS;
    const TickType_t elapsed = xTaskGetTickCount() - start_tick;
    const size_t phase = (size_t)((elapsed / pdMS_TO_TICKS(GAME_STORE_SCROLL_STEP_MS)) % cycle_width);

    for(size_t i = 0U; (i < visible_chars) && ((i + 1U) < out_len); i++)
    {
        const size_t source = (phase + i) % cycle_width;
        out[i] = (source < text_len) ? text[source] : ' ';
        out[i + 1U] = '\0';
    }
}

static void game_store_free_categories_(void)
{
    if(s_buffers != NULL)
    {
        free(s_categories);
        s_categories = NULL;
    }
    s_category_count = 0U;
    s_selected_category = 0U;
    s_category_list_start = 0U;
}

static esp_err_t game_store_build_categories_(void)
{
    game_store_free_categories_();
    if((s_catalog.games == NULL) || (s_catalog.count == 0U))
    {
        return ESP_ERR_NOT_FOUND;
    }

    s_categories = (game_store_category_name_t*)calloc(s_catalog.count,
                                                        sizeof(*s_categories));
    if(s_categories == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    for(size_t game_index = 0U; game_index < s_catalog.count; game_index++)
    {
        bool found = false;
        for(size_t category_index = 0U; category_index < s_category_count; category_index++)
        {
            if(strcmp(s_categories[category_index],
                      s_catalog.games[game_index].category) == 0)
            {
                found = true;
                break;
            }
        }
        if(!found)
        {
            (void)snprintf(s_categories[s_category_count],
                           sizeof(s_categories[s_category_count]),
                           "%s",
                           s_catalog.games[game_index].category);
            s_category_count++;
        }
    }
    return (s_category_count > 0U) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool game_store_game_in_selected_category_(size_t game_index)
{
    return (game_index < s_catalog.count) &&
           (s_selected_category < s_category_count) &&
           (strcmp(s_catalog.games[game_index].category,
                   s_categories[s_selected_category]) == 0);
}

static bool game_store_select_first_category_game_(void)
{
    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        if(game_store_game_in_selected_category_(i))
        {
            s_selected_game = i;
            return true;
        }
    }
    return false;
}

static size_t game_store_selected_category_game_count_(void)
{
    size_t count = 0U;
    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        if(game_store_game_in_selected_category_(i))
        {
            count++;
        }
    }
    return count;
}

static size_t game_store_selected_category_game_position_(void)
{
    size_t position = 0U;
    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        if(game_store_game_in_selected_category_(i))
        {
            if(i == s_selected_game)
            {
                return position;
            }
            position++;
        }
    }
    return 0U;
}

static bool game_store_move_gallery_(int direction)
{
    if(direction < 0)
    {
        for(size_t i = s_selected_game; i > 0U; i--)
        {
            if(game_store_game_in_selected_category_(i - 1U))
            {
                s_selected_game = i - 1U;
                return true;
            }
        }
    }
    else if(direction > 0)
    {
        for(size_t i = s_selected_game + 1U; i < s_catalog.count; i++)
        {
            if(game_store_game_in_selected_category_(i))
            {
                s_selected_game = i;
                return true;
            }
        }
    }
    return false;
}

static void game_store_invalidate_thumbnail_(void)
{
    s_thumbnail_game_index = SIZE_MAX;
    s_thumbnail_loaded = false;
}

static int game_store_compare_versions_(const char* available, const char* installed)
{
    unsigned available_parts[3] = {0U};
    unsigned installed_parts[3] = {0U};

    if((available == NULL) || (installed == NULL))
    {
        return 0;
    }
    if((sscanf(available, "%u.%u.%u", &available_parts[0], &available_parts[1], &available_parts[2]) == 3) &&
       (sscanf(installed, "%u.%u.%u", &installed_parts[0], &installed_parts[1], &installed_parts[2]) == 3))
    {
        for(size_t i = 0U; i < 3U; i++)
        {
            if(available_parts[i] != installed_parts[i])
            {
                return (available_parts[i] > installed_parts[i]) ? 1 : -1;
            }
        }
        return 0;
    }
    return strcmp(available, installed);
}

static void game_store_refresh_installed_identity_(void)
{
    s_installed_present = poom_boot_policy_game_present();
    s_installed_identity_known = false;
    s_installed_game_id[0] = '\0';
    s_installed_game_version[0] = '\0';

    if(s_installed_present &&
       (poom_boot_policy_get_game_identity(s_installed_game_id,
                                           sizeof(s_installed_game_id),
                                           s_installed_game_version,
                                           sizeof(s_installed_game_version)) == ESP_OK))
    {
        s_installed_identity_known = true;
    }
}

static game_store_catalog_action_t game_store_action_for_(const poom_game_store_game_t* game)
{
    if((game == NULL) || !s_installed_present || !s_installed_identity_known ||
       (strcmp(game->id, s_installed_game_id) != 0))
    {
        return GAME_STORE_ACTION_GET;
    }

    return (game_store_compare_versions_(game->version, s_installed_game_version) > 0) ?
        GAME_STORE_ACTION_UPDATE : GAME_STORE_ACTION_PLAY;
}

static const char* game_store_action_label_(game_store_catalog_action_t action)
{
    switch(action)
    {
        case GAME_STORE_ACTION_UPDATE:
            return "A:UPDATE";
        case GAME_STORE_ACTION_PLAY:
            return "A:PLAY";
        case GAME_STORE_ACTION_GET:
        default:
            return "A:GET";
    }
}

static bool game_store_refresh_selected_file_(void)
{
    struct stat st;
    const poom_game_store_game_t* game;

    s_selected_file_on_sd = false;
    if((s_buffers == NULL) || (s_catalog.games == NULL) || (s_selected_game >= s_catalog.count))
    {
        return false;
    }

    game = &s_catalog.games[s_selected_game];
    if(poom_game_store_prepare_game_storage(game) != ESP_OK)
    {
        return false;
    }
    if(poom_game_store_get_local_path(game,
                                      s_downloaded_path,
                                      sizeof(s_downloaded_path)) != ESP_OK)
    {
        return false;
    }
    if((stat(s_downloaded_path, &st) == 0) && S_ISREG(st.st_mode) &&
       (st.st_size > 0) && ((size_t)st.st_size == game->size))
    {
        s_selected_file_on_sd = true;
    }
    return s_selected_file_on_sd;
}

static void game_store_publish_resume_(void)
{
    const uint8_t token = 1U;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0U);
}

static void game_store_draw_frame_(const char* title)
{
    size_t width = (title != NULL) ? strlen(title) * 6U : 0U;
    int16_t x = 0;

    if(width < (size_t)ARDUBOY_WIDTH)
    {
        x = (int16_t)(((size_t)ARDUBOY_WIDTH - width) / 2U);
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    poom_arduboy_set_cursor(x, 2);
    (void)poom_arduboy_print((title != NULL) ? title : "GAME STORE");
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
    poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 41, WHITE);
}

static void game_store_draw_footer_(const char* left, const char* right)
{
    if(left != NULL)
    {
        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(left);
    }
    if(right != NULL)
    {
        size_t width = strlen(right) * 6U;
        int16_t x = (width < (size_t)ARDUBOY_WIDTH) ?
            (int16_t)((size_t)ARDUBOY_WIDTH - width) : 0;
        poom_arduboy_set_cursor(x, 56);
        (void)poom_arduboy_print(right);
    }
}

static void game_store_draw_home_(void)
{
    static const char* const options[GAME_STORE_HOME_COUNT] = {
        "BROWSE GAMES",
        "DOWNLOAD ALL",
        "OFFLINE LIBRARY",
    };

    game_store_draw_frame_("GAME STORE");
    for(size_t i = 0U; i < GAME_STORE_HOME_COUNT; i++)
    {
        int16_t y = (int16_t)(18 + (int)i * 12);
        poom_arduboy_set_cursor(5, y);
        (void)poom_arduboy_print(options[i]);
        if(i == (size_t)s_home_option)
        {
            poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, 9, INVERT);
        }
    }
    game_store_draw_footer_("A:SELECT", "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_status_(const char* title,
                                    const char* line1,
                                    const char* line2,
                                    const char* line3,
                                    bool cancellable)
{
    game_store_draw_frame_(title);
    poom_arduboy_set_cursor(4, 17);
    (void)poom_arduboy_print((line1 != NULL) ? line1 : "");
    poom_arduboy_set_cursor(4, 28);
    (void)poom_arduboy_print((line2 != NULL) ? line2 : "");
    poom_arduboy_set_cursor(4, 39);
    (void)poom_arduboy_print((line3 != NULL) ? line3 : "");
    game_store_draw_footer_(NULL, cancellable ? "B:CANCEL" : NULL);
    poom_arduboy_display();
}

static void game_store_draw_categories_(void)
{
    char title[22];

    (void)snprintf(title, sizeof(title), "CATEGORIES %u/%u",
                   (unsigned)(s_selected_category + 1U),
                   (unsigned)s_category_count);
    game_store_draw_frame_(title);

    for(size_t row = 0U; row < GAME_STORE_VISIBLE_ROWS; row++)
    {
        size_t index = s_category_list_start + row;
        int16_t y = (int16_t)(14 + (int)row * 10);
        char label[GAME_STORE_LIST_TEXT_CHARS + 1U];

        if(index >= s_category_count)
        {
            break;
        }
        if(index == s_selected_category)
        {
            game_store_format_scrolling_text_(label,
                                              sizeof(label),
                                              s_categories[index],
                                              GAME_STORE_LIST_TEXT_CHARS,
                                              s_list_scroll_tick);
        }
        else
        {
            (void)snprintf(label, sizeof(label), "%.*s",
                           (int)GAME_STORE_LIST_TEXT_CHARS,
                           s_categories[index]);
        }
        poom_arduboy_set_cursor(GAME_STORE_LIST_TEXT_X, y);
        (void)poom_arduboy_print(label);
        if(index == s_selected_category)
        {
            poom_arduboy_fill_rect(0, y - 1, ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    if(s_category_list_start > 0U)
    {
        poom_arduboy_fill_triangle(124, 12, 120, 16, 127, 16, WHITE);
    }
    if((s_category_list_start + GAME_STORE_VISIBLE_ROWS) < s_category_count)
    {
        poom_arduboy_fill_triangle(120, 48, 127, 48, 124, 52, WHITE);
    }

    game_store_draw_footer_("A:OPEN", "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_gallery_(void)
{
    char full_title[POOM_GAME_STORE_NAME_MAX + 24U];
    char title[GAME_STORE_SCREEN_TEXT_CHARS + 1U];
    const size_t category_count = game_store_selected_category_game_count_();
    const size_t category_position = game_store_selected_category_game_position_();
    const poom_game_store_game_t* game = &s_catalog.games[s_selected_game];

    if(s_thumbnail_game_index != s_selected_game)
    {
        if(s_thumbnail_bitmap != NULL)
        {
            const esp_err_t thumbnail_err =
                poom_game_store_load_thumbnail(game,
                                               s_thumbnail_bitmap,
                                               POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE);
            s_thumbnail_loaded = (thumbnail_err == ESP_OK);
        }
        else
        {
            s_thumbnail_loaded = false;
        }
        s_thumbnail_game_index = s_selected_game;
    }
    (void)snprintf(full_title, sizeof(full_title), "%u/%u %s",
                   (unsigned)(category_position + 1U),
                   (unsigned)category_count,
                   game->name);
    game_store_format_scrolling_text_(title,
                                      sizeof(title),
                                      full_title,
                                      GAME_STORE_SCREEN_TEXT_CHARS,
                                      s_list_scroll_tick);

    poom_arduboy_clear();
    if(s_thumbnail_loaded)
    {
        poom_arduboy_draw_bitmap_rows(0,
                                      0,
                                      s_thumbnail_bitmap,
                                      POOM_GAME_STORE_THUMBNAIL_WIDTH,
                                      POOM_GAME_STORE_THUMBNAIL_HEIGHT,
                                      false);
    }
    else
    {
        poom_arduboy_draw_rect(0, 12, ARDUBOY_WIDTH, 41, WHITE);
        poom_arduboy_set_cursor(10, 25);
        (void)poom_arduboy_print("NO PREVIEW IMAGE");
    }

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, BLACK);
    poom_arduboy_set_cursor(1, 2);
    (void)poom_arduboy_print(title);
    poom_arduboy_fill_rect(0, 53, ARDUBOY_WIDTH, 11, BLACK);
    game_store_draw_footer_("A:INFO", "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_detail_(void)
{
    const poom_game_store_game_t* game = &s_catalog.games[s_selected_game];
    const game_store_catalog_action_t action = game_store_action_for_(game);
    const char* footer_action;
    char name[GAME_STORE_SCREEN_TEXT_CHARS + 1U];
    char description[GAME_STORE_SCREEN_TEXT_CHARS + 1U];
    char category[GAME_STORE_SCREEN_TEXT_CHARS + 1U];
    char version[GAME_STORE_SCREEN_TEXT_CHARS + 1U];

    game_store_format_scrolling_text_(name,
                                      sizeof(name),
                                      game->name,
                                      GAME_STORE_SCREEN_TEXT_CHARS,
                                      s_screen_scroll_tick);
    game_store_format_scrolling_text_(description,
                                      sizeof(description),
                                      game->description,
                                      GAME_STORE_SCREEN_TEXT_CHARS,
                                      s_screen_scroll_tick);
    (void)snprintf(category, sizeof(category), "CATEGORY: %.10s", game->category);
    (void)snprintf(version, sizeof(version), "VERSION: %.11s", game->version);
    footer_action = s_selected_file_on_sd ? "A:OPEN" : game_store_action_label_(action);
    if(s_offline_mode && !s_selected_file_on_sd && (action != GAME_STORE_ACTION_PLAY))
    {
        footer_action = "A:ONLINE";
    }

    game_store_draw_frame_("GAME INFO");
    poom_arduboy_set_cursor(GAME_STORE_SCREEN_TEXT_X, 14);
    (void)poom_arduboy_print(name);
    poom_arduboy_set_cursor(GAME_STORE_SCREEN_TEXT_X, 24);
    (void)poom_arduboy_print(description);
    poom_arduboy_set_cursor(GAME_STORE_SCREEN_TEXT_X, 34);
    (void)poom_arduboy_print(category);
    poom_arduboy_set_cursor(GAME_STORE_SCREEN_TEXT_X, 44);
    (void)poom_arduboy_print(version);
    game_store_draw_footer_(footer_action, "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_download_(const poom_game_store_game_t* game,
                                      size_t received,
                                      size_t total,
                                      size_t game_index,
                                      size_t game_count)
{
    char count_line[22];
    char bytes_line[22];
    uint32_t percent = (total > 0U) ? (uint32_t)((received * 100U) / total) : 0U;
    int16_t fill_width = (int16_t)((percent * 120U) / 100U);

    game_store_draw_frame_((game_count > 1U) ? "DOWNLOAD ALL" : "DOWNLOADING");
    (void)snprintf(count_line, sizeof(count_line), "%u/%u %.14s",
                   (unsigned)(game_index + 1U), (unsigned)game_count, game->name);
    (void)snprintf(bytes_line, sizeof(bytes_line), "%u%% %u/%u KB",
                   (unsigned)percent,
                   (unsigned)(received / 1024U),
                   (unsigned)((total + 1023U) / 1024U));

    poom_arduboy_set_cursor(3, 17);
    (void)poom_arduboy_print(count_line);
    poom_arduboy_draw_rect(3, 29, 122, 10, WHITE);
    if(fill_width > 0)
    {
        poom_arduboy_fill_rect(4, 30, fill_width, 8, WHITE);
    }
    poom_arduboy_set_cursor(3, 42);
    (void)poom_arduboy_print(bytes_line);
    game_store_draw_footer_(NULL, "B:CANCEL");
    poom_arduboy_display();
}

static void game_store_draw_ready_(void)
{
    static const char* const options[GAME_STORE_READY_COUNT] = {
        "INSTALL & PLAY",
        "KEEP ON SD",
    };
    const poom_game_store_game_t* game = &s_catalog.games[s_selected_game];
    char name[GAME_STORE_SCREEN_TEXT_CHARS + 1U];

    game_store_format_scrolling_text_(name,
                                      sizeof(name),
                                      game->name,
                                      GAME_STORE_SCREEN_TEXT_CHARS,
                                      s_screen_scroll_tick);

    game_store_draw_frame_("DOWNLOADED");
    poom_arduboy_set_cursor(GAME_STORE_SCREEN_TEXT_X, 14);
    (void)poom_arduboy_print(name);
    for(size_t i = 0U; i < GAME_STORE_READY_COUNT; i++)
    {
        int16_t y = (int16_t)(27 + (int)i * 11);
        poom_arduboy_set_cursor(5, y);
        (void)poom_arduboy_print(options[i]);
        if(i == (size_t)s_ready_option)
        {
            poom_arduboy_fill_rect(1, y - 1, ARDUBOY_WIDTH - 2, 9, INVERT);
        }
    }
    game_store_draw_footer_("A:SELECT", "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_result_(void)
{
    game_store_draw_frame_(s_result_title);
    poom_arduboy_set_cursor(4, 20);
    (void)poom_arduboy_print(s_result_line1);
    poom_arduboy_set_cursor(4, 34);
    (void)poom_arduboy_print(s_result_line2);
    game_store_draw_footer_("A:OK", "B:BACK");
    poom_arduboy_display();
}

static void game_store_draw_(void)
{
    switch(s_state)
    {
        case GAME_STORE_STATE_HOME:
            game_store_draw_home_();
            break;
        case GAME_STORE_STATE_CONNECTING:
            game_store_draw_status_("GAME STORE", "CONNECTING TO WIFI", "PLEASE WAIT...", "", true);
            break;
        case GAME_STORE_STATE_CATALOG:
            game_store_draw_status_("GAME STORE", "LOADING GAMES...", "FROM ONLINE STORE", "", true);
            break;
        case GAME_STORE_STATE_CATEGORY:
            game_store_draw_categories_();
            break;
        case GAME_STORE_STATE_GALLERY:
            game_store_draw_gallery_();
            break;
        case GAME_STORE_STATE_DETAIL:
            game_store_draw_detail_();
            break;
        case GAME_STORE_STATE_READY:
            game_store_draw_ready_();
            break;
        case GAME_STORE_STATE_INSTALLING:
            game_store_draw_status_("INSTALLING GAME", "PLEASE WAIT...", "DO NOT TURN OFF", "", false);
            break;
        case GAME_STORE_STATE_RESULT:
            game_store_draw_result_();
            break;
        case GAME_STORE_STATE_DOWNLOADING:
        default:
            break;
    }
}

static void game_store_set_result_(const char* title, const char* line1, const char* line2)
{
    (void)snprintf(s_result_title, sizeof(s_result_title), "%.21s", title != NULL ? title : "GAME STORE");
    (void)snprintf(s_result_line1, sizeof(s_result_line1), "%.20s", line1 != NULL ? line1 : "");
    (void)snprintf(s_result_line2, sizeof(s_result_line2), "%.20s", line2 != NULL ? line2 : "");
    s_state = GAME_STORE_STATE_RESULT;
}

static void game_store_set_error_(const char* operation, esp_err_t err)
{
    const char* detail = esp_err_to_name(err);

    if(s_cancel_requested)
    {
        game_store_set_result_("CANCELLED", operation, "NO FILE CHANGED");
        return;
    }
    if(err == ESP_ERR_NO_MEM)
    {
        detail = "NOT ENOUGH SPACE";
    }
    else if(err == ESP_ERR_INVALID_CRC)
    {
        detail = "DOWNLOAD DAMAGED";
    }
    else if(err == ESP_ERR_INVALID_SIZE)
    {
        detail = "DOWNLOAD INCOMPLETE";
    }
    else if(err == ESP_ERR_NOT_FOUND)
    {
        detail = "NOT FOUND";
    }
    else if(err == POOM_GAME_STORE_ERR_SD_PREPARE)
    {
        detail = "SD NOT READY";
    }
    else if(err == POOM_GAME_STORE_ERR_SD_OPEN)
    {
        detail = "CANNOT SAVE TO SD";
    }
    else if(err == POOM_GAME_STORE_ERR_SD_WRITE)
    {
        detail = "SD SAVE FAILED";
    }
    else if(err == POOM_GAME_STORE_ERR_SD_COMMIT)
    {
        detail = "SD SAVE FAILED";
    }
    else if(err == POOM_GAME_STORE_ERR_HASH)
    {
        detail = "FILE CHECK FAILED";
    }
    else if(err == POOM_GAME_STORE_ERR_HTTP_INIT)
    {
        detail = "CANNOT REACH STORE";
    }
    else if(err == POOM_GAME_STORE_ERR_HTTP)
    {
        detail = "DOWNLOAD FAILED";
    }
    else if(err == POOM_GAME_STORE_ERR_SD_READ)
    {
        detail = "CANNOT READ SD";
    }
    game_store_set_result_("STORE ERROR", operation, detail);
}

static esp_err_t game_store_connect_wifi_(void)
{
    char ssid[GAME_STORE_WIFI_SSID_MAX + 1U] = {0};
    char password[GAME_STORE_WIFI_PASS_MAX + 1U] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t password_len = sizeof(password);
    TickType_t start;
    esp_err_t err;

    s_state = GAME_STORE_STATE_CONNECTING;
    game_store_draw_();

    if(!s_wifi_events_registered)
    {
        err = poom_wifi_ctrl_register_cb(NULL, NULL);
        if(err != ESP_OK)
        {
            return err;
        }
        s_wifi_events_registered = true;
    }

    if(poom_wifi_ctrl_sta_has_ip())
    {
        return ESP_OK;
    }

    err = poom_secrets_init();
    if(err != ESP_OK)
    {
        return err;
    }
    err = poom_secrets_get_wifi_ssid(ssid, &ssid_len);
    if((err != ESP_OK) || (ssid[0] == '\0'))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if(poom_secrets_get_wifi_pass(password, &password_len) != ESP_OK)
    {
        password[0] = '\0';
    }

    err = poom_wifi_ctrl_sta_connect(ssid, password[0] != '\0' ? password : NULL);
    if(err != ESP_OK)
    {
        return err;
    }

    start = xTaskGetTickCount();
    while(!poom_wifi_ctrl_sta_has_ip())
    {
        if(s_cancel_requested)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(GAME_STORE_CONNECT_TIMEOUT_MS))
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(GAME_STORE_REFRESH_MS));
    }
    return ESP_OK;
}

static esp_err_t game_store_sync_thumbnails_(void)
{
    char progress[32];
    char game_name[22];

    if(poom_game_store_thumbnails_are_synced(s_catalog.version, s_catalog.count))
    {
        return ESP_OK;
    }
    poom_game_store_invalidate_thumbnail_sync();

    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        if(s_cancel_requested)
        {
            return ESP_ERR_INVALID_STATE;
        }
        (void)snprintf(progress, sizeof(progress), "IMAGE %u OF %u",
                       (unsigned)(i + 1U), (unsigned)s_catalog.count);
        (void)snprintf(game_name, sizeof(game_name), "%.20s", s_catalog.games[i].name);
        game_store_draw_status_("SYNCING STORE", progress, game_name, "ONLY IF MISSING", true);

        const esp_err_t image_err = poom_game_store_sync_thumbnail(&s_catalog.games[i], NULL);
        if(s_cancel_requested)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if(image_err != ESP_OK)
        {
            printf("[W] [game_store] thumbnail unavailable for '%s': %s\n",
                   s_catalog.games[i].id, esp_err_to_name(image_err));
        }
    }
    const esp_err_t marker_err =
        poom_game_store_mark_thumbnails_synced(s_catalog.version, s_catalog.count);
    if(marker_err != ESP_OK)
    {
        printf("[W] [game_store] thumbnail sync marker not saved: %s\n",
               esp_err_to_name(marker_err));
    }
    return ESP_OK;
}

static esp_err_t game_store_load_catalog_(bool offline)
{
    esp_err_t err;

    game_store_draw_status_("GAME STORE", "PREPARING...", "GAME STORAGE", "PLEASE WAIT", true);
    err = poom_game_store_prepare_storage();
    if(err != ESP_OK)
    {
        return err;
    }

    s_state = GAME_STORE_STATE_CATALOG;
    game_store_free_categories_();
    poom_game_store_free_catalog(&s_catalog);

    if(offline)
    {
        game_store_draw_status_("OFFLINE LIBRARY",
                                "LOADING SAVED...",
                                "NO WIFI NEEDED",
                                "PLEASE WAIT",
                                true);
        err = poom_game_store_load_cached_catalog(&s_catalog);
    }
    else
    {
        err = game_store_connect_wifi_();
        if(err != ESP_OK)
        {
            return err;
        }
        game_store_draw_status_("GAME STORE",
                                "CONTACTING STORE...",
                                "SECURE CONNECTION",
                                "PLEASE WAIT",
                                true);
        err = poom_game_store_sync_time();
        if(err != ESP_OK)
        {
            return err;
        }
        if(s_cancel_requested)
        {
            return ESP_ERR_INVALID_STATE;
        }
        game_store_draw_status_("GAME STORE",
                                "LOADING GAMES...",
                                "FROM ONLINE STORE",
                                "PLEASE WAIT",
                                true);
        err = poom_game_store_fetch_catalog(&s_catalog);
    }
    if(err != ESP_OK)
    {
        return err;
    }
    if(s_cancel_requested)
    {
        poom_game_store_free_catalog(&s_catalog);
        return ESP_ERR_INVALID_STATE;
    }

    err = game_store_build_categories_();
    if(err != ESP_OK)
    {
        poom_game_store_free_catalog(&s_catalog);
        return err;
    }
    if(!offline)
    {
        err = game_store_sync_thumbnails_();
        if(err != ESP_OK)
        {
            game_store_free_categories_();
            poom_game_store_free_catalog(&s_catalog);
            return err;
        }
    }

    s_offline_mode = offline;
    game_store_refresh_installed_identity_();
    s_selected_category = 0U;
    s_category_list_start = 0U;
    s_selected_game = 0U;
    s_selected_file_on_sd = false;
    s_list_scroll_tick = xTaskGetTickCount();
    game_store_invalidate_thumbnail_();
    return ESP_OK;
}

static bool game_store_progress_cb_(size_t received, size_t total, void* user_ctx)
{
    game_store_progress_t* progress = (game_store_progress_t*)user_ctx;
    TickType_t now;
    uint8_t percent;

    if((progress == NULL) || (progress->game == NULL) || s_cancel_requested)
    {
        return false;
    }

    percent = (total > 0U) ? (uint8_t)((received * 100U) / total) : 0U;
    now = xTaskGetTickCount();
    if((received == total) ||
       ((percent != progress->last_percent) &&
        ((now - progress->last_draw_tick) >= pdMS_TO_TICKS(150U))))
    {
        progress->last_percent = percent;
        progress->last_draw_tick = now;
        game_store_draw_download_(progress->game, received, total,
                                  progress->game_index, progress->game_count);
    }
    return true;
}

static esp_err_t game_store_download_one_(size_t index, size_t count, char* out_path)
{
    game_store_progress_t progress = {
        .game = &s_catalog.games[index],
        .game_index = (count > 1U) ? index : 0U,
        .game_count = count,
        .last_draw_tick = 0,
        .last_percent = 255U,
    };

    s_state = GAME_STORE_STATE_DOWNLOADING;
    game_store_draw_download_(progress.game, 0U, progress.game->size, index, count);
    return poom_game_store_download(progress.game,
                                    game_store_progress_cb_,
                                    &progress,
                                    out_path,
                                    POOM_GAME_STORE_LOCAL_PATH_MAX);
}

static esp_err_t game_store_check_all_space_(void)
{
    uint64_t required = 0U;
    uint64_t available = 0U;
    esp_err_t err;

    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        if(required > (UINT64_MAX - (uint64_t)s_catalog.games[i].size))
        {
            return ESP_ERR_INVALID_SIZE;
        }
        required += (uint64_t)s_catalog.games[i].size;
    }

    err = poom_game_store_get_free_bytes(&available);
    if(err != ESP_OK)
    {
        return err;
    }
    return (available >= required) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void game_store_download_all_(void)
{
    char path[POOM_GAME_STORE_LOCAL_PATH_MAX];
    esp_err_t err = game_store_check_all_space_();

    if(err != ESP_OK)
    {
        game_store_set_error_("CHECK SD SPACE", err);
        return;
    }

    for(size_t i = 0U; i < s_catalog.count; i++)
    {
        err = game_store_download_one_(i, s_catalog.count, path);
        if(err != ESP_OK)
        {
            game_store_set_error_(s_catalog.games[i].name, err);
            return;
        }
    }

    char line[22];
    (void)snprintf(line, sizeof(line), "%u GAMES ON SD", (unsigned)s_catalog.count);
    game_store_set_result_("ALL DOWNLOADED", line, "/sdcard/apps");
}

static void game_store_handle_home_action_(void)
{
    game_store_home_option_t requested = s_home_option;
    esp_err_t err;

    s_cancel_requested = false;
    err = game_store_load_catalog_(requested == GAME_STORE_HOME_OFFLINE);
    if(err != ESP_OK)
    {
        if((requested == GAME_STORE_HOME_OFFLINE) && (err == ESP_ERR_NOT_FOUND))
        {
            game_store_set_result_("OFFLINE LIBRARY",
                                   "NO SAVED CATALOG",
                                   "CONNECT ONLINE FIRST");
            return;
        }
        if((requested == GAME_STORE_HOME_OFFLINE) &&
           ((err == ESP_ERR_INVALID_RESPONSE) || (err == ESP_ERR_INVALID_SIZE) ||
            (err == ESP_ERR_NOT_SUPPORTED)))
        {
            game_store_set_result_("OFFLINE LIBRARY",
                                   "CATALOG DAMAGED",
                                   "REFRESH ONLINE");
            return;
        }
        game_store_set_error_((err == ESP_ERR_NOT_FOUND) ? "SET WIFI FIRST" : "LOAD CATALOG", err);
        return;
    }

    if(requested == GAME_STORE_HOME_DOWNLOAD_ALL)
    {
        game_store_download_all_();
    }
    else
    {
        s_state = GAME_STORE_STATE_CATEGORY;
    }
}

static void game_store_handle_action_(void)
{
    esp_err_t err;

    if(s_state == GAME_STORE_STATE_HOME)
    {
        game_store_handle_home_action_();
        return;
    }

    if(s_state == GAME_STORE_STATE_DETAIL)
    {
        const poom_game_store_game_t* game = &s_catalog.games[s_selected_game];
        const game_store_catalog_action_t action = game_store_action_for_(game);

        s_cancel_requested = false;
        if(s_selected_file_on_sd)
        {
            game_store_draw_status_("GAME STORE", "CHECKING GAME...", "ON SD CARD", "PLEASE WAIT", false);
            err = poom_game_store_verify_local(game,
                                               s_downloaded_path,
                                               sizeof(s_downloaded_path));
            if(err == ESP_OK)
            {
                s_ready_option = GAME_STORE_READY_INSTALL;
                s_screen_scroll_tick = xTaskGetTickCount();
                s_state = GAME_STORE_STATE_READY;
                return;
            }
            if((err != ESP_ERR_INVALID_CRC) && (err != ESP_ERR_INVALID_SIZE) &&
               (err != ESP_ERR_NOT_FOUND))
            {
                game_store_set_error_("CHECK SAVED GAME", err);
                return;
            }
            s_selected_file_on_sd = false;
        }
        if(action == GAME_STORE_ACTION_PLAY)
        {
            game_store_draw_status_("GAME STORE", "STARTING GAME...", game->version, "PLEASE WAIT", false);
            err = poom_boot_policy_boot_game();
            if(err != ESP_OK)
            {
                game_store_set_error_("PLAY GAME", err);
                return;
            }
            esp_restart();
            return;
        }

        if(s_offline_mode)
        {
            game_store_set_result_("OFFLINE LIBRARY",
                                   "GAME NOT ON SD",
                                   "CONNECT TO DOWNLOAD");
            return;
        }

        err = game_store_download_one_(s_selected_game, 1U, s_downloaded_path);
        if(err != ESP_OK)
        {
            game_store_set_error_("DOWNLOAD GAME", err);
            return;
        }
        s_ready_option = GAME_STORE_READY_INSTALL;
        s_selected_file_on_sd = true;
        s_screen_scroll_tick = xTaskGetTickCount();
        s_state = GAME_STORE_STATE_READY;
        return;
    }

    if(s_state == GAME_STORE_STATE_READY)
    {
        if(s_ready_option == GAME_STORE_READY_KEEP)
        {
            s_state = GAME_STORE_STATE_GALLERY;
            return;
        }

        s_state = GAME_STORE_STATE_INSTALLING;
        game_store_draw_();
        err = poom_boot_policy_install(s_downloaded_path);
        if(err == ESP_OK)
        {
            const poom_game_store_game_t* game = &s_catalog.games[s_selected_game];
            err = poom_boot_policy_set_game_identity(game->id, game->version);
        }
        if(err == ESP_OK)
        {
            err = poom_boot_policy_boot_game();
        }
        if(err != ESP_OK)
        {
            game_store_set_error_("INSTALL GAME", err);
            return;
        }
        esp_restart();
        return;
    }

    if(s_state == GAME_STORE_STATE_RESULT)
    {
        s_cancel_requested = false;
        s_state = GAME_STORE_STATE_HOME;
    }
}

static void game_store_exit_(void)
{
    s_active = false;
    s_exit_requested = false;
    s_action_requested = false;
    s_cancel_requested = false;

    if(s_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", game_store_button_cb_, s_sbus_user);
        s_buttons_subscribed = false;
    }
    if(s_wifi_events_registered)
    {
        (void)poom_wifi_ctrl_unregister_cb();
        s_wifi_events_registered = false;
    }
    game_store_free_categories_();
    poom_game_store_free_catalog(&s_catalog);
    free(s_thumbnail_bitmap);
    s_thumbnail_bitmap = NULL;
    free(s_buffers);
    s_buffers = NULL;
    s_task = NULL;
    game_store_publish_resume_();
}

static void game_store_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    game_store_button_msg_t event;

    (void)user_ctx;
    if((msg == NULL) || (msg->len < sizeof(event)))
    {
        return;
    }
    (void)memcpy(&event, msg->data, sizeof(event));
    if(event.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if((s_state == GAME_STORE_STATE_CONNECTING) ||
       (s_state == GAME_STORE_STATE_CATALOG) ||
       (s_state == GAME_STORE_STATE_DOWNLOADING))
    {
        if(event.button == BTN_B)
        {
            s_cancel_requested = true;
        }
        return;
    }
    if(s_state == GAME_STORE_STATE_INSTALLING)
    {
        return;
    }

    if(event.button == BTN_B)
    {
        if(s_state == GAME_STORE_STATE_HOME)
        {
            s_exit_requested = true;
        }
        else if(s_state == GAME_STORE_STATE_DETAIL)
        {
            s_list_scroll_tick = xTaskGetTickCount();
            s_state = GAME_STORE_STATE_GALLERY;
        }
        else if(s_state == GAME_STORE_STATE_GALLERY)
        {
            s_list_scroll_tick = xTaskGetTickCount();
            s_state = GAME_STORE_STATE_CATEGORY;
        }
        else if((s_state == GAME_STORE_STATE_CATEGORY) ||
                (s_state == GAME_STORE_STATE_RESULT))
        {
            s_state = GAME_STORE_STATE_HOME;
        }
        else if(s_state == GAME_STORE_STATE_READY)
        {
            s_list_scroll_tick = xTaskGetTickCount();
            s_state = GAME_STORE_STATE_GALLERY;
        }
        return;
    }

    if(s_state == GAME_STORE_STATE_HOME)
    {
        if((event.button == BTN_UP) && (s_home_option > 0))
        {
            s_home_option = (game_store_home_option_t)((int)s_home_option - 1);
        }
        else if((event.button == BTN_DOWN) &&
                (((int)s_home_option + 1) < (int)GAME_STORE_HOME_COUNT))
        {
            s_home_option = (game_store_home_option_t)((int)s_home_option + 1);
        }
        else if(event.button == BTN_A)
        {
            s_action_requested = true;
        }
        return;
    }

    if(s_state == GAME_STORE_STATE_CATEGORY)
    {
        if((event.button == BTN_UP) && (s_selected_category > 0U))
        {
            s_selected_category--;
            s_list_scroll_tick = xTaskGetTickCount();
        }
        else if((event.button == BTN_DOWN) &&
                ((s_selected_category + 1U) < s_category_count))
        {
            s_selected_category++;
            s_list_scroll_tick = xTaskGetTickCount();
        }
        else if((event.button == BTN_A) && game_store_select_first_category_game_())
        {
            s_selected_file_on_sd = false;
            game_store_invalidate_thumbnail_();
            s_list_scroll_tick = xTaskGetTickCount();
            s_state = GAME_STORE_STATE_GALLERY;
        }

        if(s_selected_category < s_category_list_start)
        {
            s_category_list_start = s_selected_category;
        }
        if(s_selected_category >= (s_category_list_start + GAME_STORE_VISIBLE_ROWS))
        {
            s_category_list_start = s_selected_category - GAME_STORE_VISIBLE_ROWS + 1U;
        }
        return;
    }

    if(s_state == GAME_STORE_STATE_GALLERY)
    {
        bool changed = false;

        if(event.button == BTN_UP)
        {
            changed = game_store_move_gallery_(-1);
        }
        else if(event.button == BTN_DOWN)
        {
            changed = game_store_move_gallery_(1);
        }
        else if((event.button == BTN_A) && (s_catalog.count > 0U))
        {
            (void)game_store_refresh_selected_file_();
            s_screen_scroll_tick = xTaskGetTickCount();
            s_state = GAME_STORE_STATE_DETAIL;
        }

        if(changed)
        {
            s_selected_file_on_sd = false;
            game_store_invalidate_thumbnail_();
            s_list_scroll_tick = xTaskGetTickCount();
        }
        return;
    }

    if((s_state == GAME_STORE_STATE_DETAIL) && (event.button == BTN_A))
    {
        s_action_requested = true;
        return;
    }

    if(s_state == GAME_STORE_STATE_READY)
    {
        if((event.button == BTN_UP) && (s_ready_option > 0))
        {
            s_ready_option = (game_store_ready_option_t)((int)s_ready_option - 1);
        }
        else if((event.button == BTN_DOWN) &&
                (((int)s_ready_option + 1) < (int)GAME_STORE_READY_COUNT))
        {
            s_ready_option = (game_store_ready_option_t)((int)s_ready_option + 1);
        }
        else if(event.button == BTN_A)
        {
            s_action_requested = true;
        }
        return;
    }

    if((s_state == GAME_STORE_STATE_RESULT) && (event.button == BTN_A))
    {
        s_action_requested = true;
    }
}

static void game_store_task_(void* task_arg)
{
    (void)task_arg;

    while(s_active)
    {
        if(s_exit_requested)
        {
            game_store_exit_();
            break;
        }

        if(s_action_requested)
        {
            s_action_requested = false;
            game_store_handle_action_();
        }

        game_store_draw_();
        vTaskDelay(pdMS_TO_TICKS(GAME_STORE_REFRESH_MS));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void menu_poom_game_store_show(void)
{
    if(s_task != NULL)
    {
        return;
    }

    if(s_buffers != NULL)
    {
        game_store_free_categories_();
        poom_game_store_free_catalog(&s_catalog);
        free(s_thumbnail_bitmap);
        s_thumbnail_bitmap = NULL;
        free(s_buffers);
        s_buffers = NULL;
    }
    s_buffers = (game_store_buffers_t*)calloc(1U, sizeof(*s_buffers));
    if(s_buffers == NULL)
    {
        game_store_publish_resume_();
        return;
    }
    s_thumbnail_bitmap = (uint8_t*)malloc(POOM_GAME_STORE_THUMBNAIL_BITMAP_SIZE);
    if(s_thumbnail_bitmap == NULL)
    {
        free(s_buffers);
        s_buffers = NULL;
        game_store_publish_resume_();
        return;
    }
    (void)snprintf(s_result_title, sizeof(s_result_title), "%s", "GAME STORE");

    s_active = true;
    s_exit_requested = false;
    s_action_requested = false;
    s_cancel_requested = false;
    s_state = GAME_STORE_STATE_HOME;
    s_home_option = GAME_STORE_HOME_BROWSE;
    s_ready_option = GAME_STORE_READY_INSTALL;
    s_category_count = 0U;
    s_selected_category = 0U;
    s_category_list_start = 0U;
    s_selected_game = 0U;
    s_list_scroll_tick = xTaskGetTickCount();
    s_screen_scroll_tick = s_list_scroll_tick;
    s_installed_present = false;
    s_installed_identity_known = false;
    s_selected_file_on_sd = false;
    s_offline_mode = false;
    game_store_invalidate_thumbnail_();
    s_installed_game_id[0] = '\0';
    s_installed_game_version[0] = '\0';

    if(!s_buttons_subscribed)
    {
        if(!poom_sbus_subscribe_cb("input/button", game_store_button_cb_, s_sbus_user))
        {
            s_active = false;
            free(s_thumbnail_bitmap);
            s_thumbnail_bitmap = NULL;
            free(s_buffers);
            s_buffers = NULL;
            game_store_publish_resume_();
            return;
        }
        s_buttons_subscribed = true;
    }

    if(xTaskCreate(game_store_task_,
                   "menu_game_store",
                   GAME_STORE_TASK_STACK,
                   NULL,
                   GAME_STORE_TASK_PRIORITY,
                   &s_task) != pdPASS)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", game_store_button_cb_, s_sbus_user);
        s_buttons_subscribed = false;
        s_active = false;
        s_task = NULL;
        free(s_thumbnail_bitmap);
        s_thumbnail_bitmap = NULL;
        free(s_buffers);
        s_buffers = NULL;
        game_store_publish_resume_();
    }
}
