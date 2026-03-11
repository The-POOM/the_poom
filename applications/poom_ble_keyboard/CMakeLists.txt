set(srcs "poom_ble_keyboard.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES bt nvs_flash ws2812 poom_ble_hid poom_sbus board poom_led_rainbow) #esp_wifi mqtt nvs_flash fatfs esp_adc_cal 
                #PRIV_REQUIRES bt nvs_flash esp_driver_gpio)
                #target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
