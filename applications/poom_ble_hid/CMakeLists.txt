set(srcs
    "poom_ble_hid.c"
    "esp_hidd_prf_api.c"
    "hid_dev.c"
    "hid_device_le_prf.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash)
