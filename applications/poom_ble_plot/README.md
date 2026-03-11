# poom_ble_plot

`poom_ble_plot` provides BLE plotting support over Nordic UART Service (NUS).

It is split into two modules:

- `poom_ble_plot`: BLE transport (GAP/GATT + NUS TX notify / RX write)
- `poom_imu_plot`: IMU-to-plot bridge for streaming accel/gyro data

## Structure

```text
applications/poom_ble_plot
├── CMakeLists.txt
├── component.mk
├── README.md
├── poom_ble_plot.c
├── poom_imu_plot.c
├── include/
│   ├── poom_ble_plot.h
│   └── poom_imu_plot.h
```

## Dependencies

Defined in `applications/poom_ble_plot/CMakeLists.txt`:

- `poom_imu_stream`
- `ws2812`
- `sbus`
- `button_driver`
- `poom_ble_hid`
- `board`

## Public API

### Transport API (`poom_ble_plot.h`)

- `int32_t poom_ble_plot_init(const char *device_name);`
- `int32_t poom_ble_plot_set_series_count(uint8_t n_series);`
- `void poom_ble_plot_set_format(char sep, uint8_t precision);`
- `int32_t poom_ble_plot_send_line(const float *values, size_t n);`
- `bool poom_ble_plot_is_connected(void);`
- `void poom_ble_plot_stop(void);`

### IMU Plot API (`poom_imu_plot.h`)

- `bool poom_ble_plot_imu_init(const char *device_name, char sep, uint8_t precision);`
- `bool poom_ble_plot_imu_start(imuplot_mode_t mode, uint32_t period_ms);`
- `void poom_ble_plot_imu_stop(void);` (also unsubscribes from button topic)
- `bool poom_ble_plot_imu_is_running(void);`

`imuplot_mode_t` supports:

- `IMUPLOT_ACCEL_ONLY`
- `IMUPLOT_GYRO_ONLY`
- `IMUPLOT_ACCEL_GYRO`

## Logging

Both modules use printf-based logs with compile-time switches:

- In `poom_ble_plot.c`:
  - `BLEPLOT_LOG_ENABLED`
  - `BLEPLOT_DEBUG_LOG_ENABLED`
- In `poom_imu_plot.c`:
  - `IMUPLOT_LOG_ENABLED`
  - `IMUPLOT_DEBUG_LOG_ENABLED`

## Runtime Notes

- BLE transport uses NUS 128-bit UUIDs compatible with Bluefruit UART plot workflows.
- Notifications are sent as one CSV line per BLE notification.
- Effective line size is bounded by `MTU - 3` and `BLEPLOT_MAX_LINE_LEN`.
- `poom_imu_plot` listens to `input/button` and switches stream mode on button events.

## Usage

```c
#include "poom_imu_plot.h"

(void)poom_ble_plot_imu_init("PPLOT", ',', 2);
(void)poom_ble_plot_imu_start(IMUPLOT_ACCEL_ONLY, 50);
```

## Return Codes

`poom_ble_plot_send_line()` can return negative values when:

- there is no active connection,
- arguments are invalid,
- formatted line exceeds usable payload,
- BLE notify send fails.

To fully release BLE resources, call:

```c
poom_ble_plot_imu_stop();
poom_ble_plot_stop();
```
