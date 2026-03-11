# poom_ble_spam

`poom_ble_spam` rotates BLE raw advertising payloads at a fixed dwell time.

## Purpose

- Configure BLE in non-connectable advertising mode.
- Rotate through a predefined list of manufacturer-specific payloads.
- Expose the currently advertised label through a callback.

## Structure

```text
applications/poom_ble_spam
├── CMakeLists.txt
├── component.mk
├── README.md
├── poom_ble_spam.c
└── include/
    └── poom_ble_spam.h
```

## Dependencies

Defined in `applications/poom_ble_spam/CMakeLists.txt`:

- `bt`

## Public API

Header: `applications/poom_ble_spam/include/poom_ble_spam.h`

```c
typedef void (*poom_ble_spam_cb_display)(const char *name);

void poom_ble_spam_start(void);
void poom_ble_spam_register_cb(poom_ble_spam_cb_display callback);
void poom_ble_spam_app_stop(void);
```

## Runtime Behavior

- `poom_ble_spam_start()`:
  - initializes BLE controller + Bluedroid,
  - registers GAP callback,
  - creates a one-shot timer used to cut advertising per dwell period,
  - starts the payload rotation chain.
- `poom_ble_spam_register_cb()`:
  - reports the current payload label every time rotation advances.
- `poom_ble_spam_app_stop()`:
  - stops rotation and sends an advertising stop request.

## Logging

Configurable in `poom_ble_spam.h`:

- `POOM_BLE_SPAM_LOG_ENABLED`
- `POOM_BLE_SPAM_DEBUG_LOG_ENABLED`

## Tunable Constants

In `poom_ble_spam.c`:

- `POOM_BLE_SPAM_DEFAULT_DWELL_MS`
- `POOM_BLE_SPAM_ADV_INTERVAL_FAST`
- address/randomization constants and timer name

## Reference / Idea Source

- AppleJuice project reference: https://github.com/ECTO-1A/AppleJuice/tree/main
