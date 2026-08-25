set(srcs "poom_i2c_as5600.c" "poom_i2c_as5600_driver.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES button_driver i2c poom_arduboy_display poom_sbus ws2812 poom_led_rainbow
)
