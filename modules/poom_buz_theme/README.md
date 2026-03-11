# poom_buz_theme

`poom_buz_theme` exposes melody and FX playback helpers for the buzzer.

## Public API
- `poom_buz_theme_mario()`
- `poom_buz_theme_zelda_treasure()`
- `poom_buz_theme_tetris()`
- `poom_buz_theme_pacman_intro()`
- `poom_buz_theme_gameboy_startup()`
- `poom_buz_theme_sonic_ring()`
- `poom_buz_theme_megaman_jump()`
- `poom_buz_theme_snake()`
- `poom_buz_theme_snake_eat_fx()`
- `poom_buz_theme_snake_gameover_fx()`
- `poom_buz_theme_init_melody()`
- `poom_buz_theme_stop()`

## Naming
- Primary API uses `poom_buz_theme_*`.
- Legacy include `buz_theme.h` remains as compatibility aliases.

## Build layout
- Source file is at component root: `poom_buz_theme.c`
- No `src/` folder.
