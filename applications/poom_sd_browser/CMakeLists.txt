set(srcs
    "poom_sd_browser.c"
    "poom_sd_browser_storage.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES poom_oled_screen poom_sbus sd_card
)
