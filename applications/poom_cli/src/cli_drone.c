// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "cli_drone.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"

#include "poom_drone_emul.h"

static struct
{
    struct arg_dbl *latitude;
    struct arg_dbl *longitude;
    struct arg_end *end;
} s_drone_loc_args;

static struct
{
    struct arg_int *count;
    struct arg_end *end;
} s_drone_count_args;

static struct
{
    struct arg_int *enabled;
    struct arg_end *end;
} s_drone_ble_args;

/**
 * @brief Internal helper for `cmd_drone_emul_loc_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_loc_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_drone_loc_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_drone_loc_args.end, "drone-emul-loc-set");
        return 1;
    }

    const double lat = *s_drone_loc_args.latitude->dval;
    const double lon = *s_drone_loc_args.longitude->dval;

    esp_err_t err = poom_drone_emul_set_location(lat, lon);
    if (err != ESP_OK)
    {
        printf("Set location failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Emulator location set to %.6f, %.6f (persisted)\n", lat, lon);
    return 0;
}

/**
 * @brief Starts the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    poom_drone_emul_config_t cfg;
    poom_drone_emul_config_default(&cfg);

    esp_err_t err = poom_drone_emul_start(&cfg);
    if (err != ESP_OK)
    {
        printf("Emulator start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    double lat = 0;
    double lon = 0;
    (void)poom_drone_emul_get_location(&lat, &lon);
    uint8_t count = 0;
    (void)poom_drone_emul_get_count(&count);
    bool ble = false;
    (void)poom_drone_emul_get_ble_enabled(&ble);
    printf("Emulator started (CH=%u, n=%u, ble=%s, loc=%.6f, %.6f)\n",
           (unsigned)cfg.channel,
           (unsigned)count,
           ble ? "ON" : "OFF",
           lat,
           lon);
    return 0;
}

/**
 * @brief Stops the internal runtime for this module.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = poom_drone_emul_stop();
    if (err != ESP_OK)
    {
        printf("Emulator stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Emulator stopped\n");
    return 0;
}

/**
 * @brief Internal helper for `cmd_drone_emul_status`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const bool running = poom_drone_emul_is_running();
    double lat = 0;
    double lon = 0;
    (void)poom_drone_emul_get_location(&lat, &lon);
    uint8_t count = 0;
    (void)poom_drone_emul_get_count(&count);
    bool ble = false;
    (void)poom_drone_emul_get_ble_enabled(&ble);

    printf("Emulator: %s | n=%u | ble=%s | loc=%.6f, %.6f\n",
           running ? "RUN" : "OFF",
           (unsigned)count,
           ble ? "ON" : "OFF",
           lat,
           lon);
    return 0;
}

/**
 * @brief Internal helper for `cmd_drone_emul_count_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_count_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_drone_count_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_drone_count_args.end, "drone-emul-count-set");
        return 1;
    }

    const int count_i = *s_drone_count_args.count->ival;
    if ((count_i < 1) || (count_i > 16))
    {
        printf("Invalid count: %d (valid 1..16)\n", count_i);
        return 1;
    }

    esp_err_t err = poom_drone_emul_set_count((uint8_t)count_i);
    if (err != ESP_OK)
    {
        printf("Set count failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Emulator count set to %d (persisted)\n", count_i);
    return 0;
}

/**
 * @brief Internal helper for `cmd_drone_emul_ble_set`.
 *
 * @param[in] argc Parameter passed to the function.
 * @param[in] argv Parameter passed to the function.
 * @return int
 */
static int cmd_drone_emul_ble_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_drone_ble_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_drone_ble_args.end, "drone-emul-ble-set");
        return 1;
    }

    const int enabled_i = *s_drone_ble_args.enabled->ival;
    if ((enabled_i != 0) && (enabled_i != 1))
    {
        printf("Invalid value: %d (use 0 or 1)\n", enabled_i);
        return 1;
    }

    esp_err_t err = poom_drone_emul_set_ble_enabled(enabled_i == 1);
    if (err != ESP_OK)
    {
        printf("Set BLE failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Emulator BLE set to %s (persisted)\n", (enabled_i == 1) ? "ON" : "OFF");
    return 0;
}

void cli_poom_drone_register_cmds(void)
{
    s_drone_loc_args.latitude = arg_dbl1(NULL, NULL, "<latitude>", "Latitude coordinate");
    s_drone_loc_args.longitude = arg_dbl1(NULL, NULL, "<longitude>", "Longitude coordinate");
    s_drone_loc_args.end = arg_end(2);

    s_drone_count_args.count = arg_int1(NULL, NULL, "<count>", "Number of drones (1..16)");
    s_drone_count_args.end = arg_end(1);

    s_drone_ble_args.enabled = arg_int1(NULL, NULL, "<0|1>", "BLE enabled (0=off, 1=on)");
    s_drone_ble_args.end = arg_end(1);

    const esp_console_cmd_t loc_set_cmd = {
        .command = "drone-emul-loc-set",
        .help =
            "Set emulated drone location (lat lon). Example:\n"
            "  drone-emul-loc-set -- 37.7749 -122.4194\n"
            "Use '--' to avoid parsing negative numbers as options.",
        .hint = NULL,
        .func = &cmd_drone_emul_loc_set,
        .argtable = &s_drone_loc_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&loc_set_cmd));

    const esp_console_cmd_t count_set_cmd = {
        .command = "drone-emul-count-set",
        .help = "Set number of emulated drones (1..16). Example: drone-emul-count-set 4",
        .hint = NULL,
        .func = &cmd_drone_emul_count_set,
        .argtable = &s_drone_count_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&count_set_cmd));

    const esp_console_cmd_t ble_set_cmd = {
        .command = "drone-emul-ble-set",
        .help = "Enable/disable BLE RemoteID advertising. Example: drone-emul-ble-set 1",
        .hint = NULL,
        .func = &cmd_drone_emul_ble_set,
        .argtable = &s_drone_ble_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ble_set_cmd));

    const esp_console_cmd_t start_cmd = {
        .command = "drone-emul-start",
        .help = "Start Wi-Fi RemoteID emulator (beacon message packs)",
        .hint = NULL,
        .func = &cmd_drone_emul_start,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&start_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "drone-emul-stop",
        .help = "Stop Wi-Fi RemoteID emulator",
        .hint = NULL,
        .func = &cmd_drone_emul_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "drone-emul-status",
        .help = "Show emulator status and current location",
        .hint = NULL,
        .func = &cmd_drone_emul_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
}
