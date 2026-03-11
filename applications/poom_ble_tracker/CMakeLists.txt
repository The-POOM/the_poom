set(srcs "poom_ble_tracker.c")
idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt poom_ble_gatt_client)
