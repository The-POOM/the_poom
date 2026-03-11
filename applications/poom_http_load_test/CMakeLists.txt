set(srcs "poom_http_load_test.c")
idf_component_register( SRCS "${srcs}"
                INCLUDE_DIRS "include"
                REQUIRES poom_wifi_ctrl)
