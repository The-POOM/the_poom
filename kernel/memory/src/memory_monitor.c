// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "memory_monitor.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* POOM_MEMORY_MONITOR_TAG = "poom_memory_monitor";

#if defined(CONFIG_POOM_MEMORY_MONITOR_ENABLE_LOG) && CONFIG_POOM_MEMORY_MONITOR_ENABLE_LOG

#define POOM_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", POOM_MEMORY_MONITOR_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", POOM_MEMORY_MONITOR_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", POOM_MEMORY_MONITOR_TAG, __func__, __LINE__, ##__VA_ARGS__)
#define POOM_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", POOM_MEMORY_MONITOR_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define POOM_PRINTF_E(...) \
    do {                   \
        (void)POOM_MEMORY_MONITOR_TAG; \
    } while(0)
#define POOM_PRINTF_W(...) \
    do {                   \
        (void)POOM_MEMORY_MONITOR_TAG; \
    } while(0)
#define POOM_PRINTF_I(...) \
    do {                   \
        (void)POOM_MEMORY_MONITOR_TAG; \
    } while(0)
#define POOM_PRINTF_D(...) \
    do {                   \
        (void)POOM_MEMORY_MONITOR_TAG; \
    } while(0)

#endif

static SemaphoreHandle_t s_monitor_mutex = NULL;
static bool s_is_initialized = false;
static bool s_is_running = false;
static esp_timer_handle_t s_periodic_timer = NULL;
static mem_monitor_cb_t s_event_callback = NULL;

static size_t s_threshold_warn = 50U * 1024U;
static size_t s_threshold_crit = 30U * 1024U;
static size_t s_threshold_emer = 15U * 1024U;

static mem_monitor_level_t s_last_severity = MEM_MON_LEVEL_HEALTHY;

static mem_monitor_level_t mem_monitor_evaluate_severity_(size_t free_bytes)
{
    if(free_bytes < s_threshold_emer)
    {
        return MEM_MON_LEVEL_EMERGENCY;
    }
    if(free_bytes < s_threshold_crit)
    {
        return MEM_MON_LEVEL_CRITICAL;
    }
    if(free_bytes < s_threshold_warn)
    {
        return MEM_MON_LEVEL_WARNING;
    }
    return MEM_MON_LEVEL_HEALTHY;
}

static void mem_monitor_timer_handler_(void* arg)
{
    const char* level_strs[] = {"HEALTHY", "WARNING", "CRITICAL", "EMERGENCY"};
    mem_monitor_stats_t stats;
    mem_monitor_level_t current_severity;

    (void)arg;

    stats = mem_monitor_get_current_stats();
    current_severity = mem_monitor_evaluate_severity_(stats.free_bytes);

    if(current_severity == s_last_severity)
    {
        return;
    }

    if(current_severity > MEM_MON_LEVEL_HEALTHY)
    {
        POOM_PRINTF_W("memory level changed -> %s | free=%u KB | frag=%.1f%%",
                      level_strs[current_severity],
                      (unsigned int)(stats.free_bytes / 1024U),
                      stats.fragmentation_ratio);
    }
    else
    {
        POOM_PRINTF_I("memory recovered -> %s | free=%u KB",
                      level_strs[current_severity],
                      (unsigned int)(stats.free_bytes / 1024U));
    }

    if((s_monitor_mutex != NULL) && (xSemaphoreTake(s_monitor_mutex, 0) == pdTRUE))
    {
        if(s_event_callback != NULL)
        {
            s_event_callback(current_severity, &stats);
        }
        (void)xSemaphoreGive(s_monitor_mutex);
    }

    s_last_severity = current_severity;
}

esp_err_t mem_monitor_init(void)
{
    if(s_is_initialized)
    {
        return ESP_OK;
    }

    s_monitor_mutex = xSemaphoreCreateMutex();
    if(s_monitor_mutex == NULL)
    {
        POOM_PRINTF_E("failed to create monitor mutex");
        return ESP_ERR_NO_MEM;
    }

    s_last_severity = mem_monitor_evaluate_severity_(mem_monitor_get_current_stats().free_bytes);
    s_is_initialized = true;
    POOM_PRINTF_I("memory monitor initialized");
    return ESP_OK;
}

esp_err_t mem_monitor_start(uint32_t period_ms)
{
    esp_timer_create_args_t timer_args;
    esp_err_t err;

    if(!s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(period_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if(s_is_running)
    {
        return ESP_OK;
    }

    timer_args.callback = mem_monitor_timer_handler_;
    timer_args.arg = NULL;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "mem_mon_timer";
    timer_args.skip_unhandled_events = false;

    err = esp_timer_create(&timer_args, &s_periodic_timer);
    if(err != ESP_OK)
    {
        return err;
    }

    err = esp_timer_start_periodic(s_periodic_timer, (uint64_t)period_ms * 1000ULL);
    if(err != ESP_OK)
    {
        (void)esp_timer_delete(s_periodic_timer);
        s_periodic_timer = NULL;
        return err;
    }

    s_is_running = true;
    POOM_PRINTF_I("memory monitor started (period=%lu ms)", (unsigned long)period_ms);
    return ESP_OK;
}

void mem_monitor_stop(void)
{
    if(!s_is_running || (s_periodic_timer == NULL))
    {
        return;
    }

    (void)esp_timer_stop(s_periodic_timer);
    (void)esp_timer_delete(s_periodic_timer);
    s_periodic_timer = NULL;
    s_is_running = false;
    POOM_PRINTF_I("memory monitor stopped");
}

mem_monitor_stats_t mem_monitor_get_current_stats(void)
{
    mem_monitor_stats_t stats = {0};
    multi_heap_info_t info;

    heap_caps_get_info(&info, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    stats.free_bytes = info.total_free_bytes;
    stats.allocated_bytes = info.total_allocated_bytes;
    stats.largest_contig_block = info.largest_free_block;
    stats.min_free_watermark = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    if(stats.free_bytes > 0U)
    {
        stats.fragmentation_ratio = (1.0f - ((float)stats.largest_contig_block / (float)stats.free_bytes)) * 100.0f;
    }
    else
    {
        stats.fragmentation_ratio = 100.0f;
    }

    return stats;
}

void mem_monitor_register_callback(mem_monitor_cb_t callback)
{
    if(s_monitor_mutex == NULL)
    {
        return;
    }

    if(xSemaphoreTake(s_monitor_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_event_callback = callback;
        (void)xSemaphoreGive(s_monitor_mutex);
    }
}

void mem_monitor_set_thresholds_kb(size_t warn_kb, size_t crit_kb, size_t emer_kb)
{
    const size_t one_kb = 1024U;

    if(s_monitor_mutex == NULL)
    {
        return;
    }

    if((warn_kb == 0U) || (crit_kb == 0U) || (emer_kb == 0U))
    {
        return;
    }

    if(warn_kb < crit_kb)
    {
        warn_kb = crit_kb;
    }
    if(crit_kb < emer_kb)
    {
        crit_kb = emer_kb;
    }

    if(xSemaphoreTake(s_monitor_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_threshold_warn = warn_kb * one_kb;
        s_threshold_crit = crit_kb * one_kb;
        s_threshold_emer = emer_kb * one_kb;
        (void)xSemaphoreGive(s_monitor_mutex);
    }

    POOM_PRINTF_I("thresholds updated (KB): warn=%u crit=%u emer=%u",
                  (unsigned int)warn_kb,
                  (unsigned int)crit_kb,
                  (unsigned int)emer_kb);
}

mem_monitor_level_t mem_monitor_get_last_level(void)
{
    return s_last_severity;
}

void mem_monitor_log_report(void)
{
    mem_monitor_stats_t stats = mem_monitor_get_current_stats();

    POOM_PRINTF_I("---------------- MEMORY REPORT ----------------");
    POOM_PRINTF_I("Free bytes         : %u", (unsigned int)stats.free_bytes);
    POOM_PRINTF_I("Allocated bytes    : %u", (unsigned int)stats.allocated_bytes);
    POOM_PRINTF_I("Largest free block : %u", (unsigned int)stats.largest_contig_block);
    POOM_PRINTF_I("Min free watermark : %u", (unsigned int)stats.min_free_watermark);
    POOM_PRINTF_I("Fragmentation      : %.2f%%", stats.fragmentation_ratio);
    POOM_PRINTF_I("-----------------------------------------------");
}
