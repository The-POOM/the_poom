// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file cli_154recon.c
 * @brief CLI command group for active IEEE 802.15.4 / Thread network recon.
 */

#include "cli_154recon.h"

#include <errno.h>
#include <stdio.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "poom_154_recon.h"
#include "sd_card.h"

/* Growing CSV log on SD for wardrive-style mapping. */
#define NET154_LOG_DIR  "net154"
#define NET154_LOG_PATH SD_CARD_PATH "/" NET154_LOG_DIR "/net154_log.csv"

/* ---- argument tables ------------------------------------------------------ */

typedef struct
{
    struct arg_int* channel; /* optional single channel 11..26 */
    struct arg_end* end;
} cli_154recon_chan_args_t;

static cli_154recon_chan_args_t s_scan_args;
static cli_154recon_chan_args_t s_disc_args;
static cli_154recon_chan_args_t s_energy_args;

/* ---- helpers -------------------------------------------------------------- */

/**
 * @brief Internal helper: resolve an optional channel arg into a scan mask.
 */
static bool cli_154recon_resolve_mask_(const struct arg_int* channel_arg, uint32_t* out_mask)
{
    if (channel_arg->count == 0)
    {
        *out_mask = POOM_154_RECON_CHANNELS_ALL;
        return true;
    }

    int ch = channel_arg->ival[0];
    if ((ch < (int)POOM_154_RECON_CHANNEL_MIN) || (ch > (int)POOM_154_RECON_CHANNEL_MAX))
    {
        printf("Channel must be %u..%u.\n",
               (unsigned)POOM_154_RECON_CHANNEL_MIN, (unsigned)POOM_154_RECON_CHANNEL_MAX);
        return false;
    }

    *out_mask = poom_154_recon_channel_mask((uint8_t)ch);
    return true;
}

/**
 * @brief Internal helper: print one cached network row.
 */
static void cli_154recon_print_net_(uint8_t idx, const poom_154_recon_net_t* n)
{
    printf("[%2u] PAN 0x%04X  ch %2u  %4d dBm  lqi %3u  %s%s\n",
           (unsigned)idx, (unsigned)n->pan_id, (unsigned)n->channel,
           (int)n->rssi, (unsigned)n->lqi,
           n->is_discover ? "THREAD " : "",
           n->is_joinable ? "JOINABLE" : "");
    printf("     name \"%s\"  xpan %02X%02X%02X%02X%02X%02X%02X%02X\n",
           n->network_name,
           n->ext_pan_id[0], n->ext_pan_id[1], n->ext_pan_id[2], n->ext_pan_id[3],
           n->ext_pan_id[4], n->ext_pan_id[5], n->ext_pan_id[6], n->ext_pan_id[7]);
}

/**
 * @brief Internal helper: print all cached results.
 */
static void cli_154recon_print_all_(uint8_t count)
{
    for (uint8_t i = 0U; i < count; i++)
    {
        poom_154_recon_net_t net;
        if (poom_154_recon_get_result(i, &net))
        {
            cli_154recon_print_net_(i, &net);
        }
    }
}

/* ---- command handlers ----------------------------------------------------- */

/**
 * @brief Internal helper for `net154-init`.
 */
static int cmd_154recon_init(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = poom_154_recon_init();
    if (err != ESP_OK)
    {
        printf("802.15.4 recon init failed: %s\n", esp_err_to_name(err));
        return (int)err;
    }
    printf("802.15.4 recon ready. Stop the pcap sniffer (cfg-154-stop) before scanning.\n");
    return 0;
}

/**
 * @brief Internal helper for `net154-scan` (beacon-request active scan).
 */
static int cmd_154recon_scan(int argc, char** argv)
{
    int parse_errors = arg_parse(argc, argv, (void**)&s_scan_args);
    if (parse_errors != 0)
    {
        arg_print_errors(stderr, s_scan_args.end, argv[0]);
        printf("Usage: net154-scan [channel 11..26]\n");
        return 0;
    }

    uint32_t mask = 0U;
    if (!cli_154recon_resolve_mask_(s_scan_args.channel, &mask))
    {
        return 0;
    }

    printf("Active scan%s ...\n", (mask == POOM_154_RECON_CHANNELS_ALL) ? " (all channels)" : "");

    uint8_t count = 0U;
    esp_err_t err = poom_154_recon_active_scan(mask, 0U, &count);
    if (err == ESP_ERR_INVALID_STATE)
    {
        printf("Not initialized. Run net154-init first.\n");
        return 0;
    }
    if (err != ESP_OK)
    {
        printf("Scan error: %s\n", esp_err_to_name(err));
        return (int)err;
    }

    if (count == 0U)
    {
        printf("No networks found. (Beacon scan finds Zigbee; try net154-discover for Thread/Matter.)\n");
        return 0;
    }

    printf("Found %u network(s):\n", (unsigned)count);
    cli_154recon_print_all_(count);
    return 0;
}

/**
 * @brief Internal helper for `net154-discover` (MLE Discovery for Thread/Matter).
 */
static int cmd_154recon_discover(int argc, char** argv)
{
    int parse_errors = arg_parse(argc, argv, (void**)&s_disc_args);
    if (parse_errors != 0)
    {
        arg_print_errors(stderr, s_disc_args.end, argv[0]);
        printf("Usage: net154-discover [channel 11..26]\n");
        return 0;
    }

    uint32_t mask = 0U;
    if (!cli_154recon_resolve_mask_(s_disc_args.channel, &mask))
    {
        return 0;
    }

    printf("Thread discovery%s ...\n", (mask == POOM_154_RECON_CHANNELS_ALL) ? " (all channels)" : "");

    uint8_t count = 0U;
    esp_err_t err = poom_154_recon_discover(mask, 0U, &count);
    if (err == ESP_ERR_INVALID_STATE)
    {
        printf("Not initialized. Run net154-init first.\n");
        return 0;
    }
    if (err != ESP_OK)
    {
        printf("Discovery error: %s\n", esp_err_to_name(err));
        return (int)err;
    }

    if (count == 0U)
    {
        printf("No Thread networks found.\n");
        return 0;
    }

    printf("Found %u Thread network(s):\n", (unsigned)count);
    cli_154recon_print_all_(count);
    return 0;
}

/**
 * @brief Internal helper for `net154-energy`.
 */
static int cmd_154recon_energy(int argc, char** argv)
{
    int parse_errors = arg_parse(argc, argv, (void**)&s_energy_args);
    if (parse_errors != 0)
    {
        arg_print_errors(stderr, s_energy_args.end, argv[0]);
        printf("Usage: net154-energy [channel 11..26]\n");
        return 0;
    }

    uint32_t mask = 0U;
    if (!cli_154recon_resolve_mask_(s_energy_args.channel, &mask))
    {
        return 0;
    }

    esp_err_t err = poom_154_recon_energy_scan(mask, 0U);
    if (err == ESP_ERR_INVALID_STATE)
    {
        printf("Not initialized. Run net154-init first.\n");
        return 0;
    }
    if (err != ESP_OK)
    {
        printf("Energy scan error: %s\n", esp_err_to_name(err));
        return (int)err;
    }
    return 0;
}

/**
 * @brief Internal helper for `net154-results`.
 */
static int cmd_154recon_results(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    uint8_t count = poom_154_recon_result_count();
    if (count == 0U)
    {
        printf("No cached results. Run net154-scan or net154-discover first.\n");
        return 0;
    }

    printf("Cached %u network(s):\n", (unsigned)count);
    cli_154recon_print_all_(count);
    return 0;
}

/**
 * @brief Internal helper for `net154-log`.
 *
 * Appends the cached scan/discovery results to a growing CSV on the SD card
 * (wardrive-style log). Writes a header row the first time the file is created.
 */
static int cmd_154recon_log(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    uint8_t count = poom_154_recon_result_count();
    if (count == 0U)
    {
        printf("No cached results to log. Run net154-scan or net154-discover first.\n");
        return 0;
    }

    if (sd_card_is_not_mounted())
    {
        if (sd_card_mount() != ESP_OK)
        {
            printf("SD mount failed. Is a card inserted?\n");
            return 0;
        }
    }

    (void)sd_card_create_dir(NET154_LOG_DIR); /* harmless if it already exists */

    FILE* f = fopen(NET154_LOG_PATH, "a");
    if (f == NULL)
    {
        printf("Could not open %s (errno=%d).\n", NET154_LOG_PATH, errno);
        return 0;
    }

    /* Header only when the file is new/empty. */
    if (ftell(f) == 0L)
    {
        (void)fprintf(f, "boot_ms,source,channel,pan_id,ext_pan_id,name,rssi_dbm,lqi,joinable\n");
    }

    const long long boot_ms = (long long)(esp_timer_get_time() / 1000);
    uint8_t written = 0U;

    for (uint8_t i = 0U; i < count; i++)
    {
        poom_154_recon_net_t n;
        if (!poom_154_recon_get_result(i, &n))
        {
            continue;
        }

        (void)fprintf(f,
            "%lld,%s,%u,0x%04X,%02X%02X%02X%02X%02X%02X%02X%02X,\"%s\",%d,%u,%u\n",
            boot_ms,
            n.is_discover ? "discover" : "scan",
            (unsigned)n.channel,
            (unsigned)n.pan_id,
            n.ext_pan_id[0], n.ext_pan_id[1], n.ext_pan_id[2], n.ext_pan_id[3],
            n.ext_pan_id[4], n.ext_pan_id[5], n.ext_pan_id[6], n.ext_pan_id[7],
            n.network_name,
            (int)n.rssi,
            (unsigned)n.lqi,
            n.is_joinable ? 1U : 0U);
        written++;
    }

    (void)fclose(f);
    printf("Logged %u network(s) to %s\n", (unsigned)written, NET154_LOG_PATH);
    return 0;
}

/* ---- registration --------------------------------------------------------- */

void cli_poom_154recon_register_cmds(void)
{
    s_scan_args.channel = arg_int0(NULL, NULL, "<channel>", "Single channel 11..26 (default: all)");
    s_scan_args.end     = arg_end(1);

    s_disc_args.channel = arg_int0(NULL, NULL, "<channel>", "Single channel 11..26 (default: all)");
    s_disc_args.end     = arg_end(1);

    s_energy_args.channel = arg_int0(NULL, NULL, "<channel>", "Single channel 11..26 (default: all)");
    s_energy_args.end     = arg_end(1);

    const esp_console_cmd_t init_cmd = {
        .command = "net154-init",
        .help = "Bring OpenThread stack up for 802.15.4 active recon (call once)",
        .hint = NULL,
        .func = &cmd_154recon_init,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&init_cmd));

    const esp_console_cmd_t scan_cmd = {
        .command = "net154-scan",
        .help = "Active beacon scan: Zigbee + beacon-payload Thread (PAN, name, RSSI, joinable)",
        .hint = NULL,
        .func = &cmd_154recon_scan,
        .argtable = &s_scan_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));

    const esp_console_cmd_t disc_cmd = {
        .command = "net154-discover",
        .help = "MLE Discovery: find Thread / Matter networks (that ignore beacon requests)",
        .hint = NULL,
        .func = &cmd_154recon_discover,
        .argtable = &s_disc_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&disc_cmd));

    const esp_console_cmd_t energy_cmd = {
        .command = "net154-energy",
        .help = "Energy scan: per-channel noise floor (max RSSI)",
        .hint = NULL,
        .func = &cmd_154recon_energy,
        .argtable = &s_energy_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&energy_cmd));

    const esp_console_cmd_t results_cmd = {
        .command = "net154-results",
        .help = "Reprint the last scan/discovery results",
        .hint = NULL,
        .func = &cmd_154recon_results,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&results_cmd));

    const esp_console_cmd_t log_cmd = {
        .command = "net154-log",
        .help = "Append cached results to /sdcard/net154/net154_log.csv (wardrive log)",
        .hint = NULL,
        .func = &cmd_154recon_log,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&log_cmd));
}
