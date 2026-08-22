#ifdef __cplusplus
extern "C" {
#endif
#pragma once

#include <stdbool.h>

/**
 * @brief Start Zigbee CLI (spawns Zigbee stack + console REPL).
 *
 * Safe to call multiple times (subsequent calls are ignored).
 */
void cli_poom_zigbee_begin(void);

/**
 * @brief Stop Zigbee CLI (kills Zigbee task + stops REPL).
 *
 * Safe to call multiple times.
 */
void cli_poom_zigbee_stop(void);

/**
 * @brief True if Zigbee CLI is currently running.
 */
bool cli_poom_zigbee_is_running(void);

#ifdef __cplusplus
}
#endif
