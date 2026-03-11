# ble_midi

`ble_midi` is a BLE GATT server component that exposes the standard BLE MIDI
service and sends MIDI messages over notifications.

This component is generic BLE MIDI transport code used by applications (for
example, an air-drums app), but it is not tied to any specific product name.

## Structure

```text
components/ble_midi
├── CMakeLists.txt
├── component.mk
├── ble_midi.c
└── include/
    └── ble_midi.h
```

## Dependencies

From `components/ble_midi/CMakeLists.txt`:

- `bt`
- `nvs_flash`

## Purpose

- Initializes BLE controller + Bluedroid + GATTS/GAP for a BLE MIDI service.
- Advertises a BLE MIDI service UUID.
- Buffers outgoing MIDI packets and flushes them periodically with `esp_timer`.
- Sends data as GATT notifications on the BLE MIDI characteristic.
- Handles MTU updates and bounds outgoing payload accordingly.

## Public API

Declared in `include/ble_midi.h`:

- `int32_t blemidi_init(void *callback_midi_message_received);`
- `int32_t blemidi_send_message(uint8_t blemidi_port, uint8_t *stream, size_t len);`
- `void blemidi_receive_packet_callback_for_debugging(...);`
- `void blemidi_tick(void);`

Current implementation details:

- `blemidi_init()` and `blemidi_send_message()` are implemented and used.
- `blemidi_receive_packet_callback_for_debugging()` and `blemidi_tick()` are
  declared in the header but are not implemented in `ble_midi.c`.

## Usage

```c
#include "ble_midi.h"

static void midi_rx_cb(uint8_t port,
                       uint16_t timestamp,
                       uint8_t status,
                       uint8_t *data,
                       size_t len,
                       size_t continued_sysex_pos)
{
    (void)port;
    (void)timestamp;
    (void)status;
    (void)data;
    (void)len;
    (void)continued_sysex_pos;
}

void app_start(void)
{
    (void)blemidi_init((void *)midi_rx_cb);
}

void send_note_on(void)
{
    uint8_t msg[3] = {0x90, 60, 100}; // channel 1, note C4, velocity 100
    (void)blemidi_send_message(0, msg, sizeof(msg));
}
```

## Configuration Macros

In `include/ble_midi.h`:

- `BLEMIDI_DEVICE_NAME` (default: `"poom-midi"`)
- `BLEMIDI_TAG`
- `BLEMIDI_NUM_PORTS` (default: `1`)
- `BLEMIDI_OUTBUFFER_FLUSH_MS` (default: `30`)

In `ble_midi.c`:

- `BLEMIDI_LOG_ENABLED` controls info/warn/error `printf` logs.
- `BLEMIDI_DEBUG_LOG_ENABLED` enables debug-level logs.

## Notes

- There is no public `blemidi_stop()` API in this component at the moment.
- Outgoing buffering is single-port by default (`BLEMIDI_NUM_PORTS = 1`).
- RX parsing code exists internally but is currently not wired to normal write
  handling (the call is commented in `ESP_GATTS_WRITE_EVT`).
