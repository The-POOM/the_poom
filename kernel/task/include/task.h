// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_TASK_H
#define POOM_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN];
    uint32_t stack_size_words;
    uint32_t stack_size_bytes;
    uint32_t stack_high_watermark_words;
    uint32_t stack_used_bytes;
    uint32_t uptime_seconds;
    UBaseType_t priority;
    bool active;
} poom_task_snapshot_t;

esp_err_t poom_task_init(void);
esp_err_t poom_task_create(TaskFunction_t task_func,
                           const char* name,
                           uint32_t stack_size_words,
                           void* params,
                           UBaseType_t priority,
                           TaskHandle_t* handle_out);
esp_err_t poom_task_delete(TaskHandle_t handle);

void poom_task_print(void);
void poom_task_stack_usage(void);
uint32_t poom_task_get_active_count(void);
bool poom_task_get_snapshot(TaskHandle_t handle, poom_task_snapshot_t* snapshot_out);

#ifdef __cplusplus
}
#endif

#endif
