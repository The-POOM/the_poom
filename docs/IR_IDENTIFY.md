# IR DISCOVERY

IR DISCOVERY looks for a Mute or Power On signal that works with a TV you own or have permission to test. A Mute signal can be shared by many remotes. The results list shows every associated profile in the installed database, and tracks which buttons you confirm during the profile test.

## Prepare the SD card

Use Python 3.10 or later and the [Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB) checkout. The firmware uses its `TVs` folder and the [Flipper infrared remote format](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/documentation/file_formats/InfraredFileFormats.md). The database is generated locally. The source project dedicates its database under CC0-1.0. The generator copies the upstream license and records the source URL and revision beside the prepared files. Preserve those files when distributing the prepared directory.

From the POOM checkout:

```sh
git clone --depth 1 https://github.com/Lucaslhm/Flipper-IRDB.git
python3 tools/prepare_ir_identify.py Flipper-IRDB /path/to/sd-card/ir/identify
```

The destination must be new. When updating, prepare a new directory off-card and replace the entire `ir/identify` directory together. The index and numbered profiles belong to the same build. The expected layout is:

```text
ir/
  identify/
    tv.idx
    power.idx
    profiles/
      00000000.ir
      ...
    LICENSE-Flipper-IRDB.txt
    SOURCE.txt
  remotes/
    ... saved remotes
```

The checkout at `d126fb1b6f1e114c52b4a8c19839ea65e3a9c24d` produced 93 Mute signals and 96 Power On signals across 388 profiles. The generator skipped 17 unsupported source commands. Existing Mute-only SD cards still work in Mute mode. Regenerate and copy the complete directory to enable Power On.

## Find and save a remote

1. Open **THE ZEN → IR DISCOVERY**. Aim the POOM IR emitter at the TV, select **Mute**, and press **A**.
2. Press **A** during a group as soon as you see or hear a Mute response. On **ANY TV RESPONSE?**, select **YES** if the sound muted or unmuted, even if a later command restored it. Select **YES (Other Signal)** if the TV did something else. Either response narrows the group. **REPEAT** is selected by default on the group and individual confirmation screens. **B** stops the scan after any press already in flight.
3. For the final candidates, press **A** to send each individual Mute command and select **YES** only when that press mutes or unmutes the sound. An individual **YES (Other Signal)** is kept for **VIEW OTHER HITS** after the scan. POOM keeps looking for Mute while retaining those responses for the current session. If a positive group cannot be reproduced, the scan returns to its untested candidates. When the scan runs out, choose **SLOW RETRY** for an 800 ms gap between commands or **RESTART** for the default 350 ms gap.
4. Scroll through compatible profiles with **Up/Down** and open one with **A**. **Left/Right** moves a long profile label. Test the suggested command, or use **Up/Down** to choose another. The suggestion prefers a Volume command whose signal differs across profiles when available. Sending each command requires **A**.
5. Tap **A** to send. Hold **Left** for two seconds to record the result. Choose **CORRECT ACTION** only if the TV did what the displayed button says. Choose **WRONG ACTION** if it did something else or **NO RESPONSE** if nothing happened. If actions differ, go back and try another compatible profile. Hold **A** to save the selected profile. Saving includes the full profile. If you marked any buttons as failed, POOM asks before saving. Enter a name on the on-screen keyboard. Existing names are protected. The file appears under `/ir/remotes/<name>.ir` and can be opened from **THE ZEN → IR UNIV → FLIPPER FILE**.

## Identify from standby

Open **THE ZEN → IR DISCOVERY** and select **Power On**. Put the TV in standby and press **A** at the group prompt. POOM tests the group automatically, pausing 1.5 seconds between signals. Press **A** as soon as the standby light changes or the TV starts to turn on. If the group finishes, answer **YES** if the TV powered on at any point, **NO** to advance, or **REPEAT** to retest the group. Use **B** to stop.

POOM narrows a positive group and asks you to put the TV back in standby before starting the next group. Once three or fewer candidates remain, send and confirm each individually. A confirmed candidate opens the compatible profile list. If needed, **SLOW RETRY** uses a three-second gap. Dedicated Power On signals come first in the index, followed by power toggles, which can also turn a running TV off.

For direct button shortcuts, import the saved Flipper file in [Remote Suite](REMOTE_SUITE.md).

## Find a better profile on the POOM

1. Tap **A** to send the selected command. Hold **Left** for two seconds to record what the TV did. For **WRONG ACTION**, pick the actual action from the list or type its name.
2. Record three distinct responding signals. **NO RESPONSE** is recorded for the button but adds no signal/action pair to the search.
3. Hold **Right** for two seconds. POOM searches the existing SD profiles for another profile containing all three recorded signal/action pairs. The progress screen counts checked SD profiles. Select a result with **A:USE**, then test its buttons on the **REMAPPED REMOTE** screen. **B** cancels the search or backs out of the results.
4. Hold **A** for two seconds to save. The holds activate as soon as the two seconds elapse. If the database has no matching profile, POOM returns to the current remote and keeps the observations for this session.

## Protocols and limits

- Parsed playback uses the POOM RMT driver for NEC, NECext, NEC42, NEC42ext, Samsung32, RC5, RC5X, RC6, SIRC, SIRC15, SIRC20, RCA, Pioneer and Kaseikyo. NECext in prepared files preserves both command bytes. SIRC uses three frames per press. Parsed RC5/RC5X and RC6 alternate their toggle bit on each press within a session.
- RAW playback preserves supplied timings and frequency within POOM's limits of 10–60 kHz, 7–1,024 intervals and 3 ms–2 s. It removes trailing idle intervals and rounds carrier duty cycle to thousandths. A fixed RAW capture can contain repeated frames or a fixed toggle bit.
- Deduplication uses encoded parsed payloads, including equivalent NEC and NECext frames, and exact normalized RAW data. Similar RAW captures with different measured timings remain separate candidates. RAW and parsed records may represent the same physical button.
- The SD database supports up to 4,096 indexed signals per mode and 255 supported commands per profile. Discovery reads only the relevant offsets and reuses one scratch buffer. Only buttons confirmed as performing the displayed action are marked as confirmed during the session.
- Legacy IR UNIV parsed remotes preserve their existing playback conventions. IR DISCOVERY exports use a header comment to select full NECext playback when opened in IR UNIV.

## Build and flash

Use the repository's [ESP32-C5 build instructions](BUILD_C5.md) and ESP-IDF v6.1 at `fff9895c82d744c7237be8847347bdd1b07c6643`. With the exported SDK environment active, this command uses the two defaults tracked at the feature branch's base revision:

```sh
idf.py -B build/esp32c5 -D IDF_TARGET=esp32c5 \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.esp32c5.ram_optimization' build size
idf.py -B build/esp32c5 -p /dev/ttyACM0 flash monitor
```

Use an empty build directory when changing defaults. For a binary package, use the included `flash_args` from inside the package with `esptool --chip esp32c5 --port /dev/ttyACM0 write-flash @flash_args`. For an existing installation whose bootloader and partitions should be retained, use the project's normal app-only update flow with `poom.bin`.

## Hardware checklist

- Boot the ESP32-C5 POOM with its SD card inserted. Confirm startup, back navigation and clean relaunch. Repeat with no SD card, a missing index, a truncated index, an unsupported protocol and a missing profile.
- Test known parsed and RAW-TV profiles. Watch for transient Mute changes, use **A** partway through a group, and confirm that no further unstarted press is emitted. Repeat with **B** during a frame and during the inter-command gap, then verify IR is idle after returning to the menu.
- With the TV in standby, test Power On discovery. Check the confirmation after the TV boots, then verify a volume command and save the profile.
- Test all-NO, REPEAT, slow retry, one and two candidates, uneven groups, and a deliberate false group hit that falls back to the other untested ranges.
- With a logic analyser or IR receiver, check carrier, RAW timing, SIRC spacing and RC5/RC6 toggle bits. Check that another POOM TX caller receives a busy error while IR DISCOVERY owns the pin and can send after it exits.
- Open a shared-Mute result. Confirm additional commands on one profile, save it under a new name, and replay both parsed and RAW commands through IR UNIV. Check duplicate-name handling, card removal during save, and that a previous remote remains readable.
- Smoke-test TV OFF, legacy IR UNIV learned and SD commands, and POOM Web IR after leaving IR DISCOVERY.
