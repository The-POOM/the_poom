#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Assets come from `applications/poom_menu/include/poom_menu_icons/*.h` (C++ namespace). This C API bridges them.
uint8_t poom_menu_mode_count(void);
const uint8_t *poom_menu_mode_icon(uint8_t idx);
const uint8_t *poom_menu_mode_title(uint8_t idx);

#ifdef __cplusplus
} // extern "C"
#endif
