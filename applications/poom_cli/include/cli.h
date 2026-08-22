#pragma once
#include <stdbool.h>

typedef void (*ctrl_c_callback_t)(void);

void poom_console_register_ctrl_c_handler(ctrl_c_callback_t callback);
void poom_console_begin(void (*registrar_cmds_fn)(void));

void poom_console_pause(void);
void poom_console_resume(void);
bool poom_console_is_paused(void);
bool poom_console_is_active(void);
