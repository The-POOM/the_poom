// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize module and load persisted setting.
 *
 * Safe to call multiple times.
 */
void poom_button_sound_init(void);

/**
 * @brief Get persisted (and currently cached) setting value.
 */
bool poom_button_sound_get_enabled_setting(void);

/**
 * @brief Persist setting and apply immediately.
 *
 * When enabled and not suspended, starts the sound task + SBUS subscription.
 * When disabled, stops the sound task + unsubscribes.
 *
 * @return true on success, false on NVS error.
 */
bool poom_button_sound_set_enabled_setting(bool enabled);

/**
 * @brief Temporarily disable (used when entering apps to free resources).
 *
 * Does not change the persisted setting.
 */
void poom_button_sound_suspend(void);

/**
 * @brief Re-enable after suspend (if setting is enabled).
 *
 * Does not change the persisted setting.
 */
void poom_button_sound_resume(void);

#ifdef __cplusplus
} // extern "C"
#endif
