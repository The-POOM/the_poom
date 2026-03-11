// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file i2c.c
 * @brief Generic shared I2C bus abstraction for project drivers.
 */

#include "i2c.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_pong.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef I2C_PORT_NUM
#define I2C_PORT_NUM I2C_NUM_0
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 7
#endif

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 6
#endif

#ifndef I2C_FREQ_HZ
#define I2C_FREQ_HZ 100000
#endif

#ifndef I2C_TIMEOUT_MS
#define I2C_TIMEOUT_MS 1000
#endif

#define I2C_GLITCH_IGNORE_COUNT        (7U)
#define I2C_SCAN_FIRST_ADDRESS         (0x01U)
#define I2C_SCAN_LAST_ADDRESS          (0x7EU)
#define I2C_TX_COMMAND_BYTES           (1U)
#define I2C_TX_DATA_BUFFER_BYTES       (512U)
#define I2C_TX_ACCUM_BUFFER_SIZE       (I2C_TX_COMMAND_BYTES + I2C_TX_DATA_BUFFER_BYTES)
#define I2C_MAX_REGISTERED_DEVICES     (4U)

static const char *I2C_TAG = "i2c";

#if defined(CONFIG_I2C_ENABLE_LOG) && CONFIG_I2C_ENABLE_LOG

#define I2C_PRINTF_E(fmt, ...) \
    printf("[E] [%s] %s:%d: " fmt "\n", I2C_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define I2C_PRINTF_W(fmt, ...) \
    printf("[W] [%s] %s:%d: " fmt "\n", I2C_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define I2C_PRINTF_I(fmt, ...) \
    printf("[I] [%s] %s:%d: " fmt "\n", I2C_TAG, __func__, __LINE__, ##__VA_ARGS__)

#define I2C_PRINTF_D(fmt, ...) \
    printf("[D] [%s] %s:%d: " fmt "\n", I2C_TAG, __func__, __LINE__, ##__VA_ARGS__)

#else

#define I2C_PRINTF_E(...)
#define I2C_PRINTF_W(...)
#define I2C_PRINTF_I(...)
#define I2C_PRINTF_D(...)

#endif

typedef struct
{
    uint8_t addr;
    i2c_master_dev_handle_t handle;
} i2c_device_slot_t;

static uint8_t s_tx_accum_buffer[I2C_TX_ACCUM_BUFFER_SIZE];
static uint16_t s_tx_accum_len = 0U;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static SemaphoreHandle_t s_i2c_lock = NULL;
static i2c_device_slot_t s_devices[I2C_MAX_REGISTERED_DEVICES] = {0};

/**
 * @brief Convert timeout from milliseconds to RTOS ticks.
 */
static uint32_t i2c_timeout_ticks_(void)
{
    return pdMS_TO_TICKS(I2C_TIMEOUT_MS);
}

/**
 * @brief Return an already registered device handle by address.
 */
static i2c_master_dev_handle_t i2c_find_device_(uint8_t addr)
{
    size_t i = 0U;

    for (i = 0U; i < I2C_MAX_REGISTERED_DEVICES; i++)
    {
        if ((s_devices[i].handle != NULL) && (s_devices[i].addr == addr))
        {
            return s_devices[i].handle;
        }
    }
    return NULL;
}

/**
 * @brief Reset staged TX buffer state.
 */
static void i2c_reset_staged_tx_(void)
{
    s_tx_accum_len = 0U;
    (void)memset(s_tx_accum_buffer, 0, sizeof(s_tx_accum_buffer));
}

/**
 * @brief Append data to staged TX buffer.
 */
static uint8_t i2c_stage_tx_(const uint8_t *tx_buf, uint16_t tx_buf_len)
{
    if ((tx_buf == NULL) || (tx_buf_len == 0U))
    {
        return I2C_STATUS_OK;
    }

    if ((uint32_t)s_tx_accum_len + (uint32_t)tx_buf_len > (uint32_t)I2C_TX_ACCUM_BUFFER_SIZE)
    {
        I2C_PRINTF_E("TX staging overflow (current=%u add=%u max=%u)",
                          (unsigned)s_tx_accum_len,
                          (unsigned)tx_buf_len,
                          (unsigned)I2C_TX_ACCUM_BUFFER_SIZE);
        i2c_reset_staged_tx_();
        return I2C_STATUS_ERROR;
    }

    (void)memcpy(&s_tx_accum_buffer[s_tx_accum_len], tx_buf, tx_buf_len);
    s_tx_accum_len = (uint16_t)(s_tx_accum_len + tx_buf_len);
    return I2C_STATUS_OK;
}

uint8_t i2c_init(void)
{
    if (s_i2c_lock == NULL)
    {
        s_i2c_lock = xSemaphoreCreateMutex();
        if (s_i2c_lock == NULL)
        {
            I2C_PRINTF_E("failed to create I2C mutex");
            return I2C_STATUS_ERROR;
        }
    }

    if (s_i2c_bus == NULL)
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_PORT_NUM,
            .scl_io_num = I2C_SCL_PIN,
            .sda_io_num = I2C_SDA_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = I2C_GLITCH_IGNORE_COUNT,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK)
        {
            I2C_PRINTF_E("i2c_new_master_bus failed (%d)", (int)err);
            return I2C_STATUS_ERROR;
        }
        I2C_PRINTF_I("I2C bus initialized");
    }

    return I2C_STATUS_OK;
}

uint8_t i2c_register_device(uint8_t dev_addr)
{
    size_t i = 0U;

    if (s_i2c_bus == NULL)
    {
        I2C_PRINTF_E("I2C bus not initialized");
        return I2C_STATUS_ERROR;
    }

    if (i2c_find_device_(dev_addr) != NULL)
    {
        return I2C_STATUS_OK;
    }

    for (i = 0U; i < I2C_MAX_REGISTERED_DEVICES; i++)
    {
        if (s_devices[i].handle == NULL)
        {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = dev_addr,
                .scl_speed_hz = I2C_FREQ_HZ,
            };
            esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_devices[i].handle);
            if (err != ESP_OK)
            {
                I2C_PRINTF_E("add_device failed (addr=0x%02X err=%d)", dev_addr, (int)err);
                return I2C_STATUS_ERROR;
            }
            s_devices[i].addr = dev_addr;
            I2C_PRINTF_D("device registered at 0x%02X", dev_addr);
            return I2C_STATUS_OK;
        }
    }

    I2C_PRINTF_E("no free slot for I2C device 0x%02X", dev_addr);
    return I2C_STATUS_ERROR;
}

void i2c_start(void)
{
    i2c_reset_staged_tx_();
}

uint8_t i2c_tx_dev(uint8_t dev_addr,
                   const uint8_t *tx_buf,
                   uint16_t tx_buf_len,
                   bool last,
                   bool tx_only)
{
    i2c_master_dev_handle_t dev = i2c_find_device_(dev_addr);

    if (dev == NULL)
    {
        I2C_PRINTF_E("unknown device 0x%02X", dev_addr);
        return I2C_STATUS_ERROR;
    }

    if (i2c_stage_tx_(tx_buf, tx_buf_len) != I2C_STATUS_OK)
    {
        return I2C_STATUS_ERROR;
    }

    if (last && tx_only)
    {
        esp_err_t err = i2c_master_transmit(dev,
                                            s_tx_accum_buffer,
                                            s_tx_accum_len,
                                            i2c_timeout_ticks_());
        i2c_reset_staged_tx_();
        if (err != ESP_OK)
        {
            I2C_PRINTF_E("transmit failed (addr=0x%02X err=%d)", dev_addr, (int)err);
            return I2C_STATUS_ERROR;
        }
    }

    return I2C_STATUS_OK;
}

uint8_t i2c_rx_dev(uint8_t dev_addr, uint8_t *rx_buf, uint16_t rx_buf_len)
{
    i2c_master_dev_handle_t dev = i2c_find_device_(dev_addr);
    esp_err_t err;

    if ((rx_buf == NULL) || (rx_buf_len == 0U))
    {
        i2c_reset_staged_tx_();
        return I2C_STATUS_OK;
    }

    if (dev == NULL)
    {
        I2C_PRINTF_E("unknown device 0x%02X", dev_addr);
        i2c_reset_staged_tx_();
        return I2C_STATUS_ERROR;
    }

    err = i2c_master_transmit_receive(dev,
                                      s_tx_accum_buffer,
                                      s_tx_accum_len,
                                      rx_buf,
                                      rx_buf_len,
                                      i2c_timeout_ticks_());
    i2c_reset_staged_tx_();

    if (err != ESP_OK)
    {
        I2C_PRINTF_E("transmit_receive failed (addr=0x%02X err=%d)", dev_addr, (int)err);
        return I2C_STATUS_ERROR;
    }
    return I2C_STATUS_OK;
}

uint8_t i2c_rx_only(uint8_t dev_addr, uint8_t *rx_buf, uint16_t rx_buf_len)
{
    i2c_master_dev_handle_t dev = i2c_find_device_(dev_addr);
    esp_err_t err;

    if ((rx_buf == NULL) || (rx_buf_len == 0U))
    {
        return I2C_STATUS_OK;
    }

    if (dev == NULL)
    {
        I2C_PRINTF_E("unknown device 0x%02X", dev_addr);
        return I2C_STATUS_ERROR;
    }

    err = i2c_master_receive(dev, rx_buf, rx_buf_len, i2c_timeout_ticks_());
    if (err != ESP_OK)
    {
        I2C_PRINTF_E("receive failed (addr=0x%02X err=%d)", dev_addr, (int)err);
        return I2C_STATUS_ERROR;
    }
    return I2C_STATUS_OK;
}

void i2c_lock(void)
{
    if (s_i2c_lock != NULL)
    {
        (void)xSemaphoreTake(s_i2c_lock, portMAX_DELAY);
    }
}

void i2c_unlock(void)
{
    if (s_i2c_lock != NULL)
    {
        (void)xSemaphoreGive(s_i2c_lock);
    }
}

void i2c_scan_devices(uint8_t **found_addresses, size_t *num_addresses)
{
    uint8_t address = 0U;

    if ((found_addresses == NULL) || (num_addresses == NULL))
    {
        I2C_PRINTF_E("invalid scan output pointers");
        return;
    }

    *found_addresses = NULL;
    *num_addresses = 0U;

    if (s_i2c_bus == NULL)
    {
        I2C_PRINTF_E("I2C bus not initialized");
        return;
    }

    i2c_lock();
    for (address = I2C_SCAN_FIRST_ADDRESS; address <= I2C_SCAN_LAST_ADDRESS; address++)
    {
        esp_err_t err = i2c_master_probe(s_i2c_bus, address, i2c_timeout_ticks_());
        if (err == ESP_OK)
        {
            uint8_t *tmp = (uint8_t *)realloc(*found_addresses, (*num_addresses + 1U) * sizeof(uint8_t));
            if (tmp == NULL)
            {
                I2C_PRINTF_E("scan realloc failed");
                free(*found_addresses);
                *found_addresses = NULL;
                *num_addresses = 0U;
                break;
            }

            *found_addresses = tmp;
            (*found_addresses)[*num_addresses] = address;
            *num_addresses = *num_addresses + 1U;
            I2C_PRINTF_D("found I2C address 0x%02X", address);
        }
    }
    i2c_unlock();

    I2C_PRINTF_I("scan completed, %u devices found", (unsigned)*num_addresses);
}
