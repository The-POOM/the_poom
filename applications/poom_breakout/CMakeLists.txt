set(srcs
    "poom_breakout.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES
        button_driver
        buzzer
        poom_arduboy_display
        poom_sbus
)
