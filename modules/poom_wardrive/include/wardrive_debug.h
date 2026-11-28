// SPDX-License-Identifier: MIT
#pragma once
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* Verbose traces are opt-in. Keep the disabled branch type-checked. */
#if defined(CONFIG_POOM_WARDRIVE_DIAGNOSTICS) && CONFIG_POOM_WARDRIVE_DIAGNOSTICS
#define WD_DEBUG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define WD_DEBUG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#endif
