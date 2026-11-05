#!/usr/bin/env python3
"""Builds assets/xmb/netplay/players.bin: how many players each game takes at once, for online play's Choose Together
rooms (src/NetplayLibrary.h) - the games of the consoles that play online, only these facts, compressed.

  make-netplay-players.py [FinalBurn Neo's source: /root/src/FBNeo]

- The cartridge consoles: libretro-database's metadat/maxusers DATs (github.com/libretro/libretro-database, licensed
  CC BY-SA 4.0 - so is this file's data; THIRD_PARTY_NOTICES.md says so): each entry's ROM CRC32 and its No-Intro
  name's key (TitleKey, as src/NetplayLibrary.cpp makes it), with its players ("users").
- The Arcade: FinalBurn Neo's drivers (its source, the app's own core): each driver's set name and its players, the
  number after its BDF_ flags in its struct BurnDriver.
The file: its size before compression (4 bytes, little-endian), then zlib's: "console<TAB>kind<TAB>key<TAB>players"
lines (kind: c CRC32, t name's key, s arcade set).
"""
import os
import re
import sys
import urllib.parse
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "xmb", "netplay", "players.bin")
BASE = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/maxusers/"
DATS = {
    "nes": ["Nintendo - Nintendo Entertainment System.dat", "Nintendo - Family Computer Disk System.dat"],
    "snes": ["Nintendo - Super Nintendo Entertainment System.dat"],
    "md": ["Sega - Mega Drive - Genesis.dat"],
    "sms": ["Sega - Master System - Mark III.dat"],
    "gg": ["Sega - Game Gear.dat"],
    "sg1000": ["Sega - SG-1000.dat"],
    "pce": ["NEC - PC Engine - TurboGrafx 16.dat"],
}


def title_key(name):
    """src/NetplayLibrary.cpp's TitleKey: before the first ( or [, ASCII letters and digits in lower case, 'the' out."""
    head = re.split(r"[(\[]", name, maxsplit=1)[0]
    words = re.findall(r"[a-z0-9]+", head.lower().encode("ascii", "replace").decode("ascii"))
    return "".join(w for w in words if w != "the")


def maxusers(console, dat):
    text = urllib.request.urlopen(BASE + urllib.parse.quote(dat), timeout=60).read().decode("utf-8", "replace")
    entries = {}
    for block in re.findall(r"game \((.*?)\n\)", text, re.S):
        name = re.search(r'comment "([^"]*)"', block)
        users = re.search(r"users (\d+)", block)
        crc = re.search(r"crc ([0-9A-Fa-f]{8})", block)
        if not users:
            continue
        n = int(users.group(1))
        if crc:
            entries[("c", crc.group(1).lower())] = n
        if name:
            key = ("t", title_key(name.group(1)))
            if key[1]:
                entries[key] = max(n, entries.get(key, 0))
    print("%-6s %-55s %5d entries" % (console, dat, len(entries)))
    return entries


def fbneo(source):
    entries = {}
    pattern = re.compile(r"struct BurnDriver\s+\w+\s*=\s*\{(.*?)\};", re.S)
    players = re.compile(r"(?:BDF_[A-Z0-9_]+(?:\s*\|\s*BDF_[A-Z0-9_]+)*|\b0)\s*,\s*(\d+)\s*,\s*(?:HARDWARE_|0x)")
    # (Its console drivers - the Mega Drive's, the NES's... - left out: the Arcade tab has the arcade boards only.)
    consoles = {"channelf", "coleco", "gba", "megadrive", "msx", "nes", "pce", "sg1000", "sms", "snes", "spectrum"}
    for root, dirs, files in os.walk(os.path.join(source, "src", "burn", "drv")):
        dirs[:] = [d for d in dirs if d not in consoles]
        for f in files:
            if not f.endswith(".cpp"):
                continue
            text = open(os.path.join(root, f), encoding="latin-1").read()
            for body in pattern.findall(text):
                name = re.match(r'\s*"([^"]+)"', body)
                n = players.search(body)
                if name and n:
                    entries[("s", name.group(1).lower())] = int(n.group(1))
    print("arcade FinalBurn Neo's drivers %5d entries" % len(entries))
    return entries


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else "/root/src/FBNeo"
    lines = ["# EmuSuNi online play: players per game. Cartridge consoles: libretro-database metadat/maxusers "
             "(CC BY-SA 4.0); Arcade: FinalBurn Neo's drivers."]
    for console, dats in DATS.items():
        merged = {}
        for dat in dats:
            for key, n in maxusers(console, dat).items():
                merged[key] = max(n, merged.get(key, 0))
        lines += ["%s\t%s\t%s\t%d" % (console, kind, key, n) for (kind, key), n in sorted(merged.items())]
    lines += ["arcade\t%s\t%s\t%d" % (kind, key, n) for (kind, key), n in sorted(fbneo(source).items())]
    raw = ("\n".join(lines) + "\n").encode("utf-8")
    packed = len(raw).to_bytes(4, "little") + zlib.compress(raw, 9)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(packed)
    print("%s: %d lines, %d bytes (%d before compression)" % (OUT, len(lines), len(packed), len(raw)))


main()
