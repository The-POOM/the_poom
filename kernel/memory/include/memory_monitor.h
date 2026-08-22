// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEM_MON_LEVEL_HEALTHY = 0,
    MEM_MON_LEVEL_WARNING,
    MEM_MON_LEVEL_CRITICAL,
    MEM_MON_LEVEL_EMERGENCY
} mem_monitor_level_t;

typedef struct {
    size_t free_bytes;
    size_t allocated_bytes;
    size_t largest_contig_block;
    size_t min_free_watermark;
    float fragmentation_ratio;
} mem_monitor_stats_t;

typedef void (*mem_monitor_cb_t)(mem_monitor_level_t level, const mem_monitor_stats_t* stats);

esp_err_t mem_monitor_init(void);
esp_err_t mem_monitor_start(uint32_t period_ms);
void mem_monitor_stop(void);
void mem_monitor_set_thresholds_kb(size_t warn_kb, size_t crit_kb, size_t emer_kb);
void mem_monitor_register_callback(mem_monitor_cb_t callback);
mem_monitor_stats_t mem_monitor_get_current_stats(void);
mem_monitor_level_t mem_monitor_get_last_level(void);
void mem_monitor_log_report(void);

#ifdef __cplusplus
}
#endif

#endif
