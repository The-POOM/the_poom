#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a versioned POOM TV discovery indexes and portable Flipper profiles."""

import argparse
from collections import defaultdict
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

HEADER = struct.Struct("<4sII")
SIGNAL = struct.Struct("<III")
PROFILE = struct.Struct("<IB3x64s")
# Bit widths used by ir_tx_send_press(). NECext has two full command bytes.
PROTOCOLS = {
    "NEC": (8, 8), "NECext": (16, 16), "NEC42": (13, 8),
    "NEC42ext": (26, 16), "Samsung32": (8, 8), "RC5": (5, 6),
    "RC5X": (5, 7), "RC6": (8, 8), "SIRC": (5, 7),
    "SIRC15": (8, 7), "SIRC20": (13, 7), "RCA": (4, 8),
    "Pioneer": (8, 8), "Kaseikyo": (26, 10),
}
ALIASES = {
    "Mute": ("Mute", "0Mute", "MuteToggle", "ToggleMute", "VolMute",
             "VolumeMute", "AudioMute", "SoundMute", "Muting", "Silence"),
    "Volume Up": ("VolUp", "Vol+", "VolumeUp", "Volume+", "V+", "VolInc"),
    "Volume Down": ("VolDn", "VolDown", "Vol-", "VolumeDn", "VolumeDown",
                    "Volume-", "V-", "VolDec"),
    "Channel Up": ("ChUp", "Ch+", "ChNext", "ChannelUp", "Channel+", "ChannelNext"),
    "Channel Down": ("ChDn", "ChDown", "Ch-", "ChPrev", "ChannelDown",
                     "Channel-", "ChannelPrev"),
    "Power": ("Power", "0Power", "PowerToggle", "OnOff", "TVPower", "TVOnOff"),
    "Power On": ("PowerOn", "PowerOn(3a)", "TVPowerOn"),
    "Power Off": ("PowerOff", "PowerOffOnly", "PowerOff(3b)", "TVPowerOff"),
    "Input": ("Input", "Source", "InputSource"),
    "OK": ("OK", "Enter", "Select"),
    "Back": ("Back", "Return"),
}
FUNCTIONS = ("Volume Up", "Volume Down", "Mute", "Power", "Power On", "Power Off", "Channel Up",
             "Channel Down", "Input", "Up", "Down", "Left", "Right", "OK",
             "Back", "Menu", "Home")
CANONICAL = {re.sub(r"[^a-z0-9+-]", "", alias.lower()): name
             for name in (*FUNCTIONS, *ALIASES) for alias in ALIASES.get(name, (name,))}


def records(path):
    fields = {}
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if line.startswith("#") or line.startswith("name:"):
            if "name" in fields:
                yield fields
            fields = {}
        key, sep, value = line.partition(":")
        if sep:
            fields[key] = value.strip()
    if "name" in fields:
        yield fields


def hex_payload(text):
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}\s+){3}[0-9a-fA-F]{2}", text):
        raise ValueError("expected four bytes")
    return int.from_bytes(bytes.fromhex(text), "little")


def parse_signal(fields):
    if fields["type"] == "parsed":
        protocol = fields["protocol"]
        address_bits, command_bits = PROTOCOLS[protocol]
        address, command = (hex_payload(fields[key]) for key in ("address", "command"))
        if address >= 1 << address_bits or command >= 1 << command_bits:
            raise ValueError("payload out of range")
        return protocol, address, command
    if fields["type"] != "raw":
        raise ValueError("unsupported type")
    hz = int(fields["frequency"])
    duty = int(float(fields["duty_cycle"]) * 1000 + 0.5)
    timings = tuple(map(int, fields["data"].split()))
    if (not 10000 <= hz <= 60000 or not 0 < duty < 1000 or
            not 7 <= len(timings) <= 1024 or min(timings) < 1 or
            not 3000 <= sum(timings) <= 2000000):
        raise ValueError("unsupported RAW capture")
    if len(timings) % 2 == 0:
        timings = timings[:-1]  # Drop the receiver's trailing idle space.
    if sum(timings) < 3000:
        raise ValueError("short RAW waveform")
    return "raw", hz, duty, timings


def waveform_key(signal):
    if signal[0] == "RC5X":
        # The protocol itself encodes the extended field bit. Sources may
        # include that bit in the displayed command or leave it implicit.
        _, address, command = signal
        return "RC5X", address, command & 63
    if signal[0] in ("NEC", "NECext"):
        protocol, address, command = signal
        if protocol == "NEC":
            address |= (address ^ 255) << 8
            command |= (command ^ 255) << 8
        return "NECext", address, command
    return signal


def command_text(name, signal):
    if signal[0] == "raw":
        _, hz, duty, durations = signal
        data = " ".join(map(str, durations))
        return (f"name: {name}\ntype: raw\nfrequency: {hz}\n"
                f"duty_cycle: {duty / 1000:.3f}\ndata: {data}\n#\n")
    protocol, address, command = signal
    address = address.to_bytes(4, "little").hex(" ").upper()
    command = command.to_bytes(4, "little").hex(" ").upper()
    return (f"name: {name}\ntype: parsed\nprotocol: {protocol}\n"
            f"address: {address}\ncommand: {command}\n#\n")


def recommendation(profile, alternatives, profiles):
    commands = profile[1]
    shared = defaultdict(set)
    for other in alternatives:
        for _, key, signal in profiles[other][1]:
            shared[key].add(waveform_key(signal))
    for index, (_, key, signal) in enumerate(commands):
        if key != "Mute" and not key.startswith("Power") and (
                shared[key] - {waveform_key(signal)}):
            return index
    for index, (_, key, _) in enumerate(commands):
        if key in ("Volume Up", "Volume Down"):
            return index
    for index, (_, key, _) in enumerate(commands):
        if key != "Mute" and not key.startswith("Power"):
            return index
    return 255


def write_index(path, name, groups, signals, profiles):
    with path.open("wb") as out:
        out.write(HEADER.pack(b"PID1", len(groups), len(profiles)))
        index_offset = out.tell()
        out.write(bytes(SIGNAL.size * len(groups)))
        index = []
        for key, members in groups.items():
            profiles_offset = out.tell()
            for member in sorted(members):
                label = profiles[member][0].encode("ascii")
                hint = recommendation(profiles[member], members - {member}, profiles)
                out.write(PROFILE.pack(member, hint, label))
            signal_offset = out.tell()
            out.write(command_text(name, signals[key]).encode("ascii"))
            index.append(SIGNAL.pack(signal_offset, profiles_offset, len(members)))
        out.seek(index_offset)
        out.writelines(index)


def prepare(source, target):
    indexes = (
        ("tv.idx", "Mute", (defaultdict(set), {})),
        ("power.idx", "Power On", (defaultdict(set), {})),
    )
    power_order = set()
    profiles = []
    skipped = 0
    for path in sorted((source / "TVs").rglob("*")):
        if not path.is_file() or path.suffix.lower() != ".ir":
            continue
        commands = []
        seen = set()
        for fields in records(path):
            name = fields["name"]
            key = CANONICAL.get(re.sub(r"[^a-z0-9+-]", "", name.lower()), name)
            try:
                if not name or not name.isascii() or not name.isprintable():
                    raise ValueError("invalid button label")
                signal = parse_signal(fields)
            except (ValueError, KeyError, OverflowError):
                skipped += 1
                continue
            identity = key, waveform_key(signal)
            if identity not in seen:
                seen.add(identity)
                commands.append((key[:20], key, signal))
        if not any(key in ("Mute", "Power On", "Power") for _, key, _ in commands):
            continue
        commands.sort(key=lambda entry: FUNCTIONS.index(entry[1])
                      if entry[1] in FUNCTIONS else len(FUNCTIONS))
        if len(commands) > 255:
            raise ValueError(f"More than 255 supported commands in {path}")
        index = len(profiles)
        label = path.relative_to(source / "TVs").with_suffix("").as_posix()
        label = "".join(c if c.isascii() and c.isprintable() else "?" for c in label)[-63:]
        profiles.append((label, commands))
        for _, name, signal in commands:
            if name == "Mute":
                groups, signals = indexes[0][2]
            elif name in ("Power On", "Power"):
                groups, signals = indexes[1][2]
                if name == "Power On":
                    power_order.add(waveform_key(signal))
            else:
                continue
            key = waveform_key(signal)
            signals.setdefault(key, signal)
            groups[key].add(index)

    power_groups, power_signals = indexes[1][2]
    indexes = (indexes[0], ("power.idx", "Power On", (
        dict(sorted(power_groups.items(), key=lambda entry: entry[0] not in power_order)),
        power_signals,
    )))
    if not profiles or any(len(groups) > 4096 for _, _, (groups, _) in indexes):
        raise ValueError("Expected compatible TV profiles and at most 4096 signals per mode")
    profile_dir = target / "profiles"
    profile_dir.mkdir()
    for index, (label, commands) in enumerate(profiles):
        text = f"Filetype: IR signals file\nVersion: 1\n# {label}\n# POOM IR IDENTIFY\n"
        text += "".join(command_text(name, signal) for name, _, signal in commands)
        (profile_dir / f"{index:08x}.ir").write_bytes(text.encode("ascii"))

    for filename, name, (groups, signals) in indexes:
        write_index(target / filename, name, groups, signals, profiles)
        print(f"{len(groups)} distinct {name} signals, {len(profiles)} profiles, "
              f"{sum(map(len, groups.values()))} associations")
    print(f"{skipped} skipped source commands")

    shutil.copyfile(source / "LICENSE", target / "LICENSE-Flipper-IRDB.txt")
    try:
        revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                           stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        revision = "unknown (source archive)"
    (target / "SOURCE.txt").write_text(
        "Flipper-IRDB TV profiles, dedicated under CC0-1.0\n"
        "https://github.com/Lucaslhm/Flipper-IRDB\n"
        f"Revision: {revision}\n"
        "POOM filtered supported signals, normalized names and built Mute and Power On indexes.\n",
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Flipper-IRDB checkout root")
    parser.add_argument("destination", type=Path, help="new SD-card ir/identify directory")
    args = parser.parse_args()
    if args.destination.exists() or not (args.source / "TVs").is_dir():
        parser.error("use a Flipper-IRDB checkout and a destination that does not exist")
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.destination.parent) as directory:
        stage = Path(directory) / "identify"
        stage.mkdir()
        prepare(args.source, stage)
        stage.rename(args.destination)


if __name__ == "__main__":
    main()
