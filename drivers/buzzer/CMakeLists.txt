set(srcs "buzzer.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES driver esp_driver_gpio esp_driver_ledc) 
