idf_component_register(SRCS "src/poom_154_recon.c"
                    INCLUDE_DIRS "include"
                    REQUIRES openthread open_thread
                    PRIV_REQUIRES esp_timer)
