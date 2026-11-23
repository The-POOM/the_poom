# CSI motion provenance

`CsiMotionModel.h/.cpp` are adapted from JustCallMeKoko's ESP32 Marauder,
revision `ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da` on `develop`:
https://github.com/justcallmekoko/ESP32Marauder/tree/ef9a4bbb0ea53a2d0d3ed5b6c7bca590dadc20da/esp32_marauder

The upstream MIT copyright and permission notice is reproduced in
[LICENSE.Marauder](LICENSE.Marauder). The feature extraction, median/MAD
calibration, fused score, quality estimate and voting originate in that model.
The C5 force-L-LTF setup follows upstream `CsiMotionMonitor.cpp` and the actual
ESP-IDF 6.1 `esp_wifi_he_types.h` definition. Native POOM radio lifetime, bounded
PSRAM queue, selected-AP filtering and OLED/button UI are separate implementations.

Native adaptations:

- Explicit signed 12-bit little-endian decoding masks the storage word's unused
  high bits. IDF C5 `lltf_bit_mode=0` requests the corresponding format.
- Model ingestion returns whether it accepted a nonzero complete sample.
- Calibration requires at least eight usable one-second windows, as well as the
  selected 30-second minimum. A silent AP never produces a calibrated result.
- Out-of-order samples are rejected; large scheduling gaps advance in bounded
  time and reset carrier continuity. A two-second sample gap invalidates the rate
  and motion state instead of retaining a prior motion result.
- Window sample counts saturate safely; model tests cover full counter range,
  timestamp wrap, signed decode values, invalid first words and no-data recovery.

This is an experimental RF change estimate. It does not identify people, count
occupants, infer actions, or establish that a particular movement caused a score.
