# poom_led_rainbow

`poom_led_rainbow` provides a simple WS2812 rainbow animation module.

## Purpose

- Initialize the WS2812 strip using board pin/LED definitions.
- Start a FreeRTOS task that renders a moving rainbow.
- Stop the task and switch all LEDs off.

## Structure

```text
applications/poom_led_rainbow
├── CMakeLists.txt
├── component.mk
├── README.md
├── poom_led_rainbow.c
└── include/
    └── poom_led_rainbow.h
```

## Dependencies

Defined in `applications/poom_led_rainbow/CMakeLists.txt`:

- `ws2812`
- `board`

## Public API

Header: `applications/poom_led_rainbow/include/poom_led_rainbow.h`

```c
void poom_led_rainbow_init(void);
bool poom_led_rainbow_start(void);
void poom_led_rainbow_stop(void);
```

## Runtime Behavior

- `poom_led_rainbow_init()`:
  - initializes the strip,
  - sets base brightness,
  - performs a short blue boot blink,
  - clears LEDs.
- `poom_led_rainbow_start()`:
  - creates the rainbow task if not already running,
  - auto-initializes if needed.
- `poom_led_rainbow_stop()`:
  - deletes the task if running,
  - clears LEDs.

## Tunable Constants

In `poom_led_rainbow.c`, key values are defined as macros (task stack/priority,
resolution, brightness, animation speed, boot blink timing) to avoid magic
numbers and simplify tuning.

## Usage

```c
#include "poom_led_rainbow.h"

poom_led_rainbow_init();
(void)poom_led_rainbow_start();

/* ... */

poom_led_rainbow_stop();
```
