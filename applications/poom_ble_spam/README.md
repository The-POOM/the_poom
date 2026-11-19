# BLE advertising modes

`BEAST > SPAM BLE` opens mode selection without starting advertising. UP/DOWN
chooses a family, LEFT/RIGHT chooses 15/30/60 seconds, A starts or stops, and B
stops the worker and returns. The displayed count is **advertising cycles accepted
by the controller**, not a packet count or proof of a receiver popup/effect.

| Mode | Native payload behavior |
| --- | --- |
| Apple Devices | Preserves POOM's 17 AirPods/Beats proximity payloads. |
| Apple Juice | Marauder's current Apple action/device formats, deterministic 9:1 selection, 500ms default dwell. |
| Sour Apple | The same current upstream Apple payload family, with 150ms default dwell. |
| Swift Pair | Microsoft manufacturer `0006`, Swift Pair header and a generated five-character name. |
| Samsung | Manufacturer `0075`, cycling the 17 watch model/color values in upstream. |
| Google Fast Pair | Service `FE2C`, upstream model ID `00B727`, fixed nominal TX-power metadata byte zero. |
| Flipper | Five-character name, service `3081`/`3082`/`3083`, manufacturer `0FBA` reference data. |
| All Families | Sequential rotation through the seven families above, within the same bounded run. |

The current Marauder implementation uses the same Apple builder for Sour Apple
and Apple Juice; this port does not substitute an older commented-out exploit
payload. Receiver behavior depends on device/software state and is not claimed
from an advertising acknowledgement.

Payload formats and mode references were reviewed against
[Just Call Me Koko / ESP32 Marauder develop](https://github.com/justcallmekoko/ESP32Marauder/tree/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da),
commit `ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da`,
`esp32_marauder/WiFiScan.cpp` (`GetUniversalAdvertisementData`, `executeBLESpam`)
and `MenuFunctions.cpp` (`buildBluetoothAttackMenu`). The upstream source also
credits Spooks4576 for the universal advertisement builder and RapierXbox / ECTO-1A
for the Sour Apple lineage. Marauder's full MIT notice is preserved in
[LICENSE-MARAUDER](LICENSE-MARAUDER). Existing POOM proximity templates retain
their original POOM attribution.

Two deliberate native adaptations keep payloads reviewable: the upstream Flipper
manufacturer AD field declares length five while appending eight data bytes; the
native builder declares nine (type plus all eight bytes), producing a valid
27-byte advertisement. Fast Pair's random power byte is replaced with fixed
nominal metadata; it is not a measured C5 transmit-power value.

## Lifecycle and API

One native Bluedroid worker owns an otherwise idle controller. There are no
Arduino/NimBLE dependencies, timer callbacks that rotate during teardown, repeated
stack initialization per payload, unchecked `ESP_ERROR_CHECK` aborts, or base-MAC
changes. Each cycle waits for random-address configuration, payload configuration,
advertising start and stop acknowledgements. Payloads fit legacy 31-byte ADV data.
All use nonconnectable advertising, the three primary advertising channels and a
100ms advertising interval. Random static addresses are changed only between
stopped cycles; no captured target address is used.

`poom_ble_spam_start_mode()` validates mode, duration (1..60 seconds, default 30)
and optional dwell (100..5000ms). Its success means the worker was scheduled;
`poom_ble_spam_get_status()` reports subsequent driver failures and current state.
Legacy `poom_ble_spam_start()` now returns `esp_err_t` and selects Apple Devices
for 30 seconds. The existing void stop wrapper remains compatible, while
`poom_ble_spam_stop()` reports whether the owned radio was released.

GAP operations have individual 3-second deadlines. Natural run expiry during a
pending operation completes the session normally and tears down the owned stack;
an individual operation timeout while run time remains reports a timeout error.
Stopping waits for worker exit and Bluetooth release. A driver teardown failure
remains visible and blocks a new run; no other active Bluetooth owner is silently
deinitialized. The UI releases previous POOM scanner/tracker/GATT consumers using
their own stop functions before an explicit start.

The legacy label callback runs on the worker, outside its status lock. Clearing
the callback prevents future snapshots but is not an in-flight callback barrier.
Call the acknowledged stop before freeing resources used by such a callback.
The OLED uses status snapshots and does not register a label callback.

## Validation and limits

`tests/host/ble_advert/run.ps1 -Compiler gcc` runs strict native C tests for all
payload lengths/AD boundaries, every model/UUID family, buffer sentinels, exact
reference fields, actual backend start/configuration/stop failures, ordinary
duration expiry versus stage timeout, cancellation with a pending start,
partial-init cleanup, foreign-owner refusal and persistent teardown failure.
The actual OLED source is tested for explicit start, mode/time selection, immediate
asynchronous failure display, late button callbacks and allocation/subscription
cleanup. The runner puts the chosen compiler's directory on PATH temporarily.

The complete C5 firmware compiles against official ESP-IDF 6.1. Host mocks do not
verify RF reception, receiver reactions, controller scheduling jitter, or repeated
physical detector/advertiser handoffs. No radio or device operation was used to
validate these advertising additions. Selected AirTag advertisement replay is
not part of this change.
