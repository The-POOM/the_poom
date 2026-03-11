set(srcs "poom_game_snake.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES poom_sbus poom_ble_hid button_driver esp_driver_ledc poom_buz_theme buzzer poom_oled_screen) 
