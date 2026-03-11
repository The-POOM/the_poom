set(srcs "oled_driver.c" "oled_transport.c")
idf_component_register( SRCS "${srcs}"
					INCLUDE_DIRS "include"
					REQUIRES driver i2c board)
#					REQUIRES driver esp_wifi mqtt nvs_flash fatfs esp_adc_cal )
