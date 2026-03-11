// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#ifndef BSP_PONG_H
#define BSP_PONG_H

#include "sdkconfig.h"

/* Common settings for both supported targets. */
#define I2C_FREQ_HZ  (100000)
#define I2C_PORT_NUM (I2C_NUM_0)
#define PIN_NUM_LEDS (9)

/* Target-specific pin mapping. */
#if defined(CONFIG_IDF_TARGET_ESP32C6)

/* ESP32-C6 */
#define I2C_SCL_PIN (7)
#define I2C_SDA_PIN (6)

#define PIN_NUM_MISO (20)
#define PIN_NUM_MOSI (19)
#define PIN_NUM_CLK  (21)
#define PIN_NUM_CS   (18)

#define PIN_NUM_WS2812 (8)

#define PIN_NUM_A     (9)
#define PIN_NUM_B     (10)
#define PIN_NUM_LEFT  (22)
#define PIN_NUM_RIGHT (23)
#define PIN_NUM_UP    (1)
#define PIN_NUM_DOWN  (15)

#define PIN_NUM_BUZZER  (2)
#define PIN_NUM_INT_NFC (4)

#elif defined(CONFIG_IDF_TARGET_ESP32C5)

/* ESP32-C5 */
#define I2C_SCL_PIN (1)
#define I2C_SDA_PIN (0)

#define PIN_NUM_MISO (8)
#define PIN_NUM_MOSI (4)
#define PIN_NUM_CLK  (6)
#define PIN_NUM_CS   (5)

#define PIN_NUM_WS2812 (27)

#define PIN_NUM_A     (28)
#define PIN_NUM_B     (9)
#define PIN_NUM_LEFT  (3)
#define PIN_NUM_RIGHT (7)
#define PIN_NUM_UP    (24)
#define PIN_NUM_DOWN  (23)

#define PIN_NUM_BUZZER  (26)
#define PIN_NUM_INT_NFC (2)

#else
#error "Unsupported target: BSP supports only ESP32-C6 and ESP32-C5."
#endif

#endif /* BSP_PONG_H */
