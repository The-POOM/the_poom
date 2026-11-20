// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opens BEAST > WARDRIVING NODE.
 *
 * Turns the POOM into an ESP-NOW scanning node for an external wardriving
 * core: a Biscuit Shop core (BISCUIT NODE), a JustCallMeKoko dual-band
 * wardriver (JCMK NODE) or a Piglet in Core mode (PIGLET NODE). The core
 * owns GPS and logging; the POOM scans the channels it is assigned and
 * forwards every access point it hears.
 */
void menu_wardrive_node_show(void);

#ifdef __cplusplus
}
#endif
