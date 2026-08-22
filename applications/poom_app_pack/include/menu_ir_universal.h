// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_IR_UNIVERSAL_H
#define MENU_IR_UNIVERSAL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts IR Universal workflow and returns immediately.
 *
 * Provides:
 * - Learn mode: reads IR and assigns a code to a POOM button.
 * - Emulator mode: sends stored codes when POOM buttons are pressed.
 *
 * This menu is available only on boards that define `PIN_NUM_IR_TX` and `PIN_NUM_IR_RX`.
 *
 * @return void
 */
void menu_ir_universal_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_IR_UNIVERSAL_H */
