#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define pdPASS 1
