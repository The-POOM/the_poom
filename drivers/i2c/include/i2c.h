// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

/**
 * @file i2c.h
 * @brief Generic shared I2C bus abstraction for project drivers.
 */

#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Generic success status used by this module. */
#define I2C_STATUS_OK (0U)
/** @brief Generic error status used by this module. */
#define I2C_STATUS_ERROR (1U)

/**
 * @brief Initialize the I2C master bus and internal synchronization objects.
 *
 * This function is idempotent.
 *
 * @return I2C_STATUS_OK on success, I2C_STATUS_ERROR on failure.
 */
uint8_t i2c_init(void);

/**
 * @brief Register a 7-bit slave address on the current I2C bus.
 *
 * If the device address is already registered, this function returns success.
 *
 * @param dev_addr 7-bit I2C slave address.
 * @return I2C_STATUS_OK on success, I2C_STATUS_ERROR on failure.
 */
uint8_t i2c_register_device(uint8_t dev_addr);

/**
 * @brief Start a staged I2C transaction by clearing the internal TX buffer.
 */
void i2c_start(void);

/**
 * @brief Append bytes to the staged TX buffer and optionally transmit it.
 *
 * @param dev_addr 7-bit I2C slave address.
 * @param tx_buf Pointer to data to append.
 * @param tx_buf_len Data length in bytes.
 * @param last When true and @p tx_only is true, transmit staged bytes.
 * @param tx_only True for TX-only transfer, false when preparing TX+RX.
 * @return I2C_STATUS_OK on success, I2C_STATUS_ERROR on failure.
 */
uint8_t i2c_tx_dev(uint8_t dev_addr,
                   const uint8_t *tx_buf,
                   uint16_t tx_buf_len,
                   bool last,
                   bool tx_only);

/**
 * @brief Execute TX+RX using the staged TX buffer and clear staged state.
 *
 * @param dev_addr 7-bit I2C slave address.
 * @param rx_buf Output buffer.
 * @param rx_buf_len Output buffer size in bytes.
 * @return I2C_STATUS_OK on success, I2C_STATUS_ERROR on failure.
 */
uint8_t i2c_rx_dev(uint8_t dev_addr, uint8_t *rx_buf, uint16_t rx_buf_len);

/**
 * @brief Execute RX-only transaction.
 *
 * @param dev_addr 7-bit I2C slave address.
 * @param rx_buf Output buffer.
 * @param rx_buf_len Output buffer size in bytes.
 * @return I2C_STATUS_OK on success, I2C_STATUS_ERROR on failure.
 */
uint8_t i2c_rx_only(uint8_t dev_addr, uint8_t *rx_buf, uint16_t rx_buf_len);

/**
 * @brief Acquire communication lock before multi-step shared-bus transfers.
 */
void i2c_lock(void);

/**
 * @brief Release communication lock acquired with i2c_lock().
 */
void i2c_unlock(void);

/**
 * @brief Probe and return all detected addresses on the current I2C bus.
 *
 * The function allocates memory for @p found_addresses with malloc/realloc.
 * Caller must release it with free().
 *
 * @param[out] found_addresses Allocated array with detected addresses.
 * @param[out] num_addresses Number of elements in @p found_addresses.
 */
void i2c_scan_devices(uint8_t **found_addresses, size_t *num_addresses);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_I2C_H */
