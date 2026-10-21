// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_WIFI_DETECTOR_OUI_H
#define MENU_WIFI_DETECTOR_OUI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Looks up a device vendor from the OUI (first 3 MAC bytes).
 *
 * @param[in] mac Six-byte client MAC.
 * @return Static vendor display string, or NULL when the OUI is unknown or the
 *         MAC is locally-administered (randomized/private).
 */
const char *menu_wifi_detector_oui_vendor(const uint8_t mac[6]);

/**
 * @brief Reports whether a MAC is locally-administered (randomized/private).
 *
 * @param[in] mac Six-byte MAC.
 * @return true if the locally-administered bit is set.
 */
bool menu_wifi_detector_oui_is_random(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* MENU_WIFI_DETECTOR_OUI_H */
