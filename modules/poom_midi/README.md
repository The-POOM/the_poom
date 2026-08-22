# poom_midi

`poom_midi` groups reusable BLE MIDI output, harmony, and playback helpers used by POOM music-oriented apps and tools.

It separates three concerns:

* BLE MIDI transport output,
* music-theory helpers for scales/chords/patterns,
* a small JSON-driven playback engine.

## Structure

```text
modules/poom_midi
├── CMakeLists.txt
├── README.md
├── poom_midi_ble_out.c
├── poom_midi_harmony.c
├── poom_midi_player.c
└── include/
    ├── poom_midi_ble_out.h
    ├── poom_midi_harmony.h
    └── poom_midi_player.h
```

## Dependencies

Declared in `modules/poom_midi/CMakeLists.txt`:

* `cjson`
* `ble_midi`
* `esp_timer`

## Public API

Headers:

* `modules/poom_midi/include/poom_midi_ble_out.h`
* `modules/poom_midi/include/poom_midi_harmony.h`
* `modules/poom_midi/include/poom_midi_player.h`

Main helpers include:

* transport:
  * `poom_midi_ble_out_init()`
  * `poom_midi_ble_out_note_on()`
  * `poom_midi_ble_out_note_off()`
  * `poom_midi_ble_out_all_notes_off()`
* harmony:
  * `poom_midi_parse_key_semitone()`
  * `poom_midi_parse_scale()`
  * `poom_midi_parse_pattern()`
  * `poom_midi_parse_degree()`
  * `poom_midi_harmony_build_triad()`
* player:
  * `poom_midi_player_load_json()`
  * `poom_midi_player_validate_json()`
  * `poom_midi_player_play()`
  * `poom_midi_player_stop()`
  * `poom_midi_player_pause()`
  * `poom_midi_player_resume()`
  * `poom_midi_player_update()`

## Runtime Behavior

The player module accepts a JSON harmony config and schedules note output over BLE MIDI.

That flow typically looks like:

1. parse and validate JSON config,
2. derive notes/chords from key + scale + pattern,
3. emit note/program-change events through `poom_midi_ble_out`,
4. advance timing from an internal task plus scheduler updates.

## Integration

* This module is reusable infrastructure for apps such as `poom_motion_midi` and menu-driven MIDI tools.
* The three submodules can also be used independently if a caller only needs transport, only harmony helpers, or only the player.
