#!/usr/bin/env python3
"""Builds assets/xmb/netplay/standard.bin (01.05, the owner: "Revisar mis juegos"): the standard versions of the games of
the consoles EmuSuNi plays online - only each one's CRC32, size and name, compressed - for src/NetplayLibrary.h's
StandardName (a game file's content, NetplayLibrary::ContentId: unzipped, without a copier's or an emulator's header).
Those consoles only (the owner: "solo las consolas que se juegan en online con EmuSuNi"; src/Menu.cpp's PlaysOnline):
the online rooms' - NES (and its Famicom Disk System), Super Nintendo, Mega Drive, Master System, Game Gear, SG-1000,
PC Engine, Atari 2600, Atari 7800, Arcade - and the link cable rooms' - Game Boy, Game Boy Color, Game Boy Advance.

  make-standard-games.py [--libretro DIR] [--fbneo DIR] [--out FILE]

- The cartridge consoles: No-Intro's names and CRC32s as libretro-database's metadat/no-intro DATs have them
  (github.com/libretro/libretro-database, CC BY-SA 4.0 - so is this file's data; THIRD_PARTY_NOTICES.md says so). Their
  headerless entries only: the NES's .unh (its .nes ones have the iNES header), the 7800's .bin (not its .a78), the
  FDS's .fds (without fwNES's header) - as ContentId reads a file.
- The Arcade: FinalBurn Neo v1.0.0.03's "Arcade only" DAT (the app's own core's: its source's dats folder), each set
  as ContentId reads its zip - its ROMs' CRC32s, sorted, as one CRC32 (their sizes summed): the whole set (its parent's
  ROMs in it too, those with merge=) and the split one (its own ROMs only), "nodump" ROMs left out. Its name: the set's
  description and the set's name ("Metal Slug 2 - ... [mslug2]").
--libretro: a checkout's (or a copy's) metadat folder, default ~/emusuni-ratings-cache/libretro-database/metadat
(scripts/make-ratings.py's downloads); a DAT not there is downloaded from raw.githubusercontent.com.
The file: its size before compression (4 bytes, little-endian), then zlib's: "console<TAB>crc<TAB>size<TAB>name" lines.
"""
import argparse
import os
import re
import struct
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "xmb", "netplay", "standard.bin")
LIBRETRO_RAW = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/"
# console key (src/Systems.cpp) -> its DATs, and the extensions of the entries kept (the headerless ones).
CONSOLES = {
    "nes": [("Nintendo - Nintendo Entertainment System.dat", (".unh",)),
            ("Nintendo - Family Computer Disk System.dat", (".fds",))],
    "snes": [("Nintendo - Super Nintendo Entertainment System.dat", (".sfc", ".bin"))],
    "md": [("Sega - Mega Drive - Genesis.dat", (".md", ".bin"))],
    "sms": [("Sega - Master System - Mark III.dat", (".sms",))],
    "gg": [("Sega - Game Gear.dat", (".gg", ".sms"))],
    "sg1000": [("Sega - SG-1000.dat", (".sg", ".sf"))],
    "pce": [("NEC - PC Engine - TurboGrafx 16.dat", (".pce",))],
    "atari2600": [("Atari - 2600.dat", (".a26", ".bin"))],
    "atari7800": [("Atari - 7800.dat", (".bin",))],
    "gb": [("Nintendo - Game Boy.dat", (".gb",))],
    "gbc": [("Nintendo - Game Boy Color.dat", (".gbc",))],
    "gba": [("Nintendo - Game Boy Advance.dat", (".gba",))],
}
ROM = re.compile(r'rom \( name "((?:[^"\\]|\\.)*)" size (\d+) crc ([0-9A-Fa-f]{8})')


def dat_text(folder, name):
    path = os.path.join(folder, "no-intro", name)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data = urllib.request.urlopen(LIBRETRO_RAW + urllib.parse.quote(name), timeout=120).read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def plain(name):
    """A name as the menu draws it: tabs and line ends out."""
    return re.sub(r"[\t\r\n]+", " ", name).strip()


def set_id(roms):
    """NetplayLibrary::ContentId of a set's zip: its ROMs' CRC32s sorted, as one CRC32; their sizes summed."""
    crcs = sorted(crc for crc, size in roms if size > 0)
    data = b"".join(struct.pack("<I", c) for c in crcs)
    return zlib.crc32(data) & 0xFFFFFFFF, sum(size for crc, size in roms if size > 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--libretro", default=os.path.expanduser("~/emusuni-ratings-cache/libretro-database/metadat"))
    ap.add_argument("--fbneo", default=os.path.expanduser("~/src/FBNeo"))
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()
    lines = []
    counts = {}
    for console, dats in CONSOLES.items():
        seen = set()
        for dat, extensions in dats:
            for name, size, crc in ROM.findall(dat_text(args.libretro, dat)):
                stem, ext = os.path.splitext(name)
                if ext.lower() not in extensions:
                    continue
                key = (int(crc, 16), int(size))
                if key in seen:
                    continue
                seen.add(key)
                lines.append("%s\t%08x\t%d\t%s" % (console, key[0], key[1], plain(stem)))
        counts[console] = len(seen)
    dat = os.path.join(args.fbneo, "dats", "FinalBurn Neo (ClrMame Pro XML, Arcade only).dat")
    root = ET.parse(dat).getroot()
    seen = set()
    for game in root.iter("game"):
        whole, split = [], []
        for rom in game.iter("rom"):
            if rom.get("status") == "nodump" or not rom.get("crc"):
                continue
            entry = (int(rom.get("crc"), 16), int(rom.get("size", "0")))
            whole.append(entry)
            if not rom.get("merge"):
                split.append(entry)
        name = "%s [%s]" % (plain(game.findtext("description") or game.get("name")), game.get("name"))
        for roms in (whole, split):
            if not roms:
                continue
            key = set_id(roms)
            if key in seen:
                continue
            seen.add(key)
            lines.append("arcade\t%08x\t%d\t%s" % (key[0], key[1], name))
    counts["arcade"] = len(seen)
    text = ("# EmuSuNi's standard versions (scripts/make-standard-games.py): No-Intro's names (libretro-database, "
            "CC BY-SA 4.0) and FinalBurn Neo v1.0.0.03's sets\n" + "\n".join(lines) + "\n").encode("utf-8")
    packed = struct.pack("<I", len(text)) + zlib.compress(text, 9)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(packed)
    print(" ".join("%s %d" % kv for kv in counts.items()))
    print("%d entries, %d bytes (%d before compression): %s" % (len(lines), len(packed), len(text), args.out))


if __name__ == "__main__":
    sys.exit(main())
