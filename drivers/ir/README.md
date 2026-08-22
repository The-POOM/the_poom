# IR Driver

`drivers/ir` is POOM's base infrared component for ESP-IDF. Its role is to split the IR flow into three simple layers:

- `ir_rcv`: raw pulse capture with RMT RX.
- `ir_dec`: protocol decoding from captured symbols.
- `ir_tx`: IR transmission with RMT TX.

This component does not draw menus, store files on SD, or manage presets by itself. It only provides the primitives that higher-level apps such as `menu_ir_universal` use to learn, decode, save, and replay IR commands.

## What It Solves

It supports a typical IR control workflow:

1. Listen for an IR frame from a receiver.
2. Obtain the raw `rmt_symbol_word_t` symbols.
3. Attempt to decode the protocol plus its `address` and `command` fields.
4. Re-send that command through an IR LED.

It can also be used independently as a:

- raw timing sniffer,
- multi-protocol decoder,
- protocol-aware IR transmitter.

## Component Layout

```text
drivers/ir
├── CMakeLists.txt
├── include/
│   ├── ir_dec.h
│   ├── ir_rcv.h
│   └── ir_tx.h
└── src/
    ├── ir_dec.c
    ├── ir_rcv.c
    └── ir_tx.c
```

## Dependencies

- `driver`
- `esp_driver_rmt`
- FreeRTOS

## Modules

### `ir_rcv`

This is the receive layer. It configures an RMT RX channel, allocates a symbol buffer, and returns each capture through `rmt_rx_done_event_data_t`.

Main public API:

- `ir_rcv_default_config()`
- `ir_rcv_init()`
- `ir_rcv_start()`
- `ir_rcv_wait()`
- `ir_rcv_dump()`
- `ir_rcv_deinit()`

Typical flow:

1. Create a config with `ir_rcv_default_config()`.
2. Set at least `gpio`.
3. Initialize with `ir_rcv_init()`.
4. Start a capture with `ir_rcv_start()`.
5. Wait for completion with `ir_rcv_wait()`.
6. Decode using `rx.received_symbols` and `rx.num_symbols`.

Useful notes:

- The symbol buffer is owned by `ir_rcv_handle_t`.
- `ir_rcv_dump()` is a debug helper that prints marks and spaces in readable time units.
- `clk_hz` matters directly because the decoder converts ticks to microseconds from that resolution.

### `ir_dec`

This is the interpretation layer. It takes RMT symbols and attempts to recognize the protocol and its fields.

Main public API:

- `ir_protocol_name()`
- `ir_protocol_parse_name()`
- `ir_protocol_is_supported()`
- `ir_decoder_context_reset()`
- `ir_decode_any_ex()`
- `ir_decode_any()`
- `nec_decode()`
- `nec_decode_ex()`
- `samsung32_decode()`

The standard output type is `ir_decoded_frame_t`:

- `protocol`
- `address`
- `command`
- `repeat`

If you need better repeat handling, use `ir_decode_any_ex()` with `ir_decoder_context_t`:

- detects dedicated repeat frames for NEC and Samsung32,
- marks repeats from toggle handling in RC5 and RC6,
- keeps the last frame for later comparisons.

### `ir_tx`

This is the transmit layer. It builds the RMT waveform and sends it using the proper carrier.

Main public API:

- `ir_tx_default_config()`
- `ir_tx_init()`
- `ir_tx_send()`
- `ir_tx_nec_send()`
- `ir_tx_nec_ext_send()`
- `ir_tx_samsung32_send()`
- `ir_tx_deinit()`

`ir_tx_send()` is the recommended generic entry point when the protocol is already known. Internally it selects the appropriate carrier for the protocol before sending.

## Supported Protocols

The decoder and transmitter share the `ir_protocol_t` enum. The component currently supports:

- `NEC`
- `NECext`
- `Samsung32`
- `SIRC`
- `SIRC15`
- `SIRC20`
- `RC5`
- `RC5X`
- `RC6`
- `RCA`
- `Pioneer`
- `Kaseikyo`
- `NEC42`
- `NEC42ext`

## Configuration Recommendations

### RMT Resolution

The most convenient resolution is:

```c
clk_hz = 1000000U;
```

With `1 MHz`, each tick equals `1 us`, which makes it easier to:

- debug captures,
- compare timings,
- reuse helpers such as `nec_decode()` and `samsung32_decode()`.

The multi-protocol decoder `ir_decode_any()` and `ir_decode_any_ex()` do accept `clk_hz`, so they are not limited to 1 MHz, but keeping `1 MHz` is still the most practical default.

### Carrier

Typical values for common IR remotes:

- `38000 Hz`
- `duty_cycle = 0.33f`

When you use `ir_tx_send()`, the driver automatically adjusts the carrier for protocols that use different frequencies, for example:

- Sony SIRC: `40 kHz`
- RC5 / RC5X / RC6: `36 kHz`

## Example: Capture and Decode

```c
#include "ir_dec.h"
#include "ir_rcv.h"

#include "esp_err.h"

void app_main(void)
{
    ir_rcv_config_t rcv_cfg = ir_rcv_default_config();
    rcv_cfg.gpio = 10;
    rcv_cfg.clk_hz = 1000000U;
    rcv_cfg.buffer_symbols = 256U;

    ir_rcv_handle_t rcv;
    ir_decoder_context_t dec_ctx;
    rmt_rx_done_event_data_t rx = {0};
    ir_decoded_frame_t frame = {0};

    ir_decoder_context_reset(&dec_ctx);
    ESP_ERROR_CHECK(ir_rcv_init(&rcv, &rcv_cfg, "ir_rx"));

    while (true)
    {
        ESP_ERROR_CHECK(ir_rcv_start(&rcv, &rcv_cfg));

        if (!ir_rcv_wait(&rcv, &rx, 1000U))
        {
            continue;
        }

        if (ir_decode_any_ex(rx.received_symbols, rx.num_symbols, rcv_cfg.clk_hz, &dec_ctx, &frame))
        {
            printf("proto=%s addr=0x%08lX cmd=0x%08lX repeat=%d\n",
                   ir_protocol_name(frame.protocol),
                   (unsigned long)frame.address,
                   (unsigned long)frame.command,
                   frame.repeat);
        }
    }
}
```

## Example: Transmit a Command

```c
#include "ir_tx.h"

#include "esp_err.h"

void app_main(void)
{
    ir_tx_config_t tx_cfg = ir_tx_default_config();
    tx_cfg.gpio = 25;
    tx_cfg.clk_hz = 1000000U;
    tx_cfg.carrier_hz = 38000U;
    tx_cfg.duty_cycle = 0.33f;

    ir_tx_handle_t tx;
    ESP_ERROR_CHECK(ir_tx_init(&tx, &tx_cfg, "ir_tx"));

    ESP_ERROR_CHECK(ir_tx_send(&tx, IR_PROTOCOL_NEC, 0x20U, 0x10U));
}
```

## When to Use Each API

- Use `ir_rcv_*` when you need to capture pulses from an IR receiver.
- Use `ir_decode_any_ex()` when you want multi-protocol decoding plus repeat handling.
- Use `ir_decode_any()` when you do not need state across frames.
- Use `nec_decode()` or `samsung32_decode()` only when you already know the signal belongs to that protocol.
- Use `ir_tx_send()` as the normal transmit API.
- Use `ir_tx_nec_send()` or other protocol-specific helpers only when you explicitly want to force that protocol.

## Important Limitations

- The component does not implement SD storage.
- The component does not save codes in NVS.
- The component does not include a graphical interface.
- `ir_rcv` captures symbols; it does not filter by protocol.
- Decode success depends on a clean capture and reasonably accurate IR receiver timings.

## Relationship to POOM Apps

The `menu_ir_universal` app uses this component to:

- learn commands from a physical remote,
- detect protocol, address, and command,
- save commands in higher-level storage,
- replay them through an IR LED.

In other words, `drivers/ir` is the technical foundation; menu logic, persistence, and user-facing behavior live outside the driver.

## Logging

The component includes simple `printf`-based logs:

- `IR_RCV_ENABLE_LOG`
- `IR_TX_ENABLE_LOG`

They are useful during hardware bring-up, GPIO validation, and timing verification.
