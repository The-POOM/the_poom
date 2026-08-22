// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_poom_wifi_arp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduboy2.h"
#include "button_driver.h"
#include "lwip/ip4_addr.h"
#include "poom_secrets_store.h"
#include "poom_wifi_arp.h"
#include "poom_wifi_ctrl.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_POOM_WIFI_ARP_OLED_COL (6)
#define MENU_POOM_WIFI_ARP_BOX_Y (10)
#define MENU_POOM_WIFI_ARP_BOX_H (52)
#define MENU_POOM_WIFI_ARP_REFRESH_MS (250U)
#define MENU_POOM_WIFI_ARP_STATUS_STACK (3584U)
#define MENU_POOM_WIFI_ARP_STATUS_PRIO (4U)

#define MENU_POOM_WIFI_ARP_SSID_MAX_LEN (32U)
#define MENU_POOM_WIFI_ARP_PASS_MAX_LEN (63U)
#define MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS (16U)
#define MENU_POOM_WIFI_ARP_VISIBLE_LINES (4U)

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

#ifndef BUTTON_LONG_PRESS_START
#define BUTTON_LONG_PRESS_START (6U)
#endif

#ifndef BUTTON_LONG_PRESS_HOLD
#define BUTTON_LONG_PRESS_HOLD (7U)
#endif

#ifndef BUTTON_LONG_PRESS_UP
#define BUTTON_LONG_PRESS_UP (8U)
#endif

typedef struct
{
    uint8_t button;
    uint8_t event;
    uint32_t ts_ms;
} menu_poom_wifi_arp_button_msg_t;

typedef enum
{
    MENU_POOM_WIFI_ARP_STATE_MAIN = 0,
    MENU_POOM_WIFI_ARP_STATE_EDIT_SSID,
    MENU_POOM_WIFI_ARP_STATE_EDIT_PASS,
    MENU_POOM_WIFI_ARP_STATE_EDIT_PORT,
    MENU_POOM_WIFI_ARP_STATE_SCAN_WAIT,
    MENU_POOM_WIFI_ARP_STATE_SCAN_LIST,
    MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_WAIT,
    MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_RESULT,
    MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_WAIT,
    MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST,
} menu_poom_wifi_arp_state_t;

typedef enum
{
    MENU_POOM_WIFI_ARP_SET_LETTERS = 0,
    MENU_POOM_WIFI_ARP_SET_NUMBERS,
    MENU_POOM_WIFI_ARP_SET_SYMBOLS,
    MENU_POOM_WIFI_ARP_SET_COUNT,
} menu_poom_wifi_arp_char_set_t;

typedef enum
{
    MENU_POOM_WIFI_ARP_FIND_MODE_TCP = 0,
    MENU_POOM_WIFI_ARP_FIND_MODE_UDP,
    MENU_POOM_WIFI_ARP_FIND_MODE_SSH,
    MENU_POOM_WIFI_ARP_FIND_MODE_COUNT,
} menu_poom_wifi_arp_find_mode_t;

typedef enum
{
    MENU_POOM_WIFI_ARP_ITEM_SSID = 0,
    MENU_POOM_WIFI_ARP_ITEM_PASS,
    MENU_POOM_WIFI_ARP_ITEM_SCAN,
    MENU_POOM_WIFI_ARP_ITEM_FIND_PORT,
    MENU_POOM_WIFI_ARP_ITEM_BACK,
    MENU_POOM_WIFI_ARP_ITEM_COUNT,
} menu_poom_wifi_arp_item_t;

static bool s_menu_poom_wifi_arp_active = false;
static bool s_menu_poom_wifi_arp_buttons_subscribed = false;
static bool s_menu_poom_wifi_arp_exit_requested = false;
static TaskHandle_t s_menu_poom_wifi_arp_status_task = NULL;
static TaskHandle_t s_menu_poom_wifi_arp_scan_task = NULL;
static TaskHandle_t s_menu_poom_wifi_arp_host_scan_task = NULL;
static char s_menu_poom_wifi_arp_sbus_user[] = "menu_poom_wifi_arp";
static menu_poom_wifi_arp_state_t s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;

static menu_poom_wifi_arp_item_t s_menu_poom_wifi_arp_selected = MENU_POOM_WIFI_ARP_ITEM_SSID;
static uint16_t s_menu_poom_wifi_arp_window_start = 0U;

static char s_menu_poom_wifi_arp_cached_ssid[MENU_POOM_WIFI_ARP_SSID_MAX_LEN + 1U] = {0};
static size_t s_menu_poom_wifi_arp_cached_pass_len = 0U;
static uint16_t s_menu_poom_wifi_arp_target_port = 80U;
static menu_poom_wifi_arp_find_mode_t s_menu_poom_wifi_arp_find_mode = MENU_POOM_WIFI_ARP_FIND_MODE_TCP;
static char s_menu_poom_wifi_arp_port_digits[6] = "00080";
static uint8_t s_menu_poom_wifi_arp_port_cursor = 4U;

static char s_menu_poom_wifi_arp_edit_buf[MENU_POOM_WIFI_ARP_PASS_MAX_LEN + 1U] = {0};
static size_t s_menu_poom_wifi_arp_edit_len = 0U;
static size_t s_menu_poom_wifi_arp_edit_cursor = 0U;
static menu_poom_wifi_arp_char_set_t s_menu_poom_wifi_arp_char_set = MENU_POOM_WIFI_ARP_SET_LETTERS;

static char s_menu_poom_wifi_arp_status[22] = "A:Select  B:Exit";
static uint8_t s_menu_poom_wifi_arp_status_hold_cycles = 0U;

#define MENU_POOM_WIFI_ARP_SCAN_MAX_HOSTS (64U)
#define MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS (4)
#define MENU_POOM_WIFI_ARP_SCAN_LIST_Y0 (14)
#define MENU_POOM_WIFI_ARP_SCAN_ROW_STEP (10)
#define MENU_POOM_WIFI_ARP_SCAN_HILITE_H (9)
#define MENU_POOM_WIFI_ARP_SCAN_CONNECT_TIMEOUT_CYCLES (48U)

static poom_wifi_arp_scan_host_t *s_menu_poom_wifi_arp_scan_hosts = NULL;
static size_t s_menu_poom_wifi_arp_scan_count = 0U;
static int s_menu_poom_wifi_arp_scan_selected = 0;
static int s_menu_poom_wifi_arp_scan_scroll = 0;
static bool s_menu_poom_wifi_arp_scan_started = false;
static bool s_menu_poom_wifi_arp_scan_connect_started = false;
static uint8_t s_menu_poom_wifi_arp_scan_connect_wait_cycles = 0U;

#define MENU_POOM_WIFI_ARP_PORT_SWEEP_MAX_HITS (64U)
static uint8_t s_menu_poom_wifi_arp_port_hit_indices[MENU_POOM_WIFI_ARP_PORT_SWEEP_MAX_HITS];
static size_t s_menu_poom_wifi_arp_port_hit_count = 0U;
static int s_menu_poom_wifi_arp_port_selected = 0;
static int s_menu_poom_wifi_arp_port_scroll = 0;
static bool s_menu_poom_wifi_arp_port_started = false;
static bool s_menu_poom_wifi_arp_port_connect_started = false;
static uint8_t s_menu_poom_wifi_arp_port_connect_wait_cycles = 0U;
static size_t s_menu_poom_wifi_arp_port_total = 0U;
static size_t s_menu_poom_wifi_arp_port_done = 0U;
static TaskHandle_t s_menu_poom_wifi_arp_port_task = NULL;

static char s_menu_poom_wifi_arp_wifi_last_line[22] = {0};

static bool s_menu_poom_wifi_arp_wifi_cb_registered = false;

static poom_wifi_arp_service_probe_result_t s_menu_poom_wifi_arp_host_scan_result;
static char s_menu_poom_wifi_arp_host_scan_ip[16] = {0};
static uint8_t s_menu_poom_wifi_arp_host_scan_mac[6] = {0};
static uint32_t s_menu_poom_wifi_arp_host_scan_seq = 0U;
static uint8_t s_menu_poom_wifi_arp_host_scan_page = 0U;
static menu_poom_wifi_arp_state_t s_menu_poom_wifi_arp_host_scan_return_state = MENU_POOM_WIFI_ARP_STATE_SCAN_LIST;

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] info Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_wifi_evt_cb_(const poom_wifi_ctrl_evt_info_t *info, void *user_ctx)
{
    (void)user_ctx;

    if (info == NULL)
    {
        return;
    }

    switch (info->evt)
    {
        case POOM_WIFI_CTRL_EVT_STA_CONNECTED:
            printf("[BeastMenu] WiFi STA connected\n");
            (void)snprintf(s_menu_poom_wifi_arp_wifi_last_line,
                           sizeof(s_menu_poom_wifi_arp_wifi_last_line),
                           "STA connected");
            break;

        case POOM_WIFI_CTRL_EVT_STA_DISCONNECTED:
            printf("[BeastMenu] WiFi STA disconnected (reason=%ld)\n", (long)info->reason);
            (void)snprintf(s_menu_poom_wifi_arp_wifi_last_line,
                           sizeof(s_menu_poom_wifi_arp_wifi_last_line),
                           "Disc r=%ld",
                           (long)info->reason);
            break;

        case POOM_WIFI_CTRL_EVT_STA_GOT_IP:
        {
            ip4_addr_t ip = {.addr = info->ip.addr};
            char ip_str[16] = {0};
            (void)ip4addr_ntoa_r(&ip, ip_str, sizeof(ip_str));
            printf("[BeastMenu] WiFi STA got IP: %s\n", ip_str);
            (void)snprintf(s_menu_poom_wifi_arp_wifi_last_line,
                           sizeof(s_menu_poom_wifi_arp_wifi_last_line),
                           "IP %s",
                           ip_str);
            break;
        }

        default:
            break;
    }
}

static const char s_menu_poom_wifi_arp_set_letters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char s_menu_poom_wifi_arp_set_numbers[] = "0123456789";
static const char s_menu_poom_wifi_arp_set_symbols[] = " !@#$%^&*()-_=+[]{};:,.?/\\|";
static const char* const s_menu_poom_wifi_arp_sets[MENU_POOM_WIFI_ARP_SET_COUNT] = {
    s_menu_poom_wifi_arp_set_letters,
    s_menu_poom_wifi_arp_set_numbers,
    s_menu_poom_wifi_arp_set_symbols,
};
static const char* const s_menu_poom_wifi_arp_set_names[MENU_POOM_WIFI_ARP_SET_COUNT] = {
    "LET",
    "NUM",
    "SYM",
};

static void menu_poom_wifi_arp_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);

/**
 * @brief Internal helper for `menu_poom_wifi_arp_ensure_scan_hosts`.
 *
 * @return bool
 */
static bool menu_poom_wifi_arp_ensure_scan_hosts_(void)
{
    if (s_menu_poom_wifi_arp_scan_hosts != NULL)
    {
        return true;
    }

    s_menu_poom_wifi_arp_scan_hosts = (poom_wifi_arp_scan_host_t *)calloc(MENU_POOM_WIFI_ARP_SCAN_MAX_HOSTS,
                                                                           sizeof(*s_menu_poom_wifi_arp_scan_hosts));
    return (s_menu_poom_wifi_arp_scan_hosts != NULL);
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_release_scan_hosts`.
 *
 * @return void
 */
static void menu_poom_wifi_arp_release_scan_hosts_(void)
{
    free(s_menu_poom_wifi_arp_scan_hosts);
    s_menu_poom_wifi_arp_scan_hosts = NULL;
}

/**
 * @brief Internal helper for menu_poom_wifi_arp_get_port_hit_host.
 *
 * @param[in] hit_index Parameter passed to the helper.
 * @return const poom_wifi_arp_scan_host_t *
 */
static const poom_wifi_arp_scan_host_t *menu_poom_wifi_arp_get_port_hit_host_(size_t hit_index)
{
    const size_t host_index = (size_t)s_menu_poom_wifi_arp_port_hit_indices[hit_index];

    if ((s_menu_poom_wifi_arp_scan_hosts == NULL) || (host_index >= s_menu_poom_wifi_arp_scan_count))
    {
        return NULL;
    }

    return &s_menu_poom_wifi_arp_scan_hosts[host_index];
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_is_long_press_event`.
 *
 * @param[in] event Parameter passed to the helper.
 * @return bool
 */
static bool menu_poom_wifi_arp_is_long_press_event_(uint8_t event)
{
    return (event == BUTTON_LONG_PRESS_START) ||
           (event == BUTTON_LONG_PRESS_HOLD) ||
           (event == BUTTON_LONG_PRESS_UP);
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_find_char_index`.
 *
 * @param[in] haystack Parameter passed to the helper.
 * @param[in] needle Parameter passed to the helper.
 * @return int
 */
static int menu_poom_wifi_arp_find_char_index_(const char* haystack, char needle)
{
    const char* pos;

    if(haystack == NULL)
    {
        return -1;
    }

    pos = strchr(haystack, needle);
    if(pos == NULL)
    {
        return -1;
    }

    return (int)(pos - haystack);
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_cycle_current_char`.
 *
 * @param[in] direction Parameter passed to the helper.
 * @param[in] max_len Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_cycle_current_char_(int direction, size_t max_len)
{
    const char* set_chars = s_menu_poom_wifi_arp_sets[s_menu_poom_wifi_arp_char_set];
    size_t set_len;
    int index;

    if((set_chars == NULL) || (max_len == 0U))
    {
        return;
    }

    set_len = strlen(set_chars);
    if(set_len == 0U)
    {
        return;
    }

    if((s_menu_poom_wifi_arp_edit_cursor == s_menu_poom_wifi_arp_edit_len) &&
       (s_menu_poom_wifi_arp_edit_len < max_len))
    {
        s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_cursor] =
            (direction >= 0) ? set_chars[0] : set_chars[set_len - 1U];
        s_menu_poom_wifi_arp_edit_len++;
        s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_len] = '\0';
        return;
    }

    if(s_menu_poom_wifi_arp_edit_cursor >= s_menu_poom_wifi_arp_edit_len)
    {
        return;
    }

    index = menu_poom_wifi_arp_find_char_index_(set_chars,
                                                s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_cursor]);
    if(index < 0)
    {
        index = (direction >= 0) ? 0 : (int)(set_len - 1U);
    }
    else if(direction >= 0)
    {
        index = (index + 1) % (int)set_len;
    }
    else
    {
        index = (index + (int)set_len - 1) % (int)set_len;
    }

    s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_cursor] = set_chars[index];
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_edit_reset`.
 *
 * @return void
 */
static void menu_poom_wifi_arp_edit_reset_(void)
{
    (void)memset(s_menu_poom_wifi_arp_edit_buf, 0, sizeof(s_menu_poom_wifi_arp_edit_buf));
    s_menu_poom_wifi_arp_edit_len = 0U;
    s_menu_poom_wifi_arp_edit_cursor = 0U;
    s_menu_poom_wifi_arp_char_set = MENU_POOM_WIFI_ARP_SET_LETTERS;
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_set_status`.
 *
 * @param[in] text Parameter passed to the helper.
 * @param[in] hold_cycles Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_set_status_(const char* text, uint8_t hold_cycles)
{
    (void)snprintf(s_menu_poom_wifi_arp_status,
                   sizeof(s_menu_poom_wifi_arp_status),
                   "%.21s",
                   (text != NULL) ? text : "");
    s_menu_poom_wifi_arp_status_hold_cycles = hold_cycles;
}

/**
 * @brief Returns the text representation for the current state.
 *
 * @param[in] mode Parameter passed to the helper.
 * @return const char *
 */
static const char *menu_poom_wifi_arp_find_mode_str_(menu_poom_wifi_arp_find_mode_t mode)
{
    switch (mode)
    {
        case MENU_POOM_WIFI_ARP_FIND_MODE_TCP:
            return "TCP";
        case MENU_POOM_WIFI_ARP_FIND_MODE_UDP:
            return "UDP";
        case MENU_POOM_WIFI_ARP_FIND_MODE_SSH:
            return "SSH";
        default:
            return "?";
    }
}

/**
 * @brief Parses input data for this menu module.
 *
 * @param[in] digits Parameter passed to the helper.
 * @return uint16_t
 */
static uint16_t menu_poom_wifi_arp_parse_port_digits_(const char digits[6])
{
    uint32_t v = 0U;
    if (digits == NULL)
    {
        return 1U;
    }

    for (size_t i = 0; i < 5U; i++)
    {
        const char c = digits[i];
        if ((c < '0') || (c > '9'))
        {
            continue;
        }
        v = (v * 10U) + (uint32_t)(c - '0');
    }

    if (v < 1U)
    {
        v = 1U;
    }
    if (v > 65535U)
    {
        v = 65535U;
    }

    return (uint16_t)v;
}

/**
 * @brief Loads internal data used by this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_arp_load_cached_values_(void)
{
    esp_err_t status;
    size_t ssid_len;
    size_t pass_len;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        (void)memset(s_menu_poom_wifi_arp_cached_ssid, 0, sizeof(s_menu_poom_wifi_arp_cached_ssid));
        s_menu_poom_wifi_arp_cached_pass_len = 0U;
        return;
    }

    ssid_len = sizeof(s_menu_poom_wifi_arp_cached_ssid);
    status = poom_secrets_get_wifi_ssid(s_menu_poom_wifi_arp_cached_ssid, &ssid_len);
    if(status != ESP_OK)
    {
        (void)memset(s_menu_poom_wifi_arp_cached_ssid, 0, sizeof(s_menu_poom_wifi_arp_cached_ssid));
    }

    pass_len = sizeof(s_menu_poom_wifi_arp_edit_buf);
    status = poom_secrets_get_wifi_pass(s_menu_poom_wifi_arp_edit_buf, &pass_len);
    if(status == ESP_OK)
    {
        s_menu_poom_wifi_arp_cached_pass_len = strlen(s_menu_poom_wifi_arp_edit_buf);
    }
    else
    {
        s_menu_poom_wifi_arp_cached_pass_len = 0U;
    }
}

/**
 * @brief Adjusts the internal selection or scroll state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_adjust_window_(void)
{
    if((uint16_t)s_menu_poom_wifi_arp_selected < s_menu_poom_wifi_arp_window_start)
    {
        s_menu_poom_wifi_arp_window_start = (uint16_t)s_menu_poom_wifi_arp_selected;
    }
    else if(((uint16_t)s_menu_poom_wifi_arp_selected) >=
            (uint16_t)(s_menu_poom_wifi_arp_window_start + MENU_POOM_WIFI_ARP_VISIBLE_LINES))
    {
        s_menu_poom_wifi_arp_window_start =
            (uint16_t)((uint16_t)s_menu_poom_wifi_arp_selected - MENU_POOM_WIFI_ARP_VISIBLE_LINES + 1U);
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] right_text Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_draw_title_(const char* right_text)
{
    poom_arduboy_set_cursor(28, 2);
    (void)poom_arduboy_print(F("NET SCAN"));

    if(right_text != NULL && right_text[0] != '\0')
    {
        poom_arduboy_set_cursor(90, 2);
        (void)poom_arduboy_print(right_text);
    }

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] msg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_draw_scan_wait_(const char* msg)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("NET SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(10, 28);
    (void)poom_arduboy_print((msg != NULL) ? msg : "Working...");

    if (s_menu_poom_wifi_arp_wifi_last_line[0] != '\0')
    {
        poom_arduboy_set_cursor(10, 38);
        (void)poom_arduboy_print(s_menu_poom_wifi_arp_wifi_last_line);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_scan_list_(void)
{
    char count_buf[8];
    int max_scroll;

    if (s_menu_poom_wifi_arp_scan_count == 0U)
    {
        menu_poom_wifi_arp_draw_scan_wait_("No hosts");
        return;
    }

    if (s_menu_poom_wifi_arp_scan_selected < 0)
    {
        s_menu_poom_wifi_arp_scan_selected = 0;
    }
    if (s_menu_poom_wifi_arp_scan_selected > (int)s_menu_poom_wifi_arp_scan_count - 1)
    {
        s_menu_poom_wifi_arp_scan_selected = (int)s_menu_poom_wifi_arp_scan_count - 1;
    }

    if (s_menu_poom_wifi_arp_scan_selected < s_menu_poom_wifi_arp_scan_scroll)
    {
        s_menu_poom_wifi_arp_scan_scroll = s_menu_poom_wifi_arp_scan_selected;
    }
    if (s_menu_poom_wifi_arp_scan_selected >= (s_menu_poom_wifi_arp_scan_scroll + MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS))
    {
        s_menu_poom_wifi_arp_scan_scroll = s_menu_poom_wifi_arp_scan_selected - MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS + 1;
    }

    max_scroll = (int)s_menu_poom_wifi_arp_scan_count - MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_menu_poom_wifi_arp_scan_scroll < 0)
    {
        s_menu_poom_wifi_arp_scan_scroll = 0;
    }
    if (s_menu_poom_wifi_arp_scan_scroll > max_scroll)
    {
        s_menu_poom_wifi_arp_scan_scroll = max_scroll;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(26, 2);
    (void)poom_arduboy_print(F("NET SCAN"));
    (void)snprintf(count_buf, sizeof(count_buf), "%u", (unsigned)s_menu_poom_wifi_arp_scan_count);
    poom_arduboy_set_cursor(110, 2);
    (void)poom_arduboy_print(count_buf);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    for (int row = 0; row < MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS; row++)
    {
        const int idx = s_menu_poom_wifi_arp_scan_scroll + row;
        if ((idx < 0) || (idx >= (int)s_menu_poom_wifi_arp_scan_count))
        {
            break;
        }

        const int16_t y = (int16_t)(MENU_POOM_WIFI_ARP_SCAN_LIST_Y0 + row * MENU_POOM_WIFI_ARP_SCAN_ROW_STEP);
        const poom_wifi_arp_scan_host_t *host = &s_menu_poom_wifi_arp_scan_hosts[idx];

        char ip_line[18];
        (void)snprintf(ip_line, sizeof(ip_line), "%.15s", host->ip);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(ip_line);

        if (idx == s_menu_poom_wifi_arp_scan_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MENU_POOM_WIFI_ARP_SCAN_HILITE_H, INVERT);
        }
    }

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_port_sweep_wait_(void)
{
    char line_1[22];
    char line_2[22];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(18, 2);
    (void)poom_arduboy_print(F("FIND"));
    poom_arduboy_set_cursor(52, 2);
    (void)poom_arduboy_print(menu_poom_wifi_arp_find_mode_str_(s_menu_poom_wifi_arp_find_mode));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    (void)snprintf(line_1, sizeof(line_1), "Port:%u", (unsigned)s_menu_poom_wifi_arp_target_port);
    poom_arduboy_set_cursor(10, 24);
    (void)poom_arduboy_print(line_1);

    if (s_menu_poom_wifi_arp_port_total > 0U)
    {
        (void)snprintf(line_2,
                       sizeof(line_2),
                       "%u/%u hits:%u",
                       (unsigned)s_menu_poom_wifi_arp_port_done,
                       (unsigned)s_menu_poom_wifi_arp_port_total,
                       (unsigned)s_menu_poom_wifi_arp_port_hit_count);
    }
    else
    {
        (void)snprintf(line_2, sizeof(line_2), "Starting...");
    }
    poom_arduboy_set_cursor(10, 34);
    (void)poom_arduboy_print(line_2);

    if (s_menu_poom_wifi_arp_wifi_last_line[0] != '\0')
    {
        poom_arduboy_set_cursor(10, 44);
        (void)poom_arduboy_print(s_menu_poom_wifi_arp_wifi_last_line);
    }

    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_port_sweep_list_(void)
{
    char count_buf[8];
    char port_buf[12];
    int max_scroll;

    if (s_menu_poom_wifi_arp_port_hit_count == 0U)
    {
        poom_arduboy_clear();
        poom_arduboy_set_text_size(1);

        poom_arduboy_set_cursor(18, 2);
        (void)poom_arduboy_print(F("FIND"));
        poom_arduboy_set_cursor(54, 2);
        (void)poom_arduboy_print(menu_poom_wifi_arp_find_mode_str_(s_menu_poom_wifi_arp_find_mode));
        poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

        poom_arduboy_set_cursor(10, 30);
        (void)poom_arduboy_print(F("No hits"));

        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:EXIT"));

        poom_arduboy_display();
        return;
    }

    if (s_menu_poom_wifi_arp_port_selected < 0)
    {
        s_menu_poom_wifi_arp_port_selected = 0;
    }
    if (s_menu_poom_wifi_arp_port_selected > (int)s_menu_poom_wifi_arp_port_hit_count - 1)
    {
        s_menu_poom_wifi_arp_port_selected = (int)s_menu_poom_wifi_arp_port_hit_count - 1;
    }

    if (s_menu_poom_wifi_arp_port_selected < s_menu_poom_wifi_arp_port_scroll)
    {
        s_menu_poom_wifi_arp_port_scroll = s_menu_poom_wifi_arp_port_selected;
    }
    if (s_menu_poom_wifi_arp_port_selected >= (s_menu_poom_wifi_arp_port_scroll + MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS))
    {
        s_menu_poom_wifi_arp_port_scroll = s_menu_poom_wifi_arp_port_selected - MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS + 1;
    }

    max_scroll = (int)s_menu_poom_wifi_arp_port_hit_count - MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS;
    if (max_scroll < 0)
    {
        max_scroll = 0;
    }
    if (s_menu_poom_wifi_arp_port_scroll < 0)
    {
        s_menu_poom_wifi_arp_port_scroll = 0;
    }
    if (s_menu_poom_wifi_arp_port_scroll > max_scroll)
    {
        s_menu_poom_wifi_arp_port_scroll = max_scroll;
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(18, 2);
    (void)poom_arduboy_print(F("FIND"));
    poom_arduboy_set_cursor(54, 2);
    (void)poom_arduboy_print(menu_poom_wifi_arp_find_mode_str_(s_menu_poom_wifi_arp_find_mode));
    (void)snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)s_menu_poom_wifi_arp_target_port);
    (void)snprintf(count_buf, sizeof(count_buf), "%u", (unsigned)s_menu_poom_wifi_arp_port_hit_count);

    const int16_t char_w = 6;
    int16_t count_x = (int16_t)(ARDUBOY_WIDTH - (int16_t)(strlen(count_buf) * (size_t)char_w));
    if (count_x < 0)
    {
        count_x = 0;
    }

    int16_t port_x = (int16_t)(count_x - char_w - (int16_t)(strlen(port_buf) * (size_t)char_w));
    if (port_x < 78)
    {
        port_x = 78;
    }

    poom_arduboy_set_cursor(port_x, 2);
    (void)poom_arduboy_print(port_buf);
    poom_arduboy_set_cursor(count_x, 2);
    (void)poom_arduboy_print(count_buf);
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:SEL"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:EXIT"));

    for (int row = 0; row < MENU_POOM_WIFI_ARP_SCAN_VISIBLE_ROWS; row++)
    {
        const int idx = s_menu_poom_wifi_arp_port_scroll + row;
        if ((idx < 0) || (idx >= (int)s_menu_poom_wifi_arp_port_hit_count))
        {
            break;
        }

        const int16_t y = (int16_t)(MENU_POOM_WIFI_ARP_SCAN_LIST_Y0 + row * MENU_POOM_WIFI_ARP_SCAN_ROW_STEP);
        const poom_wifi_arp_scan_host_t *host = menu_poom_wifi_arp_get_port_hit_host_((size_t)idx);
        if (host == NULL)
        {
            continue;
        }

        char ip_line[18];
        (void)snprintf(ip_line, sizeof(ip_line), "%.15s", host->ip);

        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(ip_line);

        if (idx == s_menu_poom_wifi_arp_port_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, MENU_POOM_WIFI_ARP_SCAN_HILITE_H, INVERT);
        }
    }

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_host_scan_wait_(void)
{
    char ip_line[22];
    char mac_line[22];

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(20, 2);
    (void)poom_arduboy_print(F("PORT SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    (void)snprintf(ip_line, sizeof(ip_line), "IP:%.15s", s_menu_poom_wifi_arp_host_scan_ip);
    poom_arduboy_set_cursor(2, 18);
    (void)poom_arduboy_print(ip_line);

    (void)snprintf(mac_line,
                   sizeof(mac_line),
                   "MAC:%02X:%02X:%02X:%02X:%02X:%02X",
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[0],
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[1],
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[2],
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[3],
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[4],
                   (unsigned)s_menu_poom_wifi_arp_host_scan_mac[5]);
    poom_arduboy_set_cursor(2, 28);
    (void)poom_arduboy_print(mac_line);

    poom_arduboy_set_cursor(2, 42);
    (void)poom_arduboy_print(F("Scanning..."));

    if (s_menu_poom_wifi_arp_wifi_last_line[0] != '\0')
    {
        poom_arduboy_set_cursor(2, 52);
        (void)poom_arduboy_print(s_menu_poom_wifi_arp_wifi_last_line);
    }


    poom_arduboy_display();
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_append_port`.
 *
 * @param[in] line Parameter passed to the helper.
 * @param[in] line_size Parameter passed to the helper.
 * @param[in] port Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_append_port_(char *line, size_t line_size, uint16_t port)
{
    char add[8];
    const size_t used = strlen(line);

    if (used >= (line_size - 1U))
    {
        return;
    }

    if (used == 0U)
    {
        (void)snprintf(add, sizeof(add), "%u", (unsigned)port);
    }
    else
    {
        (void)snprintf(add, sizeof(add), " %u", (unsigned)port);
    }

    if ((used + strlen(add)) < line_size)
    {
        (void)strncat(line, add, line_size - used - 1U);
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_host_scan_result_(void)
{
    char line_ip[22];
    char line_open[22];
    char line_ports_1[22] = {0};
    char line_ports_2[22] = {0};
    char ssh_line[22] = {0};

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(20, 2);
    (void)poom_arduboy_print(F("PORT SCAN"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, INVERT);

    (void)snprintf(line_ip, sizeof(line_ip), "IP:%.15s", s_menu_poom_wifi_arp_host_scan_ip);
    poom_arduboy_set_cursor(2, 14);
    (void)poom_arduboy_print(line_ip);

    if (s_menu_poom_wifi_arp_host_scan_page == 0U)
    {
        (void)snprintf(line_open,
                       sizeof(line_open),
                       "Open TCP:%u",
                       (unsigned)s_menu_poom_wifi_arp_host_scan_result.tcp.num_open_ports);
    }
    else
    {
        (void)snprintf(line_open,
                       sizeof(line_open),
                       "UDP RESP:%u",
                       (unsigned)s_menu_poom_wifi_arp_host_scan_result.udp.num_open_ports);
    }
    poom_arduboy_set_cursor(2, 24);
    (void)poom_arduboy_print(line_open);

    if (s_menu_poom_wifi_arp_host_scan_page == 0U)
    {
        for (size_t i = 0; i < s_menu_poom_wifi_arp_host_scan_result.tcp.num_open_ports; i++)
        {
            if (line_ports_1[0] == '\0' || (strlen(line_ports_1) < 18U))
            {
                menu_poom_wifi_arp_append_port_(line_ports_1,
                                               sizeof(line_ports_1),
                                               s_menu_poom_wifi_arp_host_scan_result.tcp.open_ports[i]);
            }
            else
            {
                menu_poom_wifi_arp_append_port_(line_ports_2,
                                               sizeof(line_ports_2),
                                               s_menu_poom_wifi_arp_host_scan_result.tcp.open_ports[i]);
            }
        }
    }
    else
    {
        for (size_t i = 0; i < s_menu_poom_wifi_arp_host_scan_result.udp.num_open_ports; i++)
        {
            if (line_ports_1[0] == '\0' || (strlen(line_ports_1) < 18U))
            {
                menu_poom_wifi_arp_append_port_(line_ports_1,
                                               sizeof(line_ports_1),
                                               s_menu_poom_wifi_arp_host_scan_result.udp.open_ports[i]);
            }
            else
            {
                menu_poom_wifi_arp_append_port_(line_ports_2,
                                               sizeof(line_ports_2),
                                               s_menu_poom_wifi_arp_host_scan_result.udp.open_ports[i]);
            }
        }
    }

    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print((line_ports_1[0] != '\0') ? line_ports_1 : "None");

    if ((s_menu_poom_wifi_arp_host_scan_page != 0U) && s_menu_poom_wifi_arp_host_scan_result.ssh_found)
    {
        (void)snprintf(ssh_line,
                       sizeof(ssh_line),
                       "SSH:%u",
                       (unsigned)s_menu_poom_wifi_arp_host_scan_result.ssh_port);
    }

    poom_arduboy_set_cursor(2, 46);
    if ((ssh_line[0] != '\0') && (line_ports_2[0] == '\0'))
    {
        (void)poom_arduboy_print(ssh_line);
    }
    else
    {
        (void)poom_arduboy_print(line_ports_2);
    }

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(F("A:NEXT"));
    poom_arduboy_set_cursor(72, 56);
    (void)poom_arduboy_print(F("B:BACK"));

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] wifi_ready Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_draw_main_(bool wifi_ready)
{
    char line_item[22];
    char status_line[22];
    uint16_t row;

    char right[12];
    (void)snprintf(right,
                   sizeof(right),
                   "%s",
                   wifi_ready ? "IP" : "NOIP");

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_wifi_arp_draw_title_(right);

    for(row = 0U; row < MENU_POOM_WIFI_ARP_VISIBLE_LINES; row++)
    {
        uint16_t index = (uint16_t)(s_menu_poom_wifi_arp_window_start + row);

        if(index >= (uint16_t)MENU_POOM_WIFI_ARP_ITEM_COUNT)
        {
            break;
        }

        (void)memset(line_item, 0, sizeof(line_item));

        switch((menu_poom_wifi_arp_item_t)index)
        {
            case MENU_POOM_WIFI_ARP_ITEM_SSID:
                if(s_menu_poom_wifi_arp_cached_ssid[0] == '\0')
                {
                    (void)snprintf(line_item, sizeof(line_item), "SSID:<not set>");
                }
                else
                {
                    (void)snprintf(line_item, sizeof(line_item), "SSID:%.14s", s_menu_poom_wifi_arp_cached_ssid);
                }
                break;

            case MENU_POOM_WIFI_ARP_ITEM_PASS:
                (void)snprintf(line_item, sizeof(line_item), "PASS:len=%u", (unsigned)s_menu_poom_wifi_arp_cached_pass_len);
                break;

            case MENU_POOM_WIFI_ARP_ITEM_SCAN:
                (void)snprintf(line_item, sizeof(line_item), "Scan Hosts");
                break;

            case MENU_POOM_WIFI_ARP_ITEM_FIND_PORT:
                (void)snprintf(line_item,
                               sizeof(line_item),
                               "Find %s:%u",
                               menu_poom_wifi_arp_find_mode_str_(s_menu_poom_wifi_arp_find_mode),
                               (unsigned)s_menu_poom_wifi_arp_target_port);
                break;

            case MENU_POOM_WIFI_ARP_ITEM_BACK:
                (void)snprintf(line_item, sizeof(line_item), "Back");
                break;

            default:
                (void)snprintf(line_item, sizeof(line_item), "?");
                break;
        }

        const int16_t y = (int16_t)(14 + (int16_t)row * 10);
        poom_arduboy_set_cursor(2, y);
        (void)poom_arduboy_print(line_item);

        if(index == (uint16_t)s_menu_poom_wifi_arp_selected)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), ARDUBOY_WIDTH, 9, INVERT);
        }
    }

    (void)snprintf(status_line, sizeof(status_line), "%.21s", s_menu_poom_wifi_arp_status);
    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(status_line);

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @param[in] title Parameter passed to the helper.
 * @param[in] help Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_draw_edit_text_(const char* title, const char* help)
{
    char line_title[22];
    char line_set[22];
    char text_line[MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS + 1U];
    char cursor_line[MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS + 1U];
    size_t i;
    size_t view_start = 0U;

    if(s_menu_poom_wifi_arp_edit_cursor >= MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS)
    {
        view_start = s_menu_poom_wifi_arp_edit_cursor - MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS + 1U;
    }

    (void)memset(text_line, ' ', sizeof(text_line));
    (void)memset(cursor_line, ' ', sizeof(cursor_line));
    text_line[MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS] = '\0';
    cursor_line[MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS] = '\0';

    for(i = 0U; i < MENU_POOM_WIFI_ARP_TEXT_VIEW_CHARS; i++)
    {
        size_t pos = view_start + i;

        if(pos < s_menu_poom_wifi_arp_edit_len)
        {
            text_line[i] = s_menu_poom_wifi_arp_edit_buf[pos];
        }
        else if(pos == s_menu_poom_wifi_arp_edit_len)
        {
            text_line[i] = '_';
        }

        if(pos == s_menu_poom_wifi_arp_edit_cursor)
        {
            cursor_line[i] = '^';
        }
    }

    (void)snprintf(line_title, sizeof(line_title), "%.21s", (title != NULL) ? title : "");
    (void)snprintf(line_set,
                   sizeof(line_set),
                   "Set:%s Len:%u",
                   s_menu_poom_wifi_arp_set_names[s_menu_poom_wifi_arp_char_set],
                   (unsigned)s_menu_poom_wifi_arp_edit_len);

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_wifi_arp_draw_title_(NULL);

    poom_arduboy_set_cursor(2, 14);
    (void)poom_arduboy_print(line_title);
    poom_arduboy_set_cursor(2, 24);
    (void)poom_arduboy_print(line_set);

    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print(text_line);
    poom_arduboy_set_cursor(2, 46);
    (void)poom_arduboy_print(cursor_line);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print((help != NULL) ? help : "");

    poom_arduboy_display();
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_poom_wifi_arp_draw_edit_port_(void)
{
    char line_1[22];
    char line_2[22];
    char line_3[22];
    char help[22];
    uint8_t caret_pos;

    (void)snprintf(line_1, sizeof(line_1), "Type:%s", menu_poom_wifi_arp_find_mode_str_(s_menu_poom_wifi_arp_find_mode));
    (void)snprintf(line_2, sizeof(line_2), "Port:%.5s", s_menu_poom_wifi_arp_port_digits);
    (void)snprintf(help, sizeof(help), "U/D:num A:type HoldA");

    (void)memset(line_3, ' ', sizeof(line_3));
    line_3[sizeof(line_3) - 1U] = '\0';
    caret_pos = (uint8_t)(5U + (s_menu_poom_wifi_arp_port_cursor % 5U));
    if (caret_pos < (uint8_t)(sizeof(line_3) - 1U))
    {
        line_3[caret_pos] = '^';
    }

    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);
    menu_poom_wifi_arp_draw_title_(NULL);

    poom_arduboy_set_cursor(2, 14);
    (void)poom_arduboy_print(F("FIND PORT"));

    poom_arduboy_set_cursor(2, 26);
    (void)poom_arduboy_print(line_1);

    poom_arduboy_set_cursor(2, 36);
    (void)poom_arduboy_print(line_2);

    poom_arduboy_set_cursor(2, 46);
    (void)poom_arduboy_print(line_3);

    poom_arduboy_set_cursor(0, 56);
    (void)poom_arduboy_print(help);

    poom_arduboy_display();
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return esp_err_t
 */
static esp_err_t menu_poom_wifi_arp_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_poom_wifi_arp_active = false;
    s_menu_poom_wifi_arp_exit_requested = false;
    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;

    if (s_menu_poom_wifi_arp_scan_task != NULL)
    {
        TaskHandle_t scan_task = s_menu_poom_wifi_arp_scan_task;
        s_menu_poom_wifi_arp_scan_task = NULL;
        vTaskDelete(scan_task);
    }

    if (s_menu_poom_wifi_arp_port_task != NULL)
    {
        TaskHandle_t scan_task = s_menu_poom_wifi_arp_port_task;
        s_menu_poom_wifi_arp_port_task = NULL;
        vTaskDelete(scan_task);
    }

    if (s_menu_poom_wifi_arp_host_scan_task != NULL)
    {
        TaskHandle_t scan_task = s_menu_poom_wifi_arp_host_scan_task;
        s_menu_poom_wifi_arp_host_scan_task = NULL;
        vTaskDelete(scan_task);
    }

    if(s_menu_poom_wifi_arp_status_task != NULL)
    {
        if(s_menu_poom_wifi_arp_status_task != current_task)
        {
            TaskHandle_t status_task = s_menu_poom_wifi_arp_status_task;
            s_menu_poom_wifi_arp_status_task = NULL;
            vTaskDelete(status_task);
        }
        else
        {
            s_menu_poom_wifi_arp_status_task = NULL;
        }
    }

    if(s_menu_poom_wifi_arp_buttons_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_poom_wifi_arp_button_cb_, s_menu_poom_wifi_arp_sbus_user);
        s_menu_poom_wifi_arp_buttons_subscribed = false;
    }

    if (s_menu_poom_wifi_arp_wifi_cb_registered)
    {
        (void)poom_wifi_ctrl_sta_disconnect();

        for (uint8_t i = 0; i < 12U; i++)
        {
            if (!poom_wifi_ctrl_sta_has_ip() && !poom_wifi_ctrl_sta_is_connected())
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100U));
        }

        (void)poom_wifi_ctrl_unregister_cb();
        s_menu_poom_wifi_arp_wifi_cb_registered = false;
    }
    else
    {
        (void)poom_wifi_ctrl_sta_disconnect();
    }

    menu_poom_wifi_arp_release_scan_hosts_();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
    return ESP_OK;
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_scan_task_(void* task_arg)
{
    (void)task_arg;

    poom_wifi_arp_scan_ctx_t ctx = {
        .hosts = s_menu_poom_wifi_arp_scan_hosts,
        .num_active_hosts = 0U,
        .max_hosts = MENU_POOM_WIFI_ARP_SCAN_MAX_HOSTS,
        .subnet_prefix = {0},
    };

    (void)poom_wifi_arp_scan_subnet(&ctx);

    s_menu_poom_wifi_arp_scan_count = ctx.num_active_hosts;
    s_menu_poom_wifi_arp_scan_selected = 0;
    s_menu_poom_wifi_arp_scan_scroll = 0;

    s_menu_poom_wifi_arp_scan_task = NULL;
    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_SCAN_LIST;
    vTaskDelete(NULL);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_arp_scan_start_if_ready_(void)
{
    if (s_menu_poom_wifi_arp_scan_started)
    {
        return;
    }

    if (!menu_poom_wifi_arp_ensure_scan_hosts_())
    {
        menu_poom_wifi_arp_set_status_("No RAM hosts", 10U);
        s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
        return;
    }

    s_menu_poom_wifi_arp_scan_started = true;
    s_menu_poom_wifi_arp_scan_count = 0U;
    (void)memset(s_menu_poom_wifi_arp_scan_hosts,
                 0,
                 MENU_POOM_WIFI_ARP_SCAN_MAX_HOSTS * sizeof(*s_menu_poom_wifi_arp_scan_hosts));

    if (s_menu_poom_wifi_arp_scan_task == NULL)
    {
        (void)xTaskCreate(menu_poom_wifi_arp_scan_task_,
                          "beast_scan",
                          4096,
                          NULL,
                          5,
                          &s_menu_poom_wifi_arp_scan_task);
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_port_sweep_task_(void *task_arg)
{
    (void)task_arg;

    s_menu_poom_wifi_arp_port_hit_count = 0U;
    s_menu_poom_wifi_arp_port_total = 0U;
    s_menu_poom_wifi_arp_port_done = 0U;
    (void)memset(s_menu_poom_wifi_arp_port_hit_indices, 0, sizeof(s_menu_poom_wifi_arp_port_hit_indices));

    poom_wifi_arp_scan_ctx_t ctx = {
        .hosts = s_menu_poom_wifi_arp_scan_hosts,
        .num_active_hosts = 0U,
        .max_hosts = MENU_POOM_WIFI_ARP_SCAN_MAX_HOSTS,
        .subnet_prefix = {0},
    };

    (void)poom_wifi_arp_scan_subnet(&ctx);

    s_menu_poom_wifi_arp_port_total = ctx.num_active_hosts;
    for (size_t i = 0; i < ctx.num_active_hosts; i++)
    {
        bool match = false;
        const poom_wifi_arp_scan_host_t *host = &ctx.hosts[i];
        switch (s_menu_poom_wifi_arp_find_mode)
        {
            case MENU_POOM_WIFI_ARP_FIND_MODE_TCP:
                (void)poom_wifi_arp_probe_tcp_port(host->ip, s_menu_poom_wifi_arp_target_port, &match);
                break;
            case MENU_POOM_WIFI_ARP_FIND_MODE_UDP:
                (void)poom_wifi_arp_probe_udp_port(host->ip, s_menu_poom_wifi_arp_target_port, &match);
                break;
            case MENU_POOM_WIFI_ARP_FIND_MODE_SSH:
                (void)poom_wifi_arp_probe_ssh_port(host->ip, s_menu_poom_wifi_arp_target_port, &match);
                break;
            default:
                match = false;
                break;
        }

        if (match && (s_menu_poom_wifi_arp_port_hit_count < MENU_POOM_WIFI_ARP_PORT_SWEEP_MAX_HITS))
        {
            s_menu_poom_wifi_arp_port_hit_indices[s_menu_poom_wifi_arp_port_hit_count++] = (uint8_t)i;
        }

        s_menu_poom_wifi_arp_port_done = i + 1U;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_menu_poom_wifi_arp_port_selected = 0;
    s_menu_poom_wifi_arp_port_scroll = 0;

    s_menu_poom_wifi_arp_port_task = NULL;
    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST;
    vTaskDelete(NULL);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_arp_port_sweep_start_if_ready_(void)
{
    if (s_menu_poom_wifi_arp_port_started)
    {
        return;
    }

    if (!menu_poom_wifi_arp_ensure_scan_hosts_())
    {
        menu_poom_wifi_arp_set_status_("No RAM hosts", 10U);
        s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
        return;
    }

    s_menu_poom_wifi_arp_port_started = true;
    s_menu_poom_wifi_arp_port_hit_count = 0U;
    s_menu_poom_wifi_arp_port_total = 0U;
    s_menu_poom_wifi_arp_port_done = 0U;
    (void)memset(s_menu_poom_wifi_arp_port_hit_indices, 0, sizeof(s_menu_poom_wifi_arp_port_hit_indices));

    if (s_menu_poom_wifi_arp_port_task == NULL)
    {
        (void)xTaskCreate(menu_poom_wifi_arp_port_sweep_task_,
                          "beast_find",
                          4096,
                          NULL,
                          5,
                          &s_menu_poom_wifi_arp_port_task);
    }
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_host_scan_task_(void *task_arg)
{
    const uint32_t seq = (uint32_t)(uintptr_t)task_arg;

    (void)poom_wifi_arp_probe_host_services(s_menu_poom_wifi_arp_host_scan_ip, &s_menu_poom_wifi_arp_host_scan_result);

    if ((seq == s_menu_poom_wifi_arp_host_scan_seq) && (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_WAIT))
    {
        s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_RESULT;
    }

    s_menu_poom_wifi_arp_host_scan_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Starts the internal runtime for this menu module.
 *
 * @param[in] host Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_host_scan_start_(const poom_wifi_arp_scan_host_t *host)
{
    if (host == NULL)
    {
        return;
    }

    if (!poom_wifi_ctrl_sta_has_ip())
    {
        menu_poom_wifi_arp_set_status_("No WiFi IP", 10U);
        s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
        return;
    }

    (void)snprintf(s_menu_poom_wifi_arp_host_scan_ip, sizeof(s_menu_poom_wifi_arp_host_scan_ip), "%.15s", host->ip);
    memcpy(s_menu_poom_wifi_arp_host_scan_mac, host->mac, sizeof(s_menu_poom_wifi_arp_host_scan_mac));
    (void)memset(&s_menu_poom_wifi_arp_host_scan_result, 0, sizeof(s_menu_poom_wifi_arp_host_scan_result));
    s_menu_poom_wifi_arp_host_scan_page = 0U;

    s_menu_poom_wifi_arp_host_scan_seq++;
    const uint32_t seq = s_menu_poom_wifi_arp_host_scan_seq;

    if (s_menu_poom_wifi_arp_host_scan_task == NULL)
    {
        (void)xTaskCreate(menu_poom_wifi_arp_host_scan_task_,
                          "beast_probe",
                          4096,
                          (void *)(uintptr_t)seq,
                          5,
                          &s_menu_poom_wifi_arp_host_scan_task);
    }

    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_WAIT;
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] task_arg Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_status_task_(void* task_arg)
{
    (void)task_arg;

    while(s_menu_poom_wifi_arp_active)
    {
        bool wifi_ready = poom_wifi_ctrl_sta_has_ip();

        if(s_menu_poom_wifi_arp_exit_requested)
        {
            (void)menu_poom_wifi_arp_exit_();
            break;
        }

        if((s_menu_poom_wifi_arp_status_hold_cycles > 0U) &&
           (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_MAIN))
        {
            s_menu_poom_wifi_arp_status_hold_cycles--;
            if(s_menu_poom_wifi_arp_status_hold_cycles == 0U)
            {
                (void)snprintf(s_menu_poom_wifi_arp_status, sizeof(s_menu_poom_wifi_arp_status), "A:Select  B:Exit");
            }
        }

        switch(s_menu_poom_wifi_arp_state)
        {
            case MENU_POOM_WIFI_ARP_STATE_MAIN:
                menu_poom_wifi_arp_draw_main_(wifi_ready);
                break;
            case MENU_POOM_WIFI_ARP_STATE_EDIT_SSID:
                menu_poom_wifi_arp_draw_edit_text_("Edit SSID", "A:Set HoldA:Save");
                break;
            case MENU_POOM_WIFI_ARP_STATE_EDIT_PASS:
                menu_poom_wifi_arp_draw_edit_text_("Edit PASS", "A:Set HoldA:Save");
                break;
            case MENU_POOM_WIFI_ARP_STATE_EDIT_PORT:
                menu_poom_wifi_arp_draw_edit_port_();
                break;
            case MENU_POOM_WIFI_ARP_STATE_SCAN_WAIT: {
                const bool sta_connected = poom_wifi_ctrl_sta_is_connected();
                if (!wifi_ready)
                {
                    if (!s_menu_poom_wifi_arp_scan_connect_started)
                    {
                        char pass[MENU_POOM_WIFI_ARP_PASS_MAX_LEN + 1U] = {0};
                        size_t pass_len = sizeof(pass);
                        esp_err_t status = poom_secrets_init();
                        if ((status == ESP_OK) && (s_menu_poom_wifi_arp_cached_ssid[0] != '\0') &&
                            (poom_secrets_get_wifi_pass(pass, &pass_len) == ESP_OK))
                        {
                            esp_err_t c = poom_wifi_ctrl_sta_connect(
                                s_menu_poom_wifi_arp_cached_ssid,
                                (strlen(pass) > 0U) ? pass : NULL);
                            printf("[BeastMenu] Connecting to SSID='%s' (%s)\n",
                                   s_menu_poom_wifi_arp_cached_ssid,
                                   (c == ESP_OK) ? "OK" : esp_err_to_name(c));
                            s_menu_poom_wifi_arp_scan_connect_started = true;
                            s_menu_poom_wifi_arp_scan_connect_wait_cycles = 0U;
                        }
                        else
                        {
                            menu_poom_wifi_arp_set_status_("Set SSID/PASS", 10U);
                            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
                            break;
                        }
                    }
                    else
                    {
                        s_menu_poom_wifi_arp_scan_connect_wait_cycles++;
                        if (s_menu_poom_wifi_arp_scan_connect_wait_cycles > MENU_POOM_WIFI_ARP_SCAN_CONNECT_TIMEOUT_CYCLES)
                        {
                            menu_poom_wifi_arp_set_status_("WiFi timeout", 10U);
                            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
                            break;
                        }
                    }

                    menu_poom_wifi_arp_draw_scan_wait_(sta_connected ? "Connected (DHCP...)" : "Connecting...");
                    break;
                }

                menu_poom_wifi_arp_scan_start_if_ready_();
                menu_poom_wifi_arp_draw_scan_wait_("Scanning...");
                break;
            }
            case MENU_POOM_WIFI_ARP_STATE_SCAN_LIST:
                menu_poom_wifi_arp_draw_scan_list_();
                break;
            case MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_WAIT:
                menu_poom_wifi_arp_draw_host_scan_wait_();
                break;
            case MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_RESULT:
                menu_poom_wifi_arp_draw_host_scan_result_();
                break;
            case MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_WAIT: {
                const bool sta_connected = poom_wifi_ctrl_sta_is_connected();
                if (!wifi_ready)
                {
                    if (!s_menu_poom_wifi_arp_port_connect_started)
                    {
                        char pass[MENU_POOM_WIFI_ARP_PASS_MAX_LEN + 1U] = {0};
                        size_t pass_len = sizeof(pass);
                        esp_err_t status = poom_secrets_init();
                        if ((status == ESP_OK) && (s_menu_poom_wifi_arp_cached_ssid[0] != '\0') &&
                            (poom_secrets_get_wifi_pass(pass, &pass_len) == ESP_OK))
                        {
                            esp_err_t c = poom_wifi_ctrl_sta_connect(
                                s_menu_poom_wifi_arp_cached_ssid,
                                (strlen(pass) > 0U) ? pass : NULL);
                            printf("[BeastMenu] Connecting to SSID='%s' (%s)\n",
                                   s_menu_poom_wifi_arp_cached_ssid,
                                   (c == ESP_OK) ? "OK" : esp_err_to_name(c));
                            s_menu_poom_wifi_arp_port_connect_started = true;
                            s_menu_poom_wifi_arp_port_connect_wait_cycles = 0U;
                        }
                        else
                        {
                            menu_poom_wifi_arp_set_status_("Set SSID/PASS", 10U);
                            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
                            break;
                        }
                    }
                    else
                    {
                        s_menu_poom_wifi_arp_port_connect_wait_cycles++;
                        if (s_menu_poom_wifi_arp_port_connect_wait_cycles > MENU_POOM_WIFI_ARP_SCAN_CONNECT_TIMEOUT_CYCLES)
                        {
                            menu_poom_wifi_arp_set_status_("WiFi timeout", 10U);
                            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
                            break;
                        }
                    }

                    menu_poom_wifi_arp_draw_scan_wait_(sta_connected ? "Connected (DHCP...)" : "Connecting...");
                    break;
                }

                menu_poom_wifi_arp_port_sweep_start_if_ready_();
                menu_poom_wifi_arp_draw_port_sweep_wait_();
                break;
            }
            case MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST:
                menu_poom_wifi_arp_draw_port_sweep_list_();
                break;
            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(MENU_POOM_WIFI_ARP_REFRESH_MS));
    }

    s_menu_poom_wifi_arp_status_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_enter_edit_ssid`.
 *
 * @return void
 */
static void menu_poom_wifi_arp_enter_edit_ssid_(void)
{
    size_t len;
    esp_err_t status;

    menu_poom_wifi_arp_edit_reset_();

    status = poom_secrets_init();
    if(status == ESP_OK)
    {
        len = MENU_POOM_WIFI_ARP_SSID_MAX_LEN + 1U;
        status = poom_secrets_get_wifi_ssid(s_menu_poom_wifi_arp_edit_buf, &len);
        if(status == ESP_OK)
        {
            s_menu_poom_wifi_arp_edit_len = strlen(s_menu_poom_wifi_arp_edit_buf);
            if(s_menu_poom_wifi_arp_edit_len > MENU_POOM_WIFI_ARP_SSID_MAX_LEN)
            {
                s_menu_poom_wifi_arp_edit_len = MENU_POOM_WIFI_ARP_SSID_MAX_LEN;
                s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_len] = '\0';
            }
            s_menu_poom_wifi_arp_edit_cursor = s_menu_poom_wifi_arp_edit_len;
        }
    }

    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_EDIT_SSID;
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_enter_edit_pass`.
 *
 * @return void
 */
static void menu_poom_wifi_arp_enter_edit_pass_(void)
{
    size_t len;
    esp_err_t status;

    menu_poom_wifi_arp_edit_reset_();

    status = poom_secrets_init();
    if(status == ESP_OK)
    {
        len = MENU_POOM_WIFI_ARP_PASS_MAX_LEN + 1U;
        status = poom_secrets_get_wifi_pass(s_menu_poom_wifi_arp_edit_buf, &len);
        if(status == ESP_OK)
        {
            s_menu_poom_wifi_arp_edit_len = strlen(s_menu_poom_wifi_arp_edit_buf);
            if(s_menu_poom_wifi_arp_edit_len > MENU_POOM_WIFI_ARP_PASS_MAX_LEN)
            {
                s_menu_poom_wifi_arp_edit_len = MENU_POOM_WIFI_ARP_PASS_MAX_LEN;
                s_menu_poom_wifi_arp_edit_buf[s_menu_poom_wifi_arp_edit_len] = '\0';
            }
            s_menu_poom_wifi_arp_edit_cursor = s_menu_poom_wifi_arp_edit_len;
        }
    }

    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_EDIT_PASS;
}

/**
 * @brief Internal helper for `menu_poom_wifi_arp_enter_edit_port`.
 *
 * @return void
 */
static void menu_poom_wifi_arp_enter_edit_port_(void)
{
    (void)snprintf(s_menu_poom_wifi_arp_port_digits,
                   sizeof(s_menu_poom_wifi_arp_port_digits),
                   "%05u",
                   (unsigned)s_menu_poom_wifi_arp_target_port);
    s_menu_poom_wifi_arp_port_cursor = 4U;
    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_EDIT_PORT;
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_handle_edit_port_button_(uint8_t button)
{
    if (button == BTN_LEFT)
    {
        if (s_menu_poom_wifi_arp_port_cursor > 0U)
        {
            s_menu_poom_wifi_arp_port_cursor--;
        }
        return;
    }

    if (button == BTN_RIGHT)
    {
        if (s_menu_poom_wifi_arp_port_cursor < 4U)
        {
            s_menu_poom_wifi_arp_port_cursor++;
        }
        return;
    }

    if (button == BTN_UP)
    {
        const uint8_t idx = (uint8_t)(s_menu_poom_wifi_arp_port_cursor % 5U);
        char c = s_menu_poom_wifi_arp_port_digits[idx];
        if ((c < '0') || (c > '9'))
        {
            c = '0';
        }
        c = (c == '9') ? '0' : (char)(c + 1);
        s_menu_poom_wifi_arp_port_digits[idx] = c;
        return;
    }

    if (button == BTN_DOWN)
    {
        const uint8_t idx = (uint8_t)(s_menu_poom_wifi_arp_port_cursor % 5U);
        char c = s_menu_poom_wifi_arp_port_digits[idx];
        if ((c < '0') || (c > '9'))
        {
            c = '0';
        }
        c = (c == '0') ? '9' : (char)(c - 1);
        s_menu_poom_wifi_arp_port_digits[idx] = c;
        return;
    }

    if (button == BTN_A)
    {
        s_menu_poom_wifi_arp_find_mode =
            (menu_poom_wifi_arp_find_mode_t)((s_menu_poom_wifi_arp_find_mode + 1U) % MENU_POOM_WIFI_ARP_FIND_MODE_COUNT);
        return;
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_handle_main_button_(uint8_t button)
{
    if(button == BTN_UP)
    {
        if(s_menu_poom_wifi_arp_selected == 0)
        {
            s_menu_poom_wifi_arp_selected = (menu_poom_wifi_arp_item_t)(MENU_POOM_WIFI_ARP_ITEM_COUNT - 1U);
        }
        else
        {
            s_menu_poom_wifi_arp_selected = (menu_poom_wifi_arp_item_t)(s_menu_poom_wifi_arp_selected - 1);
        }
        menu_poom_wifi_arp_adjust_window_();
        return;
    }

    if(button == BTN_DOWN)
    {
        s_menu_poom_wifi_arp_selected = (menu_poom_wifi_arp_item_t)((s_menu_poom_wifi_arp_selected + 1U) % MENU_POOM_WIFI_ARP_ITEM_COUNT);
        menu_poom_wifi_arp_adjust_window_();
        return;
    }

    if(button != BTN_A)
    {
        return;
    }

    switch(s_menu_poom_wifi_arp_selected)
    {
        case MENU_POOM_WIFI_ARP_ITEM_SSID:
            menu_poom_wifi_arp_enter_edit_ssid_();
            break;
        case MENU_POOM_WIFI_ARP_ITEM_PASS:
            menu_poom_wifi_arp_enter_edit_pass_();
            break;
        case MENU_POOM_WIFI_ARP_ITEM_SCAN:
            s_menu_poom_wifi_arp_scan_started = false;
            s_menu_poom_wifi_arp_scan_connect_started = false;
            s_menu_poom_wifi_arp_scan_connect_wait_cycles = 0U;
            s_menu_poom_wifi_arp_scan_count = 0U;
            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_SCAN_WAIT;
            break;
        case MENU_POOM_WIFI_ARP_ITEM_FIND_PORT:
            menu_poom_wifi_arp_enter_edit_port_();
            break;
        case MENU_POOM_WIFI_ARP_ITEM_BACK:
        default:
            s_menu_poom_wifi_arp_exit_requested = true;
            break;
    }
}

/**
 * @brief Handles the current menu action.
 *
 * @param[in] button Parameter passed to the helper.
 * @param[in] max_len Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_handle_edit_text_button_(uint8_t button, size_t max_len)
{
    if(button == BTN_LEFT)
    {
        if(s_menu_poom_wifi_arp_edit_cursor > 0U)
        {
            s_menu_poom_wifi_arp_edit_cursor--;
        }
    }
    else if(button == BTN_RIGHT)
    {
        if(s_menu_poom_wifi_arp_edit_cursor < s_menu_poom_wifi_arp_edit_len)
        {
            s_menu_poom_wifi_arp_edit_cursor++;
        }
    }
    else if(button == BTN_UP)
    {
        menu_poom_wifi_arp_cycle_current_char_(1, max_len);
    }
    else if(button == BTN_DOWN)
    {
        menu_poom_wifi_arp_cycle_current_char_(-1, max_len);
    }
    else if(button == BTN_A)
    {
        s_menu_poom_wifi_arp_char_set =
            (menu_poom_wifi_arp_char_set_t)((s_menu_poom_wifi_arp_char_set + 1U) % MENU_POOM_WIFI_ARP_SET_COUNT);
    }
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_arp_save_ssid_(void)
{
    esp_err_t status;
    char ssid_buf[MENU_POOM_WIFI_ARP_SSID_MAX_LEN + 1U] = {0};
    size_t ssid_len;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        menu_poom_wifi_arp_set_status_("NVS error", 8U);
        return;
    }

    ssid_len = strlen(s_menu_poom_wifi_arp_edit_buf);
    if(ssid_len > MENU_POOM_WIFI_ARP_SSID_MAX_LEN)
    {
        ssid_len = MENU_POOM_WIFI_ARP_SSID_MAX_LEN;
    }

    (void)memcpy(ssid_buf, s_menu_poom_wifi_arp_edit_buf, ssid_len);
    ssid_buf[ssid_len] = '\0';

    status = poom_secrets_set_wifi_ssid(ssid_buf);
    if(status == ESP_OK)
    {
        (void)memcpy(s_menu_poom_wifi_arp_cached_ssid, ssid_buf, ssid_len);
        s_menu_poom_wifi_arp_cached_ssid[ssid_len] = '\0';
        menu_poom_wifi_arp_set_status_("SSID saved", 6U);
    }
    else
    {
        menu_poom_wifi_arp_set_status_("Save failed", 8U);
    }
}

/**
 * @brief Saves internal data used by this menu module.
 *
 * @return void
 */
static void menu_poom_wifi_arp_save_pass_(void)
{
    esp_err_t status;

    status = poom_secrets_init();
    if(status != ESP_OK)
    {
        menu_poom_wifi_arp_set_status_("NVS error", 8U);
        return;
    }

    status = poom_secrets_set_wifi_pass(s_menu_poom_wifi_arp_edit_buf);
    if(status == ESP_OK)
    {
        s_menu_poom_wifi_arp_cached_pass_len = strlen(s_menu_poom_wifi_arp_edit_buf);
        menu_poom_wifi_arp_set_status_("PASS saved", 6U);
    }
    else
    {
        menu_poom_wifi_arp_set_status_("Save failed", 8U);
    }
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_poom_wifi_arp_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    menu_poom_wifi_arp_button_msg_t button_msg;

    (void)user_ctx;

    if((msg == NULL) || (msg->len < sizeof(button_msg)))
    {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));

    if((button_msg.button == BTN_B) && (button_msg.event == BUTTON_SINGLE_CLICK))
    {
        if ((s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_WAIT) ||
            (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_RESULT))
        {
            if (s_menu_poom_wifi_arp_host_scan_task != NULL)
            {
                TaskHandle_t scan_task = s_menu_poom_wifi_arp_host_scan_task;
                s_menu_poom_wifi_arp_host_scan_task = NULL;
                s_menu_poom_wifi_arp_host_scan_seq++;
                vTaskDelete(scan_task);
            }
            s_menu_poom_wifi_arp_state = s_menu_poom_wifi_arp_host_scan_return_state;
            return;
        }

        if((s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_SCAN_WAIT) ||
           (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_SCAN_LIST) ||
           (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_WAIT) ||
           (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST))
        {
            if(s_menu_poom_wifi_arp_status_task == NULL)
            {
                (void)menu_poom_wifi_arp_exit_();
            }
            else
            {
                s_menu_poom_wifi_arp_exit_requested = true;
            }
            return;
        }

        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_MAIN)
        {
            if(s_menu_poom_wifi_arp_status_task == NULL)
            {
                (void)menu_poom_wifi_arp_exit_();
            }
            else
            {
                s_menu_poom_wifi_arp_exit_requested = true;
            }
        }
        else
        {
            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
        }
        return;
    }

    if(button_msg.event == BUTTON_SINGLE_CLICK)
    {
        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_SCAN_LIST)
        {
            if (button_msg.button == BTN_UP)
            {
                s_menu_poom_wifi_arp_scan_selected--;
                return;
            }
            if (button_msg.button == BTN_DOWN)
            {
                s_menu_poom_wifi_arp_scan_selected++;
                return;
            }
            if (button_msg.button == BTN_A)
            {
                if (s_menu_poom_wifi_arp_scan_count == 0U)
                {
                    return;
                }

                if (s_menu_poom_wifi_arp_scan_selected < 0)
                {
                    s_menu_poom_wifi_arp_scan_selected = 0;
                }
                if (s_menu_poom_wifi_arp_scan_selected > (int)s_menu_poom_wifi_arp_scan_count - 1)
                {
                    s_menu_poom_wifi_arp_scan_selected = (int)s_menu_poom_wifi_arp_scan_count - 1;
                }

                const poom_wifi_arp_scan_host_t *host = &s_menu_poom_wifi_arp_scan_hosts[s_menu_poom_wifi_arp_scan_selected];
                s_menu_poom_wifi_arp_host_scan_return_state = MENU_POOM_WIFI_ARP_STATE_SCAN_LIST;
                menu_poom_wifi_arp_host_scan_start_(host);
                return;
            }
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST)
        {
            if (button_msg.button == BTN_UP)
            {
                s_menu_poom_wifi_arp_port_selected--;
                return;
            }
            if (button_msg.button == BTN_DOWN)
            {
                s_menu_poom_wifi_arp_port_selected++;
                return;
            }
            if (button_msg.button == BTN_A)
            {
                if (s_menu_poom_wifi_arp_port_hit_count == 0U)
                {
                    return;
                }

                if (s_menu_poom_wifi_arp_port_selected < 0)
                {
                    s_menu_poom_wifi_arp_port_selected = 0;
                }
                if (s_menu_poom_wifi_arp_port_selected > (int)s_menu_poom_wifi_arp_port_hit_count - 1)
                {
                    s_menu_poom_wifi_arp_port_selected = (int)s_menu_poom_wifi_arp_port_hit_count - 1;
                }

                const poom_wifi_arp_scan_host_t *host =
                    menu_poom_wifi_arp_get_port_hit_host_((size_t)s_menu_poom_wifi_arp_port_selected);
                if (host == NULL)
                {
                    return;
                }
                s_menu_poom_wifi_arp_host_scan_return_state = MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_LIST;
                menu_poom_wifi_arp_host_scan_start_(host);
                return;
            }
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_HOST_SCAN_RESULT)
        {
            if (button_msg.button == BTN_A)
            {
                s_menu_poom_wifi_arp_host_scan_page = (uint8_t)((s_menu_poom_wifi_arp_host_scan_page + 1U) & 0x01U);
            }
            return;
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_SCAN_WAIT)
        {
            return;
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_WAIT)
        {
            return;
        }

        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_MAIN)
        {
            menu_poom_wifi_arp_handle_main_button_(button_msg.button);
            return;
        }

        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_SSID)
        {
            menu_poom_wifi_arp_handle_edit_text_button_(button_msg.button, MENU_POOM_WIFI_ARP_SSID_MAX_LEN);
            return;
        }

        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_PASS)
        {
            menu_poom_wifi_arp_handle_edit_text_button_(button_msg.button, MENU_POOM_WIFI_ARP_PASS_MAX_LEN);
            return;
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_PORT)
        {
            menu_poom_wifi_arp_handle_edit_port_button_(button_msg.button);
            return;
        }
    }
    else if((button_msg.button == BTN_A) && menu_poom_wifi_arp_is_long_press_event_(button_msg.event))
    {
        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_SSID)
        {
            menu_poom_wifi_arp_save_ssid_();
            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
            return;
        }

        if(s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_PASS)
        {
            menu_poom_wifi_arp_save_pass_();
            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
            return;
        }

        if (s_menu_poom_wifi_arp_state == MENU_POOM_WIFI_ARP_STATE_EDIT_PORT)
        {
            s_menu_poom_wifi_arp_target_port = menu_poom_wifi_arp_parse_port_digits_(s_menu_poom_wifi_arp_port_digits);
            s_menu_poom_wifi_arp_port_started = false;
            s_menu_poom_wifi_arp_port_connect_started = false;
            s_menu_poom_wifi_arp_port_connect_wait_cycles = 0U;
            s_menu_poom_wifi_arp_port_hit_count = 0U;
            s_menu_poom_wifi_arp_port_total = 0U;
            s_menu_poom_wifi_arp_port_done = 0U;
            s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_PORT_SWEEP_WAIT;
            return;
        }
    }
}

void menu_poom_wifi_arp_show(void)
{
    s_menu_poom_wifi_arp_active = true;
    s_menu_poom_wifi_arp_exit_requested = false;
    s_menu_poom_wifi_arp_state = MENU_POOM_WIFI_ARP_STATE_MAIN;
    s_menu_poom_wifi_arp_selected = MENU_POOM_WIFI_ARP_ITEM_SSID;
    s_menu_poom_wifi_arp_window_start = 0U;
    (void)snprintf(s_menu_poom_wifi_arp_status, sizeof(s_menu_poom_wifi_arp_status), "A:Select  B:Exit");
    s_menu_poom_wifi_arp_status_hold_cycles = 0U;

    s_menu_poom_wifi_arp_scan_started = false;
    s_menu_poom_wifi_arp_scan_connect_started = false;
    s_menu_poom_wifi_arp_scan_connect_wait_cycles = 0U;
    s_menu_poom_wifi_arp_scan_count = 0U;

    s_menu_poom_wifi_arp_port_started = false;
    s_menu_poom_wifi_arp_port_connect_started = false;
    s_menu_poom_wifi_arp_port_connect_wait_cycles = 0U;
    s_menu_poom_wifi_arp_port_hit_count = 0U;
    s_menu_poom_wifi_arp_port_total = 0U;
    s_menu_poom_wifi_arp_port_done = 0U;
    s_menu_poom_wifi_arp_host_scan_return_state = MENU_POOM_WIFI_ARP_STATE_SCAN_LIST;
    s_menu_poom_wifi_arp_find_mode = MENU_POOM_WIFI_ARP_FIND_MODE_TCP;

    s_menu_poom_wifi_arp_wifi_cb_registered = false;

    menu_poom_wifi_arp_load_cached_values_();
    if (poom_wifi_ctrl_register_cb(menu_poom_wifi_arp_wifi_evt_cb_, NULL) == ESP_OK)
    {
        s_menu_poom_wifi_arp_wifi_cb_registered = true;
    }

    if(!s_menu_poom_wifi_arp_buttons_subscribed)
    {
        if(poom_sbus_subscribe_cb("input/button", menu_poom_wifi_arp_button_cb_, s_menu_poom_wifi_arp_sbus_user))
        {
            s_menu_poom_wifi_arp_buttons_subscribed = true;
        }
        else
        {
            s_menu_poom_wifi_arp_active = false;
            menu_poom_wifi_arp_release_scan_hosts_();
            poom_arduboy_clear();
            poom_arduboy_set_text_size(1);
            menu_poom_wifi_arp_draw_title_(NULL);
            poom_arduboy_set_cursor(10, 30);
            (void)poom_arduboy_print(F("Button sub error"));
            poom_arduboy_display();

            const uint8_t token = 1;
            (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
            return;
        }
    }

    if(s_menu_poom_wifi_arp_status_task == NULL)
    {
        (void)xTaskCreate(menu_poom_wifi_arp_status_task_,
                          "menu_wifi_arp",
                          MENU_POOM_WIFI_ARP_STATUS_STACK,
                          NULL,
                          MENU_POOM_WIFI_ARP_STATUS_PRIO,
                          &s_menu_poom_wifi_arp_status_task);
    }
}
