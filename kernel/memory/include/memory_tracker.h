// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef MEMORY_TRACKER_H
#define MEMORY_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t current_bytes;
    size_t peak_bytes;
    uint32_t allocations;
    uint32_t frees;
    uint32_t open_allocations;
} mem_tracker_global_stats_t;

typedef struct {
    TaskHandle_t task_handle;
    char task_name[configMAX_TASK_NAME_LEN];
    size_t current_bytes;
    size_t peak_bytes;
    uint32_t allocations;
    uint32_t frees;
    uint32_t open_allocations;
    bool active;
} mem_tracker_task_stats_t;

esp_err_t mem_tracker_init(void);
esp_err_t mem_tracker_task_begin(TaskHandle_t task_handle, const char* task_name);
esp_err_t mem_tracker_task_end(TaskHandle_t task_handle, bool free_remaining_allocations);

void* mem_tracker_malloc_debug(size_t size, const char* file, uint32_t line);
void mem_tracker_free(void* ptr);

bool mem_tracker_get_global_stats(mem_tracker_global_stats_t* stats_out);
bool mem_tracker_get_task_stats(TaskHandle_t task_handle, mem_tracker_task_stats_t* stats_out);

void mem_tracker_log_global_report(void);
void mem_tracker_log_task_report(TaskHandle_t task_handle);

#define MEM_TRACKER_MALLOC(size) mem_tracker_malloc_debug((size), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif
