# poom_imu_stream

Component used to initialize and read the `LSM6DS3TR-C` IMU over I2C in POOM.

## Purpose

- Configure the sensor (accelerometer + gyroscope + temperature).
- Expose a simple API to read samples.
- Keep the I2C access layer encapsulated.

## Structure

```text
applications/poom_imu_stream
├── CMakeLists.txt
├── component.mk
├── poom_imu_stream.c
├── include/
│   └── poom_imu_stream.h
└── README.md
```

## Dependencies

Defined in `applications/poom_imu_stream/CMakeLists.txt`:

- `driver`
- `i2c`
- `lsm6ds3`

## Public API

Header: `applications/poom_imu_stream/include/poom_imu_stream.h`

```c
typedef struct
{
    float acceleration_mg[3];
    float angular_rate_mdps[3];
    float temperature_degC;
} poom_imu_data_t;

void poom_imu_stream_init(void);
bool poom_imu_stream_read_data(poom_imu_data_t *out);
```

## Output Units

- `acceleration_mg[]`: milli-g (mg)
- `angular_rate_mdps[]`: milli-degrees per second (mdps)
- `temperature_degC`: degrees Celsius

## Runtime Behavior

- `poom_imu_stream_init()`:
  - registers the I2C device (`0x6B`),
  - validates `WHO_AM_I`,
  - applies ODR/full-scale/filter configuration.
- `poom_imu_stream_read_data()`:
  - returns `true` only when at least one channel has new data,
  - keeps and returns the last valid sample in `out`.

## Logging

Macros used in `poom_imu_stream.c`:

- `POOM_IMU_STREAM_ENABLE_LOG`
- `POOM_IMU_STREAM_DEBUG_LOG_ENABLED`

If these macros are not defined in build flags/Kconfig, the preprocessor evaluates them as `0`, so logs stay disabled.

## Usage

```c
#include "poom_imu_stream.h"

poom_imu_data_t d = {0};

poom_imu_stream_init();
if (poom_imu_stream_read_data(&d)) {
    /* use d.acceleration_mg / d.angular_rate_mdps / d.temperature_degC */
}
```
