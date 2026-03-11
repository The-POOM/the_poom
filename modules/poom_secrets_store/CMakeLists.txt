idf_component_register(
    SRCS
        "poom_secrets_store.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
)
