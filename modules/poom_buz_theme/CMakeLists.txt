set(srcs "poom_buz_theme.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES buzzer)
