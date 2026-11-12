// SPDX-License-Identifier: MIT
// Host-only shim of esp_err.h, enough to compile poom_net_scanner_logic.c off
// target for unit testing. NOT used in the firmware build (ESP-IDF provides the
// real header, which takes include precedence there).
#ifndef POOM_TEST_ESP_ERR_SHIM_H
#define POOM_TEST_ESP_ERR_SHIM_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  (0)
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          (0x101)
#define ESP_ERR_INVALID_ARG     (0x102)
#define ESP_ERR_INVALID_STATE   (0x103)
#define ESP_ERR_INVALID_SIZE    (0x104)
#define ESP_ERR_NOT_FOUND       (0x105)
#define ESP_ERR_NOT_SUPPORTED   (0x106)
#define ESP_ERR_TIMEOUT         (0x107)

#endif /* POOM_TEST_ESP_ERR_SHIM_H */
#define ESP_ERR_NOT_ALLOWED (0x10A)
const char *esp_err_to_name(esp_err_t e);
