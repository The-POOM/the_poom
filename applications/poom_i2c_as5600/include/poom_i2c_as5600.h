// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hugo Trippaers <hugo@trippaers.nl>

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu entrypoint
// See poom_menu.c
void app_poom_i2c_as5600_menu(void);

#ifdef __cplusplus
}
#endif