/**
 * @file oled_def.h
 * @brief SSD1306 command definitions and OLED context types.
 *
 * References:
 * - SSD1306 datasheet command tables (fundamental, addressing, hardware config).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Control byte for command stream transfer. */
#define OLED_CONTROL_BYTE_CMD_STREAM  (0x00U)
/** @brief Control byte for data stream transfer. */
#define OLED_CONTROL_BYTE_DATA_STREAM (0x40U)

/** @name Fundamental Commands */
/** @{ */
#define OLED_CMD_SET_CONTRAST         (0x81U)
#define OLED_CMD_DISPLAY_RAM          (0xA4U)
#define OLED_CMD_DISPLAY_NORMAL       (0xA6U)
#define OLED_CMD_DISPLAY_OFF          (0xAEU)
#define OLED_CMD_DISPLAY_ON           (0xAFU)
/** @} */

/** @name Addressing Commands */
/** @{ */
#define OLED_CMD_SET_MEMORY_ADDR_MODE (0x20U)
#define OLED_CMD_SET_PAGE_ADDR_MODE   (0x02U)
/** @} */

/** @name Hardware Configuration Commands */
/** @{ */
#define OLED_CMD_SET_DISPLAY_START_LINE (0x40U)
#define OLED_CMD_SET_SEGMENT_REMAP_1    (0xA1U)
#define OLED_CMD_SET_MUX_RATIO          (0xA8U)
#define OLED_CMD_SET_COM_SCAN_MODE      (0xC8U)
#define OLED_CMD_SET_DISPLAY_OFFSET     (0xD3U)
#define OLED_CMD_SET_COM_PIN_MAP        (0xDAU)
/** @} */

/** @name Timing and Driving Commands */
/** @{ */
#define OLED_CMD_SET_DISPLAY_CLK_DIV (0xD5U)
#define OLED_CMD_SET_VCOMH_DESELCT   (0xDBU)
/** @} */

/** @brief Charge pump command. */
#define OLED_CMD_SET_CHARGE_PUMP (0x8DU)

/** @name Scrolling Commands */
/** @{ */
#define OLED_CMD_DEACTIVE_SCROLL   (0x2EU)
/** @} */

/** @brief Default 7-bit OLED I2C slave address. */
#define OLED_I2C_ADDRESS_DEFAULT (0x3CU)

/** @brief Number of segment bytes per page for 128px-wide OLED panels. */
#define OLED_PAGE_SEGMENTS_MAX (128U)
/** @brief Maximum number of pages for a 64px-high OLED panel. */
#define OLED_PAGES_MAX (8U)

/**
 * @brief One display page buffer (8 vertical pixels x full width).
 */
typedef struct
{
    uint8_t _segs[OLED_PAGE_SEGMENTS_MAX];
} PAGE_t;

/**
 * @brief OLED runtime state and software framebuffer.
 */
typedef struct
{
    int _address;
    int _width;
    int _height;
    int _pages;
    PAGE_t _page[OLED_PAGES_MAX];
    bool _flip;
} oled_driver_t;
