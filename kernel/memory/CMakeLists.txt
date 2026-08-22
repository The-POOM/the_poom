set(srcs
    "src/memory_monitor.c"
    "src/memory_tracker.c"
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer heap
)
