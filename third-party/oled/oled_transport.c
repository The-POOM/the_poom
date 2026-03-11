#include "oled_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i2c.h"

#define OLED_HEIGHT_32                (32)
#define OLED_HEIGHT_64                (64)
#define OLED_PAGES_FOR_32PX           (4)
#define OLED_PAGES_FOR_64PX           (8)
#define OLED_COLUMN_LOW_MASK          (0x0FU)
#define OLED_COLUMN_HIGH_SHIFT        (4U)
#define OLED_COLUMN_HIGH_MASK         (0x0FU)
#define OLED_COLUMN_HIGH_BASE         (0x10U)
#define OLED_PAGE_SET_BASE            (0xB0U)
#define OLED_INIT_MUX_FOR_64PX        (0x3FU)
#define OLED_INIT_MUX_FOR_32PX        (0x1FU)
#define OLED_INIT_DISPLAY_OFFSET      (0x00U)
#define OLED_INIT_CLOCK_DIV           (0x80U)
#define OLED_INIT_COM_PIN_64PX        (0x12U)
#define OLED_INIT_COM_PIN_32PX        (0x02U)
#define OLED_INIT_DEFAULT_CONTRAST    (0xFFU)
#define OLED_INIT_VCOMH_LEVEL         (0x40U)
#define OLED_INIT_ADDR_LOW            (0x00U)
#define OLED_INIT_ADDR_HIGH           (0x10U)
#define OLED_CHARGE_PUMP_ENABLE       (0x14U)

static const char *OLED_TRANSPORT_TAG = "oled_transport";

#if defined(CONFIG_OLED_DRIVER_ENABLE_LOG) && CONFIG_OLED_DRIVER_ENABLE_LOG

#define OLED_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", OLED_TRANSPORT_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", OLED_TRANSPORT_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", OLED_TRANSPORT_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define OLED_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", OLED_TRANSPORT_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define OLED_PRINTF_E(...)
#define OLED_PRINTF_W(...)
#define OLED_PRINTF_I(...)
#define OLED_PRINTF_D(...)

#endif

/**
 * @brief Convert page index according to configured display flip mode.
 */
static int oled_transport_get_page_index_(const oled_driver_t *dev, int page)
{
    if (dev->_flip)
    {
        return (dev->_pages - page) - 1;
    }
    return page;
}

/**
 * @brief Build command bytes used to position cursor before writing image data.
 */
static void oled_transport_build_page_setup_cmd_(int page, int seg, uint8_t *cmd, size_t cmd_len)
{
    uint8_t column_low = (uint8_t)seg & OLED_COLUMN_LOW_MASK;
    uint8_t column_high = ((uint8_t)seg >> OLED_COLUMN_HIGH_SHIFT) & OLED_COLUMN_HIGH_MASK;

    if ((cmd == NULL) || (cmd_len < 4U))
    {
        return;
    }

    cmd[0] = OLED_CONTROL_BYTE_CMD_STREAM;
    cmd[1] = OLED_INIT_ADDR_LOW + column_low;
    cmd[2] = OLED_COLUMN_HIGH_BASE + column_high;
    cmd[3] = OLED_PAGE_SET_BASE | ((uint8_t)page);
}

void oled_transport_init(oled_driver_t *dev, int width, int height)
{
    uint8_t init_seq[] = {
        OLED_CONTROL_BYTE_CMD_STREAM,
        OLED_CMD_DISPLAY_OFF,
        OLED_CMD_SET_MUX_RATIO,
        (uint8_t)((height == OLED_HEIGHT_64) ? OLED_INIT_MUX_FOR_64PX : OLED_INIT_MUX_FOR_32PX),
        OLED_CMD_SET_DISPLAY_OFFSET, OLED_INIT_DISPLAY_OFFSET,
        OLED_CMD_SET_DISPLAY_START_LINE,
        OLED_CMD_SET_SEGMENT_REMAP_1,
        OLED_CMD_SET_COM_SCAN_MODE,
        OLED_CMD_SET_DISPLAY_CLK_DIV, OLED_INIT_CLOCK_DIV,
        OLED_CMD_SET_COM_PIN_MAP, (uint8_t)((height == OLED_HEIGHT_64) ? OLED_INIT_COM_PIN_64PX : OLED_INIT_COM_PIN_32PX),
        OLED_CMD_SET_CONTRAST, OLED_INIT_DEFAULT_CONTRAST,
        OLED_CMD_DISPLAY_RAM,
        OLED_CMD_SET_VCOMH_DESELCT, OLED_INIT_VCOMH_LEVEL,
        OLED_CMD_SET_MEMORY_ADDR_MODE, OLED_CMD_SET_PAGE_ADDR_MODE,
        OLED_INIT_ADDR_LOW, OLED_INIT_ADDR_HIGH,
        OLED_CMD_SET_CHARGE_PUMP, OLED_CHARGE_PUMP_ENABLE,
        OLED_CMD_DEACTIVE_SCROLL,
        OLED_CMD_DISPLAY_NORMAL,
        OLED_CMD_DISPLAY_ON
    };
    int err;

    if (dev == NULL)
    {
        OLED_PRINTF_E("null device context");
        return;
    }

    dev->_width = width;
    dev->_height = height;
    dev->_pages = (height == OLED_HEIGHT_32) ? OLED_PAGES_FOR_32PX : OLED_PAGES_FOR_64PX;
    dev->_address = OLED_I2C_ADDRESS_DEFAULT;
    dev->_flip = false;

    err = i2c_register_device((uint8_t)dev->_address);
    if (err != I2C_STATUS_OK)
    {
        OLED_PRINTF_E("failed to register OLED device (addr=0x%02X)", (unsigned)dev->_address);
        return;
    }

    err = i2c_tx_dev((uint8_t)dev->_address, init_seq, (uint16_t)sizeof(init_seq), true, true);
    if (err != I2C_STATUS_OK)
    {
        OLED_PRINTF_E("OLED init transmit failed (%d)", err);
        return;
    }

    OLED_PRINTF_I("OLED initialized (%dx%d)", width, height);
}

void oled_transport_display_image(oled_driver_t *dev,
                                  int page,
                                  int seg,
                                  const uint8_t *images,
                                  int width)
{
    uint8_t page_setup_cmd[4];
    uint8_t *tx_buf = NULL;
    int mapped_page;

    if ((dev == NULL) || (images == NULL))
    {
        return;
    }
    if ((page < 0) || (page >= dev->_pages) || (seg < 0) || (seg >= dev->_width) || (width <= 0))
    {
        return;
    }

    mapped_page = oled_transport_get_page_index_(dev, page);
    oled_transport_build_page_setup_cmd_(mapped_page, seg, page_setup_cmd, sizeof(page_setup_cmd));

    (void)i2c_tx_dev((uint8_t)dev->_address, page_setup_cmd, (uint16_t)sizeof(page_setup_cmd), true, true);

    tx_buf = (uint8_t *)malloc((size_t)width + 1U);
    if (tx_buf == NULL)
    {
        OLED_PRINTF_W("failed to alloc transport TX buffer");
        return;
    }

    tx_buf[0] = OLED_CONTROL_BYTE_DATA_STREAM;
    (void)memcpy(&tx_buf[1], images, (size_t)width);
    (void)i2c_tx_dev((uint8_t)dev->_address, tx_buf, (uint16_t)((size_t)width + 1U), true, true);
    free(tx_buf);
}
