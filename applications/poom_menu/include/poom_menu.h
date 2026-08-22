#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Principal (tiles) menu based on `poom/menu/menu.ino` + SBUS button events.
void app_poom_menu_principal(void);

// Optional hook: override this (provide a non-weak definition elsewhere) to actually launch apps.
// Default implementation publishes to SBUS and prints to UART.
void poom_menu_launch_app(uint8_t mode, uint8_t app);

#define POOM_MENU_LAUNCH_TOPIC "poom/menu/launch"
#define POOM_MENU_RESUME_TOPIC "poom/menu/resume"

typedef struct
{
    uint8_t mode;
    uint8_t app;
} poom_menu_launch_msg_t;

#ifdef __cplusplus
} // extern "C"
#endif
