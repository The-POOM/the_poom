#include "poom_menu_assets.h"

#include <stdint.h>

// `applications/poom_menu/include/poom_menu_icons/*.h` are Arduino-style headers using C++ namespaces and PROGMEM.
// Provide compatibility for ESP-IDF builds.
#ifndef PROGMEM
#define PROGMEM
#endif

#include "poom_menu_icons/beast_icon.h"
#include "poom_menu_icons/gamer_icon.h"
#include "poom_menu_icons/maker_icon.h"
#include "poom_menu_icons/settings_icon.h"
#include "poom_menu_icons/the_beast_title.h"
#include "poom_menu_icons/the_gamer_title.h"
#include "poom_menu_icons/the_maker_title.h"
#include "poom_menu_icons/the_zen_title.h"
#include "poom_menu_icons/settings_title.h"
#include "poom_menu_icons/zen_icon.h"

extern "C" {

uint8_t poom_menu_mode_count(void)
{
    return 5;
}

const uint8_t *poom_menu_mode_icon(uint8_t idx)
{
    switch (idx % poom_menu_mode_count())
    {
    case 0:
        return MenuIcons::Beast;
    case 1:
        return MenuIcons::Zen;
    case 2:
        return MenuIcons::Gamer;
    case 3:
        return MenuIcons::Maker;
    default:
        return MenuIcons::Settings;
    }
}

const uint8_t *poom_menu_mode_title(uint8_t idx)
{
    switch (idx % poom_menu_mode_count())
    {
    case 0:
        return MenuIcons::TheBeastTitle;
    case 1:
        return MenuIcons::TheZenTitle;
    case 2:
        return MenuIcons::TheGamerTitle;
    case 3:
        return MenuIcons::TheMakerTitle;
    default:
        return MenuIcons::SettingsTitle;
    }
}

} // extern "C"
