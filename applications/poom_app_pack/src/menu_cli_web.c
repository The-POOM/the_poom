// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_cli_web.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/fcntl.h>

#include "Arduboy2.h"
#include "button_driver.h"
#include "esp_err.h"
#include "esp_console.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_events.h"
#include "cli.h"
#include "cli_nfc.h"
#include "poom_web.h"
#include "poom_secrets_store.h"
#include "poom_sbus.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

#define MENU_CLI_WEB_REFRESH_MS (250U)
#define MENU_CLI_WEB_TASK_STACK (3072U)
#define MENU_CLI_WEB_TASK_PRIO (4U)
#define MENU_CLI_WEB_CLI_ENABLED_FOR_MEM_TEST (1U)

#define HEADER_H (11)
#define BOX_Y (12)
#define BOX_H (40)

#define TEXT_X (4)
#define ROW0_Y (16)
#define ROW_STEP (10)
#define ROW_HILITE_H (9)

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK (4U)
#endif

typedef enum
{
    MENU_CLI_WEB_SCREEN_SELECT = 0,
    MENU_CLI_WEB_SCREEN_STARTING,
    MENU_CLI_WEB_SCREEN_READY,
    MENU_CLI_WEB_SCREEN_ERROR,
} menu_cli_web_screen_t;

typedef enum
{
    MENU_CLI_WEB_MODE_AP = 0,
    MENU_CLI_WEB_MODE_STA,
} menu_cli_web_mode_t;

static bool s_menu_cli_web_buttons_subscribed = false;
static bool s_menu_cli_web_active = false;
static bool s_menu_cli_web_exit_requested = false;
static bool s_menu_cli_web_start_requested = false;
static bool s_menu_cli_web_input_dirty = false;
static TaskHandle_t s_menu_cli_web_task = NULL;

static menu_cli_web_mode_t s_menu_cli_web_selected_mode = MENU_CLI_WEB_MODE_AP;
static menu_cli_web_mode_t s_menu_cli_web_running_mode = MENU_CLI_WEB_MODE_AP;
static menu_cli_web_screen_t s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_SELECT;

static char s_menu_cli_web_status[22] = "Select mode";
static char s_menu_cli_web_help[22] = "A:START";
static char s_menu_cli_web_ip[16] = {0};
static char s_menu_cli_web_sta_ssid[33] = {0};

#define MENU_CLI_WEB_STDIO_VFS_PATH "/dev/pweb"
#define MENU_CLI_WEB_STDIO_LINE_MAX (256U)

typedef struct
{
    char line[MENU_CLI_WEB_STDIO_LINE_MAX];
    size_t len;
} menu_cli_web_line_buf_t;

static bool s_menu_cli_web_console_inited = false;
static bool s_menu_cli_web_stdio_installed = false;
static bool s_menu_cli_web_stdio_send_enabled = false;
static SemaphoreHandle_t s_menu_cli_web_stdio_mutex = NULL;
static SemaphoreHandle_t s_menu_cli_web_console_mutex = NULL;
static menu_cli_web_line_buf_t s_menu_cli_web_line_out = {0};
static menu_cli_web_line_buf_t s_menu_cli_web_line_err = {0};
static bool s_menu_cli_web_stdio_in_write = false;
static esp_err_t s_menu_cli_web_stdio_last_register_err = ESP_OK;
static int s_menu_cli_web_stdio_last_stdout_errno = 0;
static int s_menu_cli_web_stdio_last_stderr_errno = 0;
static int s_menu_cli_web_stdio_next_local_fd = 1;

static void menu_cli_web_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx);
static void menu_cli_web_task_(void* arg);
static esp_err_t menu_cli_web_console_init_once_(void);
static esp_err_t menu_cli_web_stdio_install_once_(void);
static void menu_cli_web_stdio_flush_pending_(void);
static ssize_t menu_cli_web_vfs_write_(int fd, const void* data, size_t size);
static ssize_t menu_cli_web_vfs_read_(int fd, void* dst, size_t size);
static int menu_cli_web_vfs_open_(const char* path, int flags, int mode);
static int menu_cli_web_vfs_close_(int fd);
static int menu_cli_web_vfs_fstat_(int fd, struct stat* st);
static int menu_cli_web_vfs_fcntl_(int fd, int cmd, int arg);

/**
 * @brief Renders the current menu state.
 *
 * @return void
 */
static void menu_cli_web_request_render_(void)
{
    s_menu_cli_web_input_dirty = true;
    if (s_menu_cli_web_task != NULL)
    {
        (void)xTaskNotifyGive(s_menu_cli_web_task);
    }
}

/**
 * @brief Internal helper for `menu_cli_web_stdio_flush_buf`.
 *
 * @param[in] b Parameter passed to the helper.
 * @return void
 */
static void menu_cli_web_stdio_flush_buf_(menu_cli_web_line_buf_t* b)
{
    if((b == NULL) || (b->len == 0U)) {
        return;
    }

    b->line[b->len] = '\0';
    if(s_menu_cli_web_stdio_send_enabled && !s_menu_cli_web_stdio_in_write) {
        s_menu_cli_web_stdio_in_write = true;
        (void)poom_web_send_text(b->line);
        s_menu_cli_web_stdio_in_write = false;
    }
    b->len = 0U;
}

/**
 * @brief Internal helper for `menu_cli_web_stdio_buffer_and_maybe_flush`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @param[in] data Parameter passed to the helper.
 * @param[in] size Parameter passed to the helper.
 * @return void
 */
static void menu_cli_web_stdio_buffer_and_maybe_flush_(int fd, const char* data, size_t size)
{
    menu_cli_web_line_buf_t* b = (fd == STDERR_FILENO) ? &s_menu_cli_web_line_err : &s_menu_cli_web_line_out;

    for(size_t i = 0U; i < size; i++) {
        const char c = data[i];
        if(b->len < (MENU_CLI_WEB_STDIO_LINE_MAX - 1U)) {
            b->line[b->len++] = c;
        }

        if((c == '\n') || (b->len >= (MENU_CLI_WEB_STDIO_LINE_MAX - 1U))) {
            menu_cli_web_stdio_flush_buf_(b);
        }
    }
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_write`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @param[in] data Parameter passed to the helper.
 * @param[in] size Parameter passed to the helper.
 * @return ssize_t
 */
static ssize_t menu_cli_web_vfs_write_(int fd, const void* data, size_t size)
{
    if((data == NULL) || (size == 0U)) {
        return 0;
    }

    if(s_menu_cli_web_stdio_mutex != NULL) {
        (void)xSemaphoreTake(s_menu_cli_web_stdio_mutex, portMAX_DELAY);
    }

    menu_cli_web_stdio_buffer_and_maybe_flush_(fd, (const char*)data, size);

    if(s_menu_cli_web_stdio_mutex != NULL) {
        (void)xSemaphoreGive(s_menu_cli_web_stdio_mutex);
    }

    return (ssize_t)size;
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_read`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @param[in] dst Parameter passed to the helper.
 * @param[in] size Parameter passed to the helper.
 * @return ssize_t
 */
static ssize_t menu_cli_web_vfs_read_(int fd, void* dst, size_t size)
{
    (void)fd;
    (void)dst;
    (void)size;
    return 0;
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_open`.
 *
 * @param[in] path Parameter passed to the helper.
 * @param[in] flags Parameter passed to the helper.
 * @param[in] mode Parameter passed to the helper.
 * @return int
 */
static int menu_cli_web_vfs_open_(const char* path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    const int fd = s_menu_cli_web_stdio_next_local_fd;
    if(s_menu_cli_web_stdio_next_local_fd < 3) {
        s_menu_cli_web_stdio_next_local_fd++;
    }
    return fd;
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_close`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @return int
 */
static int menu_cli_web_vfs_close_(int fd)
{
    (void)fd;
    return 0;
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_fstat`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @param[in] st Parameter passed to the helper.
 * @return int
 */
static int menu_cli_web_vfs_fstat_(int fd, struct stat* st)
{
    (void)fd;
    if(st == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

/**
 * @brief Internal helper for `menu_cli_web_vfs_fcntl`.
 *
 * @param[in] fd Parameter passed to the helper.
 * @param[in] cmd Parameter passed to the helper.
 * @param[in] arg Parameter passed to the helper.
 * @return int
 */
static int menu_cli_web_vfs_fcntl_(int fd, int cmd, int arg)
{
    (void)fd;
    (void)arg;
    if(cmd == F_GETFL) {
        return O_WRONLY;
    }
    errno = ENOSYS;
    return -1;
}

/**
 * @brief Internal helper for `menu_cli_web_stdio_flush_pending`.
 *
 * @return void
 */
static void menu_cli_web_stdio_flush_pending_(void)
{
    if(s_menu_cli_web_stdio_mutex != NULL) {
        (void)xSemaphoreTake(s_menu_cli_web_stdio_mutex, portMAX_DELAY);
    }
    menu_cli_web_stdio_flush_buf_(&s_menu_cli_web_line_out);
    menu_cli_web_stdio_flush_buf_(&s_menu_cli_web_line_err);
    if(s_menu_cli_web_stdio_mutex != NULL) {
        (void)xSemaphoreGive(s_menu_cli_web_stdio_mutex);
    }
}

/**
 * @brief Internal helper for `menu_cli_web_stdio_install_once`.
 *
 * @return esp_err_t
 */
static esp_err_t menu_cli_web_stdio_install_once_(void)
{
    if(s_menu_cli_web_stdio_installed) {
        return ESP_OK;
    }

    if(s_menu_cli_web_stdio_mutex == NULL) {
        s_menu_cli_web_stdio_mutex = xSemaphoreCreateMutex();
        if(s_menu_cli_web_stdio_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_t vfs = {0};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.write = &menu_cli_web_vfs_write_;
    vfs.read  = &menu_cli_web_vfs_read_;
    vfs.open  = &menu_cli_web_vfs_open_;
    vfs.close = &menu_cli_web_vfs_close_;
    vfs.fstat = &menu_cli_web_vfs_fstat_;
    vfs.fcntl = &menu_cli_web_vfs_fcntl_;

    s_menu_cli_web_stdio_last_register_err = ESP_OK;
    s_menu_cli_web_stdio_last_stdout_errno = 0;
    s_menu_cli_web_stdio_last_stderr_errno = 0;

    esp_err_t err = esp_vfs_register(MENU_CLI_WEB_STDIO_VFS_PATH, &vfs, NULL);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        s_menu_cli_web_stdio_last_register_err = err;
        return err;
    }

    errno = 0;
    FILE* fout = freopen(MENU_CLI_WEB_STDIO_VFS_PATH, "w", stdout);
    s_menu_cli_web_stdio_last_stdout_errno = errno;

    errno = 0;
    FILE* ferr = freopen(MENU_CLI_WEB_STDIO_VFS_PATH, "w", stderr);
    s_menu_cli_web_stdio_last_stderr_errno = errno;

    if((fout == NULL) || (ferr == NULL)) {
        return ESP_FAIL;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    s_menu_cli_web_stdio_installed = true;
    return ESP_OK;
}

/**
 * @brief Initializes internal resources for this menu module.
 *
 * @return esp_err_t
 */
static esp_err_t menu_cli_web_console_init_once_(void)
{
    if(s_menu_cli_web_console_inited) {
        return ESP_OK;
    }

    if(s_menu_cli_web_console_mutex == NULL) {
        s_menu_cli_web_console_mutex = xSemaphoreCreateMutex();
        if(s_menu_cli_web_console_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
    esp_err_t err = esp_console_init(&cfg);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    (void)esp_console_register_help_command();
    cli_poom_nfc_register_cmds();

    s_menu_cli_web_console_inited = true;
    return ESP_OK;
}

/**
 * @brief Releases internal state before leaving this menu.
 *
 * @return void
 */
static void menu_cli_web_exit_(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();

    s_menu_cli_web_active = false;
    s_menu_cli_web_exit_requested = false;
    s_menu_cli_web_stdio_send_enabled = false;
    menu_cli_web_stdio_flush_pending_();

    if (s_menu_cli_web_task != NULL)
    {
        if (s_menu_cli_web_task != current_task)
        {
            TaskHandle_t task = s_menu_cli_web_task;
            s_menu_cli_web_task = NULL;
            vTaskDelete(task);
        }
        else
        {
            s_menu_cli_web_task = NULL;
        }
    }

    if (s_menu_cli_web_buttons_subscribed) {
        (void)poom_sbus_unsubscribe_cb("input/button", menu_cli_web_button_cb_, "menu_cli_web");
        s_menu_cli_web_buttons_subscribed = false;
    }

    (void)poom_web_set_command_cb(NULL, NULL);
    (void)poom_web_deinit();

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Handles an internal callback for this menu module.
 *
 * @param[in] command Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void cli_web_command_cb_(const char* command, void* user_ctx) {
    (void)user_ctx;
    if(command == NULL) {
        return;
    }

    if(MENU_CLI_WEB_CLI_ENABLED_FOR_MEM_TEST == 0U) {
        (void)poom_web_send_text("CLI disabled for memory test\n");
        return;
    }

    if(menu_cli_web_console_init_once_() != ESP_OK) {
        (void)poom_web_send_text("ERR: console init failed\n");
        return;
    }

    esp_err_t stdio_err = menu_cli_web_stdio_install_once_();
    if(stdio_err != ESP_OK) {
        char resp[160];
        (void)snprintf(resp,
                       sizeof(resp),
                       "ERR: stdio hook failed (ret=%s reg=%s errno_out=%d errno_err=%d)\n",
                       esp_err_to_name(stdio_err),
                       esp_err_to_name(s_menu_cli_web_stdio_last_register_err),
                       s_menu_cli_web_stdio_last_stdout_errno,
                       s_menu_cli_web_stdio_last_stderr_errno);
        (void)poom_web_send_text(resp);
        return;
    }

    s_menu_cli_web_stdio_send_enabled = true;

    if(s_menu_cli_web_console_mutex != NULL) {
        (void)xSemaphoreTake(s_menu_cli_web_console_mutex, portMAX_DELAY);
    }

    int cmd_ret = 0;
    esp_err_t err = esp_console_run(command, &cmd_ret);
    if(err == ESP_ERR_NOT_FOUND) {
        (void)poom_web_send_text("Command not found\n");
    } else if(err == ESP_ERR_INVALID_ARG) {
    } else if(err == ESP_OK && cmd_ret != ESP_OK) {
        char resp[96];
        (void)snprintf(resp, sizeof(resp), "Command returned: 0x%x\n", cmd_ret);
        (void)poom_web_send_text(resp);
    } else if(err != ESP_OK) {
        char resp[96];
        (void)snprintf(resp, sizeof(resp), "Console error: %s\n", esp_err_to_name(err));
        (void)poom_web_send_text(resp);
    }

    if(s_menu_cli_web_console_mutex != NULL) {
        (void)xSemaphoreGive(s_menu_cli_web_console_mutex);
    }

    menu_cli_web_stdio_flush_pending_();
}

/**
 * @brief Handles button events for this menu module.
 *
 * @param[in] msg Parameter passed to the helper.
 * @param[in] user_ctx Parameter passed to the helper.
 * @return void
 */
static void menu_cli_web_button_cb_(const poom_sbus_msg_t* msg, void* user_ctx)
{
    button_event_msg_t button_msg;

    (void)user_ctx;
    if ((msg == NULL) || (msg->len < sizeof(button_msg))) {
        return;
    }

    (void)memcpy(&button_msg, msg->data, sizeof(button_msg));
    if (button_msg.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (button_msg.button == BUTTON_B)
    {
        if (s_menu_cli_web_task == NULL)
        {
            menu_cli_web_exit_();
        }
        else
        {
            s_menu_cli_web_exit_requested = true;
            menu_cli_web_request_render_();
        }
        return;
    }

    if (s_menu_cli_web_screen != MENU_CLI_WEB_SCREEN_SELECT)
    {
        return;
    }

    if (button_msg.button == BUTTON_UP)
    {
        s_menu_cli_web_selected_mode = MENU_CLI_WEB_MODE_AP;
        menu_cli_web_request_render_();
    }
    else if (button_msg.button == BUTTON_DOWN)
    {
        s_menu_cli_web_selected_mode = MENU_CLI_WEB_MODE_STA;
        menu_cli_web_request_render_();
    }
    else if (button_msg.button == BUTTON_A)
    {
        s_menu_cli_web_start_requested = true;
        s_menu_cli_web_running_mode = s_menu_cli_web_selected_mode;
        s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_STARTING;
        if (s_menu_cli_web_selected_mode == MENU_CLI_WEB_MODE_STA)
        {
            (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Connecting...");
            (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Waiting IP...");
        }
        else
        {
            (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Starting...");
            (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Please wait");
        }
        menu_cli_web_request_render_();
    }
}

/**
 * @brief Draws the current menu state.
 *
 * @return void
 */
static void menu_cli_web_draw_(void)
{
    poom_arduboy_clear();
    poom_arduboy_set_text_size(1);

    poom_arduboy_set_cursor(43, 2);
    (void)poom_arduboy_print(F("POOM WEB"));
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, HEADER_H, INVERT);

    poom_arduboy_draw_rect(0, BOX_Y, ARDUBOY_WIDTH, BOX_H, WHITE);

    if (s_menu_cli_web_screen == MENU_CLI_WEB_SCREEN_SELECT)
    {
        const int16_t y_ap = ROW0_Y;
        const int16_t y_wifi = (int16_t)(ROW0_Y + ROW_STEP);

        poom_arduboy_set_cursor(TEXT_X, y_ap);
        (void)poom_arduboy_print(F("Poom AP"));

        poom_arduboy_set_cursor(TEXT_X, y_wifi);
        (void)poom_arduboy_print(F("Poom STA"));

        if (s_menu_cli_web_selected_mode == MENU_CLI_WEB_MODE_AP)
        {
            poom_arduboy_fill_rect(1, (int16_t)(y_ap - 1), ARDUBOY_WIDTH - 2, ROW_HILITE_H, INVERT);
        }
        else
        {
            poom_arduboy_fill_rect(1, (int16_t)(y_wifi - 1), ARDUBOY_WIDTH - 2, ROW_HILITE_H, INVERT);
        }

        poom_arduboy_set_cursor(0, 56);
        (void)poom_arduboy_print(F("A:START"));
        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }
    else
    {
        char line0[22] = {0};
        char line1[22] = {0};
        char line2[22] = {0};
        char line3[22] = {0};

        (void)snprintf(line0, sizeof(line0), "State: %.14s", s_menu_cli_web_status);

        if (s_menu_cli_web_running_mode == MENU_CLI_WEB_MODE_AP)
        {
            const char* ssid = poom_web_get_wifi_ap_ssid();
            const char* pass = poom_web_get_wifi_ap_password();
            const char* ip = poom_web_get_wifi_ap_ip();

            if ((ssid == NULL) || (ssid[0] == '\0'))
            {
                ssid = "N/A";
            }
            if ((pass == NULL) || (pass[0] == '\0'))
            {
                pass = "-";
            }
            if ((ip == NULL) || (ip[0] == '\0'))
            {
                ip = "-";
            }

            (void)snprintf(line1, sizeof(line1), "%.8s / %.8s", ssid, pass);
            (void)snprintf(line2, sizeof(line2), "URL: poom.local");
            (void)snprintf(line3, sizeof(line3), "IP: %.15s", ip);

            poom_arduboy_set_cursor(TEXT_X, 14);
            (void)poom_arduboy_print(line0);
            poom_arduboy_set_cursor(TEXT_X, 22);
            (void)poom_arduboy_print(line1);
            poom_arduboy_set_cursor(TEXT_X, 30);
            (void)poom_arduboy_print(line2);
            poom_arduboy_set_cursor(TEXT_X, 38);
            (void)poom_arduboy_print(line3);
        }
        else
        {
            poom_arduboy_set_cursor(TEXT_X, ROW0_Y);
            (void)poom_arduboy_print(line0);

            const char* ssid = NULL;
            if ((s_menu_cli_web_screen == MENU_CLI_WEB_SCREEN_STARTING) && (s_menu_cli_web_sta_ssid[0] != '\0'))
            {
                ssid = s_menu_cli_web_sta_ssid;
            }
            else
            {
                ssid = poom_web_get_wifi_sta_ssid();
            }
            if ((ssid == NULL) || (ssid[0] == '\0'))
            {
                ssid = "N/A";
            }
            (void)snprintf(line1, sizeof(line1), "WiFi: %.15s", ssid);
            poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + ROW_STEP));
            (void)poom_arduboy_print(line1);
            (void)snprintf(line2, sizeof(line2), "IP: %.15s", (s_menu_cli_web_ip[0] != '\0') ? s_menu_cli_web_ip : "-");
            poom_arduboy_set_cursor(TEXT_X, (int16_t)(ROW0_Y + 2 * ROW_STEP));
            (void)poom_arduboy_print(line2);
        }

        if (s_menu_cli_web_running_mode != MENU_CLI_WEB_MODE_AP)
        {
            poom_arduboy_set_cursor(TEXT_X, 44);
            (void)poom_arduboy_print(s_menu_cli_web_help);
        }

        poom_arduboy_set_cursor(72, 56);
        (void)poom_arduboy_print(F("B:BACK"));
    }

    poom_arduboy_display();
}

void menu_cli_web_show(void) {
    s_menu_cli_web_active = true;
    s_menu_cli_web_exit_requested = false;
    s_menu_cli_web_start_requested = false;
    s_menu_cli_web_input_dirty = true;
    s_menu_cli_web_selected_mode = MENU_CLI_WEB_MODE_AP;
    s_menu_cli_web_running_mode = MENU_CLI_WEB_MODE_AP;
    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_SELECT;
    s_menu_cli_web_ip[0] = '\0';
    s_menu_cli_web_sta_ssid[0] = '\0';
    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Select mode");
    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "A:START");

    if (!s_menu_cli_web_buttons_subscribed) {
        (void)poom_sbus_subscribe_cb("input/button", menu_cli_web_button_cb_, "menu_cli_web");
        s_menu_cli_web_buttons_subscribed = true;
    }

    (void)poom_web_set_command_cb(cli_web_command_cb_, NULL);

    if (s_menu_cli_web_task == NULL)
    {
        (void)xTaskCreate(menu_cli_web_task_,
                          "menu_cli_web",
                          MENU_CLI_WEB_TASK_STACK,
                          NULL,
                          MENU_CLI_WEB_TASK_PRIO,
                          &s_menu_cli_web_task);
    }

    menu_cli_web_request_render_();
}

/**
 * @brief Runs the internal task for this menu module.
 *
 * @param[in] arg Parameter passed to the helper.
 * @return void
 */
static void menu_cli_web_task_(void* arg)
{
    (void)arg;

    menu_cli_web_draw_();
    s_menu_cli_web_input_dirty = false;

    while (s_menu_cli_web_active)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MENU_CLI_WEB_REFRESH_MS));

        if (s_menu_cli_web_exit_requested)
        {
            menu_cli_web_exit_();
            break;
        }

        if (s_menu_cli_web_start_requested)
        {
            esp_err_t err;

            s_menu_cli_web_start_requested = false;
            s_menu_cli_web_ip[0] = '\0';

            if (s_menu_cli_web_running_mode == MENU_CLI_WEB_MODE_AP)
            {
                err = poom_web_init();
                if (err == ESP_OK)
                {
                    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_READY;
                    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Ready");
                    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Open browser");
                }
                else
                {
                    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_ERROR;
                    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Error");
                    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Init failed");
                }
            }
            else
            {
                (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Connecting...");
                (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Waiting IP...");
                s_menu_cli_web_sta_ssid[0] = '\0';
                (void)poom_secrets_init();
                size_t ssid_len = sizeof(s_menu_cli_web_sta_ssid);
                (void)poom_secrets_get_wifi_ssid(s_menu_cli_web_sta_ssid, &ssid_len);
                menu_cli_web_draw_();

                err = poom_web_init_sta_saved(s_menu_cli_web_ip, sizeof(s_menu_cli_web_ip));
                if (err == ESP_OK)
                {
                    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_READY;
                    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Ready");
                    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Use IP / poom.local");
                }
                else if (err == ESP_ERR_NOT_FOUND)
                {
                    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_ERROR;
                    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "No WiFi saved");
                    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Run WiFi Scan");
                }
                else
                {
                    s_menu_cli_web_screen = MENU_CLI_WEB_SCREEN_ERROR;
                    (void)snprintf(s_menu_cli_web_status, sizeof(s_menu_cli_web_status), "Conn failed");
                    (void)snprintf(s_menu_cli_web_help, sizeof(s_menu_cli_web_help), "Try again");
                }
            }

            printf("CLI WEB start (%s): %s\n",
                   (s_menu_cli_web_running_mode == MENU_CLI_WEB_MODE_AP) ? "AP" : "STA",
                   esp_err_to_name(err));
            s_menu_cli_web_input_dirty = true;
        }

        if (s_menu_cli_web_input_dirty)
        {
            menu_cli_web_draw_();
            s_menu_cli_web_input_dirty = false;
        }
    }

    s_menu_cli_web_task = NULL;
    vTaskDelete(NULL);
}
