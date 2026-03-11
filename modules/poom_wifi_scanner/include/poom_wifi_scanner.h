// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WIFI_SCANNER_H
#define POOM_WIFI_SCANNER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifndef POOM_WIFI_SCANNER_ENABLE_LOG
#define POOM_WIFI_SCANNER_ENABLE_LOG (1)
#endif

#ifndef POOM_WIFI_SCANNER_DEBUG_LOG_ENABLED
#define POOM_WIFI_SCANNER_DEBUG_LOG_ENABLED (0)
#endif

#ifndef POOM_WIFI_SCANNER_MAX_AP
  #if defined(CONFIG_POOM_WIFI_CTRL_SCAN_MAX_AP)
    #define POOM_WIFI_SCANNER_MAX_AP CONFIG_POOM_WIFI_CTRL_SCAN_MAX_AP
  #elif defined(CONFIG_SCAN_MAX_AP)
    #define POOM_WIFI_SCANNER_MAX_AP CONFIG_SCAN_MAX_AP
  #else
    #define POOM_WIFI_SCANNER_MAX_AP (20U)
  #endif
#endif

typedef struct
{
    uint16_t count;
    wifi_ap_record_t records[POOM_WIFI_SCANNER_MAX_AP];
} poom_wifi_scanner_ap_records_t;

/**
 * @brief Runs a blocking Wi-Fi scan and updates cached AP records.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_scanner_scan(void);

/**
 * @brief Gets the cached AP records container.
 * @param[in/out] none Not used.
 * @return poom_wifi_scanner_ap_records_t*
 */
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void);

/**
 * @brief Gets one AP record by index from the cache.
 * @param[in] index AP record index in cache.
 * @return wifi_ap_record_t*
 */
wifi_ap_record_t *poom_wifi_scanner_get_ap_record(unsigned index);

/**
 * @brief Clears cached AP records and clears driver AP list.
 * @param[in/out] none Not used.
 * @return esp_err_t
 */
esp_err_t poom_wifi_scanner_clear_ap_records(void);

#ifdef __cplusplus
}
#endif

#endif /* POOM_WIFI_SCANNER_H */
