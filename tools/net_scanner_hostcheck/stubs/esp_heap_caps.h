#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT (1u<<2)
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
