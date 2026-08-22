// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file poom_ui_keyboard.h
 * @brief Minimal on-screen keyboard (OSK) for POOM menus using `poom_arduboy_display`.
 */

#ifndef POOM_UI_KEYBOARD_H
#define POOM_UI_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Result of handling a button event.
 */
typedef enum
{
    /** @brief No high-level action requested. */
    POOM_UI_KEYBOARD_ACTION_NONE = 0,
    /** @brief User selected OK. */
    POOM_UI_KEYBOARD_ACTION_ACCEPT,
} poom_ui_keyboard_action_t;

/**
 * @brief Keyboard UI state container.
 *
 * The caller owns the backing text buffer (`text`, `text_cap`) and decides what a
 * cancel action means (usually `B:BACK`).
 */
typedef struct
{
    /** @brief Optional target label shown in the title bar (e.g., SSID). */
    const char *target_label;

    /** @brief Text buffer where the keyboard appends characters. */
    char *text;
    /** @brief Capacity of `text` including the NUL terminator. */
    size_t text_cap;
    /** @brief Current length of `text` not including the NUL terminator. */
    size_t text_len;

    /** @brief Alpha-mode caps toggle. */
    bool caps;
    /** @brief When true, use numeric/symbol layout. */
    bool numeric;

    /** @brief Selected row index [0..2]. */
    uint8_t sel_row;
    /** @brief Selected key index within the current row. */
    uint8_t sel_key;
} poom_ui_keyboard_t;

/**
 * @brief Initialize the keyboard state.
 *
 * @param[in,out] kb Keyboard state object.
 * @param[in,out] text Backing text buffer (updated in-place).
 * @param[in] text_cap Capacity of `text` including the NUL terminator.
 * @param[in] target_label Optional title-bar label (e.g., SSID).
 */
void poom_ui_keyboard_init(poom_ui_keyboard_t *kb, char *text, size_t text_cap, const char *target_label);

/**
 * @brief Update the title-bar label shown as `To:<label>`.
 * @param[in,out] kb Keyboard state object.
 * @param[in] target_label New label string (may be NULL).
 */
void poom_ui_keyboard_set_target_label(poom_ui_keyboard_t *kb, const char *target_label);

/**
 * @brief Clear the current text buffer (sets length to 0).
 * @param[in,out] kb Keyboard state object.
 */
void poom_ui_keyboard_clear_text(poom_ui_keyboard_t *kb);

/**
 * @brief Handle one button input.
 *
 * Expected mapping is compatible with POOM menus:
 * `BTN_LEFT/RIGHT/UP/DOWN` navigates and `BTN_A` selects.
 *
 * @param[in,out] kb Keyboard state object.
 * @param[in] button Logical button ID (BTN_*).
 * @return poom_ui_keyboard_action_t Requested action.
 */
poom_ui_keyboard_action_t poom_ui_keyboard_handle_button(poom_ui_keyboard_t *kb, uint8_t button);

/**
 * @brief Draw the keyboard modal to the OLED.
 *
 * This function renders the full modal, including clearing the screen and calling
 * `poom_arduboy_display()`.
 *
 * @param[in] kb Keyboard state object.
 */
void poom_ui_keyboard_draw(const poom_ui_keyboard_t *kb);

#ifdef __cplusplus
}
#endif

#endif /* POOM_UI_KEYBOARD_H */
