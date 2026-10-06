// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Hugo Trippaers <hugo@trippaers.nl>

#ifndef POOM_POOM_I2C_AS5600_DRIVER_H
#define POOM_POOM_I2C_AS5600_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// STATUS register (0x0B) flags
#define AS5600_STATUS_MH (1U << 3)   // magnet too strong (AGC at minimum)
#define AS5600_STATUS_ML (1U << 4)   // magnet too weak (AGC at maximum)
#define AS5600_STATUS_MD (1U << 5)   // magnet detected

bool as5600_init(void);

// Plain ACK probe: something answers at 0x36 (DETECT state).
bool as5600_detect_presence(void);

// Magnet diagnostics. All return true on success, false on I2C error.
// MD/ML/MH can be set together: check MD first, then ML/MH as qualifiers.
bool as5600_read_status(uint8_t *status);

// AGC gain: 0..128 at 3.3V supply, sweet spot mid-range (~64).
// Near 0 = magnet too strong/close, near 128 = too weak/far.
bool as5600_read_agc(uint8_t *agc);

// 12-bit CORDIC magnitude (0..4095).
bool as5600_read_magnitude(uint16_t *magnitude);

// Unscaled 12-bit angle (0..4095), straight from RAW ANGLE (0x0C).
// Ignores the programmed ZPOS/MPOS range, so it always covers the
// full turn -- use this for the rotation/direction view.
bool as5600_read_raw_angle(uint16_t *raw_angle);

// Pure conversion: raw 12-bit angle to degrees (0.0 .. 359.9),
// 360/4096 deg per LSB. No I2C access.
float as5600_raw_to_degrees(uint16_t raw_angle);

// CONF register (0x07-0x08), decoded. Field values are the raw
// datasheet codes.
typedef struct
{
    uint8_t pm;    // power mode: 0=NOM 1=LPM1 2=LPM2 3=LPM3
    uint8_t hyst;  // hysteresis: 0=off, 1..3 LSB
    uint8_t outs;  // output stage: 0=analog full, 1=analog 10-90%, 2=PWM
    uint8_t pwmf;  // PWM frequency: 0=115Hz 1=230Hz 2=460Hz 3=920Hz
    uint8_t sf;    // slow filter: 0=16x 1=8x 2=4x 3=2x
    uint8_t fth;   // fast filter threshold: 0=slow only, 1..7=6/7/9/18/21/24/10 LSB
    bool wd;       // watchdog: auto low-power after 1 min at standstill
} as5600_conf_t;

bool as5600_read_conf(as5600_conf_t *conf);

// Volatile write: settings revert on power cycle. This never burns
// anything into OTP (that would need the BURN register, which this
// driver deliberately does not expose).
bool as5600_write_conf(const as5600_conf_t *conf);

// Programmed-position registers (volatile view; burned values show
// up here too). ZMCO is the number of BURN_ANGLE operations (0..3).
bool as5600_read_zmco(uint8_t *zmco);
bool as5600_read_zpos(uint16_t *zpos);
bool as5600_read_mpos(uint16_t *mpos);
bool as5600_read_mang(uint16_t *mang);

#ifdef __cplusplus
}
#endif

#endif  // POOM_POOM_I2C_AS5600_DRIVER_H
