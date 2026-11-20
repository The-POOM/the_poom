# BEAST CSI RF MOTION

Open `BEAST > CSI RF MOTION`, select an AP from the passive scan, then press A.
The selected BSSID and channel remain fixed during the session. Keep POOM and
the test area still for the first 30 seconds. A restarts calibration; B stops
the session. Low sample volume keeps the app in calibration or reports
`INSUFFICIENT SAMPLES` rather than showing a motion claim.

The screen displays the received CSI sample rate, RSSI, quality estimate (`Q`),
fused RF motion score (`M`), amplitude/phase/correlation features and recent score
history. These are experimental RF changes relative to calibration, not an
occupancy detector or a people count. Moving POOM, moving objects, multipath,
interference, AP behavior and radio traffic can all affect the result. A usable
sample stream from the selected AP is required; the capture session does not
associate to it or generate traffic to force samples.

The native implementation is for ESP32-C5 with ESP-IDF 6.1 and
`CONFIG_ESP_WIFI_CSI_ENABLED=y`. The C5 release defaults enable this; existing
generated sdkconfig files retain their old choice, so enable it in menuconfig
or start with a fresh build directory when adopting the feature. CSI support
adds Wi-Fi RX allocation cost described by IDF (about one KB per configured
static RX buffer), even outside this app. The model and 12-slot receive queue
use PSRAM only while the CSI screen is open.

The C5 driver is explicitly configured for 12-bit forced L-LTF. Samples are
filtered by source BSSID and tuned channel; only up to 256 bytes of complete
complex tones are copied. The callback never allocates, draws, writes SD or
runs the model. The app processes samples outside the callback, skips invalid
first words, rejects late/empty samples and records queue losses. Calibration
needs at least eight usable windows over at least 30 seconds. A two-second gap
invalidates the rate/estimate. No CSI session report or raw CSI export is claimed.

Failed start/tuning is shown. Teardown detaches the callback's session reference
under the same lock used to copy samples, then disables CSI/promiscuous mode and
deinitializes Wi-Fi. Errors keep the owner active and are displayed while teardown
is retried. CLI handoff uses `menu_wifi_csi_stop()` and `menu_wifi_csi_is_active()`;
the stop call acknowledges completion or returns false after five seconds.

Host checks:

```powershell
.\tests\host\run_wifi_csi.ps1 -Compiler g++
```

The tests use synthetic stable/changing carrier vectors, all 65,536 storage-word
patterns for signed 12-bit decoding, timestamp wrap, large gaps, stale/short/zero
samples, invalid first words and sample counter saturation. Actual native backend
code is exercised against mock IDF APIs for start failures, ownership refusal,
channel/target filters, queue overflow, recalibration, failed cleanup/retry and
late callbacks. These tests and a C5 compile do not validate physical RF motion
accuracy, CSI reception on the installed board or a successful channel demo.
