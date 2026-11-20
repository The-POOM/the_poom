# Security Audit: reproducible release build

Base: upstream POOM **1.0.16**, commit
`3533ee027efcf06a610265f3c4b4be8c73548fb5`.
The feature was ported from `l3allzh4ck/the_poom` at source commit
`c703cea793c4e7edf3e3add565735014bdf10316`.
Device Inspector, Wi-Fi Guard and release version 16 remain intact.

Use ESP-IDF **`95b6f32350e1118181d1a1c375972cbcd85a6540`**
(`v6.1-dev-1391-g95b6f32350e`), the published release SDK identified
and used during Wardrive validation. ESP-IDF 6.1 final is different.
Initialize recursive SDK/project submodules and install/export that SDK.
Keep the upstream dependency versions.

## Memory profile

Use POOM's existing `sdkconfig.esp32c5.ram_optimization` alongside
`sdkconfig.defaults`. The profile enables PSRAM allocation, external BSS,
Wi-Fi/LWIP external allocation and external mbedTLS allocation.

Fresh generation on the pinned SDK selects static Wi-Fi RX16 and BA16.
Published POOM 1.0.16 and 1.0.17 binaries use RX10 and BA6. A stock-only
hardware comparison failed at 16/16 and passed at 10/6 with external mbedTLS
allocation enabled. For release-matched hardware testing, seed a separate
build directory with the passing generated stock config and pass its absolute
path as `SDKCONFIG`. Check the generated values before flashing. This keeps
the upstream profile files unchanged. Security Audit passed its standalone smoke test with that configuration.
See the [validation notes](SECURITY_AUDIT_VALIDATION.md).

## Build with the upstream profile

Use a fresh build directory with its own generated sdkconfig; defaults do
not reliably override an existing configuration.

```sh
git submodule update --init --recursive
idf.py -B build-security-audit-release -D IDF_TARGET=esp32c5 \
  -D SDKCONFIG=build-security-audit-release/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization' build size
```

Verify the generated PSRAM and allocator settings, including
`SPIRAM_USE_MALLOC=y`,
`SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, `ALWAYSINTERNAL=512` and
`RESERVE_INTERNAL=32768`. For the hardware run, also verify static RX10,
BA6 and external mbedTLS allocation before flashing.

```sh
idf.py -B build-security-audit-release -p YOUR_PORT flash
idf.py -B build-security-audit-release -p YOUR_PORT monitor --no-reset
```

## Host checks

The optional host harness, tests and stubs are preserved at source commit
`320f0e8` on the local `validation/security-audit-pr51` branch. The firmware
build has no host-test dependency.

## Hardware regression checks

On an owned test network, verify entry/exit, cancel and hold-B during each
scan/audit phase, reconnect after offline lookup, repeated sessions, missing
and populated SD cards, index rebuild/cancel, report saving and transitions
between Security Audit, Device Inspector, Wi-Fi Guard and BLE apps. Record boot
logs, heap/stack readings and the firmware/configuration hashes. Exercise
Wi-Fi init failure and task-creation failure, and delayed ICMP completion:
ping stop/delete in the pinned SDK is asynchronous, so the caller must wait
for its end callback before releasing callback storage.
