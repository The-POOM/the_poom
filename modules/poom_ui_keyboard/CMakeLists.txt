set(srcs
    "poom_ui_keyboard.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES poom_arduboy_display
)

