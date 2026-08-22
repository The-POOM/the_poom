# poom_buz_theme

`poom_buz_theme` exposes reusable melody and sound-effect helpers for the buzzer.

The module centralizes small audio cues used across POOM menus and apps so callers do not need to hardcode note sequences in each feature.

## Structure

```text
modules/poom_buz_theme
├── CMakeLists.txt
├── README.md
├── component.mk
├── poom_buz_theme.c
└── include/
    └── poom_buz_theme.h
```

## Public API

Header:
`modules/poom_buz_theme/include/poom_buz_theme.h`

Common helpers include:

* `poom_buz_theme_mario()`
* `poom_buz_theme_zelda_treasure()`
* `poom_buz_theme_tetris()`
* `poom_buz_theme_pacman_intro()`
* `poom_buz_theme_gameboy_startup()`
* `poom_buz_theme_sonic_ring()`
* `poom_buz_theme_megaman_jump()`
* `poom_buz_theme_snake()`
* `poom_buz_theme_snake_eat_fx()`
* `poom_buz_theme_snake_gameover_fx()`
* `poom_buz_theme_init_melody()`
* `poom_buz_theme_stop()`

## Runtime Behavior

When a helper is called, the module plays a predefined melody or short FX pattern through the shared buzzer driver.

This keeps common POOM audio feedback:

* centralized,
* reusable across apps,
* easier to rename or retune later.

## Integration

* The primary API uses the `poom_buz_theme_*` prefix.
* Legacy include compatibility for older buzzer-theme code is still preserved in the codebase.
* The component keeps a flat layout at the module root and does not use a `src/` directory.
