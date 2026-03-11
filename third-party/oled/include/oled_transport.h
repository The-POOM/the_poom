/**
 * @file oled_transport.h
 * @brief OLED transport layer that writes controller frames through I2C.
 */

#pragma once

#include <stdint.h>

#include "oled_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize OLED transport state and send controller init sequence.
 *
 * @param dev OLED device context.
 * @param width Display width in pixels.
 * @param height Display height in pixels.
 */
void oled_transport_init(oled_driver_t *dev, int width, int height);

/**
 * @brief Write an image slice to one OLED page and segment offset.
 *
 * @param dev OLED device context.
 * @param page Page index.
 * @param seg Segment index.
 * @param images Image bytes.
 * @param width Number of bytes to write.
 */
void oled_transport_display_image(oled_driver_t *dev,
                                  int page,
                                  int seg,
                                  const uint8_t *images,
                                  int width);

#ifdef __cplusplus
}
#endif
