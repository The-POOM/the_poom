# Building POOM for ESP32-C5 on Windows

The build recipe targets the POOM ESP32-C5 revision 1.0 with 8 MB flash and
8 MB PSRAM. It uses ESP-IDF **v6.1**, pinned to official commit
`fff9895c82d744c7237be8847347bdd1b07c6643`, and RISC-V GCC **15.2.0**,
build `esp-15.2.0_20251204`. The script rejects a different SDK commit or
compiler build.

## Install the SDK

Follow Espressif's [Windows setup instructions](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/get-started/windows-setup.html)
for Python, Git, and the native build tools. To use a separate SDK checkout:

```powershell
git -c core.longpaths=true clone --branch v6.1 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v6.1
Set-Location esp-idf-v6.1
.\install.ps1 esp32c5
. .\export.ps1
```

Run the remaining commands from the POOM source checkout in that exported
ESP-IDF PowerShell environment. Initialize the repository's NFC dependencies:

```powershell
git submodule update --init --recursive
```

ESP-IDF component manager installs the other dependencies from
`dependencies.lock`. Schema 3 of that lockfile is produced by component
manager 3.1.2. Use the official v6.1 Python constraints when provisioning the
environment; keep the pinned dependency versions instead of updating them.

## Build and inspect size

```powershell
.\tools\build_c5.ps1 -FirmwareVersion '1.0-r17-beast1'
.\tools\build_c5.ps1 -Action size -FirmwareVersion '1.0-r17-beast1'
```

The default output directory is `build\esp32c5`. Pass `-BuildDir` to keep the
output elsewhere. The script stores the generated `sdkconfig` inside the
output directory and combines these tracked defaults, in this order:

1. `sdkconfig.defaults`
2. `sdkconfig.esp32c5.ram_optimization`
3. `sdkconfig.esp32c5.release`

The release overlay selects size optimization, DIO flash at 80 MHz, and 8 MB
flash. The RAM defaults select quad PSRAM at 80 MHz, allocate eligible Wi-Fi
and LwIP data in PSRAM, and retain a 32 KiB internal allocation reserve.
The existing USB Serial/JTAG console and default logging configuration remain
in effect.

An existing generated `sdkconfig` takes precedence over defaults. Use a fresh
`-BuildDir` when changing the profile and verify the generated configuration
before packaging. Set `-FirmwareVersion` consistently for all actions in a
release build; omitting it uses `POOM_FIRMWARE_VERSION` if already set, or the
repository's automatic version calculation.

## Use an isolated SDK installation

The script also accepts explicit paths without a globally exported SDK. All
environment changes are limited to the script's process and are restored on
success or failure. For example, set these variables to your installations:

```powershell
$sdkRoot = 'D:\firmware-tools\esp-idf-v6.1'
$idfToolsRoot = 'D:\firmware-tools\idf-tools'
$pythonEnv = Join-Path $idfToolsRoot 'python_env\idf6.1_py3.12_env'
$toolBins = @(
    'D:\firmware-tools\riscv32-esp-elf\bin',
    (Join-Path $idfToolsRoot 'tools\cmake\4.0.3\bin'),
    (Join-Path $idfToolsRoot 'tools\ninja\1.12.1')
)
.\tools\build_c5.ps1 -IdfPath $sdkRoot -IdfToolsPath $idfToolsRoot `
    -PythonEnvironment $pythonEnv -ToolBin $toolBins `
    -RomElfDir (Join-Path $idfToolsRoot 'tools\esp-rom-elfs\20241011') `
    -BuildDir 'D:\firmware-builds\poom-c5' -FirmwareVersion '1.0-r17-beast1'
```

The verified native environment used Python 3.12.14, CMake 4.0.3, Ninja 1.12.1,
ESP ROM ELF package 20241011, and the official v6.1 Python dependency
constraints. Mbed TLS 4.1.0 and its PSA crypto support come from this SDK.

## Output and hardware validation

`poom.bin` is the application image. `flasher_args.json` records the matching
bootloader, partition table, initial OTA data, flash settings, and offsets.
The custom partition table has two 3712 KiB application slots; the build
checks that the application fits. A size report's unused SRAM is link-time
headroom and does not measure the heap remaining while an action runs.

The script supports compilation and size reports only. Installing firmware
is a separate device operation. Verify chip revision, flash size, the current
partition layout, and the intended update slot before using an application
image. A successful build does not establish device boot, navigation, radio
behavior, or interoperability; record those checks separately.
