set(srcs "oled_driver.c" "oled_transport.c")
idf_component_register( SRCS "${srcs}"
					INCLUDE_DIRS "include"
					REQUIRES driver i2c board)
if(IDF_TARGET STREQUAL "esp32c5")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CFG_SH1106)
endif()