# poom_lua

`poom_lua` executes Lua scripts stored on the SD card and exposes a minimal, curated native API to Lua.

This module is intended for rapid prototyping and small automations without rebuilding firmware.

## Runtime behavior

- Scripts run in a dedicated FreeRTOS task (Lua VM lifetime is per-run).
- SD mount point is `/sdcard`.
- Menu runner (`THE MAKER -> LUA`) does not redraw while Lua is running so scripts can own the OLED.
- Stop is cooperative: requesting stop triggers a Lua hook and aborts execution with error `"stopped"`.

## C API

Header: `modules/poom_lua/include/poom_lua.h`

- `esp_err_t poom_lua_run_file_async(const char* abs_path, poom_lua_done_cb_t cb, void* user_ctx);`
  - Runs a script file (absolute path under `/sdcard`) in a background task.
- `bool poom_lua_is_running(void);`
  - Returns true while a script task is running.
- `esp_err_t poom_lua_request_stop(void);`
  - Requests the running script to stop (hook abort).

## Lua API

The VM registers these globals:

### `App`

General utilities.

- `App.log(message)`
  - Prints to UART (`printf("[lua] ...")`).
- `App.sleep(ms)`
  - `vTaskDelay(ms)` (integer milliseconds).
- `App.beep(freq_hz, duration_ms)`
  - Plays a tone via buzzer driver.
- `App.exit()`
  - Stops the current script (raises `"stopped"`).

### `Oled`

Minimal OLED drawing helpers (Arduboy-style wrappers).

- `Oled.clear()`
- `Oled.display()`
- `Oled.textSize(size)`
- `Oled.setCursor(x, y)`
- `Oled.print(value)`
  - Converts to string using Lua `tostring()` semantics.
- `Oled.fillRect(x, y, w, h, color=WHITE)`
- `Oled.drawRect(x, y, w, h, color=WHITE)`
- `Oled.drawHLine(x, y, w, color=WHITE)`
- `Oled.width() -> 128`
- `Oled.height() -> 64`

Colors follow Arduboy constants:

- `WHITE = 1`
- `BLACK = 0`
- `INVERT = 2`

### `Buttons`

Button events from SBUS (`input/button`).

- `Buttons.poll(timeout_ms=0) -> button, event, ts_ms`
  - Returns the next queued event, or returns nothing on timeout.
  - `timeout_ms=0` is non-blocking.

Button constants:

- `Buttons.A`, `Buttons.B`, `Buttons.LEFT`, `Buttons.RIGHT`, `Buttons.UP`, `Buttons.DOWN`
- `Buttons.SINGLE_CLICK`

### `Led`

WS2812 on-board LED strip helpers.

- `Led.init(brightness=32) -> ok`
- `Led.deinit() -> ok`
- `Led.count() -> n`
- `Led.setPixel(idx, r, g, b, w=0) -> ok` (idx is 0-based)
- `Led.fill(r, g, b, w=0) -> ok`
- `Led.clear() -> ok`
- `Led.brightness(value) -> ok` (0..255)
- `Led.show() -> ok, err`

Runtime note: first `Led.*` call takes over the strip by stopping/deinitializing `poom_led_rainbow`
and restores it when the script exits.

### `NFC`

High-level NFC reader/emulation helpers (wraps `applications/poom_nfc` controller APIs).

**Lifecycle + tech filter**

- `NFC.start() -> ok`
- `NFC.stop() -> ok`
- `NFC.setTech(tech) -> ok`
  - `tech`: `"all"|"a"|"b"|"f"|"v"|"st25tb"` (also available as constants `NFC.TECH_*`)
- `NFC.getTech() -> tech_str`

**Scan / dump**

- `NFC.scan(timeout_ms=500) -> ok, cards`
  - `cards[i]`: `{ uid_hex, uid_len, type, type_str, atqa_hex?, sak? }`
- `NFC.captureDump(timeout_ms=800) -> ok, dump`
- `NFC.dumpToSd(timeout_ms=800) -> ok, rel_path, err`
- `NFC.mfulToSd(timeout_ms=800) -> ok, rel_path, err`

**Connect + raw send (ISO-DEP / 15693)**

- `NFC.connect() -> ok`
- `NFC.send(hex_ascii) -> ok, rapdu_hex_or_nil`
- `NFC.getLastRapdu() -> rapdu_hex` (returns nothing if not available)
- `NFC.getLastProfile() -> profile_table` (returns nothing if missing)

**Tuning**

- `NFC.tune.auto() -> ok, result`
- `NFC.tune.get() -> ok, result`
- `NFC.tune.set(aat_a, aat_b) -> ok, result`

**Profiles (NVS)**

- `NFC.profiles.list() -> ok, profiles, err` (each entry has `index` 0-based)
- `NFC.profiles.add(profile_table) -> ok, added, updated, no_space, err`
- `NFC.profiles.addLast(name=nil) -> ok, added, updated, no_space, err`
- `NFC.profiles.remove(index0) -> ok, removed, err`
- `NFC.profiles.clear() -> ok, err`

**Emulation**

- `NFC.emul.isRunning() -> bool`
- `NFC.emul.getConfig() -> cfg_table`
- `NFC.emul.reset() -> ok` (requires emulation stopped)
- `NFC.emul.setMode("3a"|"t4t"|"mful") -> ok`
- `NFC.emul.setUid(uid_hex) -> ok`
- `NFC.emul.setAtqa(atqa_hex_2bytes) -> ok`
- `NFC.emul.setSak(byte) -> ok`
- `NFC.emul.setAts(ats_hex) -> ok`
- `NFC.emul.setUri(uri) -> ok` (T4T)
- `NFC.emul.setImage(path) -> ok` (MFUL; SD path)
- `NFC.emul.setFromLastProfile() -> ok`
- `NFC.emul.startLast() -> ok`
- `NFC.emul.start() -> ok`
- `NFC.emul.stop() -> ok`

## Example script

Save as `/sdcard/main.lua`:

```lua
Oled.clear()
Oled.textSize(1)
Oled.setCursor(0, 0)
Oled.print("Lua OK")
Oled.setCursor(0, 10)
Oled.print("B=STOP")
Oled.display()

while true do
  local btn, ev = Buttons.poll(50)
  if btn == Buttons.B and ev == Buttons.SINGLE_CLICK then
    App.exit()
  end
  if btn == Buttons.A and ev == Buttons.SINGLE_CLICK then
    App.beep(2000, 120)
    Led.init(32)
    Led.setPixel(0, 0, 80, 255)
    Led.show()
  end
end
```

## NFC example

Save as `/sdcard/nfc.lua`:

```lua
local ok = NFC.start()
App.log("NFC.start=" .. tostring(ok))
NFC.setTech(NFC.TECH_ALL)

while true do
  local btn, ev = Buttons.poll(50)
  if btn == Buttons.B and ev == Buttons.SINGLE_CLICK then
    NFC.stop()
    App.exit()
  end

  if btn == Buttons.A and ev == Buttons.SINGLE_CLICK then
    local ok2, cards = NFC.scan(800)
    App.log("scan ok=" .. tostring(ok2))
    if ok2 and cards and #cards > 0 then
      App.log("uid=" .. cards[1].uid_hex .. " type=" .. cards[1].type_str)
    end
  end
end
```
