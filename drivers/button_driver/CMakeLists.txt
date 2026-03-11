set(srcs "button_driver.c")
idf_component_register( SRCS "${srcs}"
					INCLUDE_DIRS "include"
					REQUIRES button esp_timer poom_sbus board)
#					REQUIRES button)
#					REQUIRES driver esp_wifi mqtt nvs_flash fatfs esp_adc_cal )
