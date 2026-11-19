# SPDX-License-Identifier: GPL-3.0-or-later

set(srcs "poom_ble_detector.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES bt poom_ble_scan esp_timer
)
