set(srcs
    "poom_button_sound.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        button_driver
        buzzer
        poom_secrets_store
        poom_sbus
)
