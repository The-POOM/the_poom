set(component_srcs "ws2812.c")

idf_component_register(SRCS "${component_srcs}"
                       REQUIRES "driver esp_driver_rmt"
                       INCLUDE_DIRS "." "include")
