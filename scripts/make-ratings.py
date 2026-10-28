#!/usr/bin/env python3
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
"""Makes the index of names the star ratings are matched with (src/Ratings.h): assets/xmb/ratings/<console key>.tsv,
one file per console, each line a name a game is known by and its game's id in the LaunchBox Games Database:
    name<TAB>LaunchBox id
The ratings themselves - LaunchBox's community ratings, the average of its players' own, with how many voted - are
not in the package (LaunchBox gives its data no licence to hand on): each PS4 downloads them from LaunchBox
(https://gamesdb.launchbox-app.com/Metadata.zip) and keeps the rated games of its consoles (src/Ratings.cpp: Extract).

- A console's games: their No-Intro / Redump names (libretro-database's metadat, CC BY-SA 4.0 - the names of
  libretro's thumbnails, which the covers are matched with), each region's, matched to LaunchBox's rated games by
  their words as the app compares names (src/CoverDownload.cpp: TitlePart, Words), LaunchBox's own names and its
  other ones; a name that matches none takes the game of its region's sibling (the same game code in the cartridge's
  serial: "BPES" Pokemon - Edicion Esmeralda (Spain), "BPEE" Pokemon - Emerald Version). A title of one game is
  written once, without its region and tags; one title of two games (Tetris and Tengen's Tetris) keeps each name
  whole, for the app to choose by the region and the tags. (The app adds LaunchBox's own names from the copy it
  downloads.)
- The Arcade (FinalBurn Neo), the NAOMI and the Atomiswave: each set by its name (the zip's: "mslug2"), as MAME's
  list in LaunchBox's Metadata.zip names it (Mame.xml), else by its full name (FinalBurn Neo's DAT, Flycast's list);
  the NAOMI's full names too (its cartridge images are matched by them).
Names are written in plain ASCII ("Pokemon", not "Pokémon").

Sources, downloaded when not given:
  --metadata  LaunchBox's https://gamesdb.launchbox-app.com/Metadata.zip (about 110 MB; Metadata.xml and Mame.xml)
  --libretro  a checkout of github.com/libretro/libretro-database (metadat/no-intro, metadat/redump), else the
              files needed from raw.githubusercontent.com
  --fbneo     the app's FinalBurn Neo checkout (scripts/build-arcade-core.sh: dats/FinalBurn Neo (ClrMame Pro XML,
              Arcade only).dat), default ~/src/FBNeo
  --flycast   the app's Flycast checkout (core/hw/naomi/naomi_roms.cpp), default ~/src/flycast
  --cache     where the downloads are kept (default ~/emusuni-ratings-cache)
For the PC tests (what the app makes of a download, written the app's way):
  --copy FILE      the copy the app keeps (src/Ratings.cpp: Extract): id, platform, half stars, votes, names
  --ratings DIR    each console's names with their ratings as the app joins them (Ratings::Names): name, half
                   stars, votes

  python3 scripts/make-ratings.py [--out assets/xmb/ratings] [sources...]
"""
import argparse
import collections
import os
import re
import sys
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
METADATA_URL = "https://gamesdb.launchbox-app.com/Metadata.zip"
LIBRETRO_RAW = "https://raw.githubusercontent.com/libretro/libretro-database/master/"
USER_AGENT = "EmuSuNi-make-ratings/1.0 (+https://github.com/arturofuentes1988/EmuSuNi)"

# Each console (src/Systems.cpp's keys): LaunchBox's platforms (the first is the console's own; src/Ratings.cpp has the
# same table), the libretro-database files of its names, and whether its serials link the regions' names (a
# 4-character game code, the last its region).
CONSOLES = {
    "snes": (["Super Nintendo Entertainment System", "Nintendo Satellaview"],
             ["no-intro/Nintendo - Super Nintendo Entertainment System.dat", "no-intro/Nintendo - Satellaview.dat",
              "no-intro/Nintendo - Sufami Turbo.dat"], True),
    "nes": (["Nintendo Entertainment System", "Nintendo Famicom Disk System"],
            ["no-intro/Nintendo - Nintendo Entertainment System.dat",
             "no-intro/Nintendo - Family Computer Disk System.dat"], False),
    "n64": (["Nintendo 64"], ["no-intro/Nintendo - Nintendo 64.dat"], True),
    "gb": (["Nintendo Game Boy"], ["no-intro/Nintendo - Game Boy.dat"], True),
    "gbc": (["Nintendo Game Boy Color"], ["no-intro/Nintendo - Game Boy Color.dat"], True),
    "gba": (["Nintendo Game Boy Advance"], ["no-intro/Nintendo - Game Boy Advance.dat"], True),
    "nds": (["Nintendo DS"], ["no-intro/Nintendo - Nintendo DS.dat"], True),
    "3ds": (["Nintendo 3DS"], ["no-intro/Nintendo - Nintendo 3DS.dat"], True),
    "ps1": (["Sony Playstation"], ["redump/Sony - PlayStation.dat"], False),
    "psp": (["Sony PSP", "Sony PSP Minis"], ["redump/Sony - PlayStation Portable.dat"], False),
    "dc": (["Sega Dreamcast"], ["redump/Sega - Dreamcast.dat"], False),
    "saturn": (["Sega Saturn"], ["redump/Sega - Saturn.dat"], False),
    "sg1000": (["Sega SG-1000"], ["no-intro/Sega - SG-1000.dat"], False),
    "sms": (["Sega Master System"], ["no-intro/Sega - Master System - Mark III.dat"], False),
    "gg": (["Sega Game Gear"], ["no-intro/Sega - Game Gear.dat"], False),
    "md": (["Sega Genesis"], ["no-intro/Sega - Mega Drive - Genesis.dat"], False),
    # 00.82: the PC Engine (LaunchBox's TurboGrafx-16 has the PC Engine's games too; the SuperGrafx's own five are
    # left out, as the core has none; its serials are numbers, not a game code and its region: no siblings). (00.80 to
    # 00.82 also had the Sega CD, the 32X, the PC Engine CD, the Neo Geo Pockets and the WonderSwans: gone in 00.83.)
    "pce": (["NEC TurboGrafx-16"], ["no-intro/NEC - PC Engine - TurboGrafx 16.dat"], False),
}
# The romset consoles: LaunchBox's platforms, the first preferred (a Neo Geo game is the Arcade's, then the MVS's...).
ROMSETS = {
    "arcade": ["Arcade", "SNK Neo Geo MVS", "SNK Neo Geo AES"],
    "naomi": ["Sega Naomi", "Sega Naomi 2", "Arcade"],
    "atomiswave": ["Sammy Atomiswave", "Arcade"],
}
NAMES = {"snes": "Super Nintendo", "nes": "NES", "n64": "Nintendo 64", "gb": "Game Boy", "gbc": "Game Boy Color",
         "gba": "Game Boy Advance", "nds": "Nintendo DS", "3ds": "Nintendo 3DS", "ps1": "PlayStation", "psp": "PSP",
         "dc": "Dreamcast", "saturn": "Saturn", "sg1000": "SG-1000", "sms": "Master System", "gg": "Game Gear",
         "md": "Mega Drive / Genesis", "pce": "PC Engine / TurboGrafx-16",
         "arcade": "Arcade (FinalBurn Neo)", "naomi": "NAOMI", "atomiswave": "Atomiswave"}
# The Game Boy's and the Game Boy Color's games are matched against both consoles' names at once (src/Ratings.cpp).
FAMILY = {"gb": ["gb", "gbc"], "gbc": ["gb", "gbc"]}
# src/Ratings.cpp's Table() order (the copy's platforms line lists them so).
APP_ORDER = ["snes", "nes", "n64", "gb", "gbc", "gba", "nds", "3ds", "ps1", "psp", "dc", "saturn", "sg1000", "sms", "gg",
             "md", "pce", "arcade", "naomi", "atomiswave"]
# LaunchBox's names of a console's games the app adds when this many players rated the game (src/Ratings.cpp).
EXTRA_VOTES = 3

# ---- The app's name matching (src/CoverDownload.cpp) ----
ROMAN = {"ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9"}
SPELT = {"ß": "ss", "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE", "ø": "o", "Ø": "O", "ð": "d", "Ð": "D", "þ": "th",
         "Þ": "Th", "ł": "l", "Ł": "L", "’": "'", "‘": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "...",
         "×": "x", "½": "1/2", "°": " ", "™": "", "®": "", "©": "", "·": " ", "•": " ", "¡": "", "¿": "", "\t": " "}


def plain(text):
    """For this script's own matching: ASCII, accents off ("Pokémon" -> "Pokemon"), a few letters spelt out, other
    signs dropped."""
    out = []
    for ch in text:
        if ch in SPELT:
            out.append(SPELT[ch])
        elif ord(ch) < 128:
            out.append(ch)
        else:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))
            out.append(base if base and all(ord(c) < 128 for c in base) else " ")
    return re.sub(r"  +", " ", "".join(out)).strip()


LATIN = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
EXTENDED_A = {0x100: "A", 0x102: "A", 0x104: "A", 0x101: "a", 0x103: "a", 0x105: "a", 0x112: "E", 0x113: "e",
              0x12A: "I", 0x12B: "i", 0x14C: "O", 0x14D: "o", 0x16A: "U", 0x16B: "u", 0x152: "OE", 0x153: "oe",
              0x141: "L", 0x142: "l", 0x160: "S", 0x161: "s", 0x17D: "Z", 0x17E: "z", 0x106: "C", 0x10C: "C",
              0x107: "c", 0x10D: "c"}
GENERAL = {0x2018: "'", 0x2019: "'", 0x201C: '"', 0x201D: '"', 0x2013: "-", 0x2014: "-", 0x2026: "...", 0x2022: " ",
           0x2122: ""}


def plain_app(text):
    """The app's Plain() (src/Ratings.cpp), the same table."""
    out = []
    for ch in text:
        code = ord(ch)
        if 0xC0 <= code <= 0xFF:
            out.append("AE" if code == 0xC6 else "ae" if code == 0xE6 else "ss" if code == 0xDF else LATIN[code - 0xC0])
        elif 0xA0 <= code <= 0xBF:
            out.append(" " if code in (0xA0, 0xB0, 0xB7) else "1/2" if code == 0xBD else "2" if code == 0xB2 else
                       "3" if code == 0xB3 else "")
        elif code in EXTENDED_A:
            out.append(EXTENDED_A[code])
        elif code in GENERAL:
            out.append(GENERAL[code])
        else:
            out.append(ch)
    return "".join(out)


def lb_name_app(name):
    """The app's LbName() (src/Ratings.cpp): plain, ": " as " - ", one space between words."""
    text = plain_app(name)
    out = ""
    i = 0
    while i < len(text):
        c = " " if text[i] == "\t" else text[i]
        if c == ":" and i + 1 < len(text) and text[i + 1] == " ":
            out = out.rstrip(" ") + " - "
            while i + 1 < len(text) and text[i + 1] == " ":
                i += 1
            i += 1
            continue
        if c == " " and (not out or out[-1] == " "):
            i += 1
            continue
        out += c
        i += 1
    return out.rstrip(" -").lstrip(" -")


def title_part(name):
    """The name before its first "(" or "[" (src/CoverDownload.cpp's TitlePart)."""
    for i in range(1, len(name)):
        if name[i] in "([":
            return name[:i]
    return name


def words(text):
    """Lower-case words: ' and . dropped, $ an s, any other sign between words, II to IX as digits, "the", "a",
    "an" and "and" left out (src/CoverDownload.cpp's Words)."""
    out, word = [], []

    def flush():
        if word:
            w = ROMAN.get("".join(word), "".join(word))
            if w not in ("the", "a", "an", "and"):
                out.append(w)
            word.clear()

    for ch in text:
        if ch in "'.":
            continue
        if ch == "$":
            word.append("s")
        elif ch.isascii() and ch.isalnum():
            word.append(ch.lower())
        else:
            flush()
    flush()
    return out


def key(name):
    """src/CoverDownload.cpp's NameKey."""
    return "".join(words(title_part(name)))


def lb_name(name):
    """A LaunchBox name as No-Intro writes one, for this script's matching: plain, ": " as " - "."""
    name = plain(name)
    name = re.sub(r"\s*:\s+", " - ", name)
    return name.strip(" -")


# ---- Downloads ----
def download(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print("downloading", url, file=sys.stderr)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=600) as answer, open(path + ".part", "wb") as out:
        while True:
            block = answer.read(1 << 20)
            if not block:
                break
            out.write(block)
    os.replace(path + ".part", path)
    return path


def libretro_file(args, relative):
    if args.libretro:
        return os.path.join(args.libretro, "metadat", relative)
    url = LIBRETRO_RAW + "metadat/" + urllib.parse.quote(relative)
    return download(url, os.path.join(args.cache, "libretro-database", "metadat", relative))


# ---- LaunchBox ----
class Game:
    __slots__ = ("id", "platform", "name", "rating", "votes", "alternates")

    def __init__(self, id, platform, name, rating, votes):
        self.id, self.platform, self.name, self.rating, self.votes = id, platform, name, rating, votes
        self.alternates = []

    def half_stars(self):
        return max(0, min(10, int(self.rating * 2.0 + 0.5)))


def read_launchbox(path, platforms):
    """LaunchBox's rated games of these platforms (id -> Game, with their other names, in Metadata.xml's order),
    the date of its Metadata.xml, and MAME's set names (Mame.xml: set -> its name)."""
    games = {}
    alternates = []
    with zipfile.ZipFile(path) as archive:
        date = "%04d-%02d-%02d" % archive.getinfo("Metadata.xml").date_time[:3]
        depth = 0
        with archive.open("Metadata.xml") as f:
            for event, element in ET.iterparse(f, events=("start", "end")):
                if event == "start":
                    depth += 1
                    continue
                depth -= 1
                if depth != 1:
                    continue
                if element.tag == "Game" and element.findtext("Platform") in platforms:
                    rating = element.findtext("CommunityRating")
                    votes = int(element.findtext("CommunityRatingCount") or 0)
                    id = element.findtext("DatabaseID") or ""
                    name = element.findtext("Name") or ""
                    if rating and votes > 0 and id.isdigit() and int(id) > 0 and name.strip(" \t\r\n"):
                        games[id] = Game(id, element.findtext("Platform"), name, float(rating), votes)
                elif element.tag == "GameAlternateName":
                    alternates.append((element.findtext("DatabaseID") or "", element.findtext("AlternateName") or ""))
                element.clear()
        for id, name in alternates:
            if id in games and name.strip(" \t\r\n"):
                games[id].alternates.append(name)
        mame = {}
        with archive.open("Mame.xml") as f:
            for _, element in ET.iterparse(f):
                if element.tag == "MameFile":
                    mame[(element.findtext("FileName") or "").lower()] = element.findtext("Name") or ""
                    element.clear()
    return games, date, mame


def index_games(games, platforms):
    """key -> the ids of the games of these platforms with that name (their own name: by_name, any of their names:
    by_any)."""
    by_name, by_any = collections.defaultdict(set), collections.defaultdict(set)
    for game in games.values():
        if game.platform not in platforms:
            continue
        k = key(lb_name(game.name))
        if k:
            by_name[k].add(game.id)
            by_any[k].add(game.id)
        for alternate in game.alternates:
            k = key(lb_name(alternate))
            if k:
                by_any[k].add(game.id)
    return by_name, by_any


def choose(ids, games, platforms):
    """Of several games, the console's own platform's first, then the one most voted for."""
    return max(ids, key=lambda i: (-platforms.index(games[i].platform), games[i].votes))


# ---- A console's names ----
def read_dat(path):
    """A clrmamepro DAT's games: (name, serial)."""
    entries = []
    name = serial = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("game ("):
            name, serial = None, None
        elif line.startswith('name "') and name is None:
            name = line[6:-1]
        elif line.startswith('serial "') and serial is None:
            serial = line[8:-1]
        elif line == ")" and name:
            entries.append((name, serial))
            name = None
    return entries


def game_code(serial):
    """The game code of a cartridge's serial, its region left out ("BPES" -> "BPE", "CTR-P-AMKE" -> "AMK")."""
    if not serial:
        return None
    last = re.split(r"[-\s,]", serial.strip())[-1]
    return last[:3].upper() if len(last) == 4 and last.isalnum() else None


SKIP = re.compile(r"\[BIOS\]|\((Test Program|Program|Kiosk|Enhancement Chip)\)", re.I)


def console_index(console, games, args):
    """The console's index lines: (name, LaunchBox id)."""
    platforms, dats, serials = CONSOLES[console]
    by_name, by_any = index_games(games, platforms)
    entries = []
    for dat in dats:
        path = libretro_file(args, dat)
        if os.path.exists(path):
            entries += read_dat(path)
    # Each name's game: by its own words, then by a region's sibling (its serial's game code).
    found = {}
    for name, serial in entries:
        if SKIP.search(name):
            continue
        k = key(plain(name))
        ids = by_name.get(k) or by_any.get(k)
        if ids:
            found[name] = choose(ids, games, platforms)
    if serials:
        by_code = collections.defaultdict(collections.Counter)
        for name, serial in entries:
            code = game_code(serial)
            if code and name in found:
                by_code[code][found[name]] += 1
        for name, serial in entries:
            code = game_code(serial)
            if name not in found and code in by_code and not SKIP.search(name):
                found[name] = by_code[code].most_common(1)[0][0]
    # The titles: once when all their names are one game, else each name whole.
    titles = collections.defaultdict(list)
    for name, id in found.items():
        titles[key(plain(name))].append((plain(name), id))
    lines = {}
    for k, named in titles.items():
        if len({id for _, id in named}) == 1:
            lines[plain(title_part(named[0][0])).strip()] = named[0][1]
        else:
            for name, id in named:
                lines[name] = id
    stats = {"names": len(entries), "matched": len(found), "games": len(set(lines.values()))}
    return lines, stats


# ---- The romsets ----
def read_fbneo(path):
    """FinalBurn Neo's arcade sets: name -> (title, parent)."""
    sets = {}
    root = ET.parse(path).getroot()
    for game in root.iter("game"):
        sets[game.get("name").lower()] = (game.findtext("description") or "", (game.get("cloneof") or "").lower())
    return sets


def read_flycast(path):
    """Flycast's NAOMI / NAOMI 2 / Atomiswave sets (core/hw/naomi/naomi_roms.cpp): name -> (title, parent, bios)."""
    text = open(path, encoding="utf-8", errors="replace").read()
    sets = {}
    pattern = re.compile(r'\{\s*"(\w+)",\s*(?:nullptr|NULL|"(\w*)"),\s*"((?:[^"\\]|\\.)*)",\s*[^,]+,\s*[^,]+,\s*'
                         r'(?:nullptr|NULL|"(\w*)")')
    for m in pattern.finditer(text):
        sets[m.group(1).lower()] = (m.group(3).replace('\\"', '"'), (m.group(2) or "").lower(), m.group(4) or "")
    return sets


def cover_name(title):
    """libretro's thumbnail name of a title (src/Arcade.cpp's CoverName): &*/:`<>?\\|" made '_'."""
    return re.sub(r'[&*/:`<>?\\|"]', "_", title)


def romset_index(sets, games, mame, platforms, titles_too=False):
    """Each set's LaunchBox game: by MAME's name for it (Mame.xml), else by its full name, else by its parent's."""
    by_name, by_any = index_games(games, platforms)
    # LaunchBox's titles before their subtitle ("Cyber Police ESWAT" of "Cyber Police ESWAT: Enhanced Special Weapons
    # and Tactics"): a set's name that is one game's only.
    by_first = collections.defaultdict(set)
    for game in games.values():
        parts = lb_name(game.name).split(" - ")
        if game.platform in platforms and len(parts) > 1 and key(parts[0]):
            by_first[key(parts[0])].add(game.id)

    def find(text):
        # The name, without its groups at the end ("(USA, set 1)", "(NGM-043 ~ NGH-043)"), each of its names
        # ("3 Count Bout / Fire Suplex"); then a LaunchBox title's first part.
        tried = [text]
        while True:
            stripped = re.sub(r"\s*\([^()]*\)\s*$", "", tried[-1])
            if stripped == tried[-1] or not stripped:
                break
            tried.append(stripped)
        tried += [part.strip() for part in tried[-1].split(" / ") if part.strip()]
        for name in tried:
            k = key(lb_name(name))
            ids = by_name.get(k) or by_any.get(k)
            if k and ids:
                return choose(ids, games, platforms)
        for name in tried:
            ids = by_first.get(key(lb_name(name)))
            if ids and len(ids) == 1:
                return next(iter(ids))
        return None

    lines = {}
    for name, (title, parent, *_) in sorted(sets.items()):
        id = None
        if name in mame:
            id = find(mame[name])
        if not id:
            id = find(title)
        if not id and parent:
            if parent in mame:
                id = find(mame[parent])
            if not id and parent in sets:
                id = find(sets[parent][0])
        if id:
            lines[name] = id
            if titles_too:
                lines[cover_name(title)] = id
    return lines


# ---- Writing ----
HEADER = ("# EmuSuNi star ratings (scripts/make-ratings.py): the %s names and each one's game in the LaunchBox\n"
          "# Games Database (its id: the app downloads LaunchBox's ratings itself, src/Ratings.h).\n"
          "# Names: %s.\n"
          "# Matched with LaunchBox's Metadata.xml of %s. This file is under CC BY-SA 4.0.\n"
          "# name<TAB>LaunchBox id\n")


def write_index(path, console, lines, names_from, date):
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER % (console, names_from, date))
        for name in sorted(lines, key=lambda n: (n.lower(), n)):
            if "\t" not in name and name.strip():
                out.write("%s\t%s\n" % (name, lines[name]))


def clean(text):
    return text.replace("\t", " ").replace("\n", " ").replace("\r", " ")


def write_copy(path, games, date, platforms):
    """The copy the app keeps (src/Ratings.cpp's Extract), the same lines (00.80: the platforms it was made for, the
    app's table's order, so that a copy of before a console came is made again)."""
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# EmuSuNi star ratings: the LaunchBox Games Database's community ratings of the app's consoles "
                  "(src/Ratings.h), from %s\n# LaunchBox Metadata.xml of %s: %d rated games\n"
                  "# id<TAB>platform<TAB>half stars (0-10)<TAB>votes<TAB>name<TAB>other names...\n"
                  "# platforms: %s\n" % (METADATA_URL, date, len(games), "|".join(platforms)))
        for game in games.values():
            out.write("\t".join([game.id, game.platform, str(game.half_stars()), str(game.votes), clean(game.name)] +
                                [clean(a) for a in game.alternates]) + "\n")


def app_names(console, indexes, games):
    """A console's names with their ratings as the app joins them (src/Ratings.cpp's BuildFile)."""
    family = FAMILY.get(console, [console])
    names, written = [], set()
    for member in family:
        for name, id in sorted(indexes[member].items(), key=lambda item: (item[0].lower(), item[0])):
            if console not in ROMSETS:
                written.add(key(name))
            if id in games:
                names.append((name, games[id].half_stars(), games[id].votes))
    if console in ROMSETS:
        return names
    platforms = [p for member in family for p in CONSOLES[member][0]]
    chosen = [g for g in games.values() if g.votes >= EXTRA_VOTES and g.platform in platforms]
    chosen.sort(key=lambda g: (platforms.index(g.platform), -g.votes))
    for game in chosen:
        for raw in [game.name] + game.alternates:
            name = lb_name_app(clean(raw))
            k = key(name)
            if k and k not in written:
                written.add(k)
                names.append((name, game.half_stars(), game.votes))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=os.path.join(ROOT, "assets", "xmb", "ratings"))
    parser.add_argument("--metadata")
    parser.add_argument("--libretro")
    parser.add_argument("--fbneo", default=os.path.expanduser("~/src/FBNeo"))
    parser.add_argument("--flycast", default=os.path.expanduser("~/src/flycast"))
    parser.add_argument("--cache", default=os.path.expanduser("~/emusuni-ratings-cache"))
    parser.add_argument("--copy")
    parser.add_argument("--ratings")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    metadata = args.metadata or download(METADATA_URL, os.path.join(args.cache, "Metadata.zip"))
    platforms = set(p for ps in ROMSETS.values() for p in ps)
    for p, _, _ in CONSOLES.values():
        platforms.update(p)
    games, date, mame = read_launchbox(metadata, platforms)
    os.makedirs(args.out, exist_ok=True)
    indexes = {}
    for console in CONSOLES:
        lines, stats = console_index(console, games, args)
        indexes[console] = lines
        write_index(os.path.join(args.out, console + ".tsv"), NAMES[console], lines,
                    "No-Intro's and Redump's, from libretro-database's metadat (github.com/libretro/libretro-database, "
                    "CC BY-SA 4.0)", date)
        print("%-10s %6d names of %5d games; %5d of %5d No-Intro/Redump names matched" % (
            console, len(lines), stats["games"], stats["matched"], stats["names"]))
    fbneo = os.path.join(args.fbneo, "dats", "FinalBurn Neo (ClrMame Pro XML, Arcade only).dat")
    sets = read_fbneo(fbneo)
    indexes["arcade"] = romset_index(sets, games, mame, ROMSETS["arcade"])
    write_index(os.path.join(args.out, "arcade.tsv"), NAMES["arcade"], indexes["arcade"],
                "FinalBurn Neo's set names (its arcade DAT)", date)
    print("%-10s %6d of %d sets" % ("arcade", len(indexes["arcade"]), len(sets)))
    sets = read_flycast(os.path.join(args.flycast, "core", "hw", "naomi", "naomi_roms.cpp"))
    for console, board_sets in (("naomi", {k: v for k, v in sets.items() if v[2] != "awbios"}),
                                ("atomiswave", {k: v for k, v in sets.items() if v[2] == "awbios"})):
        indexes[console] = romset_index(board_sets, games, mame, ROMSETS[console], titles_too=console == "naomi")
        write_index(os.path.join(args.out, console + ".tsv"), NAMES[console], indexes[console],
                    "Flycast's set names and full names (core/hw/naomi/naomi_roms.cpp)", date)
        print("%-10s %6d names for %d sets" % (console, len(indexes[console]), len(board_sets)))
    if args.copy:
        # (The app's table's order: src/Ratings.cpp's Table(), each platform once.)
        ordered = []
        for console in APP_ORDER:
            for p in (CONSOLES[console][0] if console in CONSOLES else ROMSETS[console]):
                if p not in ordered:
                    ordered.append(p)
        write_copy(args.copy, games, date, ordered)
    if args.ratings:
        os.makedirs(args.ratings, exist_ok=True)
        for console in list(CONSOLES) + list(ROMSETS):
            with open(os.path.join(args.ratings, console + ".tsv"), "w", encoding="utf-8", newline="\n") as out:
                for name, half, votes in app_names(console, indexes, games):
                    out.write("%s\t%d\t%d\n" % (name, half, votes))


if __name__ == "__main__":
    main()
