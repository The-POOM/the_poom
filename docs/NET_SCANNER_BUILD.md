# Net Scanner: reproducible release build

Base: upstream POOM **1.0.16**, commit
`3533ee027efcf06a610265f3c4b4be8c73548fb5`.
The scanner was ported from `l3allzh4ck/the_poom`,
`feature/net-scanner-auditor-clean` at
`c703cea793c4e7edf3e3add565735014bdf10316`.
Device Inspector, Wi-Fi Guard and release version 16 remain intact.

Use ESP-IDF **`95b6f32350e1118181d1a1c375972cbcd85a6540`**
(`v6.1-dev-1391-g95b6f32350e`), the published release SDK identified
and used during Wardrive validation. ESP-IDF 6.1 final is different.
Initialize recursive SDK/project submodules and install/export that SDK.
Keep the upstream dependency versions.

## Memory profile

`sdkconfig.net_scanner_release` reproduces the Wardrive-verified memory
settings from `feature/wardrive-pr-1.0.16` at `41509ed`:
quad PSRAM at 80 MHz, malloc enabled, internal threshold 512 bytes,
internal reserve 32768 bytes, Wi-Fi/LWIP allocations in PSRAM, external BSS,
static RX 10, dynamic RX 32, static TX 16, cache TX 32 and RX BA window 6.

External BSS is required for memory equivalence with that verified profile.
This is opt-in: stock defaults, partitions, TLS and Wi-Fi IRAM settings are
unchanged. The broader `sdkconfig.esp32c5.ram_optimization` profile is not
this acceptance configuration. Wardrive's hardware acceptance does not
establish hardware acceptance of Net Scanner.

## Fresh build

Use a fresh build directory with its own generated sdkconfig; defaults do
not reliably override an existing configuration.

```sh
git submodule update --init --recursive
idf.py -B build-net-scanner-release -D IDF_TARGET=esp32c5 \
  -D SDKCONFIG=build-net-scanner-release/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.net_scanner_release' build size
```

Verify the generated sdkconfig contains every setting in the profile,
especially `SPIRAM_USE_MALLOC=y`,
`SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, `ALWAYSINTERNAL=512` and
`RESERVE_INTERNAL=32768`. Flash only the intended test device:

```sh
idf.py -B build-net-scanner-release -p YOUR_PORT flash
idf.py -B build-net-scanner-release -p YOUR_PORT monitor --no-reset
```

## Host checks

With GCC, Clang and sanitizer runtimes installed:

```sh
sh tools/net_scanner_hostcheck/check.sh
sh tools/net_scanner_hostcheck/test_harness.sh
```

The harness runs six pure-logic suites under ASan/UBSan and compile checks
with both GCC and Clang in C23 mode with warnings as errors. It uses isolated
temporary files, propagates compiler and test failures, and may be run from
any working directory. The separate harness regression injects compiler and
test failures. Host stubs do not verify lwIP, drivers or radio transitions;
the full firmware build is required for those source files.

## Hardware acceptance still required

On an owned test network, verify entry/exit, cancel and hold-B during each
scan/audit phase, reconnect after offline lookup, repeated sessions, missing
and populated SD cards, index rebuild/cancel, report saving and transitions
between Net Scanner, Device Inspector, Wi-Fi Guard and BLE apps. Record boot
logs, heap/stack readings and the firmware/configuration hashes. Exercise
Wi-Fi init failure and task-creation failure, and delayed ICMP completion:
ping stop/delete in the pinned SDK is asynchronous, so the caller must wait
for its end callback before releasing callback storage.
