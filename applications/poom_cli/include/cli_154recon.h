// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registers the IEEE 802.15.4 / Thread active-recon CLI commands.
 *
 * Commands:
 *   net154-init              Bring the OpenThread stack up for scanning.
 *   net154-scan [channel]    Active scan (all 2.4 GHz channels, or one 11..26).
 *   net154-energy [channel]  Energy / noise-floor scan.
 *   net154-results           Reprint the last active-scan results.
 *
 * @return void
 */
void cli_poom_154recon_register_cmds(void);

#ifdef __cplusplus
}
#endif
