set(srcs
    "poom_arduboy_display.cpp"
    "poom_arduboy_font5x7.c"
    "poom_arduboy_sprites.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES oled i2c
)
