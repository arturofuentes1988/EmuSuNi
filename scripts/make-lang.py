#!/usr/bin/env python3
"""Builds the menu translations and their fonts (src/PS4Lang.cpp).

assets/xmb/lang/keys.txt has the English texts of the menus that came from DolphinPS4, src/<code>.txt their
translations in the same order and sections ("# ---" lines). (01.24, the owner: "quiero que todo esté habilitado para
todos los lenguajes") EmuSuNi's own texts are in src/<code>-emusuni.txt, "English = translation" lines in sections:
src/es-emusuni.txt is the reference - every other language's has the same lines, the same English, in the same order.
This writes assets/xmb/lang/<code>.ini (the first part, then the EmuSuNi one) and, for Japanese, Chinese and Korean,
small fonts with only the characters the menus use: assets/xmb/fonts/NotoSans{JP,SC,KR}-{Regular,Bold}.ttf, cut from
Google's Noto Sans variable fonts (SIL Open Font License) in $NOTO_DIR (default ~/noto: NotoSansJP.ttf,
NotoSansSC.ttf, NotoSansKR.ttf). Without fontTools or those fonts the fonts are left as they are and the characters
each language uses that its font lacks are counted.

  make-lang.py            check and write the .ini files (and the fonts when it can)
fontTools: python3 -m venv ~/fontenv && ~/fontenv/bin/pip install fonttools; run it with ~/fontenv/bin/python3.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG = os.path.join(HERE, "..", "assets", "xmb", "lang")
FONTS = os.path.join(HERE, "..", "assets", "xmb", "fonts")
NOTO = os.environ.get("NOTO_DIR", os.path.expanduser("~/noto"))
CJK = {"ja": "JP", "zh": "SC", "ko": "KR"}
# Settings -> Language shows every language in its own script.
LANGUAGE_NAMES = "English Español Français Deutsch Italiano Português Nederlands Polski Türkçe Русский 日本語 中文 (简体) 한국어"
# ((01.72) PS Vita Language's own list - src/Settings.cpp, vita_language -: Chinese Traditional too.)
LANGUAGE_NAMES += " 中文 (繁體)"
PRINTF = re.compile(r"%[-+0#]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXfFeEgGcsp](?![A-Za-z])")


def sections(path):
    """[(section name, [lines])] without comments; sections start at '# ---' lines."""
    out = [("start", [])]
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n").rstrip("\r")
        if line.startswith("# ---"):
            out.append((line[5:].strip(), []))
        elif line.startswith("#") or not line.strip():
            continue
        else:
            out[-1][1].append(line)
    return out


def placeholders(text):
    return len(re.findall(r"\{\d?\}", text))


def same_placeholders(english, translation):
    """{} kept (the same count, or {1}..{n} each), the printf ones and the \\n escapes the same."""
    n = english.count("{}")
    numbered = re.findall(r"\{(\d)\}", translation)
    if numbered:
        braces = translation.count("{}") == 0 and set(numbered) == {str(i) for i in range(1, n + 1)}
    else:
        braces = translation.count("{}") == n
    return (braces and sorted(PRINTF.findall(english)) == sorted(PRINTF.findall(translation)) and
            english.count("\\n") == translation.count("\\n"))


def emusuni_lines(path):
    return [l.rstrip("\n").rstrip("\r") for l in open(path, encoding="utf-8")]


def build(code, keys, reference):
    trans = sections(os.path.join(LANG, "src", code + ".txt"))
    if [s[0] for s in trans] != [s[0] for s in keys]:
        sys.exit(f"{code}: sections differ: {[s[0] for s in trans]}")
    own = os.path.join(LANG, "src", code + "-emusuni.txt")
    sources = (f"src/{code}.txt and src/{code}-emusuni.txt - edit those" if os.path.exists(own) else
               f"src/{code}.txt - edit that file")
    lines = [f"# {code}: written by scripts/make-lang.py from {sources}"]
    used = set()
    for (name, english), (_, translated) in zip(keys, trans):
        if len(english) != len(translated):
            sys.exit(f"{code}: section '{name}' has {len(translated)} lines, keys.txt {len(english)}")
        for e, t in zip(english, translated):
            if placeholders(e) != placeholders(t):
                sys.exit(f"{code}: placeholders differ:\n  {e}\n  {t}")
            if " = " in e:
                sys.exit(f"' = ' in an English text: {e}")
            lines.append(f"{e} = {t}")
            used.update(t)
    texts = len(lines) - 1
    if os.path.exists(own):
        mine = emusuni_lines(own)
        if len(mine) != len(reference):
            sys.exit(f"{code}: src/{code}-emusuni.txt has {len(mine)} lines, src/es-emusuni.txt {len(reference)}")
        for i, (r, m) in enumerate(zip(reference, mine), 1):
            if r.startswith("#") or not r.strip():
                if r != m:
                    sys.exit(f"{code}-emusuni.txt:{i}: a comment or blank line differs from es-emusuni.txt's:\n  {m}")
                continue
            if " = " not in m:
                sys.exit(f"{code}-emusuni.txt:{i}: no ' = ': {m}")
            english, t = m.split(" = ", 1)
            if english != r.split(" = ", 1)[0]:
                sys.exit(f"{code}-emusuni.txt:{i}: its English differs from es-emusuni.txt's:\n  {english}")
            if not t.strip() or not same_placeholders(english, t):
                sys.exit(f"{code}-emusuni.txt:{i}: empty, or its placeholders ({{}} % \\n) differ:\n  {m}")
            used.update(t)
            texts += 1
        lines += mine
    else:
        print(f"{code}: no src/{code}-emusuni.txt: EmuSuNi's own texts show in English")
    open(os.path.join(LANG, code + ".ini"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"{code}: {texts} texts")
    return used


def font_chars(path):
    """The characters a TrueType font maps (its cmap's formats 4 and 12), without fontTools."""
    d = open(path, "rb").read()
    tables = {d[12 + 16 * i:16 + 16 * i]: struct.unpack(">II", d[20 + 16 * i:28 + 16 * i])
              for i in range(struct.unpack(">H", d[4:6])[0])}
    off = tables[b"cmap"][0]
    chars = set()
    for i in range(struct.unpack(">H", d[off + 2:off + 4])[0]):
        s = off + struct.unpack(">I", d[off + 8 + 8 * i:off + 12 + 8 * i])[0]
        fmt = struct.unpack(">H", d[s:s + 2])[0]
        if fmt == 4:
            n = struct.unpack(">H", d[s + 6:s + 8])[0] // 2
            ends = struct.unpack(">%dH" % n, d[s + 14:s + 14 + 2 * n])
            starts = struct.unpack(">%dH" % n, d[s + 16 + 2 * n:s + 16 + 4 * n])
            for a, b in zip(starts, ends):
                chars.update(range(a, b + 1))
        elif fmt == 12:
            for g in range(struct.unpack(">I", d[s + 12:s + 16])[0]):
                a, b, _ = struct.unpack(">III", d[s + 16 + 12 * g:s + 28 + 12 * g])
                chars.update(range(a, b + 1))
    chars.discard(0xFFFF)
    return chars


def wanted(code, chars):
    text = set(chars) | set(LANGUAGE_NAMES)
    # Kana in full for Japanese (save names, game titles); the rest only as used.
    if code == "ja":
        text |= {chr(c) for c in range(0x3040, 0x3100)}
    text |= {chr(c) for c in range(0x3000, 0x3040)}  # CJK punctuation
    text |= {chr(c) for c in range(0xFF01, 0xFF5F)}  # full-width forms
    return text


def make_font(code, chars):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    from fontTools import subset

    source = os.path.join(NOTO, f"NotoSans{CJK[code]}.ttf")
    text = wanted(code, chars)
    for weight, name in ((400, "Regular"), (800, "Bold")):
        font = instancer.instantiateVariableFont(TTFont(source), {"wght": weight})
        options = subset.Options()
        options.layout_features = ["*"]
        options.name_IDs = ["*"]
        subsetter = subset.Subsetter(options)
        subsetter.populate(text="".join(sorted(text)))
        subsetter.subset(font)
        out = os.path.join(FONTS, f"NotoSans{CJK[code]}-{name}.ttf")
        font.save(out)
        print(f"  {os.path.basename(out)}: {os.path.getsize(out) // 1024} KiB, {len(text)} characters")


def main():
    keys = sections(os.path.join(LANG, "keys.txt"))
    reference = emusuni_lines(os.path.join(LANG, "src", "es-emusuni.txt"))
    used = {}
    for name in sorted(os.listdir(os.path.join(LANG, "src"))):
        if name.endswith(".txt") and not name.endswith("-emusuni.txt"):
            code = name[:-4]
            used[code] = build(code, keys, reference)
    try:
        import fontTools  # noqa: F401
        can = all(os.path.exists(os.path.join(NOTO, f"NotoSans{CJK[c]}.ttf")) for c in CJK)
    except ImportError:
        can = False
    for code in CJK:
        if can:
            make_font(code, used.get(code, set()))
    # What each language would miss: the app merges the three fonts (PS4Lang::ExtraFonts), so a character missing
    # from all three (a code point with no character, as U+3040, left out).
    have = set()
    for code in CJK:
        have |= font_chars(os.path.join(FONTS, f"NotoSans{CJK[code]}-Regular.ttf"))
    for code in CJK:
        missing = sorted(c for c in used.get(code, set()) | set(LANGUAGE_NAMES) if ord(c) > 0x2E7F and ord(c) not in have)
        print(f"  NotoSans{CJK[code]}: {'cut again' if can else 'as it was (no fontTools or no Noto fonts)'}; "
              f"{len(missing)} characters of {code}.ini missing from the three fonts{': ' if missing else ''}" +
              "".join(missing[:40]))


if __name__ == "__main__":
    main()
