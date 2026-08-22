// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "task.h"

#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "memory_tracker.h"

#define POOM_TASK_MAX_TASKS 30U

typedef struct {
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN];
    uint32_t stack_size_words;
    uint32_t created_at_ms;
    UBaseType_t priority;
    bool is_active;
} poom_task_info_t;

static const char* POOM_TASK_TAG = "poom_task";

#if defined(CONFIG_POOM_TASK_ENABLE_LOG) && CONFIG_POOM_TASK_ENABLE_LOG

#define POOM_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_TASK_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_TASK_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_TASK_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_TASK_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define POOM_PRINTF_E(...) \
    do {                   \
        (void)POOM_TASK_TAG; \
    } while(0)
#define POOM_PRINTF_W(...) \
    do {                   \
        (void)POOM_TASK_TAG; \
    } while(0)
#define POOM_PRINTF_I(...) \
    do {                   \
        (void)POOM_TASK_TAG; \
    } while(0)
#define POOM_PRINTF_D(...) \
    do {                   \
        (void)POOM_TASK_TAG; \
    } while(0)

#endif

static poom_task_info_t s_task_registry[POOM_TASK_MAX_TASKS];
static SemaphoreHandle_t s_task_mutex = NULL;
static bool s_task_initialized = false;

static bool poom_task_lock_(void)
{
    return (s_task_mutex != NULL) && (xSemaphoreTake(s_task_mutex, portMAX_DELAY) == pdTRUE);
}

static void poom_task_unlock_(void)
{
    if(s_task_mutex != NULL)
    {
        (void)xSemaphoreGive(s_task_mutex);
    }
}

static uint32_t poom_task_now_ms_(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int poom_task_find_slot_(TaskHandle_t handle)
{
    for(uint32_t i = 0U; i < POOM_TASK_MAX_TASKS; i++)
    {
        if(s_task_registry[i].is_active && (s_task_registry[i].handle == handle))
        {
            return (int)i;
        }
    }
    return -1;
}

static int poom_task_find_free_slot_(void)
{
    for(uint32_t i = 0U; i < POOM_TASK_MAX_TASKS; i++)
    {
        if(!s_task_registry[i].is_active)
        {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t poom_task_init(void)
{
    if(s_task_initialized)
    {
        return ESP_OK;
    }

    s_task_mutex = xSemaphoreCreateMutex();
    if(s_task_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(s_task_registry, 0, sizeof(s_task_registry));
    (void)mem_tracker_init();

    s_task_initialized = true;
    POOM_PRINTF_I("task manager initialized (%u slots)", (unsigned int)POOM_TASK_MAX_TASKS);
    return ESP_OK;
}

esp_err_t poom_task_create(TaskFunction_t task_func,
                           const char* name,
                           uint32_t stack_size_words,
                           void* params,
                           UBaseType_t priority,
                           TaskHandle_t* handle_out)
{
    TaskHandle_t created_handle = NULL;
    BaseType_t rt_result;
    int slot;

    if(!s_task_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if((task_func == NULL) || (name == NULL) || (stack_size_words == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!poom_task_lock_())
    {
        return ESP_FAIL;
    }

    slot = poom_task_find_free_slot_();
    if(slot < 0)
    {
        poom_task_unlock_();
        POOM_PRINTF_E("no free task slots in registry");
        return ESP_ERR_NO_MEM;
    }

    rt_result = xTaskCreate(task_func, name, stack_size_words, params, priority, &created_handle);
    if(rt_result != pdPASS)
    {
        poom_task_unlock_();
        POOM_PRINTF_E("xTaskCreate failed for '%s'", name);
        return ESP_FAIL;
    }

    memset(&s_task_registry[slot], 0, sizeof(s_task_registry[slot]));
    s_task_registry[slot].handle = created_handle;
    s_task_registry[slot].stack_size_words = stack_size_words;
    s_task_registry[slot].priority = priority;
    s_task_registry[slot].created_at_ms = poom_task_now_ms_();
    s_task_registry[slot].is_active = true;
    strncpy(s_task_registry[slot].name, name, sizeof(s_task_registry[slot].name) - 1U);

    poom_task_unlock_();

    (void)mem_tracker_task_begin(created_handle, name);

    if(handle_out != NULL)
    {
        *handle_out = created_handle;
    }

    POOM_PRINTF_I("task created: name=%s slot=%d handle=%p prio=%u stack_words=%u",
                  name,
                  slot,
                  (void*)created_handle,
                  (unsigned int)priority,
                  (unsigned int)stack_size_words);

    return ESP_OK;
}

esp_err_t poom_task_delete(TaskHandle_t handle)
{
    int slot;
    TaskHandle_t target_handle;
    char deleted_task_name[configMAX_TASK_NAME_LEN];

    if(!s_task_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    target_handle = (handle != NULL) ? handle : xTaskGetCurrentTaskHandle();

    if(!poom_task_lock_())
    {
        return ESP_FAIL;
    }

    slot = poom_task_find_slot_(target_handle);
    if(slot < 0)
    {
        poom_task_unlock_();
        return ESP_ERR_NOT_FOUND;
    }

    memset(deleted_task_name, 0, sizeof(deleted_task_name));
    strncpy(deleted_task_name, s_task_registry[slot].name, sizeof(deleted_task_name) - 1U);

    s_task_registry[slot].is_active = false;
    s_task_registry[slot].handle = NULL;

    poom_task_unlock_();

    (void)mem_tracker_task_end(target_handle, true);

    POOM_PRINTF_W("task deleted: %s", deleted_task_name);
    vTaskDelete(target_handle);
    return ESP_OK;
}

void poom_task_print(void)
{
    if(!s_task_initialized)
    {
        return;
    }

    if(!poom_task_lock_())
    {
        return;
    }

    POOM_PRINTF_I("---------------- TASK LIST ----------------");
    for(uint32_t i = 0U; i < POOM_TASK_MAX_TASKS; i++)
    {
        if(s_task_registry[i].is_active)
        {
            const uint32_t uptime_sec = (poom_task_now_ms_() - s_task_registry[i].created_at_ms) / 1000U;
            POOM_PRINTF_I("slot=%u name=%s prio=%u uptime=%us",
                          (unsigned int)i,
                          s_task_registry[i].name,
                          (unsigned int)s_task_registry[i].priority,
                          (unsigned int)uptime_sec);
        }
    }
    POOM_PRINTF_I("-------------------------------------------");

    poom_task_unlock_();
}

void poom_task_stack_usage(void)
{
    if(!s_task_initialized)
    {
        return;
    }

    if(!poom_task_lock_())
    {
        return;
    }

    POOM_PRINTF_I("------------- TASK STACK USAGE ------------");
    for(uint32_t i = 0U; i < POOM_TASK_MAX_TASKS; i++)
    {
        if(s_task_registry[i].is_active)
        {
            const UBaseType_t watermark_words = uxTaskGetStackHighWaterMark(s_task_registry[i].handle);
            const uint32_t stack_bytes = s_task_registry[i].stack_size_words * sizeof(StackType_t);
            const uint32_t free_bytes = (uint32_t)watermark_words * sizeof(StackType_t);
            uint32_t used_bytes = 0U;
            float used_percent = 0.0f;

            if(stack_bytes >= free_bytes)
            {
                used_bytes = stack_bytes - free_bytes;
                used_percent = ((float)used_bytes / (float)stack_bytes) * 100.0f;
            }

            POOM_PRINTF_I("%s | used=%u/%u bytes (%.1f%%) | min_free=%u bytes",
                          s_task_registry[i].name,
                          (unsigned int)used_bytes,
                          (unsigned int)stack_bytes,
                          used_percent,
                          (unsigned int)free_bytes);
        }
    }
    POOM_PRINTF_I("-------------------------------------------");

    poom_task_unlock_();
}

uint32_t poom_task_get_active_count(void)
{
    uint32_t active_count = 0U;

    if(!s_task_initialized)
    {
        return 0U;
    }

    if(!poom_task_lock_())
    {
        return 0U;
    }

    for(uint32_t i = 0U; i < POOM_TASK_MAX_TASKS; i++)
    {
        if(s_task_registry[i].is_active)
        {
            active_count++;
        }
    }

    poom_task_unlock_();
    return active_count;
}

bool poom_task_get_snapshot(TaskHandle_t handle, poom_task_snapshot_t* snapshot_out)
{
    int slot;
    UBaseType_t watermark_words;

    if((!s_task_initialized) || (handle == NULL) || (snapshot_out == NULL))
    {
        return false;
    }

    if(!poom_task_lock_())
    {
        return false;
    }

    slot = poom_task_find_slot_(handle);
    if(slot < 0)
    {
        poom_task_unlock_();
        return false;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->handle = s_task_registry[slot].handle;
    strncpy(snapshot_out->name, s_task_registry[slot].name, sizeof(snapshot_out->name) - 1U);
    snapshot_out->stack_size_words = s_task_registry[slot].stack_size_words;
    snapshot_out->stack_size_bytes = s_task_registry[slot].stack_size_words * sizeof(StackType_t);
    snapshot_out->priority = s_task_registry[slot].priority;
    snapshot_out->uptime_seconds = (poom_task_now_ms_() - s_task_registry[slot].created_at_ms) / 1000U;
    snapshot_out->active = s_task_registry[slot].is_active;

    watermark_words = uxTaskGetStackHighWaterMark(s_task_registry[slot].handle);
    snapshot_out->stack_high_watermark_words = (uint32_t)watermark_words;
    if((uint32_t)watermark_words <= s_task_registry[slot].stack_size_words)
    {
        snapshot_out->stack_used_bytes =
            snapshot_out->stack_size_bytes - ((uint32_t)watermark_words * sizeof(StackType_t));
    }
    else
    {
        snapshot_out->stack_used_bytes = 0U;
    }

    poom_task_unlock_();
    return true;
}
