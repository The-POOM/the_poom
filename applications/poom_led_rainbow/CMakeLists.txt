set(component_srcs "poom_led_rainbow.c")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES ws2812 board)
