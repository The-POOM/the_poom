// SPDX-License-Identifier: MIT
// Host-only shim of esp_err.h for unit-testing poom_net_audit_logic.c.
#ifndef POOM_TEST_ESP_ERR_SHIM_H
#define POOM_TEST_ESP_ERR_SHIM_H
typedef int esp_err_t;
#define ESP_OK                  (0)
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          (0x101)
#define ESP_ERR_INVALID_ARG     (0x102)
#define ESP_ERR_INVALID_STATE   (0x103)
#define ESP_ERR_NOT_FOUND       (0x105)
#define ESP_ERR_NOT_SUPPORTED   (0x106)
#endif
