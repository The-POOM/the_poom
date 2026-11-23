# Reproducible ESP32-C5 build

Validated base: upstream POOM 1.0.18, commit
`742556e7deea0f5769fd9a63f332517ac26aa944`, merged into this branch as
`d4d957c` (app version `1.0-r18-77`).
Validated SDK: ESP-IDF `v6.1` (release tag), matching upstream's documented
framework. Earlier Wardrive acceptance on 1.0.16 used the
`v6.1-dev-1391-g95b6f32350e` snapshot; see [validation](WARDRIVE_VALIDATION.md).
Initialize recursive submodules and install/export the SDK normally.
Do not update dependency versions as part of this feature. ESP-IDF 6.1
rewrites `dependencies.lock` to lock-file format 3.0.0 during configuration;
do not commit that change with Wardrive.

## Memory profile

Use upstream's existing `sdkconfig.esp32c5.ram_optimization` together with
`sdkconfig.defaults`. Wardrive does not add or maintain another profile.
The upstream profile enables PSRAM malloc, a 512-byte internal-allocation
threshold, a 32 KiB internal reserve, Wi-Fi/LWIP external allocation, external
BSS and external mbedTLS allocation. TLS certificate verification is unchanged.

Fresh generation selects static Wi-Fi RX 16 and RX BA window 16, on both the
earlier SDK snapshot and ESP-IDF v6.1. Published POOM 1.0.16 and 1.0.17
binaries instead use 10/6, with external mbedTLS allocation. A controlled
stock-only hardware comparison failed at 16/16 and passed at 10/6. Hardware
acceptance applies to 10/6 builds only.
Do not modify the shared profile or disable Wi-Fi IRAM options for this app.

## Build (ESP-IDF v6.1)

Use a new build directory and a separate generated sdkconfig. Defaults do not
reliably override an existing generated configuration.

Supply RX 10 / BA 6 through a build-local defaults file listed after the
upstream files. Do not edit the generated `sdkconfig` by hand: on ESP-IDF 6.1
that confuses default tracking, and in testing it produced RX 2 / BA 2 in
`config/sdkconfig.h` even though the edited file showed 10/6.

```sh
B=build-wardrive-1018
mkdir -p "$B"
printf 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10\nCONFIG_ESP_WIFI_RX_BA_WIN=6\n' > "$B/wardrive-rx.defaults"
idf.py -B "$B" -D IDF_TARGET=esp32c5 -D SDKCONFIG="$B/sdkconfig" \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization;$B/wardrive-rx.defaults" reconfigure
grep -E 'ESP_WIFI_STATIC_RX_BUFFER_NUM |ESP_WIFI_RX_BA_WIN ' "$B/config/sdkconfig.h"
idf.py -B "$B" build size
idf.py -B "$B" -p YOUR_PORT flash
idf.py -B "$B" -p YOUR_PORT monitor --no-reset
```

PowerShell (after exporting the SDK):

```powershell
$b = "build-wardrive-1018"
New-Item -ItemType Directory -Force $b | Out-Null
Set-Content -Encoding ascii "$b/wardrive-rx.defaults" -Value "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10","CONFIG_ESP_WIFI_RX_BA_WIN=6"
idf.py -B $b -D IDF_TARGET=esp32c5 "-DSDKCONFIG=$b/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization;$b/wardrive-rx.defaults" reconfigure
Select-String -Path "$b/config/sdkconfig.h" -Pattern 'ESP_WIFI_STATIC_RX_BUFFER_NUM |ESP_WIFI_RX_BA_WIN '
idf.py -B $b build size
idf.py -B $b -p COM5 flash
idf.py -B $b -p COM5 monitor --no-reset
```

Before flashing, verify `config/sdkconfig.h` (the values actually compiled)
contains `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM 10`,
`CONFIG_ESP_WIFI_RX_BA_WIN 6`, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 1`,
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 512` and
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 32768`, and that the generated
sdkconfig has SPIRAM_USE_MALLOC=y and SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y.
If USB disconnects during post-flash reset, reconnect and run monitor
separately; do not assume a completed write verified unless the flasher
reports verification or the device subsequently boots correctly.

On a drive that does not record file ownership (exFAT/FAT32), Git may refuse
the checkout as "dubious ownership"; add the repository and both submodules
to `safe.directory`.

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
