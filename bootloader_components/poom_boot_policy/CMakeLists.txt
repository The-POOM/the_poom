idf_component_register(SRCS "hooks.c"
                       REQUIRES bootloader bootloader_support)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bootloader_utility_get_selected_boot_partition")
