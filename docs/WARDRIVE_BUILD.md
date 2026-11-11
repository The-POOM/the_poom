# Reproducible ESP32-C5 build

Validated base: upstream POOM 1.0.16, commit
`3533ee027efcf06a610265f3c4b4be8c73548fb5`.
Validated SDK: ESP-IDF `95b6f32350e1118181d1a1c375972cbcd85a6540`
(`v6.1-dev-1391-g95b6f32350e`), the snapshot identified in the published
release. ESP-IDF 6.1 final is not the same build environment.
Initialize recursive submodules and install/export the pinned SDK normally.
Do not update dependency versions as part of this feature.

## Memory profile

The optional root `sdkconfig.wardrive_release` preserves the memory settings
verified from published POOM 1.0.16 machine code and validated on hardware:

- PSRAM quad, 80 MHz, available to malloc.
- Internal-allocation threshold 512 bytes; internal reserve 32768 bytes.
- Wi-Fi/LWIP allocations prefer PSRAM, static TX 16 and cache TX 32.
- Static RX 10, dynamic RX 32 and RX BA window 6.
- **External BSS enabled.** This moves eligible Wi-Fi, Bluetooth and LWIP
  zero-initialized data into PSRAM, recovering about 33 KiB internally.

Caps-only PSRAM left this data internally. Pure stock source then failed BLE
startup after Device Inspector's Wi-Fi phase; the same failure occurred without
Wardrive. With the verified profile, stock and Wardrive builds passed the
owner's app-transition tests without modifying Device Inspector. Loaded image
segment sizes alone miss BSS and cannot establish runtime memory equivalence.

The profile is opt-in; it does not replace upstream `sdkconfig.defaults`,
change Wi-Fi IRAM options, weaken TLS or change partitions. It reconstructs
identified memory settings, not every unknown release configuration option.
The separate `sdkconfig.esp32c5.ram_optimization` profile changes more settings
and is not the acceptance configuration for this feature.

## Fresh build

Use a new build directory and a separate generated sdkconfig. Defaults do not
reliably override an existing generated configuration.

```sh
idf.py -B build-wardrive-release -D IDF_TARGET=esp32c5 \
  -D SDKCONFIG=build-wardrive-release/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wardrive_release' build size
idf.py -B build-wardrive-release -p YOUR_PORT flash
idf.py -B build-wardrive-release -p YOUR_PORT monitor --no-reset
```

PowerShell (after exporting the pinned SDK):

```powershell
$buildDir = "build-wardrive-release"
python "$env:IDF_PATH/tools/idf.py" -B $buildDir -D IDF_TARGET=esp32c5 "-DSDKCONFIG=$buildDir/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wardrive_release" build size
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

## Host checks

With a C compiler, Python 3 and a local cJSON source checkout:

```sh
ASAN_OPTIONS=detect_leaks=0 CJSON_DIR=/path/to/cJSON sh tools/wardrive_hostcheck/check.sh
```

The suite uses synthetic observations and offline HTTP/TLS mocks. It does not
contact WiGLE or need account credentials. See [validation](WARDRIVE_VALIDATION.md).
