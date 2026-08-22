# poom_button_sound

`poom_button_sound` adds an optional click sound when button press events are received over SBUS.

It is designed as a small shared utility that can be suspended while apps run, then resumed when returning to the launcher.

## Structure

```text
modules/poom_button_sound
├── CMakeLists.txt
├── README.md
├── poom_button_sound.c
└── include/
    └── poom_button_sound.h
```

## Dependencies

Declared in `modules/poom_button_sound/CMakeLists.txt`:

* `board`
* `button_driver`
* `buzzer`
* `poom_secrets_store`
* `poom_sbus`

## Public API

Header:
`modules/poom_button_sound/include/poom_button_sound.h`

```c
void poom_button_sound_init(void);
bool poom_button_sound_get_enabled_setting(void);
bool poom_button_sound_set_enabled_setting(bool enabled);
void poom_button_sound_suspend(void);
void poom_button_sound_resume(void);
```

## Runtime Behavior

The module:

1. loads the persisted enable/disable setting from `poom_secrets_store`,
2. subscribes to `input/button` over SBUS when enabled,
3. queues press events into a small FreeRTOS task,
4. plays a short buzzer tone for each accepted press-down event.

Suspend/resume changes only runtime behavior and does not overwrite the persisted setting.

## Integration

* Intended for shared UI feedback in the launcher and menus.
* Useful when apps need consistent button audio without coupling directly to buzzer code.
