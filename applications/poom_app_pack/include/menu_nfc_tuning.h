// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MENU_NFC_TUNING_H
#define MENU_NFC_TUNING_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show NFC antenna tuning settings UI.
 *
 * Provides access to:
 * - Read current AAT_A / AAT_B and phase/amplitude.
 * - Run ST25R3916 auto-tuning.
 *
 * Note: Display flush and NFC tuning share the same I2C bus. This menu runs
 * tuning operations synchronously in its UI task and does not refresh the
 * OLED while the tuning I2C transactions are in progress.
 */
void menu_nfc_tuning_show(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_NFC_TUNING_H */
