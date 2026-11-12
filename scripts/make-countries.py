#!/usr/bin/env python3
"""EmuSuNi 01.07 (the owner: "Soy de ..." in each player's language): assets/xmb/netplay/countries.tsv - the countries'
names (ISO 3166-1 alpha-2 codes, as the online server shows a player's country) in the app's 13 languages, from the
Unicode CLDR (cldr-json's cldr-localenames-full: main/<language>/territories.json), with the Unicode License v3 at its
top as that license asks (THIRD_PARTY_NOTICES.md). Only the names: no regions ("001", "419"), no variants ("-alt-"), no
pseudo-codes (EU, EZ, UN, QO, XA, XB, ZZ). Japanese, Chinese and Korean: the app's fonts for them have only the
characters its texts use (assets/xmb/fonts/NotoSans{JP,SC,KR}-*.ttf) - a name with one they lack is the English one
(drawn whole, never a missing glyph's box), until those fonts are made again with them.
    make-countries.py <folder of <language>/territories.json and LICENSE> <out.tsv> [CLDR version]
"""
import json
import os
import struct
import sys

LANGUAGES = ("en", "es", "fr", "de", "it", "pt", "nl", "pl", "tr", "ru", "ja", "zh", "ko")  # (src/PS4Lang.cpp's)
LEFT_OUT = {"EU", "EZ", "UN", "QO", "XA", "XB", "ZZ"}
FONTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "xmb", "fonts")
CJK = {"ja": "JP", "zh": "SC", "ko": "KR"}


def font_chars(path):
    """The characters a TrueType font has (its cmap's Unicode subtables: formats 4 and 12)."""
    data = open(path, "rb").read()
    tables = struct.unpack(">H", data[4:6])[0]
    cmap = None
    for i in range(tables):
        tag, _, offset, _ = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if tag == b"cmap":
            cmap = offset
    chars = set()
    count = struct.unpack(">H", data[cmap + 2:cmap + 4])[0]
    for i in range(count):
        platform, encoding, offset = struct.unpack(">HHI", data[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
        if platform != 3 and platform != 0:
            continue
        at = cmap + offset
        fmt = struct.unpack(">H", data[at:at + 2])[0]
        if fmt == 4:
            segs = struct.unpack(">H", data[at + 6:at + 8])[0] // 2
            ends = struct.unpack(">%dH" % segs, data[at + 14:at + 14 + 2 * segs])
            starts = struct.unpack(">%dH" % segs, data[at + 16 + 2 * segs:at + 16 + 4 * segs])
            for s, e in zip(starts, ends):
                if s != 0xFFFF:
                    chars.update(chr(c) for c in range(s, e + 1))
        elif fmt == 12:
            groups = struct.unpack(">I", data[at + 12:at + 16])[0]
            for g in range(groups):
                s, e, _ = struct.unpack(">III", data[at + 16 + 12 * g:at + 28 + 12 * g])
                chars.update(chr(c) for c in range(s, e + 1))
    return chars


def main():
    folder, out = sys.argv[1], sys.argv[2]
    version = sys.argv[3] if len(sys.argv) > 3 else "46.0.0"
    names = {}
    for lang in LANGUAGES:
        with open(os.path.join(folder, lang, "territories.json"), encoding="utf-8") as f:
            data = json.load(f)
        territories = data["main"][lang]["localeDisplayNames"]["territories"]
        names[lang] = {k: v for k, v in territories.items()
                       if len(k) == 2 and k.isalpha() and k.isupper() and k not in LEFT_OUT}
    codes = sorted(set().union(*(set(n) for n in names.values())))
    # (The CJK fonts' characters: the menus load the three together - src/PS4Lang.cpp's ExtraFonts -, both weights: a
    # name drawn with either.)
    have = None
    for weight in ("Regular", "Bold"):
        chars = set()
        for script in CJK.values():
            chars |= font_chars(os.path.join(FONTS, "NotoSans%s-%s.ttf" % (script, weight)))
        have = chars if have is None else have & chars
    for lang in CJK:
        kept = 0
        for code in list(names[lang]):
            if all(c in have or c.isascii() for c in names[lang][code]):
                kept += 1
            else:
                del names[lang][code]
        print("%s: %d of %d names in its font (the others in English)" % (lang, kept, len(codes)))
    with open(os.path.join(folder, "LICENSE"), encoding="utf-8") as f:
        license_text = f.read().strip()
    lines = ["# The countries' names (ISO 3166-1 alpha-2) in EmuSuNi's languages, from the Unicode CLDR (cldr-json's "
             "cldr-localenames-full %s) - scripts/make-countries.py. A tab between columns." % version, "#"]
    lines += [("# " + line).rstrip() for line in license_text.splitlines()]
    lines.append("code\t" + "\t".join(LANGUAGES))
    for code in codes:
        row = [names[lang].get(code, names["en"].get(code, code)).replace("\t", " ") for lang in LANGUAGES]
        lines.append(code + "\t" + "\t".join(row))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d countries, %d languages" % (out, len(codes), len(LANGUAGES)))


if __name__ == "__main__":
    main()
