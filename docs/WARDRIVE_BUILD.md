# Reproducible ESP32-C5 build

Validated base: upstream POOM 1.0.16, commit
`3533ee027efcf06a610265f3c4b4be8c73548fb5`.
Validated SDK: ESP-IDF `95b6f32350e1118181d1a1c375972cbcd85a6540`
(`v6.1-dev-1391-g95b6f32350e`), the snapshot identified in the published
release. ESP-IDF 6.1 final is not the same build environment.
Initialize recursive submodules and install/export the pinned SDK normally.
Do not update dependency versions as part of this feature.

## Memory profile

Use upstream's existing `sdkconfig.esp32c5.ram_optimization` together with
`sdkconfig.defaults`. Wardrive does not add or maintain another profile.
The upstream profile enables PSRAM malloc, a 512-byte internal-allocation
threshold, a 32 KiB internal reserve, Wi-Fi/LWIP external allocation, external
BSS and external mbedTLS allocation. TLS certificate verification is unchanged.

On the pinned release SDK, fresh generation selects static Wi-Fi RX 16 and RX
BA window 16. Published POOM 1.0.16 and 1.0.17 binaries instead use 10/6,
with external mbedTLS allocation. A controlled stock-only hardware comparison
failed at 16/16 and passed at 10/6. The hardware-accepted Wardrive handoff build reused the passing generated
stock configuration, retaining external mbedTLS allocation. Hardware
acceptance applies to that 10/6 build.
Do not modify the shared profile or disable Wi-Fi IRAM options for this app.

## Reproduce the hardware-accepted build

Copy the passing stock generated config into a new build directory as
`sdkconfig` and pass its absolute path as `-DSDKCONFIG`. Verify
`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10`, `CONFIG_ESP_WIFI_RX_BA_WIN=6`,
and external mbedTLS allocation before flashing.

## Fresh config generation

Use a new build directory and a separate generated sdkconfig. Defaults do not
reliably override an existing generated configuration.

```sh
idf.py -B build-wardrive-release -D IDF_TARGET=esp32c5 \
  -D SDKCONFIG=build-wardrive-release/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization' build size
idf.py -B build-wardrive-release -p YOUR_PORT flash
idf.py -B build-wardrive-release -p YOUR_PORT monitor --no-reset
```

PowerShell (after exporting the pinned SDK):

```powershell
$buildDir = "build-wardrive-release"
python "$env:IDF_PATH/tools/idf.py" -B $buildDir -D IDF_TARGET=esp32c5 "-DSDKCONFIG=$buildDir/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization" build size
python "$env:IDF_PATH/tools/idf.py" -B $buildDir -p COM5 flash
python "$env:IDF_PATH/tools/idf.py" -B $buildDir -p COM5 monitor --no-reset
```

Verify generated sdkconfig contains SPIRAM_USE_MALLOC=y,
SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y, ALWAYSINTERNAL=512 and
RESERVE_INTERNAL=32768. If USB disconnects during post-flash reset, reconnect
and run monitor separately; do not assume a completed write verified unless
the flasher reports verification or the device subsequently boots correctly.

Verbose tracing is available through menuconfig → Wardrive → Verbose Wardrive
serial diagnostics. It defaults off. User-selected raw GPS output remains
available separately. No global allocation-failure callback is installed.

## Optional developer checks

Host stubs and regression harnesses are maintained on the separate
`validation/wardrive-pr50` branch of the contributor fork. They are not part
of the application PR and are not firmware dependencies. That branch retains
matching source and the harness; its instructions are for optional development
validation. It uses synthetic data and offline HTTP/TLS mocks only.

See [validation](WARDRIVE_VALIDATION.md) for build and hardware-test results.
