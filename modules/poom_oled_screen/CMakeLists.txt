set(srcs "poom_oled_screen.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES driver oled
)
