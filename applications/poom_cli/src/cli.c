/* Console example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "cli.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "linenoise/linenoise.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"


#if POOM_CLI_LOG_ENABLED
static const char* POOM_CLI_TAG = "poom_console";

#define POOM_CLI_PRINTF_E(fmt, ...)                                                  \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_CLI_TAG, __func__, __LINE__,           \
           ##__VA_ARGS__)

#define POOM_CLI_PRINTF_W(fmt, ...)                                                  \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_CLI_TAG, __func__, __LINE__,           \
           ##__VA_ARGS__)

#define POOM_CLI_PRINTF_I(fmt, ...)                                                  \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_CLI_TAG, __func__, __LINE__,           \
           ##__VA_ARGS__)

#if POOM_CLI_DEBUG_LOG_ENABLED
#define POOM_CLI_PRINTF_D(fmt, ...)                                                  \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_CLI_TAG, __func__, __LINE__,           \
           ##__VA_ARGS__)
#else
#define POOM_CLI_PRINTF_D(...)                                                        \
    do                                                                                \
    {                                                                                 \
    } while(0)
#endif
#else
#define POOM_CLI_PRINTF_E(...)                                                        \
    do                                                                                \
    {                                                                                 \
    } while(0)
#define POOM_CLI_PRINTF_W(...)                                                        \
    do                                                                                \
    {                                                                                 \
    } while(0)
#define POOM_CLI_PRINTF_I(...)                                                        \
    do                                                                                \
    {                                                                                 \
    } while(0)
#define POOM_CLI_PRINTF_D(...)                                                        \
    do                                                                                \
    {                                                                                 \
    } while(0)
#endif

#define PROMPT_STR "poom"

static ctrl_c_callback_t ctrl_c_callback = NULL;
static bool console_paused = false;
static bool console_active = false;
static esp_console_repl_t *s_console_repl = NULL;
static void (*s_registrar_cmds_fn)(void) = NULL;

void poom_console_register_ctrl_c_handler(ctrl_c_callback_t callback) {
  ctrl_c_callback = callback;
}

/**
 * @brief Internal helper for `unregister_ctrl_c_handler`.
 *
 * @return void
 */
static void unregister_ctrl_c_handler() {
  ctrl_c_callback = NULL;
}

/**
 * @brief Internal helper for `register_commands`.
 *
 * @return void
 */
static void register_commands() {
  esp_console_register_help_command();
}


/**
 * @brief Internal helper for `initialize_nvs`.
 *
 * @return void
 */
static void initialize_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

/**
 * @brief Internal helper for `initialize_console`.
 *
 * @return void
 */
static void initialize_console(void) {
  if (s_console_repl != NULL) {
    return;
  }

  fflush(stdout);
  fsync(fileno(stdout));

  setvbuf(stdin, NULL, _IONBF, 0);

  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "poom>";

  esp_console_dev_usb_serial_jtag_config_t usbjtag_config =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config,
                                                       &repl_config, &s_console_repl));
}

/**
 * @brief Internal helper for `deinitialize_console`.
 *
 * @return void
 */
static void deinitialize_console(void)
{
    if (s_console_repl == NULL)
    {
        return;
    }

    (void)s_console_repl->del(s_console_repl);
    s_console_repl = NULL;
}

/**
 * @brief Internal helper for `poom_console_banner`.
 *
 * @return void
 */
static void poom_console_banner(void)
{
    printf(
        "\n"
        "██████╗  ██████╗  ██████╗ ███╗   ███╗\n"
        "██╔══██╗██╔═══██╗██╔═══██╗████╗ ████║\n"
        "██████╔╝██║   ██║██║   ██║██╔████╔██║\n"
        "██╔═══╝ ██║   ██║██║   ██║██║╚██╔╝██║\n"
        "██║     ╚██████╔╝╚██████╔╝██║ ╚═╝ ██║\n"
        "╚═╝      ╚═════╝  ╚═════╝ ╚═╝     ╚═╝\n"
        "\n"
        "PENTEST · PLAY · CREATE\n"
        "\n");
}

/**
 * @brief Internal helper for `poom_console_default`.
 *
 * @return void
 */
static void poom_console_default() {
  const char *prompt = LOG_COLOR_I PROMPT_STR "> " LOG_RESET_COLOR;
  poom_console_banner();

  printf(
        "Welcome to POOM interactive console\n"
        "Type 'help' to list available commands\n"
        "Use ↑ ↓ for command history\n"
        "Press TAB for auto-completion\n"
        "Ctrl+C to cancel running command\n"
        "\n");

  int probe_status = linenoiseProbe();
  if (probe_status) { /* zero indicates success */
      printf(
            "⚠ Limited terminal detected\n"
            "Line editing and history disabled\n"
            "Tip: use minicom, screen, or Putty\n"
            "\n");
    linenoiseSetDumbMode(1);
#if CONFIG_LOG_COLORS
    prompt = PROMPT_STR "> ";
#endif  // CONFIG_LOG_COLORS
  }

restart:
  while (true) {
    if (console_paused) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    char *line = linenoise(prompt);
    if (line == NULL) { /* Break on EOF or error */
      break;
    }

    if (strlen(line) > 0) {
      linenoiseHistoryAdd(line);
#if CONFIG_STORE_HISTORY
      linenoiseHistorySave(HISTORY_PATH);
#endif
    }

    int ret;
    esp_err_t err = esp_console_run(line, &ret);
    if (err == ESP_ERR_NOT_FOUND) {
      printf("Command not found\n");
    } else if (err == ESP_ERR_INVALID_ARG) {
    } else if (err == ESP_OK && ret != ESP_OK) {
      printf("Command returned non-zero  err code: 0x%x (%s)\n", ret,
             esp_err_to_name(ret));
    } else if (err != ESP_OK) {
      printf("Console error: %s\n", esp_err_to_name(err));
    }

    linenoiseFree(line);
  }

  if (ctrl_c_callback) {
    ctrl_c_callback();
    unregister_ctrl_c_handler();
  }
  goto restart;

  POOM_CLI_PRINTF_E("Finished console");
  esp_console_deinit();
}

//static void register_commands() {
//  /* Register commands */
//  esp_console_register_help_command();
//}

void poom_console_begin(void (*registrar_cmds_fn)(void)) {
  esp_log_level_set("poom_console", ESP_LOG_NONE);
  console_active = true;

    s_registrar_cmds_fn = registrar_cmds_fn;
    initialize_nvs();
    initialize_console();
    register_commands();
    if (registrar_cmds_fn) {
        registrar_cmds_fn();
    }
    poom_console_default();
}

void poom_console_pause(void) {
  if (!console_paused) {
    console_paused = true;
    POOM_CLI_PRINTF_I("Console paused");
    printf("\n[Console paused - UART Bridge mode active]\n");
    fflush(stdout);
    deinitialize_console();
  }
}

void poom_console_resume(void) {
  if (console_paused) {
    initialize_console();
    register_commands();
    if (s_registrar_cmds_fn) {
        s_registrar_cmds_fn();
    }
    console_paused = false;
    POOM_CLI_PRINTF_I("Console resumed");
    printf("\n[Console resumed]\n");
    fflush(stdout);
  }
}

bool poom_console_is_paused(void) {
  return console_paused;
}

bool poom_console_is_active(void) {
  return console_active;
}
