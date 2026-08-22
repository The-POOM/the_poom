// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef POOM_WEB_LOG_H
#define POOM_WEB_LOG_H

#include <stdio.h>

#ifndef POOM_WEB_ENABLE_LOG
#define POOM_WEB_ENABLE_LOG (0)
#endif

#ifndef POOM_WEB_DEBUG_LOG_ENABLED
#define POOM_WEB_DEBUG_LOG_ENABLED (0)
#endif

#if POOM_WEB_ENABLE_LOG
    #define POOM_WEB_PRINTF_E(tag, fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WEB_PRINTF_W(tag, fmt, ...) \
        printf("[W] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_WEB_PRINTF_I(tag, fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_WEB_DEBUG_LOG_ENABLED
        #define POOM_WEB_PRINTF_D(tag, fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", tag, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_WEB_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_WEB_PRINTF_E(...) do { } while (0)
    #define POOM_WEB_PRINTF_W(...) do { } while (0)
    #define POOM_WEB_PRINTF_I(...) do { } while (0)
    #define POOM_WEB_PRINTF_D(...) do { } while (0)
#endif

/* Compatibility aliases */
#ifndef POOM_CLI_WEB_ENABLE_LOG
#define POOM_CLI_WEB_ENABLE_LOG POOM_WEB_ENABLE_LOG
#endif
#ifndef POOM_CLI_WEB_DEBUG_LOG_ENABLED
#define POOM_CLI_WEB_DEBUG_LOG_ENABLED POOM_WEB_DEBUG_LOG_ENABLED
#endif

#define POOM_CLI_WEB_PRINTF_E POOM_WEB_PRINTF_E
#define POOM_CLI_WEB_PRINTF_W POOM_WEB_PRINTF_W
#define POOM_CLI_WEB_PRINTF_I POOM_WEB_PRINTF_I
#define POOM_CLI_WEB_PRINTF_D POOM_WEB_PRINTF_D

#endif /* POOM_WEB_LOG_H */
