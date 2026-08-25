#include "poom_menu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "Arduboy2.h"
#include "Sprites.h"

#include "button_driver.h"
#include "input_events.h"
#include "poom_menu_assets.h"
#include "poom_sbus.h"


#include "buzzer.h"
#include "poom_ble_keyboard.h"
#include "poom_button_sound.h"
#include "poom_led_rainbow.h"

#include "menu_air_ble.h"
#include "menu_ble_control.h"
#include "menu_ble_scan.h"
#include "menu_ble_spam.h"
#include "menu_captive.h"
#include "menu_cli_nfc.h"
#if CONFIG_ZB_ENABLED
#include "menu_cli_zigbee.h"
#endif
#if CONFIG_OPENTHREAD_ENABLED && CONFIG_OPENTHREAD_CLI
#include "menu_cli_ot.h"
#endif
#include "menu_cli_web.h"
#include "menu_control_music.h"
#include "menu_deauth.h"
#include "menu_deauth_detector.h"
#include "menu_dfu.h"
#include "menu_edge_impulse.h"
#include "menu_http_load_test.h"
#include "menu_fw_info.h"
#include "menu_i2c_scan.h"
#include "menu_ir_universal.h"
#include "menu_imu_monitor.h"
#include "menu_ws2812_color.h"
#include "menu_lua.h"
#include "menu_karma.h"
#include "menu_midi.h"
#include "menu_midi_harmony.h"
#include "menu_nfc.h"
#include "menu_picopass.h"
#include "menu_nfc_tuning.h"
#include "menu_plot.h"
#include "menu_poom_pcap.h"
#include "menu_poom_droneid.h"
#include "menu_poom_drone_scan.h"
#include "menu_poom_drone_emul.h"
#include "menu_poom_wifi_arp.h"
#include "menu_poom_wifi_scan.h"
#include "menu_scanner_core.h"
#include "menu_sd_browser.h"
#include "menu_sniffer_rt.h"
#include "menu_sniffer_device.h"
#include "menu_ssid_spam.h"
#include "menu_tone.h"
#include "menu_tracker.h"
#include "poom_breakout.h"
#include "poom_i2c_as5600.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"

#ifndef BUTTON_SINGLE_CLICK
#define BUTTON_SINGLE_CLICK 4
#endif

#ifndef POOM_MENU_ANIM_SHIFT
#define POOM_MENU_ANIM_SHIFT 0
#endif

#ifndef POOM_MENU_FLASH_FRAMES
#define POOM_MENU_FLASH_FRAMES 6
#endif

#ifndef POOM_MENU_SUB_VISIBLE_ROWS
#define POOM_MENU_SUB_VISIBLE_ROWS 4
#endif

#ifndef POOM_MENU_SUB_LIST_Y0
#define POOM_MENU_SUB_LIST_Y0 16
#endif

#ifndef POOM_MENU_SUB_ROW_STEP
#define POOM_MENU_SUB_ROW_STEP 10
#endif

#ifndef POOM_MENU_SUB_HILITE_H
#define POOM_MENU_SUB_HILITE_H 9
#endif

// Animation timing note: the OLED flush is I2C-bound (default bus is 100kHz),
// so the effective FPS is limited even if we loop faster.
static const TickType_t k_yield_delay = 1;

extern const uint8_t poom_arduboy_font5x7[];

typedef enum
{
    POOM_MENU_SCREEN_MAIN = 0,
    POOM_MENU_SCREEN_SUBMENU,
} poom_menu_screen_t;

typedef void (*poom_menu_action_fn)(void);

typedef struct
{
    const char *label;
    poom_menu_action_fn on_select;
} poom_menu_item_t;

typedef struct
{
    const char *header;
    const poom_menu_item_t *items;
    uint8_t count;
} poom_menu_submenu_t;

static TaskHandle_t s_menu_task = NULL;
static bool s_detached = false;
static bool s_input_subscribed = false;

static volatile poom_menu_screen_t s_screen = POOM_MENU_SCREEN_MAIN;

static volatile uint8_t s_arrow_tick = 0;
static volatile uint8_t s_mode_index = 0;
static volatile uint8_t s_flash_timer = 0;

static volatile uint8_t s_sub_mode = 0;
static volatile uint8_t s_sub_selected = 0;
static volatile uint8_t s_sub_scroll = 0;

static void on_button_any_(const poom_sbus_msg_t *msg, void *user);
static void on_menu_resume_(const poom_sbus_msg_t *msg, void *user);

static char s_btn_sound_label[20] = "BTN SOUND OFF";

/**
 * @brief Returns the display label for the current state.
 *
 * @return void
 */
static void update_btn_sound_label_(void)
{
    const bool enabled = poom_button_sound_get_enabled_setting();
    (void)snprintf(s_btn_sound_label, sizeof(s_btn_sound_label), "BTN SOUND %s", enabled ? "ON" : "OFF");
}

/**
 * @brief Internal helper for `detach_menu`.
 *
 * @return void
 */
static void detach_menu_(void)
{
    if (s_detached)
    {
        return;
    }
    s_detached = true;

    if (s_input_subscribed)
    {
        (void)poom_sbus_unsubscribe_cb("input/button", on_button_any_, NULL);
        s_input_subscribed = false;
    }

    poom_button_sound_suspend();
}

/**
 * @brief Internal helper for `on_menu_resume`.
 *
 * @param[in] msg Parameter passed to the function.
 * @param[in] user Parameter passed to the function.
 * @return void
 */
static void on_menu_resume_(const poom_sbus_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;

    if (!s_detached)
    {
        return;
    }

    s_detached = false;
    if (!s_input_subscribed)
    {
        (void)poom_sbus_subscribe_cb("input/button", on_button_any_, NULL);
        s_input_subscribed = true;
    }

    poom_button_sound_resume();
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] x Parameter passed to the function.
 * @param[in] y Parameter passed to the function.
 * @param[in] c Parameter passed to the function.
 * @param[in] color Parameter passed to the function.
 * @param[in] bg Parameter passed to the function.
 * @return void
 */
static void draw_char_(int16_t x, int16_t y, char c, uint8_t color, uint8_t bg)
{
    if (c < 0x20 || c > 0x7E)
    {
        c = '?';
    }

    const uint16_t base = (uint16_t)(c - 0x20) * 5u;

    for (int16_t col = 0; col < 5; col++)
    {
        const uint8_t line = poom_arduboy_font5x7[base + (uint16_t)col];
        for (int16_t row = 0; row < 7; row++)
        {
            const bool on = ((line & (uint8_t)(1u << row)) != 0u);
            poom_arduboy_draw_pixel((int16_t)(x + col), (int16_t)(y + row), on ? color : bg);
        }
    }

    for (int16_t row = 0; row < 7; row++)
    {
        poom_arduboy_draw_pixel((int16_t)(x + 5), (int16_t)(y + row), bg);
    }
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] x Parameter passed to the function.
 * @param[in] y Parameter passed to the function.
 * @param[in] text Parameter passed to the function.
 * @param[in] color Parameter passed to the function.
 * @param[in] bg Parameter passed to the function.
 * @return void
 */
static void draw_text_(int16_t x, int16_t y, const char *text, uint8_t color, uint8_t bg)
{
    if (text == NULL)
    {
        return;
    }

    for (const char *p = text; *p; ++p)
    {
        draw_char_(x, y, *p, color, bg);
        x = (int16_t)(x + 6);
    }
}

/**
 * @brief Draws the module header.
 *
 * @param[in] title Parameter passed to the function.
 * @return void
 */
static void draw_header_(const char *title)
{
    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, 11, WHITE);
    draw_text_(2, 2, title ? title : "MENU", BLACK, WHITE);
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] left Parameter passed to the function.
 * @param[in] right Parameter passed to the function.
 * @return void
 */
static void draw_footer_(const char *left, const char *right)
{
    poom_arduboy_fill_rect(0, 54, ARDUBOY_WIDTH, 10, BLACK);
    if (left)
    {
        draw_text_(0, 56, left, WHITE, BLACK);
    }
    if (right)
    {
        draw_text_(80, 56, right, WHITE, BLACK);
    }
}

// ==============================================================
// Actions (migrated from the legacy app-pack submenu wrappers)
// ==============================================================

__attribute__((weak)) void poom_menu_launch_app(uint8_t mode, uint8_t app)
{
    poom_menu_launch_msg_t msg = {.mode = mode, .app = app};
    (void)poom_sbus_publish(POOM_MENU_LAUNCH_TOPIC, &msg, sizeof(msg), 0);
}


/**
 * @brief Internal helper for `action_deauth`.
 *
 * @return void
 */
static void action_deauth_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_deauth();
}

/**
 * @brief Internal helper for `action_deauth_detector`.
 *
 * @return void
 */
static void action_deauth_detector_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_deauth_detector();
}

/**
 * @brief Internal helper for `action_karma`.
 *
 * @return void
 */
static void action_karma_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_karma_init();
}

/**
 * @brief Internal helper for `action_spam_wifi`.
 *
 * @return void
 */
static void action_spam_wifi_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_ssid_spam_init();
}

/**
 * @brief Internal helper for `action_captive`.
 *
 * @return void
 */
static void action_captive_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_captive_display();
}

/**
 * @brief Internal helper for `action_ble_spam`.
 *
 * @return void
 */
static void action_ble_spam_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_ble_spam_display();
}

/**
 * @brief Internal helper for `action_tracker`.
 *
 * @return void
 */
static void action_tracker_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_tracker_menu();
}

/**
 * @brief Internal helper for `action_pcap_snf`.
 *
 * @return void
 */
static void action_pcap_snf_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_pcap_show();
}

/**
 * @brief Internal helper for `action_sniffer_rt`.
 *
 * @return void
 */
static void action_sniffer_rt_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_sniffer_rt_show();
}

/**
 * @brief Internal helper for `action_arp`.
 *
 * @return void
 */
static void action_arp_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_wifi_arp_show();
}

/**
 * @brief Internal helper for `action_cli_nfc`.
 *
 * @return void
 */
static void action_cli_nfc_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_cli_nfc();
}

#if CONFIG_OPENTHREAD_ENABLED && CONFIG_OPENTHREAD_CLI

/**
 * @brief Internal helper for `action_cli_ot`.
 *
 * @return void
 */
static void action_cli_ot_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_cli_ot();
}
#endif

#if CONFIG_ZB_ENABLED

/**
 * @brief Internal helper for `action_cli_zigbee`.
 *
 * @return void
 */
static void action_cli_zigbee_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_cli_zigbee();
}
#endif

/**
 * @brief Internal helper for `action_cli_web`.
 *
 * @return void
 */
static void action_cli_web_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_cli_web_show();
}

/**
 * @brief Loads internal data used by this module.
 *
 * @return void
 */
static void action_http_load_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_http_load_test_show();
}

/**
 * @brief Internal helper for `action_sniffer`.
 *
 * @return void
 */
static void action_sniffer_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_sniffer_device_show();
}

/**
 * @brief Internal helper for `action_midi`.
 *
 * @return void
 */
static void action_midi_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_midi_init();
}

/**
 * @brief Internal helper for `action_midi_harmony`.
 *
 * @return void
 */
static void action_midi_harmony_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_midi_harmony_init();
}

/**
 * @brief Internal helper for `action_tone`.
 *
 * @return void
 */
static void action_tone_(void)
{
    detach_menu_();
    buzzer_init(PIN_NUM_BUZZER);
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_buzzer_menu();
}

/**
 * @brief Toggles the current runtime state.
 *
 * @return void
 */
static void action_btn_sound_toggle_(void)
{
    const bool cur = poom_button_sound_get_enabled_setting();
    (void)poom_button_sound_set_enabled_setting(!cur);
    update_btn_sound_label_();
}

/**
 * @brief Internal helper for `action_control_music`.
 *
 * @return void
 */
static void action_control_music_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_control_init();
}

/**
 * @brief Internal helper for `action_nfc`.
 *
 * @return void
 */
static void action_nfc_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_nfc_show();
}

static void action_picopass_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_picopass_show();
}

/**
 * @brief Internal helper for `action_wii`.
 *
 * @return void
 */
static void action_wii_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_air_ble_display();
}

/**
 * @brief Internal helper for `action_breakout`.
 *
 * @return void
 */
static void action_breakout_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_breakout_menu();
}

/**
 * @brief Internal helper for `action_tiny_control`.
 *
 * @return void
 */
static void action_tiny_control_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    poom_ble_keyboard_set_keyboard_mode(true);
    menu_control_display();
    poom_ble_keyboard_start();
}

/**
 * @brief Internal helper for `action_plot`.
 *
 * @return void
 */
static void action_plot_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_plot_init();
}

/**
 * @brief Internal helper for `action_ble_scan`.
 *
 * @return void
 */
static void action_ble_scan_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_ble_scan();
}

/**
 * @brief Internal helper for `action_edge_ai`.
 *
 * @return void
 */
static void action_edge_ai_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_edge_impulse_show();
}

/**
 * @brief Internal helper for `action_files`.
 *
 * @return void
 */
static void action_files_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_sd_browser_menu();
}

/**
 * @brief Internal helper for `action_ir_universal`.
 *
 * @return void
 */
static void action_ir_universal_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_ir_universal_show();
}

/**
 * @brief Internal helper for `action_dfu`.
 *
 * @return void
 */
static void action_dfu_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    dfu_start_task();
}

/**
 * @brief Internal helper for `action_fw_info`.
 *
 * @return void
 */
static void action_fw_info_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_fw_info_show();
}

/**
 * @brief Internal helper for `action_wifi_scan`.
 *
 * @return void
 */
static void action_wifi_scan_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_wifi_scan_show();
}

/**
 * @brief Internal helper for `action_nfc_tune`.
 *
 * @return void
 */
static void action_nfc_tune_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_nfc_tuning_show();
}

/**
 * @brief Internal helper for `action_scanner_core`.
 *
 * @return void
 */
static void action_scanner_core_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_scanner_core_show();
}

/**
 * @brief Internal helper for `action_imu_monitor`.
 *
 * @return void
 */
static void action_imu_monitor_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_imu_monitor_show();
}

/**
 * @brief Internal helper for `action_led_rgb`.
 *
 * @return void
 */
static void action_led_rgb_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_ws2812_color_show();
}

/**
 * @brief Internal helper for `action_reboot`.
 *
 * @return void
 */
static void action_reboot_(void)
{
    detach_menu_();

    poom_arduboy_clear();
    draw_text_(52, 28, "BOOT", WHITE, BLACK);
    poom_arduboy_display();

    vTaskDelay(pdMS_TO_TICKS(250U));
    esp_restart();
}

/**
 * @brief Internal helper for `action_droneid`.
 *
 * @return void
 */
static void action_droneid_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_droneid_show();
}

/**
 * @brief Internal helper for `action_drone_scan`.
 *
 * @return void
 */
static void action_drone_scan_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_drone_scan_show();
}

/**
 * @brief Internal helper for `action_drone_emul`.
 *
 * @return void
 */
static void action_drone_emul_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_poom_drone_emul_show();
}

/**
 * @brief Internal helper for `action_i2c_scan`.
 *
 * @return void
 */
static void action_i2c_scan_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_i2c_scan_show();
}

/**
 * @brief Internal helper for `action_lua`.
 *
 * @return void
 */
static void action_lua_(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    menu_lua_show();
}

static void action_poom_i2c_as5600(void)
{
    detach_menu_();
    vTaskDelay(pdMS_TO_TICKS(180U));
    app_poom_i2c_as5600_menu();
}

// ==============================================================
// Menu model (mirrors the app-pack categories)
// ==============================================================

static const poom_menu_item_t s_apps_beast[] = {
    {"CLI", action_cli_nfc_},
#if CONFIG_OPENTHREAD_ENABLED && CONFIG_OPENTHREAD_CLI
    {"OPENTHREAD CLI", action_cli_ot_},
#endif
    {"DEAUTH", action_deauth_},
    {"DEAUTH DET", action_deauth_detector_},
    {"KARMA", action_karma_},
    {"SPAM WIFI", action_spam_wifi_},
    {"SPAM BLE", action_ble_spam_},
    {"CAPTIVE PORTAL", action_captive_},
    {"TRACKER", action_tracker_},
    {"SNIFFER", action_pcap_snf_},
    {"SNNIFER RT", action_sniffer_rt_},
    {"SCAN CHANNELS", action_scanner_core_},
    {"SCAN NET", action_arp_},

#if CONFIG_ZB_ENABLED
    {"CLI ZIGBEE", action_cli_zigbee_},
#endif
    {"HTTP LOAD", action_http_load_},
    {"PROBE REQ", action_sniffer_},
};

static const poom_menu_item_t s_apps_zen[] = {
    {"MIDI", action_midi_},
//    {"HARMONY", action_midi_harmony_},
//    {"TONE", action_tone_},
    {"CONTROL", action_control_music_},
    {"NFC", action_nfc_},
    {"PICOPASS", action_picopass_},
    {"IR UNIV", action_ir_universal_},
    {"POOM WEB", action_cli_web_},
};

static const poom_menu_item_t s_apps_gamer[] = {
    //{"BREAKOUT", action_breakout_},
    {"TINY CONTROL", action_tiny_control_},
    {"WII", action_wii_},
};

static const poom_menu_item_t s_apps_maker[] = {
    {"PLOT", action_plot_},
    //{"BLE SCAN", action_ble_scan_},
    {"DRONE SCAN", action_drone_scan_},
    {"DRONE EMUL", action_drone_emul_},
    //{"DRONE ID", action_droneid_},
    {"I2C", action_i2c_scan_},
    {"LUA", action_lua_},
    {"EDGE AI", action_edge_ai_},
    {"AS5600 I2C", action_poom_i2c_as5600},
};

static const poom_menu_item_t s_apps_settings[] = {
    {s_btn_sound_label, action_btn_sound_toggle_},
    {"DFU", action_dfu_},
    {"FW INFO", action_fw_info_},
//    {"IMU", action_imu_monitor_},
//    {"LED RGB", action_led_rgb_},
    //{"BOOT", action_reboot_},
    {"WI-FI", action_wifi_scan_},
    //{"NFC TUNE", action_nfc_tune_},
    {"FILES", action_files_},
};

static const poom_menu_submenu_t s_submenus[] = {
    {.header = "THE BEAST", .items = s_apps_beast, .count = (uint8_t)(sizeof(s_apps_beast) / sizeof(s_apps_beast[0]))},
    {.header = "THE ZEN", .items = s_apps_zen, .count = (uint8_t)(sizeof(s_apps_zen) / sizeof(s_apps_zen[0]))},
    {.header = "THE GAMER", .items = s_apps_gamer, .count = (uint8_t)(sizeof(s_apps_gamer) / sizeof(s_apps_gamer[0]))},
    {.header = "THE MAKER", .items = s_apps_maker, .count = (uint8_t)(sizeof(s_apps_maker) / sizeof(s_apps_maker[0]))},
    {.header = "SETTINGS", .items = s_apps_settings, .count = (uint8_t)(sizeof(s_apps_settings) / sizeof(s_apps_settings[0]))},
};

/**
 * @brief Internal helper for `submenu_for_mode`.
 *
 * @param[in] mode Parameter passed to the function.
 * @return const poom_menu_submenu_t *
 */
static const poom_menu_submenu_t *submenu_for_mode_(uint8_t mode)
{
    const uint8_t count = poom_menu_mode_count();
    if (count == 0u)
    {
        return &s_submenus[0];
    }
    return &s_submenus[mode % count];
}

// ==============================================================
// Renderers
// ==============================================================

/**
 * @brief Draws the current module state.
 *
 * @param[in] anim_tick Parameter passed to the function.
 * @return void
 */
static void draw_bottom_mini_arrows_main_(uint8_t anim_tick)
{
    const int16_t nudge = (int16_t)(anim_tick & 1u);
    const int16_t y = 56;

    const int16_t lx = (int16_t)(2 - nudge);
    poom_arduboy_fill_triangle((int16_t)(lx + 4), (int16_t)(y + 3), lx, y, lx, (int16_t)(y + 6), WHITE);

    const int16_t rx = (int16_t)(118 + nudge);
    poom_arduboy_fill_triangle((int16_t)(rx + 4), (int16_t)(y + 3), rx, y, rx, (int16_t)(y + 6), WHITE);
}

/**
 * @brief Draws the current module state.
 *
 * @param[in] count Parameter passed to the function.
 * @param[in] current_index Parameter passed to the function.
 * @return void
 */
static void draw_bottom_pager_main_(uint8_t count, uint8_t current_index)
{
    const int16_t spacing = 9;
    const int16_t span = (count > 0u) ? (int16_t)((count - 1u) * (uint8_t)spacing) : 0;
    const int16_t start_x = (int16_t)(28 - (span / 2));
    const int16_t y = 60;

    for (uint8_t i = 0; i < count; ++i)
    {
        const int16_t x = (int16_t)(start_x + (int16_t)spacing * (int16_t)i);
        if (i == current_index)
        {
            poom_arduboy_fill_rect(x, (int16_t)(y - 1), 3, 3, WHITE);
        }
        else
        {
            poom_arduboy_draw_pixel((int16_t)(x + 1), y, WHITE);
        }
    }
}

/**
 * @brief Renders the current module state.
 *
 * @return void
 */
static void render_main_(void)
{
    const int16_t top_y = 8;
    const int16_t bottom_y = 55;
    const int16_t split_x = 63;

    const uint8_t anim_tick = (uint8_t)(s_arrow_tick >> POOM_MENU_ANIM_SHIFT);

    const uint8_t *title = poom_menu_mode_title(s_mode_index);
    const uint8_t *icon = poom_menu_mode_icon(s_mode_index);
    static const int8_t icon_hover_offset[8] = {0, 1, 2, 1, 0, 1, 2, 1};
    const int16_t icon_y = (int16_t)icon_hover_offset[anim_tick & 0x07u];

    poom_arduboy_clear();

    poom_arduboy_fill_rect(0, (int16_t)(top_y + 1), split_x, (int16_t)(bottom_y - top_y - 1), BLACK);
    poom_arduboy_fill_rect((int16_t)(split_x + 1),
                           (int16_t)(top_y + 1),
                           (int16_t)(ARDUBOY_WIDTH - split_x - 2),
                           (int16_t)(bottom_y - top_y - 1),
                           BLACK);

    Sprites_drawOverwrite(0, (int16_t)(top_y + 1), title, 0);

    poom_arduboy_draw_fast_hline(0, top_y, ARDUBOY_WIDTH, WHITE);
    poom_arduboy_draw_fast_hline(0, bottom_y, ARDUBOY_WIDTH, WHITE);
    poom_arduboy_draw_fast_vline(0, top_y, (int16_t)(bottom_y - top_y + 1), WHITE);
    poom_arduboy_draw_fast_vline(split_x, top_y, (int16_t)(bottom_y - top_y + 1), WHITE);
    poom_arduboy_draw_fast_vline((int16_t)(ARDUBOY_WIDTH - 1), top_y, (int16_t)(bottom_y - top_y + 1), WHITE);

    poom_arduboy_draw_rect((int16_t)(split_x + 1),
                           (int16_t)(top_y + 1),
                           (int16_t)(ARDUBOY_WIDTH - split_x - 2),
                           (int16_t)(bottom_y - top_y - 1),
                           WHITE);

    poom_arduboy_fill_rect(0, 0, ARDUBOY_WIDTH, top_y, BLACK);
    poom_arduboy_set_cursor(2, 1);
    (void)poom_arduboy_print("<<");

    poom_arduboy_fill_rect(0, (int16_t)(bottom_y + 1), ARDUBOY_WIDTH,
                           (int16_t)(ARDUBOY_HEIGHT - (bottom_y + 1)), BLACK);
    draw_bottom_pager_main_(poom_menu_mode_count(), s_mode_index);
    draw_bottom_mini_arrows_main_(anim_tick);
    poom_arduboy_set_cursor(84, 57);
    (void)poom_arduboy_print("SELECT");

    Sprites_drawOverwrite(split_x, icon_y, icon, 0);

    poom_arduboy_set_cursor(84, 57);
    (void)poom_arduboy_print("SELECT");

    poom_arduboy_display();
}

/**
 * @brief Renders the current module state.
 *
 * @return void
 */
static void render_submenu_(void)
{
    const uint8_t anim_tick = (uint8_t)(s_arrow_tick >> POOM_MENU_ANIM_SHIFT);
    const bool flash_sel = (s_flash_timer > 0u) && ((anim_tick & 1u) != 0u);

    const poom_menu_submenu_t *sub = submenu_for_mode_(s_sub_mode);
    const uint8_t total = sub->count;

    uint8_t selected = s_sub_selected;
    uint8_t scroll = s_sub_scroll;

    if (total == 0u)
    {
        selected = 0;
        scroll = 0;
    }
    else
    {
        if (selected >= total)
        {
            selected = (uint8_t)(total - 1u);
        }

        const uint8_t visible = POOM_MENU_SUB_VISIBLE_ROWS;
        if (selected < scroll)
        {
            scroll = selected;
        }
        else if (selected >= (uint8_t)(scroll + visible))
        {
            scroll = (uint8_t)(selected - (visible - 1u));
        }

        const uint8_t max_scroll = (total > visible) ? (uint8_t)(total - visible) : 0u;
        if (scroll > max_scroll)
        {
            scroll = max_scroll;
        }
    }

    s_sub_selected = selected;
    s_sub_scroll = scroll;

    poom_arduboy_clear();
    draw_header_(sub->header);

    for (uint8_t row = 0; row < POOM_MENU_SUB_VISIBLE_ROWS; ++row)
    {
        const uint8_t idx = (uint8_t)(scroll + row);
        if (idx >= total)
        {
            break;
        }

        const int16_t y = (int16_t)(POOM_MENU_SUB_LIST_Y0 + (int16_t)row * POOM_MENU_SUB_ROW_STEP);
        const bool sel = (idx == selected);

        const bool pulse_off = sel && flash_sel;
        if (sel && !pulse_off)
        {
            poom_arduboy_fill_rect(0, (int16_t)(y - 1), 128, POOM_MENU_SUB_HILITE_H, WHITE);
        }

        const uint8_t fg = (sel && !pulse_off) ? BLACK : WHITE;
        const uint8_t bg = (sel && !pulse_off) ? WHITE : BLACK;

        draw_text_(2, y, sub->items[idx].label, fg, bg);
    }

    draw_footer_("A SEL", "B BACK");
    poom_arduboy_display();
}

/**
 * @brief Internal helper for `enter_submenu`.
 *
 * @return void
 */
static void enter_submenu_(void)
{
    s_sub_mode = s_mode_index;
    s_sub_selected = 0;
    s_sub_scroll = 0;
    s_flash_timer = 0;
    s_screen = POOM_MENU_SCREEN_SUBMENU;
}

/**
 * @brief Internal helper for `back_to_main`.
 *
 * @return void
 */
static void back_to_main_(void)
{
    s_screen = POOM_MENU_SCREEN_MAIN;
    s_flash_timer = 0;
}

/**
 * @brief Internal helper for `wrap_index`.
 *
 * @param[in] current Parameter passed to the function.
 * @param[in] delta Parameter passed to the function.
 * @param[in] total Parameter passed to the function.
 * @return uint8_t
 */
static uint8_t wrap_index_(uint8_t current, int delta, uint8_t total)
{
    int next;

    if (total == 0u)
    {
        return 0u;
    }

    next = ((int)current + delta) % (int)total;
    if (next < 0)
    {
        next += (int)total;
    }

    return (uint8_t)next;
}

/**
 * @brief Internal helper for `main_move`.
 *
 * @param[in] delta Parameter passed to the function.
 * @return void
 */
static void main_move_(int delta)
{
    const uint8_t count = poom_menu_mode_count();

    if (count == 0u)
    {
        return;
    }

    s_mode_index = wrap_index_(s_mode_index, delta, count);
}

/**
 * @brief Internal helper for `submenu_move`.
 *
 * @param[in] delta Parameter passed to the function.
 * @return void
 */
static void submenu_move_(int delta)
{
    const poom_menu_submenu_t *sub = submenu_for_mode_(s_sub_mode);
    const uint8_t total = sub->count;
    if (total == 0u)
    {
        return;
    }

    s_sub_selected = wrap_index_(s_sub_selected, delta, total);
}

/**
 * @brief Handles the current module action.
 *
 * @return void
 */
static void submenu_select_(void)
{
    const poom_menu_submenu_t *sub = submenu_for_mode_(s_sub_mode);
    if (sub->count == 0u)
    {
        return;
    }

    const uint8_t idx = s_sub_selected;
    if (idx >= sub->count)
    {
        return;
    }

    poom_menu_launch_app(s_sub_mode, idx);

    poom_menu_action_fn fn = sub->items[idx].on_select;
    if (fn != NULL)
    {
        fn();
    }

    s_flash_timer = POOM_MENU_FLASH_FRAMES;
}

/**
 * @brief Handles button events for this module.
 *
 * @param[in] msg Parameter passed to the function.
 * @param[in] user Parameter passed to the function.
 * @return void
 */
static void on_button_any_(const poom_sbus_msg_t *msg, void *user)
{
    (void)user;
    if (msg == NULL || msg->len < sizeof(button_event_msg_t))
    {
        return;
    }

    button_event_msg_t ev;
    memcpy(&ev, msg->data, sizeof(ev));

    if (ev.event != BUTTON_SINGLE_CLICK)
    {
        return;
    }

    if (s_screen == POOM_MENU_SCREEN_MAIN)
    {
        const uint8_t count = poom_menu_mode_count();
        if (count == 0u)
        {
            return;
        }

        if (ev.button == BUTTON_RIGHT)
        {
            main_move_(+1);
        }
        else if (ev.button == BUTTON_LEFT)
        {
            main_move_(-1);
        }
        else if (ev.button == BUTTON_A)
        {
            enter_submenu_();
        }

        return;
    }

    if (ev.button == BUTTON_B)
    {
        back_to_main_();
        return;
    }

    if (ev.button == BUTTON_UP)
    {
        submenu_move_(-1);
    }
    else if (ev.button == BUTTON_DOWN)
    {
        submenu_move_(+1);
    }
    else if (ev.button == BUTTON_A)
    {
        submenu_select_();
    }
}

// ==============================================================
// Task
// ==============================================================

/**
 * @brief Runs the internal task for this module.
 *
 * @param[in] arg Parameter passed to the function.
 * @return void
 */
static void menu_task_(void *arg)
{
    (void)arg;

    for (;;)
    {
        s_arrow_tick++;

        if (s_detached)
        {
            vTaskDelay(k_yield_delay);
            continue;
        }

        if (s_screen == POOM_MENU_SCREEN_SUBMENU)
        {
            render_submenu_();
        }
        else
        {
            render_main_();
        }

        if (s_flash_timer > 0u)
        {
            s_flash_timer--;
        }

        vTaskDelay(k_yield_delay);
    }
}

void app_poom_menu_principal(void)
{
    static bool started = false;
    if (started)
    {
        return;
    }
    started = true;

    poom_arduboy_begin();

    poom_led_rainbow_init();

    poom_button_sound_init();
    update_btn_sound_label_();

    (void)xTaskCreate(menu_task_, "poom_menu", 4096, NULL, 5, &s_menu_task);

    (void)poom_sbus_register_topic(POOM_MENU_RESUME_TOPIC);
    (void)poom_sbus_subscribe_cb(POOM_MENU_RESUME_TOPIC, on_menu_resume_, NULL);

    (void)poom_sbus_subscribe_cb("input/button", on_button_any_, NULL);
    s_input_subscribed = true;
}
