// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "memory_tracker.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "freertos/semphr.h"

#define MEM_TRACKER_MAX_ALLOCS 256U
#define MEM_TRACKER_MAX_TASKS 32U

typedef struct {
    void* ptr;
    size_t size;
    TaskHandle_t owner;
    const char* file;
    uint32_t line;
    uint32_t ts_ms;
    bool in_use;
} mem_tracker_alloc_rec_t;

static const char* POOM_MEMORY_TRACKER_TAG = "poom_memory_tracker";

#if defined(CONFIG_POOM_MEMORY_TRACKER_ENABLE_LOG) && CONFIG_POOM_MEMORY_TRACKER_ENABLE_LOG

#define POOM_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_MEMORY_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_MEMORY_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_MEMORY_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_MEMORY_TRACKER_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define POOM_PRINTF_E(...) \
    do {                   \
        (void)POOM_MEMORY_TRACKER_TAG; \
    } while(0)
#define POOM_PRINTF_W(...) \
    do {                   \
        (void)POOM_MEMORY_TRACKER_TAG; \
    } while(0)
#define POOM_PRINTF_I(...) \
    do {                   \
        (void)POOM_MEMORY_TRACKER_TAG; \
    } while(0)
#define POOM_PRINTF_D(...) \
    do {                   \
        (void)POOM_MEMORY_TRACKER_TAG; \
    } while(0)

#endif

static SemaphoreHandle_t s_mem_tracker_mutex = NULL;
static bool s_mem_tracker_initialized = false;

static mem_tracker_alloc_rec_t s_alloc_registry[MEM_TRACKER_MAX_ALLOCS];
static mem_tracker_task_stats_t s_task_registry[MEM_TRACKER_MAX_TASKS];

static mem_tracker_global_stats_t s_global_stats;

static bool mem_tracker_lock_(void)
{
    return (s_mem_tracker_mutex != NULL) && (xSemaphoreTake(s_mem_tracker_mutex, portMAX_DELAY) == pdTRUE);
}

static void mem_tracker_unlock_(void)
{
    if(s_mem_tracker_mutex != NULL)
    {
        (void)xSemaphoreGive(s_mem_tracker_mutex);
    }
}

static uint32_t mem_tracker_now_ms_(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static int mem_tracker_find_alloc_slot_by_ptr_(void* ptr)
{
    for(uint32_t i = 0U; i < MEM_TRACKER_MAX_ALLOCS; i++)
    {
        if(s_alloc_registry[i].in_use && (s_alloc_registry[i].ptr == ptr))
        {
            return (int)i;
        }
    }
    return -1;
}

static int mem_tracker_find_free_alloc_slot_(void)
{
    for(uint32_t i = 0U; i < MEM_TRACKER_MAX_ALLOCS; i++)
    {
        if(!s_alloc_registry[i].in_use)
        {
            return (int)i;
        }
    }
    return -1;
}

static int mem_tracker_find_task_slot_(TaskHandle_t task_handle)
{
    for(uint32_t i = 0U; i < MEM_TRACKER_MAX_TASKS; i++)
    {
        if(s_task_registry[i].active && (s_task_registry[i].task_handle == task_handle))
        {
            return (int)i;
        }
    }
    return -1;
}

static int mem_tracker_find_free_task_slot_(void)
{
    for(uint32_t i = 0U; i < MEM_TRACKER_MAX_TASKS; i++)
    {
        if(!s_task_registry[i].active)
        {
            return (int)i;
        }
    }
    return -1;
}

static int mem_tracker_get_or_create_task_slot_(TaskHandle_t task_handle, const char* task_name)
{
    int task_slot = mem_tracker_find_task_slot_(task_handle);

    if(task_slot >= 0)
    {
        return task_slot;
    }

    task_slot = mem_tracker_find_free_task_slot_();
    if(task_slot < 0)
    {
        return -1;
    }

    memset(&s_task_registry[task_slot], 0, sizeof(s_task_registry[task_slot]));
    s_task_registry[task_slot].task_handle = task_handle;
    s_task_registry[task_slot].active = true;

    if(task_name != NULL)
    {
        strncpy(s_task_registry[task_slot].task_name, task_name, sizeof(s_task_registry[task_slot].task_name) - 1U);
    }
    else if(task_handle != NULL)
    {
        const char* runtime_name = pcTaskGetName(task_handle);
        if(runtime_name != NULL)
        {
            strncpy(s_task_registry[task_slot].task_name,
                    runtime_name,
                    sizeof(s_task_registry[task_slot].task_name) - 1U);
        }
    }

    return task_slot;
}

esp_err_t mem_tracker_init(void)
{
    if(s_mem_tracker_initialized)
    {
        return ESP_OK;
    }

    s_mem_tracker_mutex = xSemaphoreCreateMutex();
    if(s_mem_tracker_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(s_alloc_registry, 0, sizeof(s_alloc_registry));
    memset(s_task_registry, 0, sizeof(s_task_registry));
    memset(&s_global_stats, 0, sizeof(s_global_stats));

    s_mem_tracker_initialized = true;
    POOM_PRINTF_I("memory tracker initialized");
    return ESP_OK;
}

esp_err_t mem_tracker_task_begin(TaskHandle_t task_handle, const char* task_name)
{
    if(!s_mem_tracker_initialized)
    {
        esp_err_t err = mem_tracker_init();
        if(err != ESP_OK)
        {
            return err;
        }
    }

    if(task_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(!mem_tracker_lock_())
    {
        return ESP_FAIL;
    }

    if(mem_tracker_get_or_create_task_slot_(task_handle, task_name) < 0)
    {
        mem_tracker_unlock_();
        return ESP_ERR_NO_MEM;
    }

    mem_tracker_unlock_();
    return ESP_OK;
}

esp_err_t mem_tracker_task_end(TaskHandle_t task_handle, bool free_remaining_allocations)
{
    void* pending_ptrs[MEM_TRACKER_MAX_ALLOCS];
    uint32_t pending_count = 0U;
    int task_slot;

    if((!s_mem_tracker_initialized) || (task_handle == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(!mem_tracker_lock_())
    {
        return ESP_FAIL;
    }

    task_slot = mem_tracker_find_task_slot_(task_handle);
    if(task_slot < 0)
    {
        mem_tracker_unlock_();
        return ESP_ERR_NOT_FOUND;
    }

    for(uint32_t i = 0U; i < MEM_TRACKER_MAX_ALLOCS; i++)
    {
        if(s_alloc_registry[i].in_use && (s_alloc_registry[i].owner == task_handle))
        {
            if(free_remaining_allocations)
            {
                pending_ptrs[pending_count++] = s_alloc_registry[i].ptr;

                s_global_stats.current_bytes -= s_alloc_registry[i].size;
                if(s_global_stats.open_allocations > 0U)
                {
                    s_global_stats.open_allocations--;
                }
                s_global_stats.frees++;

                if(s_task_registry[task_slot].current_bytes >= s_alloc_registry[i].size)
                {
                    s_task_registry[task_slot].current_bytes -= s_alloc_registry[i].size;
                }
                if(s_task_registry[task_slot].open_allocations > 0U)
                {
                    s_task_registry[task_slot].open_allocations--;
                }
                s_task_registry[task_slot].frees++;

                memset(&s_alloc_registry[i], 0, sizeof(s_alloc_registry[i]));
            }
        }
    }

    s_task_registry[task_slot].active = false;
    mem_tracker_unlock_();

    if(free_remaining_allocations)
    {
        for(uint32_t i = 0U; i < pending_count; i++)
        {
            free(pending_ptrs[i]);
        }

        if(pending_count > 0U)
        {
            POOM_PRINTF_W("task cleanup freed %u leaked allocations",
                          (unsigned int)pending_count);
        }
    }

    return ESP_OK;
}

void* mem_tracker_malloc_debug(size_t size, const char* file, uint32_t line)
{
    void* ptr;
    TaskHandle_t owner;
    int alloc_slot;
    int task_slot;

    if(size == 0U)
    {
        return NULL;
    }

    if(!s_mem_tracker_initialized)
    {
        if(mem_tracker_init() != ESP_OK)
        {
            return NULL;
        }
    }

    ptr = malloc(size);
    if(ptr == NULL)
    {
        return NULL;
    }

    owner = xTaskGetCurrentTaskHandle();

    if(!mem_tracker_lock_())
    {
        return ptr;
    }

    alloc_slot = mem_tracker_find_free_alloc_slot_();
    if(alloc_slot < 0)
    {
        mem_tracker_unlock_();
        free(ptr);
        POOM_PRINTF_E("allocation registry full");
        return NULL;
    }

    task_slot = mem_tracker_get_or_create_task_slot_(owner, NULL);

    s_alloc_registry[alloc_slot].ptr = ptr;
    s_alloc_registry[alloc_slot].size = size;
    s_alloc_registry[alloc_slot].owner = owner;
    s_alloc_registry[alloc_slot].file = file;
    s_alloc_registry[alloc_slot].line = line;
    s_alloc_registry[alloc_slot].ts_ms = mem_tracker_now_ms_();
    s_alloc_registry[alloc_slot].in_use = true;

    s_global_stats.current_bytes += size;
    if(s_global_stats.current_bytes > s_global_stats.peak_bytes)
    {
        s_global_stats.peak_bytes = s_global_stats.current_bytes;
    }
    s_global_stats.allocations++;
    s_global_stats.open_allocations++;

    if(task_slot >= 0)
    {
        s_task_registry[task_slot].current_bytes += size;
        if(s_task_registry[task_slot].current_bytes > s_task_registry[task_slot].peak_bytes)
        {
            s_task_registry[task_slot].peak_bytes = s_task_registry[task_slot].current_bytes;
        }
        s_task_registry[task_slot].allocations++;
        s_task_registry[task_slot].open_allocations++;
    }

    mem_tracker_unlock_();
    return ptr;
}

void mem_tracker_free(void* ptr)
{
    int alloc_slot;
    int task_slot;
    size_t alloc_size;
    TaskHandle_t owner;

    if(ptr == NULL)
    {
        return;
    }

    if(!s_mem_tracker_initialized)
    {
        free(ptr);
        return;
    }

    if(!mem_tracker_lock_())
    {
        free(ptr);
        return;
    }

    alloc_slot = mem_tracker_find_alloc_slot_by_ptr_(ptr);
    if(alloc_slot < 0)
    {
        mem_tracker_unlock_();
        free(ptr);
        return;
    }

    alloc_size = s_alloc_registry[alloc_slot].size;
    owner = s_alloc_registry[alloc_slot].owner;

    if(s_global_stats.current_bytes >= alloc_size)
    {
        s_global_stats.current_bytes -= alloc_size;
    }
    if(s_global_stats.open_allocations > 0U)
    {
        s_global_stats.open_allocations--;
    }
    s_global_stats.frees++;

    task_slot = mem_tracker_find_task_slot_(owner);
    if(task_slot >= 0)
    {
        if(s_task_registry[task_slot].current_bytes >= alloc_size)
        {
            s_task_registry[task_slot].current_bytes -= alloc_size;
        }
        if(s_task_registry[task_slot].open_allocations > 0U)
        {
            s_task_registry[task_slot].open_allocations--;
        }
        s_task_registry[task_slot].frees++;
    }

    memset(&s_alloc_registry[alloc_slot], 0, sizeof(s_alloc_registry[alloc_slot]));

    mem_tracker_unlock_();
    free(ptr);
}

bool mem_tracker_get_global_stats(mem_tracker_global_stats_t* stats_out)
{
    if((!s_mem_tracker_initialized) || (stats_out == NULL))
    {
        return false;
    }

    if(!mem_tracker_lock_())
    {
        return false;
    }

    *stats_out = s_global_stats;
    mem_tracker_unlock_();
    return true;
}

bool mem_tracker_get_task_stats(TaskHandle_t task_handle, mem_tracker_task_stats_t* stats_out)
{
    int task_slot;

    if((!s_mem_tracker_initialized) || (task_handle == NULL) || (stats_out == NULL))
    {
        return false;
    }

    if(!mem_tracker_lock_())
    {
        return false;
    }

    task_slot = mem_tracker_find_task_slot_(task_handle);
    if(task_slot < 0)
    {
        mem_tracker_unlock_();
        return false;
    }

    *stats_out = s_task_registry[task_slot];
    mem_tracker_unlock_();
    return true;
}

void mem_tracker_log_global_report(void)
{
    mem_tracker_global_stats_t stats;

    if(!mem_tracker_get_global_stats(&stats))
    {
        return;
    }

    POOM_PRINTF_I("------------- MEMORY TRACKER GLOBAL -------------");
    POOM_PRINTF_I("Current bytes    : %u", (unsigned int)stats.current_bytes);
    POOM_PRINTF_I("Peak bytes       : %u", (unsigned int)stats.peak_bytes);
    POOM_PRINTF_I("Allocations      : %u", (unsigned int)stats.allocations);
    POOM_PRINTF_I("Frees            : %u", (unsigned int)stats.frees);
    POOM_PRINTF_I("Open allocations : %u", (unsigned int)stats.open_allocations);
    POOM_PRINTF_I("-----------------------------------------------");
}

void mem_tracker_log_task_report(TaskHandle_t task_handle)
{
    mem_tracker_task_stats_t stats;

    if(!mem_tracker_get_task_stats(task_handle, &stats))
    {
        return;
    }

    POOM_PRINTF_I("-------------- MEMORY TRACKER TASK --------------");
    POOM_PRINTF_I("Task name        : %s", stats.task_name);
    POOM_PRINTF_I("Current bytes    : %u", (unsigned int)stats.current_bytes);
    POOM_PRINTF_I("Peak bytes       : %u", (unsigned int)stats.peak_bytes);
    POOM_PRINTF_I("Allocations      : %u", (unsigned int)stats.allocations);
    POOM_PRINTF_I("Frees            : %u", (unsigned int)stats.frees);
    POOM_PRINTF_I("Open allocations : %u", (unsigned int)stats.open_allocations);
    POOM_PRINTF_I("-----------------------------------------------");
}
