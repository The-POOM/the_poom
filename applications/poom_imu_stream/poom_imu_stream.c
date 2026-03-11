// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM

#include "poom_imu_stream.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lsm6ds3tr-c_reg.h"
#include "i2c.h"

/**
 * @file poom_imu_stream.c
 * @brief IMU stream component implementation for LSM6DS3TR-C over I2C.
 */

/* =========================
 * Local log macros (printf)
 * ========================= */
#if POOM_IMU_STREAM_ENABLE_LOG
    static const char *POOM_IMU_STREAM_TAG = "poom_imu_stream";

    #define POOM_IMU_PRINTF_E(fmt, ...) \
        printf("[E] [%s] %s:%d: " fmt "\n", POOM_IMU_STREAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #define POOM_IMU_PRINTF_I(fmt, ...) \
        printf("[I] [%s] %s:%d: " fmt "\n", POOM_IMU_STREAM_TAG, __func__, __LINE__, ##__VA_ARGS__)

    #if POOM_IMU_STREAM_DEBUG_LOG_ENABLED
        #define POOM_IMU_PRINTF_D(fmt, ...) \
            printf("[D] [%s] %s:%d: " fmt "\n", POOM_IMU_STREAM_TAG, __func__, __LINE__, ##__VA_ARGS__)
    #else
        #define POOM_IMU_PRINTF_D(...) do { } while (0)
    #endif
#else
    #define POOM_IMU_PRINTF_E(...) do { } while (0)
    #define POOM_IMU_PRINTF_I(...) do { } while (0)
    #define POOM_IMU_PRINTF_D(...) do { } while (0)
#endif

/* =========================
 * Local constants/state
 * ========================= */
#define POOM_IMU_I2C_OK                    (0U)
#define POOM_IMU_ADDR7_MASK                (0x7FU)
#define POOM_IMU_DEFAULT_ADDR7             (0x6BU)
#define POOM_IMU_DEVICE_RETRY_DELAY_MS     (500U)
#define POOM_IMU_AXIS_COUNT                (3)

static void poom_imu_platform_delay_(uint32_t millisec);
static int32_t poom_imu_platform_write_(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len);
static int32_t poom_imu_platform_read_(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len);

static uint8_t s_lsm6ds3_addr7 = POOM_IMU_DEFAULT_ADDR7;
static stmdev_ctx_t s_dev_ctx = {
    .write_reg = poom_imu_platform_write_,
    .read_reg = poom_imu_platform_read_,
    .mdelay = poom_imu_platform_delay_,
    .handle = &s_lsm6ds3_addr7,
};

/* =========================
 * Local helpers
 * ========================= */
/**
 * @brief Normalizes a 7-bit I2C address.
 *
 * @param addr7 Raw address value.
 * @return Masked 7-bit address.
 */
static inline uint8_t poom_imu_i2c_norm_addr7_(uint8_t addr7)
{
    return (uint8_t)(addr7 & POOM_IMU_ADDR7_MASK);
}

/**
 * @brief Platform delay callback used by ST driver context.
 *
 * @param millisec Delay time in milliseconds.
 */
static void poom_imu_platform_delay_(uint32_t millisec)
{
    vTaskDelay(pdMS_TO_TICKS(millisec));
}

/**
 * @brief Writes IMU registers over I2C.
 *
 * @param handle Pointer to device address.
 * @param reg Register address.
 * @param bufp Payload buffer to write.
 * @param len Payload length in bytes.
 * @return 0 on success, -1 on error.
 */
static int32_t poom_imu_platform_write_(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len)
{
    if ((handle == NULL) || ((bufp == NULL) && (len != 0U)))
    {
        return -1;
    }

    uint8_t dev_addr7 = *(uint8_t *)handle;
    dev_addr7 = poom_imu_i2c_norm_addr7_(dev_addr7);

    uint8_t ret = i2c_tx_dev(dev_addr7, &reg, 1, false, true);
    if (ret != POOM_IMU_I2C_OK)
    {
        POOM_IMU_PRINTF_D("WRITE reg 0x%02X failed (%u)", (unsigned)reg, (unsigned)ret);
        return -1;
    }

    if (len > 0U)
    {
        ret = i2c_tx_dev(dev_addr7, bufp, len, true, true);
        if (ret != POOM_IMU_I2C_OK)
        {
            POOM_IMU_PRINTF_D("WRITE data len=%u failed (%u)", (unsigned)len, (unsigned)ret);
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Reads IMU registers over I2C.
 *
 * @param handle Pointer to device address.
 * @param reg Register address.
 * @param bufp Output buffer.
 * @param len Number of bytes to read.
 * @return 0 on success, -1 on error.
 */
static int32_t poom_imu_platform_read_(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len)
{
    if ((handle == NULL) || (bufp == NULL) || (len == 0U))
    {
        return -1;
    }

    uint8_t dev_addr7 = *(uint8_t *)handle;
    dev_addr7 = poom_imu_i2c_norm_addr7_(dev_addr7);

    uint8_t ret = i2c_tx_dev(dev_addr7, &reg, 1, false, true);
    if (ret != POOM_IMU_I2C_OK)
    {
        POOM_IMU_PRINTF_D("READ reg 0x%02X failed (%u)", (unsigned)reg, (unsigned)ret);
        return -1;
    }

    ret = i2c_rx_dev(dev_addr7, bufp, len);
    if (ret != POOM_IMU_I2C_OK)
    {
        POOM_IMU_PRINTF_D("READ len=%u failed (%u)", (unsigned)len, (unsigned)ret);
        return -1;
    }

    return 0;
}

/**
 * @brief Initializes the LSM6DS3TR-C and applies runtime configuration.
 */
void poom_imu_stream_init(void)
{
    uint8_t who_am_i = 0U;
    uint8_t rst = 0U;

    i2c_register_device(s_lsm6ds3_addr7);
    lsm6ds3tr_c_device_id_get(&s_dev_ctx, &who_am_i);

    if (who_am_i != LSM6DS3TR_C_ID)
    {
        POOM_IMU_PRINTF_E("LSM6DS3TR-C not found (WHO_AM_I=0x%02X)", (unsigned)who_am_i);
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(POOM_IMU_DEVICE_RETRY_DELAY_MS));
        }
    }

    POOM_IMU_PRINTF_I("LSM6DS3TR-C found (WHO_AM_I=0x%02X)", (unsigned)who_am_i);

    lsm6ds3tr_c_reset_set(&s_dev_ctx, PROPERTY_ENABLE);
    do
    {
        lsm6ds3tr_c_reset_get(&s_dev_ctx, &rst);
    } while (rst != 0U);

    lsm6ds3tr_c_block_data_update_set(&s_dev_ctx, PROPERTY_ENABLE);
    lsm6ds3tr_c_xl_data_rate_set(&s_dev_ctx, LSM6DS3TR_C_XL_ODR_104Hz);
    lsm6ds3tr_c_gy_data_rate_set(&s_dev_ctx, LSM6DS3TR_C_GY_ODR_104Hz);
    lsm6ds3tr_c_xl_full_scale_set(&s_dev_ctx, LSM6DS3TR_C_2g);
    lsm6ds3tr_c_gy_full_scale_set(&s_dev_ctx, LSM6DS3TR_C_2000dps);
    lsm6ds3tr_c_xl_filter_analog_set(&s_dev_ctx, LSM6DS3TR_C_XL_ANA_BW_400Hz);
    lsm6ds3tr_c_xl_lp2_bandwidth_set(&s_dev_ctx, LSM6DS3TR_C_XL_LOW_LAT_LP_ODR_DIV_50);
    lsm6ds3tr_c_gy_band_pass_set(&s_dev_ctx, LSM6DS3TR_C_HP_260mHz_LP1_STRONG);

    POOM_IMU_PRINTF_I("LSM6DS3TR-C initialized successfully");
}

/**
 * @brief Reads a new IMU sample when available.
 *
 * If no new data is available, the function keeps the last valid sample in
 * the output structure and returns false.
 *
 * @param out Output sample pointer.
 * @return true when at least one channel (acc/gyro/temp) has new data.
 * @return false when no new data is available or on invalid arguments.
 */
bool poom_imu_stream_read_data(poom_imu_data_t *out)
{
    static poom_imu_data_t s_last_data = {0};
    bool has_new_data = false;
    lsm6ds3tr_c_reg_t reg = {0};

    if (out == NULL)
    {
        POOM_IMU_PRINTF_E("read_data called with NULL output");
        return false;
    }

    *out = s_last_data;

    lsm6ds3tr_c_status_reg_get(&s_dev_ctx, &reg.status_reg);
    POOM_IMU_PRINTF_D("STATUS=0x%02X xlda=%d gda=%d tda=%d",
                      *(const unsigned char *)&reg.status_reg,
                      reg.status_reg.xlda,
                      reg.status_reg.gda,
                      reg.status_reg.tda);

    if (reg.status_reg.xlda)
    {
        int16_t raw[POOM_IMU_AXIS_COUNT] = {0};
        if (lsm6ds3tr_c_acceleration_raw_get(&s_dev_ctx, raw) == 0)
        {
            for (int i = 0; i < POOM_IMU_AXIS_COUNT; ++i)
            {
                out->acceleration_mg[i] = lsm6ds3tr_c_from_fs2g_to_mg(raw[i]);
            }
            has_new_data = true;
        }
        else
        {
            POOM_IMU_PRINTF_D("ACC read error");
        }
    }

    if (reg.status_reg.gda)
    {
        int16_t raw[POOM_IMU_AXIS_COUNT] = {0};
        if (lsm6ds3tr_c_angular_rate_raw_get(&s_dev_ctx, raw) == 0)
        {
            for (int i = 0; i < POOM_IMU_AXIS_COUNT; ++i)
            {
                out->angular_rate_mdps[i] = lsm6ds3tr_c_from_fs2000dps_to_mdps(raw[i]);
            }
            has_new_data = true;
        }
        else
        {
            POOM_IMU_PRINTF_D("GYRO read error");
        }
    }

    if (reg.status_reg.tda)
    {
        int16_t raw = 0;
        if (lsm6ds3tr_c_temperature_raw_get(&s_dev_ctx, &raw) == 0)
        {
            out->temperature_degC = lsm6ds3tr_c_from_lsb_to_celsius(raw);
            has_new_data = true;
        }
        else
        {
            POOM_IMU_PRINTF_D("TEMP read error");
        }
    }

    if (has_new_data)
    {
        s_last_data = *out;
    }

    return has_new_data;
}
