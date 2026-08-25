set(srcs "poom_i2c_as5600.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES button_driver i2c poom_arduboy_display poom_sbus poom_sbus
)
