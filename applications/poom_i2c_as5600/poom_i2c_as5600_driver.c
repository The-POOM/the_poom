// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hugo Trippaers <hugo@trippaers.nl>

#include "poom_i2c_as5600_driver.h"

#include "i2c.h"

#define AS5600_ADDR      (0x36U)

#define AS5600_REG_ZMCO      (0x00U)
#define AS5600_REG_ZPOS      (0x01U)
#define AS5600_REG_MPOS      (0x03U)
#define AS5600_REG_MANG      (0x05U)
#define AS5600_REG_CONF      (0x07U)
#define AS5600_REG_STATUS    (0x0BU)
#define AS5600_REG_RAW_ANGLE (0x0CU)
#define AS5600_REG_AGC       (0x1AU)
#define AS5600_REG_MAGNITUDE (0x1BU)

// Generic register read: stage the register address, then a combined
// write-read. The AS5600 auto-increments the register pointer, so
// multi-byte reads of the 16-bit registers work in one transaction.
static bool as5600_read_reg(uint8_t reg, uint8_t *buf, uint16_t len)
{
    bool ok = false;

    i2c_lock();
    i2c_start();
    if (i2c_tx_dev(AS5600_ADDR, &reg, 1, false, false) == I2C_STATUS_OK)
    {
        ok = (i2c_rx_dev(AS5600_ADDR, buf, len) == I2C_STATUS_OK);
    }
    i2c_unlock();

    return ok;
}

// Generic register write: register address and payload go out as one
// transaction. Writes to CONF/ZPOS/MPOS/MANG are volatile; nothing here
// touches the BURN register.
static bool as5600_write_reg(uint8_t reg, const uint8_t *buf, uint16_t len)
{
    bool ok = false;

    i2c_lock();
    i2c_start();
    if (i2c_tx_dev(AS5600_ADDR, &reg, 1, false, true) == I2C_STATUS_OK)
    {
        ok = (i2c_tx_dev(AS5600_ADDR, buf, len, true, true) == I2C_STATUS_OK);
    }
    i2c_unlock();

    return ok;
}

// Read a big-endian 16-bit register pair and mask to 12 bits.
static bool as5600_read_reg12(uint8_t reg, uint16_t *value)
{
    uint8_t buf[2];

    if (!as5600_read_reg(reg, buf, 2))
    {
        return false;
    }

    *value = (uint16_t)(((buf[0] & 0x0FU) << 8) | buf[1]);
    return true;
}

bool as5600_init(void)
{
    return i2c_register_device(AS5600_ADDR) == I2C_STATUS_OK;
}

// Plain ACK probe: true when any device answers at 0x36.
// Says nothing about whether it is actually an AS5600.
bool as5600_detect_presence(void)
{
    uint8_t reg = 0x00;
    bool present;

    i2c_lock();
    i2c_start();
    present = (i2c_tx_dev(AS5600_ADDR, &reg, 1, true, true) == I2C_STATUS_OK);
    i2c_unlock();

    return present;
}

bool as5600_read_status(uint8_t *status)
{
    return status != NULL && as5600_read_reg(AS5600_REG_STATUS, status, 1);
}

bool as5600_read_agc(uint8_t *agc)
{
    return agc != NULL && as5600_read_reg(AS5600_REG_AGC, agc, 1);
}

bool as5600_read_magnitude(uint16_t *magnitude)
{
    return magnitude != NULL && as5600_read_reg12(AS5600_REG_MAGNITUDE, magnitude);
}

bool as5600_read_raw_angle(uint16_t *raw_angle)
{
    return raw_angle != NULL && as5600_read_reg12(AS5600_REG_RAW_ANGLE, raw_angle);
}

float as5600_raw_to_degrees(uint16_t raw_angle)
{
    return (float)(raw_angle & 0x0FFFU) * (360.0f / 4096.0f);
}

// CONF layout: 0x07 holds bits 13:8 (SF[1:0], FTH[4:2], WD[5]),
// 0x08 holds bits 7:0 (PM[1:0], HYST[3:2], OUTS[5:4], PWMF[7:6]).
bool as5600_read_conf(as5600_conf_t *conf)
{
    uint8_t buf[2];

    if (conf == NULL)
    {
        return false;
    }

    if (!as5600_read_reg(AS5600_REG_CONF, buf, 2))
    {
        return false;
    }

    conf->sf   = buf[0] & 0x03U;
    conf->fth  = (buf[0] >> 2) & 0x07U;
    conf->wd   = (buf[0] & 0x20U) != 0U;
    conf->pm   = buf[1] & 0x03U;
    conf->hyst = (buf[1] >> 2) & 0x03U;
    conf->outs = (buf[1] >> 4) & 0x03U;
    conf->pwmf = (buf[1] >> 6) & 0x03U;
    return true;
}

bool as5600_write_conf(const as5600_conf_t *conf)
{
    uint8_t buf[2];

    if (conf == NULL)
    {
        return false;
    }

    buf[0] = (uint8_t)((conf->sf & 0x03U) |
                       (uint8_t)((conf->fth & 0x07U) << 2) |
                       (conf->wd ? 0x20U : 0x00U));
    buf[1] = (uint8_t)((conf->pm & 0x03U) |
                       (uint8_t)((conf->hyst & 0x03U) << 2) |
                       (uint8_t)((conf->outs & 0x03U) << 4) |
                       (uint8_t)((conf->pwmf & 0x03U) << 6));
    return as5600_write_reg(AS5600_REG_CONF, buf, 2);
}

bool as5600_read_zmco(uint8_t *zmco)
{
    uint8_t value;

    if (zmco == NULL)
    {
        return false;
    }

    if (!as5600_read_reg(AS5600_REG_ZMCO, &value, 1))
    {
        return false;
    }

    *zmco = value & 0x03U;
    return true;
}

bool as5600_read_zpos(uint16_t *zpos)
{
    return zpos != NULL && as5600_read_reg12(AS5600_REG_ZPOS, zpos);
}

bool as5600_read_mpos(uint16_t *mpos)
{
    return mpos != NULL && as5600_read_reg12(AS5600_REG_MPOS, mpos);
}

bool as5600_read_mang(uint16_t *mang)
{
    return mang != NULL && as5600_read_reg12(AS5600_REG_MANG, mang);
}
