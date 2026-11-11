#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 4
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t esp_get_free_heap_size(void);
#define MALLOC_CAP_INTERNAL 2048
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps);
void heap_caps_free(void *p);
