set(component_srcs "i2c.c")

idf_component_register(SRCS "${component_srcs}"
                       REQUIRES "driver esp_driver_i2c board"
                       INCLUDE_DIRS "." "include")
