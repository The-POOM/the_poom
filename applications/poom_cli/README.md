# poom_cli

`applications/poom_cli` is the POOM on-device console component. It provides the shared REPL/runtime (`poom_console_*`) plus command registrars for NFC, config/load-test helpers, drone emulation, and optional Zigbee CLI glue.

## Purpose

- Initialize and own the UART/USB console REPL.
- Register shared console helpers such as `help`.
- Expose POOM command groups that other apps can launch or embed.
- Pause and resume the REPL when another tool temporarily takes over the console.

## Structure

```text
applications/poom_cli
├── CMakeLists.txt
├── component.mk
├── README.md
├── include/
│   ├── cli.h
│   ├── cli_config.h
│   ├── cli_drone.h
│   ├── cli_nfc.h
│   ├── cli_zigbee.h
│   └── poom_cli_registry.txt
└── src/
    ├── cli.c
    ├── cli_config.c
    ├── cli_drone.c
    ├── cli_nfc.c
    ├── cli_zigbee.c
    └── poom_cli_registry.txt
```

## Dependencies

Defined in `applications/poom_cli/CMakeLists.txt`:

- `poom_sbus`
- `console`
- `nvs_flash`
- `poom_nfc`
- `poom_secrets_store`
- `poom_pcap`
- `poom_http_load_test`
- `poom_drone_emul`
- `poom_clipper`
- `poom_wifi_spam`
- `zb_cli` when `CONFIG_ZB_ENABLED=y`

## Public API

Core header: `applications/poom_cli/include/cli.h`

```c
typedef void (*ctrl_c_callback_t)(void);

void poom_console_register_ctrl_c_handler(ctrl_c_callback_t callback);
void poom_console_begin(void (*registrar_cmds_fn)(void));
void poom_console_pause(void);
void poom_console_resume(void);
bool poom_console_is_paused(void);
```

Feature entry points:

- `applications/poom_cli/include/cli_nfc.h`
  - `void cli_poom_nfc_option(void);`
  - `void cli_poom_nfc_register_cmds(void);`
- `applications/poom_cli/include/cli_config.h`
  - `void cli_poom_config_option(void);`
  - `void cli_poom_config_register_cmds(void);`
- `applications/poom_cli/include/cli_drone.h`
  - `void cli_poom_drone_register_cmds(void);`
- `applications/poom_cli/include/cli_zigbee.h`
  - `void cli_poom_zigbee_begin(void);`
  - `void cli_poom_zigbee_stop(void);`
  - `bool cli_poom_zigbee_is_running(void);`

## Runtime Behavior

1. `poom_console_begin()` initializes NVS and the ESP-IDF console REPL, then registers the base commands plus the caller-provided command group.
2. `poom_console_default()` shows the POOM banner, command help hints, and runs the linenoise-driven input loop.
3. `poom_console_pause()` tears down the active REPL so another mode can temporarily own UART/USB without console contention.
4. `poom_console_resume()` rebuilds the REPL and re-registers the last command set.
5. Feature files such as `cli_nfc.c` and `cli_config.c` can either register commands into an existing REPL or start their own REPL entry flow.

## Integration

- Use `REQUIRES poom_cli` in callers; the component directory is `applications/poom_cli`.
- The exported runtime API is POOM-namespaced through `poom_console_*`.
- `cli_zigbee.c` is only compiled when `CONFIG_ZB_ENABLED` is enabled.
- `applications/poom_app_pack/src/menu_cli_nfc.c` is the current launcher entry point that embeds this component for NFC console use.

## Usage Example

```c
#include "cli.h"
#include "cli_nfc.h"

void run_nfc_console(void)
{
    poom_console_begin(cli_poom_nfc_register_cmds);
}
```
