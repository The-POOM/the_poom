#pragma once
#include <stdint.h>
typedef void *TaskHandle_t; typedef void *QueueHandle_t; typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t; typedef int BaseType_t; typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
