#pragma once

#include <stdio.h>

#if CONFIG_POOM_DFU_ENABLE_LOG

#define POOM_DFU_PRINTF_E(tag, fmt, ...) \
  printf("[E] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_DFU_PRINTF_W(tag, fmt, ...) \
  printf("[W] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_DFU_PRINTF_I(tag, fmt, ...) \
  printf("[I] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

#define POOM_DFU_PRINTF_D(tag, fmt, ...) \
  printf("[D] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

#else

#define POOM_DFU_PRINTF_E(...)
#define POOM_DFU_PRINTF_W(...)
#define POOM_DFU_PRINTF_I(...)
#define POOM_DFU_PRINTF_D(...)

#endif
