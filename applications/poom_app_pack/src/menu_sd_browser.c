// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "menu_sd_browser.h"

#include <stdint.h>

#include "poom_sbus.h"
#include "poom_sd_browser.h"

#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

/**
 * @brief Handles SD browser exit and returns to main menu.
 *
 * @param[in,out] user_ctx Unused callback context.
 * @return void
 */
static void menu_sd_browser_exit_cb_(void* user_ctx)
{
    (void)user_ctx;

    const uint8_t token = 1;
    (void)poom_sbus_publish(POOM_MENU_RESUME_TOPIC, &token, sizeof(token), 0);
}

/**
 * @brief Starts SD browser app from submenu and configures exit callback.
 *
 * @return void
 */
void app_sd_browser_menu(void)
{
    (void)poom_sd_browser_set_exit_callback(menu_sd_browser_exit_cb_, NULL);
    (void)poom_sd_browser_start();
}
