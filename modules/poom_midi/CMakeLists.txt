idf_component_register(
    SRCS
        "poom_midi_harmony.c"
        "poom_midi_ble_out.c"
        "poom_midi_player.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cjson
        ble_midi
        esp_timer
)

