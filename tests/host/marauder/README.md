Run `./run.ps1` with MinGW GCC on PATH, or supply `-Compiler /path/to/gcc.exe`.
The script resolves all source paths relative to itself and puts executables
in the system temporary directory. `-BuildDirectory` overrides that location.
It has no dependency on a particular checkout location or ESP-IDF installation.

The frame test exercises the portable C builders with strict compiler warnings,
all SSID lengths (0..32), both bands, complete IE boundaries, frame lengths,
addresses, replay/sequence encoding, small buffers, and WPA2/WPA3 alternation.

The lifecycle test builds the real backend against isolated Win32 host mocks
for FreeRTOS, radio, timers, and PSA. It tests synchronous stop, bounded expiry,
reentrant start rejection, driver/crypto/task/init failures, temporary-key
destruction, and configuration validation. These tests never access a serial
port, physical device, or wireless interface. They do not verify ESP-IDF
scheduling, actual PSA cryptography, driver acceptance, or over-air behavior.
