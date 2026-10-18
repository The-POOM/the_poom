# `hal` is required because bootloader_support's esp_flash_encrypt.h includes
# hal/efuse_ll.h, which needs hal's target-specific include directory
# (components/hal/esp32c5/include). Without it the bootloader build fails with:
#   esp_flash_encrypt.h:12:10: fatal error: hal/efuse_ll.h: No such file or directory
idf_component_register(SRCS "hooks.c"
                       REQUIRES bootloader bootloader_support hal)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bootloader_utility_get_selected_boot_partition")
