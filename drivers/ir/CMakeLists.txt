set(component_srcs "src/ir_rcv.c" "src/ir_dec.c" "src/ir_tx.c")

idf_component_register(SRCS "${component_srcs}"
                       REQUIRES "driver esp_driver_rmt"
                       INCLUDE_DIRS "." "include")