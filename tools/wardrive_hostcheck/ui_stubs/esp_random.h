#pragma once
#include <stdint.h>
#include <stddef.h>
uint32_t esp_random(void);
void esp_fill_random(void *,size_t);
