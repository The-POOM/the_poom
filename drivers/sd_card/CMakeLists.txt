set(component_srcs "sd_card.c")

idf_component_register(SRCS "${component_srcs}"
                       REQUIRES "nvs_flash" "console" "fatfs" "board"
                       INCLUDE_DIRS "." "include")
