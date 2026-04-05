# oled

`oled` provides a minimal SSD1306 I2C transport layer, plus a small init helper (`oled_driver_init`).

## Folder Layout

```text
drivers/oled
├── CMakeLists.txt
├── component.mk
├── include/
│   ├── oled_def.h
│   ├── oled_driver.h
│   └── oled_transport.h
├── oled_driver.c
└── oled_transport.c
```

## Dependencies

- `driver`
- `i2c`
- `board`

## Public API

- `oled_driver_init`
- `oled_transport_init`
- `oled_transport_display_image`

## Quick Start

```c
#include "oled_driver.h"

void app_main(void)
{
    oled_driver_t oled = {0};
    oled_driver_init(&oled, 128, 64);

    // Rendering is handled by higher-level modules (e.g. poom_arduboy_display).
}
```

## Data Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant Driver as oled_driver
    participant Tx as oled_transport
    participant Bus as i2c
    participant OLED as OLED Panel

    App->>Driver: oled_driver_init()
    App->>Tx: oled_transport_display_image()
    Tx->>Bus: i2c_tx_dev()
    Bus->>OLED: I2C command/data frames
    Tx->>Bus: i2c_tx_dev()
    Bus->>OLED: I2C command/data frames
```
