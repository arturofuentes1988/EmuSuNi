#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds the Atari consoles' libretro cores (00.90) for the PS4, one object each, that scripts/build.sh links into
# EmuSuNi:
#   - Stella (stella-emu/stella, ~/src/stella: its own libretro port in src/os/libretro, with its zlib): the Atari
#     2600 (.a26, .bin), GPL-2.0-or-later.                                      -> <out>/a2600_core.o, stella_retro_*
#   - ProSystem (libretro/prosystem-libretro, ~/src/prosystem-libretro: Greg Stanton's ProSystem with its game
#     database, and Osman Celimli's BupBoop CoreTone for the BupChip carts): the Atari 7800 (.a78, .bin), GPL-2.0
#     (BupBoop: zlib license). Its BIOS is optional and the player's own ("7800 BIOS (U).rom" in its system folder):
#     none is in it.                                                             -> <out>/a7800_core.o, prosystem_retro_*
#   - Virtual Jaguar (libretro/virtualjaguar-libretro, ~/src/virtualjaguar-libretro: libretro's fork, v3.11.0): the
#     Atari Jaguar (.j64, .jag, .abs, .cof), GPL-3.0. Without the Atari BIOS images it builds in, its memory on the heap,
#     no CD images (see the patches).                                            -> <out>/jaguar_core.o, vjaguar_retro_*
# None has a CMake for its libretro core: the one made here lists the sources their Makefile.common compiles (read
# from it, as git has it) with their Makefile's defines, compiled with the PS4 toolchain (toolchain/ps4-love-modern.cmake).
# No threads in any (Stella's NTSC filter's are only for its Blargg filter, which the app leaves off; Virtual Jaguar's
# emulation thread is its standalone app's).
# Not as their unix builds:
#   - Every function and variable in its own section (-ffunction-sections -fdata-sections), so the app's link
#     (--gc-sections) leaves out what nothing calls; -O3 for the PS4's CPU (btver2), Stella -Os (as fast, a third
#     smaller: every MiB of the app's image is a MiB less for the games); no link-time optimisation.
#   - Stella's game properties (DefProps.hxx) made compact: 0.1 MiB, not 1.2 (see the patch).
#   - Stella without C++ exceptions: on the PS4 a C++ exception never reaches its catch (the app ends without a word),
#     and Stella throws for its errors. Its sources are rewritten each time (from the files as git has them): each try
#     becomes if (true), each catch an else that never runs, each throw a call of emusuni_stella_throw (and throw; of
#     emusuni_stella_rethrow), which logs the error and goes back to the libretro call that was running - the game's
#     load, a frame, a state, a reset (os/libretro/emusuni_throw.cxx: setjmp/longjmp) -, which fails with Stella's
#     message instead (a frame: the game ends, RETRO_ENVIRONMENT_SHUTDOWN). nlohmann/json's throws too (JSON_THROW_USER).
#     The places where Stella throws and catches in its normal course are changed not to throw (see the patches):
#       . its file streams without iostream exceptions (Serializer: a high-score file shorter than it reads);
#       . its key and joystick mappings, empty in the libretro core, not parsed as JSON (json::parse("") throws);
#       . a file that isn't there read as nothing, one that can't be written written as nothing, the file system asked
#         without a VFS (its folders made with mkdir).
#     On the PS4 its std::exception, logic_error and runtime_error are its own (os/libretro/emusuni_stdexcept.cxx,
#     libc++'s): the app takes their destructors from the system's C++ library, another layout (checked at the end).
#   - ProSystem: SuperGame carts over 144K with all their banks and 32K of RAM in two banks, and a header with sound
#     chips' bits read as its cart type says (1942 (Extended)).
#   - Virtual Jaguar with PGO (scripts/jaguar-core.profdata.xz: see JAGUAR_PGO), and its GPU recompiled to x86-64
#     (00.96: scripts/vjaguar-jit/emusuni_gpu_jit.h, in the app's JIT arena; the frontend's virtualjaguar_emusuni_jit).
#   - Virtual Jaguar's 15 MiB memory space, its colour and 68000 opcode tables and its Jaguar CD's buffers on the heap
#     (5.5 MiB of the image otherwise); its cold code (the libretro glue, CD, debugger, link cable...) at -Os.
# Their libretro APIs become stella_retro_*, prosystem_retro_* and vjaguar_retro_*, everything else private, as for the
# other cores (CMakeLists.txt). Each object is checked at the end: only its renamed API defined globally, no weak references
# left, no static array of 1 MiB or more, no C++ exceptions (nothing that throws or unwinds), no operator new/delete of
# its own, and every symbol it needs found in the PS4 libraries the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins, libc++) or in the app (operator new/delete), none of the C++ library's from
# libSceLibcInternal (but std::terminate).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-atari-cores.sh <out dir> [clean]
#   ATARI_CORE_HOST=1 build-atari-cores.sh <out dir>   the same objects for this PC (Linux, clang 21), for the PC tests:
#                                                      no PS4 checks (ATARI_CORE_ASAN=1: with AddressSanitizer)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
STELLA_SRC="${STELLA_DIR:-$HOME/src/stella}"
PROSYSTEM_SRC="${PROSYSTEM_DIR:-$HOME/src/prosystem-libretro}"
JAGUAR_SRC="${VIRTUALJAGUAR_DIR:-$HOME/src/virtualjaguar-libretro}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/atari-cores.log"
HOST="${ATARI_CORE_HOST:-0}"
ASAN="${ATARI_CORE_ASAN:-0}"
[ "$HOST" = 1 ] || ASAN=0

python3 -I - "$STELLA_SRC" "$PROSYSTEM_SRC" "$OUT" "$JAGUAR_SRC" "$HERE" <<'PATCH'
import os
import re
import subprocess
import sys
roots = {'stella': sys.argv[1], 'prosystem': sys.argv[2], 'vjaguar': sys.argv[4]}
out_dir = sys.argv[3]
patched = {}
added = {}

def original(core, path):
    return subprocess.run(['git', '-C', roots[core], 'show', 'HEAD:' + path], check=True,
                          capture_output=True).stdout

def patch(core, path, old, new, count=1):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    key = (core, path)
    if key not in patched:
        patched[key] = original(core, path)
    text = patched[key]
    if b'\r\n' in text:
        old = old.replace(b'\n', b'\r\n')
        new = new.replace(b'\n', b'\r\n')
    assert text.count(old) == count, (core, path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)

def addfile(core, path, text):
    added[(core, path)] = text.encode()

# ---- Stella: no C++ exceptions ----
IDENT = re.compile(r'[A-Za-z_0-9]')
RAW = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\n]{0,16})\(')

def skip_space(s, i):
    while i < len(s) and s[i] in ' \t\r\n':
        i += 1
    return i

def literal_end(s, i):
    # s[i] is " or ': the index after the literal.
    q = s[i]
    j = i + 1
    while j < len(s) and s[j] != q:
        j += 2 if s[j] == '\\' else 1
    return j + 1

def separator(s, i):
    # A ' between a number's digits (1'000) isn't a character literal's (L'x' is).
    if s[i] != "'":
        return False
    j = i
    while j > 0 and (s[j - 1].isalnum() or s[j - 1] == "'"):
        j -= 1
    return j < i and s[j].isdigit()

def balanced_end(s, i, stop):
    # From i, the index of the first character in stop at depth 0 (strings, characters and comments skipped).
    depth = 0
    while i < len(s):
        c = s[i]
        if s.startswith('//', i):
            i = s.index('\n', i)
            continue
        if s.startswith('/*', i):
            i = s.index('*/', i) + 2
            continue
        if c in '"\'' and not separator(s, i):
            i = literal_end(s, i)
            continue
        if depth == 0 and c in stop:
            return i
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        i += 1
    raise AssertionError('unbalanced')

def catch_clause(decl):
    # catch (...), catch (const T&), catch (const T& e), catch (T e) -> an else that never runs, its name declared.
    decl = ' '.join(decl.split())
    m = re.match(r'^(.*?)([A-Za-z_]\w*)$', decl)
    if decl != '...' and m:
        prefix = m.group(1)
        kind = re.sub(r'\b(const|volatile)\b', ' ', prefix).replace('&', ' ').replace('*', ' ').strip()
        if kind and not prefix.rstrip().endswith('::') and (prefix.endswith(('&', '*', ' '))):
            return 'else if (const %s& %s = emusuni_no_exception<%s>(); false)' % (kind, m.group(2), kind)
    return 'else if (false)'

def no_exceptions(text):
    s = text.decode('latin-1')
    out = []
    i = 0
    n = len(s)
    changes = 0
    while i < n:
        c = s[i]
        if s.startswith('//', i):
            j = s.find('\n', i)
            j = n if j < 0 else j
            out.append(s[i:j]); i = j
            continue
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(s[i:j]); i = j
            continue
        m = RAW.match(s, i)
        if m and (i == 0 or not IDENT.match(s[i - 1])):
            j = s.index(')' + m.group(1) + '"', m.end()) + len(m.group(1)) + 2
            out.append(s[i:j]); i = j
            continue
        if c in '"\'' and not separator(s, i):
            j = literal_end(s, i)
            out.append(s[i:j]); i = j
            continue
        if IDENT.match(c) and (i == 0 or not IDENT.match(s[i - 1])):
            j = i
            while j < n and IDENT.match(s[j]):
                j += 1
            word = s[i:j]
            k = skip_space(s, j)
            if word == 'try' and k < n and s[k] == '{':
                out.append('if (true)'); i = j; changes += 1
                continue
            if word == 'catch' and k < n and s[k] == '(':
                e = balanced_end(s, k + 1, ')')
                out.append(catch_clause(s[k + 1:e])); i = e + 1; changes += 1
                continue
            if word == 'throw' and k < n and s[k] == ';':
                out.append('emusuni_stella_rethrow()'); i = j; changes += 1
                continue
            if word == 'throw' and k < n and not (s[k] == '(' and s[skip_space(s, k + 1)] == ')'):
                e = balanced_end(s, k, ';')
                out.append('emusuni_stella_throw(' + s[k:e].strip() + ')'); i = e; changes += 1
                continue
            out.append(word); i = j
            continue
        out.append(c); i += 1
    return ''.join(out).encode('latin-1'), changes

STELLA = roots['stella']
makefile = original('stella', 'src/os/libretro/Makefile.common').decode()
compiled = sorted(set(re.findall(r'\$\(CORE_DIR\)/([\w/+.-]+\.cxx)', makefile)))
headers = []
for top in ('src/emucore', 'src/common', 'src/os/libretro'):
    for d, dirs, files in os.walk(os.path.join(STELLA, top)):
        dirs[:] = [x for x in dirs if x not in ('sqlite', 'sdl_blitter')]
        headers += [os.path.relpath(os.path.join(d, f), STELLA) for f in files if f.endswith('.hxx')]
rewritten = 0
for path in ['src/' + p for p in compiled] + sorted(headers):
    try:
        text = original('stella', path)
    except subprocess.CalledProcessError:
        continue  # (a file of this tree git doesn't have)
    if not re.search(rb'\b(throw|try|catch)\b', text):
        continue
    new, changes = no_exceptions(text)
    if changes:
        patched[('stella', path)] = new
        rewritten += 1

# Its key, joystick and combo mappings: an empty one (always, in the libretro core) or an older one isn't JSON - upstream
# caught json::parse's exception and converted it from the old format. Asked first (json::accept), never thrown.
patch('stella', 'src/common/PKeyboardHandler.cxx',
      b'  if (true) {\n    mapping = json::parse(serializedMapping);\n  } else if (false) {\n',
      b'  if (json::accept(serializedMapping)) {  // EmuSuNi: not thrown\n'
      b'    mapping = json::parse(serializedMapping);\n  } else {\n')
patch('stella', 'src/common/PJoystickHandler.cxx',
      b'  if (true) {\n    mappings = json::parse(serializedMapping);\n  } else if (false) {\n',
      b'  if (json::accept(serializedMapping)) {  // EmuSuNi: not thrown\n'
      b'    mappings = json::parse(serializedMapping);\n  } else {\n')
patch('stella', 'src/emucore/EventHandler.cxx',
      b'  if (true)\n  {\n    mapping = json::parse(serializedMapping);\n  }\n  else if (false)\n  {\n',
      b'  if (json::accept(serializedMapping))  // EmuSuNi: not thrown\n  {\n'
      b'    mapping = json::parse(serializedMapping);\n  }\n  else\n  {\n')

# Without a VFS (the app has none) a file was taken to exist, and reading one that doesn't threw - caught upstream: the
# ROM's own properties file (<rom>.pro) looked for at each load. The file system asked instead (stat).
patch('stella', 'src/os/libretro/FSNodeLIBRETRO.cxx',
      b'#include "libretro.h"\n',
      b'#include "libretro.h"\n#include <sys/stat.h>\n')
patch('stella', 'src/os/libretro/FSNodeLIBRETRO.cxx',
      b'  return true;  // VFS unavailable: assume exists (backward-compatible fallback)\n',
      b'  // EmuSuNi: no VFS - the file system asked (not "assume it exists": reading one that doesn\'t threw)\n'
      b'  struct stat st;\n'
      b'  return isMemoryROM() || ::stat(_path.c_str(), &st) == 0;\n')
# ... and its folders (stella/nvram in the save folder: the SaveKey's and AtariVox's EEPROM, a cart's flash) made, a
# file's or folder's kind known, without one: the file system's own stat and mkdir.
patch('stella', 'src/os/libretro/FSNodeLIBRETRO.cxx',
      b'  // Not present on disk, or no VFS/stat callback for us to ask in the first\n',
      b'  // EmuSuNi: no VFS - the file system asked\n'
      b'  struct stat st;\n'
      b'  if(!libretro_vfs && ::stat(_path.c_str(), &st) == 0)\n'
      b'  {\n'
      b'    _isDirectory = S_ISDIR(st.st_mode);\n'
      b'    _isFile = S_ISREG(st.st_mode);\n'
      b'    _size = _isFile ? SZT(st.st_size) : 0;\n'
      b'    if(_isDirectory && !_path.empty() && _path.back() != FSNode::PATH_SEPARATOR)\n'
      b'      _path += FSNode::PATH_SEPARATOR;\n'
      b'    return true;\n'
      b'  }\n'
      b'\n'
      b'  // Not present on disk, or no VFS/stat callback for us to ask in the first\n')
patch('stella', 'src/os/libretro/FSNodeLIBRETRO.cxx',
      b'    if(result == 0 || result == -2)  // 0 = created, -2 = already exists\n'
      b'    {\n'
      b'      _displayName = string(lastPathComponent(_path));\n'
      b'      return setFlags();\n'
      b'    }\n'
      b'  }\n'
      b'  return false;\n'
      b'}\n',
      b'    if(result == 0 || result == -2)  // 0 = created, -2 = already exists\n'
      b'    {\n'
      b'      _displayName = string(lastPathComponent(_path));\n'
      b'      return setFlags();\n'
      b'    }\n'
      b'  }\n'
      b'  else if(!libretro_vfs)  // EmuSuNi: no VFS - the file system\'s mkdir\n'
      b'  {\n'
      b'    string p = _path;\n'
      b'    if(!p.empty() && p.back() == FSNode::PATH_SEPARATOR)\n'
      b'      p.pop_back();\n'
      b'    if(::mkdir(p.c_str(), 0777) == 0 || errno == EEXIST)\n'
      b'    {\n'
      b'      _displayName = string(lastPathComponent(_path));\n'
      b'      return setFlags();\n'
      b'    }\n'
      b'  }\n'
      b'  return false;\n'
      b'}\n')
patch('stella', 'src/os/libretro/FSNodeLIBRETRO.cxx',
      b'#include <sys/stat.h>\n',
      b'#include <sys/stat.h>\n#include <cerrno>\n')

# A file that isn't there (or can't be opened) read as nothing, not thrown: upstream caught it where it is normal - a
# SaveKey's or AtariVox's EEPROM file before its first save, a user palette, a ROM's properties file -, and every caller
# takes a read of 0 bytes as "no file" (the ROM's load says so itself).
patch('stella', 'src/emucore/FSNode.cxx',
      b'  if(!(exists() && isReadable()))\n'
      b'    emusuni_stella_throw(std::runtime_error("File not found/readable"));\n',
      b'  if(!(exists() && isReadable()))\n'
      b'    return 0;  // EmuSuNi: no file, nothing read (not thrown)\n', count=2)
patch('stella', 'src/emucore/FSNode.cxx',
      b'  if(!in)\n'
      b'    emusuni_stella_throw(std::runtime_error("File open/read error"));\n',
      b'  if(!in)\n'
      b'    return 0;  // EmuSuNi: not opened, nothing read (not thrown)\n', count=2)
# ... and a file that can't be written written as nothing (a save's callers catch it upstream: "ERROR writing").
patch('stella', 'src/emucore/FSNode.cxx',
      b'  if(!out)\n'
      b'    emusuni_stella_throw(std::runtime_error("File open error"));\n',
      b'  if(!out)\n'
      b'    return 0;  // EmuSuNi: not opened, nothing written (not thrown)\n')
patch('stella', 'src/emucore/FSNode.cxx',
      b'  if(!out)\n'
      b'    emusuni_stella_throw(std::runtime_error("File write error"));\n',
      b'  if(!out)\n'
      b'    return 0;  // EmuSuNi: not written (not thrown)\n')
patch('stella', 'src/emucore/FSNode.cxx',
      b'    if(!out)\n'
      b'      emusuni_stella_throw(std::runtime_error("File write error"));\n'
      b'  }\n'
      b'  else\n'
      b'    emusuni_stella_throw(std::runtime_error("File open/write error"));\n',
      b'    if(!out)\n'
      b'      return 0;  // EmuSuNi: not written (not thrown)\n'
      b'  }\n'
      b'  else\n'
      b'    return 0;  // EmuSuNi: not opened, nothing written (not thrown)\n')
# A game from a .zip comes from memory with the .zip's path (the app's way, as RetroArch's when it unzips): named as
# a ROM, not taken for the archive itself ("Unrecognized ROM file type").
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  stella.setROM(info->path, info->data, info->size);\n'
      b'  libretro_rom_path = info->path ? info->path : "";\n',
      b'  string emusuni_rom_path = info->path ? info->path : "";\n'
      b'  if(BSPF::endsWithIgnoreCase(emusuni_rom_path, ".zip"))  // EmuSuNi: its ROM, from memory\n'
      b'    emusuni_rom_path.replace(emusuni_rom_path.size() - 4, 4, ".a26");\n'
      b'  stella.setROM(emusuni_rom_path.c_str(), info->data, info->size);\n'
      b'  libretro_rom_path = emusuni_rom_path;\n')

# Its game properties (emucore/DefProps.hxx: 3661 games x 30 strings, 0.9 MiB of pointers and 0.3 of text in the
# app's image, which the PS4 charges from its start) made compact: the MD5s (sorted, as upstream's) and only each game's
# other properties that are set, by number, into a list of their values - without the ones the libretro core never uses
# (the cartridge's name - the ROM's name stands for it -, maker, model number, notes, rarity, link, high-score layout and
# bezel's name). PropertiesSet's lookup and print read it instead (about 0.15 MiB).
DROPPED = {1, 2, 3, 4, 5, 9, 10, 29}  # PropType: Cart_Manufacturer, _ModelNo, _Name, _Note, _Rarity, _Highscore, _Url,
                                      # Bezel_Name
def c_string_fields(row):
    fields, i = [], 0
    while True:
        i = row.find('"', i)
        if i < 0:
            return fields
        j = i + 1
        while row[j] != '"':
            j += 2 if row[j] == '\\' else 1
        fields.append(row[i + 1:j])
        i = j + 1
defprops = original('stella', 'src/emucore/DefProps.hxx').decode('latin-1')
rows = [line for line in defprops.split('\n') if line.startswith('  { "')]
assert len(rows) == int(re.search(r'array2D<const char\*, (\d+), 30>', defprops).group(1)), len(rows)
values, value_index, md5s, first, fields_out, values_out = [], {}, [], [0], [], []
games = []
for row in rows:
    cells = c_string_fields(row)
    assert len(cells) == 30, (len(cells), row[:60])
    games.append((cells[0].lower(), cells))
# (In lower case and sorted so: upstream's has a few in capitals out of its search's order, E68E28752D3C...)
games.sort(key=lambda game: game[0])
for md5, cells in games:
    md5s.append(md5)
    for p, cell in enumerate(cells[1:], 1):
        if cell and p not in DROPPED:
            if cell not in value_index:
                value_index[cell] = len(values)
                values.append(cell)
            fields_out.append(p)
            values_out.append(value_index[cell])
    first.append(len(fields_out))
assert len(set(md5s)) == len(md5s) and len(values) < 65536 and len(fields_out) < 65536
def wrap(items, per):
    return ',\n'.join('  ' + ', '.join(items[k:k + per]) for k in range(0, len(items), per))
compact = ('// EmuSuNi (scripts/build-atari-cores.sh): Stella\'s game properties made compact - the MD5s (sorted), each\n'
           '// game\'s other set properties by number into EMUSUNI_PROP_VALUES; no name, maker, model number, note,\n'
           '// rarity, link, high-score layout or bezel name. Read by PropertiesSet (PropsSet.cxx).\n'
           '#ifndef DEF_PROPS_HXX\n#define DEF_PROPS_HXX\n\n#include "bspf.hxx"\n\n'
           'static constexpr size_t EMUSUNI_PROPS_GAMES = %d;\n' % len(md5s) +
           'static constexpr char EMUSUNI_PROPS_MD5[%d][33] = {\n' % len(md5s) +
           wrap(['"%s"' % m for m in md5s], 3) + '\n};\n' +
           'static constexpr uInt16 EMUSUNI_PROPS_FIRST[%d] = {\n' % len(first) +
           wrap([str(x) for x in first], 16) + '\n};\n' +
           'static constexpr uInt8 EMUSUNI_PROPS_FIELD[%d] = {\n' % len(fields_out) +
           wrap([str(x) for x in fields_out], 24) + '\n};\n' +
           'static constexpr uInt16 EMUSUNI_PROPS_VALUE[%d] = {\n' % len(values_out) +
           wrap([str(x) for x in values_out], 16) + '\n};\n' +
           'static constexpr const char* EMUSUNI_PROP_VALUES[%d] = {\n' % len(values) +
           wrap(['"%s"' % v for v in values], 4) + '\n};\n\n#endif\n')
patched[('stella', 'src/emucore/DefProps.hxx')] = compact.encode('latin-1')
patch('stella', 'src/emucore/PropsSet.cxx',
      b'    const auto it = std::ranges::lower_bound( // NOLINT(readability-qualified-auto)\n'
      b'      DefProps, md5,\n'
      b'      [](string_view a, string_view b) {\n'
      b'          return BSPF::compareIgnoreCase(a, b) < 0;\n'
      b'      },\n'
      b'      [](const auto& entry) {\n'
      b'          return entry[std::to_underlying(PropType::Cart_MD5)];\n'
      b'      });\n'
      b'\n'
      b'    if(it != DefProps.end() &&\n'
      b'       BSPF::compareIgnoreCase((*it)[std::to_underlying(PropType::Cart_MD5)], md5) == 0)\n'
      b'    {\n'
      b'      const auto& entry = *it;\n'
      b'      for(auto p = 0UZ; p < Properties::NUM_PROPS; ++p)\n'
      b'        if(entry[p][0] != 0)\n'
      b'          properties.set(static_cast<PropType>(p), entry[p]);\n'
      b'\n'
      b'      found = true;\n'
      b'    }\n',
      b'    // EmuSuNi: the compact table (DefProps.hxx)\n'
      b'    size_t lo = 0, hi = EMUSUNI_PROPS_GAMES;\n'
      b'    while(lo < hi)\n'
      b'    {\n'
      b'      const size_t mid = (lo + hi) / 2;\n'
      b'      if(BSPF::compareIgnoreCase(EMUSUNI_PROPS_MD5[mid], md5) < 0)\n'
      b'        lo = mid + 1;\n'
      b'      else\n'
      b'        hi = mid;\n'
      b'    }\n'
      b'    if(lo < EMUSUNI_PROPS_GAMES && BSPF::compareIgnoreCase(EMUSUNI_PROPS_MD5[lo], md5) == 0)\n'
      b'    {\n'
      b'      properties.set(PropType::Cart_MD5, EMUSUNI_PROPS_MD5[lo]);\n'
      b'      for(size_t k = EMUSUNI_PROPS_FIRST[lo]; k < EMUSUNI_PROPS_FIRST[lo + 1]; ++k)\n'
      b'        properties.set(static_cast<PropType>(EMUSUNI_PROPS_FIELD[k]),\n'
      b'                       EMUSUNI_PROP_VALUES[EMUSUNI_PROPS_VALUE[k]]);\n'
      b'\n'
      b'      found = true;\n'
      b'    }\n')
patch('stella', 'src/emucore/PropsSet.cxx',
      b'  for(const auto& DefProp: DefProps)\n'
      b'  {\n'
      b'    properties.setDefaults();\n'
      b'    for(auto p = 0UZ; p < Properties::NUM_PROPS; ++p)\n'
      b'      if(DefProp[p][0] != 0)\n'
      b'        properties.set(static_cast<PropType>(p), DefProp[p]);\n'
      b'\n'
      b'    list.emplace(DefProp[std::to_underlying(PropType::Cart_MD5)], properties);\n'
      b'  }\n',
      b'  for(size_t g = 0; g < EMUSUNI_PROPS_GAMES; ++g)  // EmuSuNi: the compact table (DefProps.hxx)\n'
      b'  {\n'
      b'    properties.setDefaults();\n'
      b'    properties.set(PropType::Cart_MD5, EMUSUNI_PROPS_MD5[g]);\n'
      b'    for(size_t k = EMUSUNI_PROPS_FIRST[g]; k < EMUSUNI_PROPS_FIRST[g + 1]; ++k)\n'
      b'      properties.set(static_cast<PropType>(EMUSUNI_PROPS_FIELD[k]), EMUSUNI_PROP_VALUES[EMUSUNI_PROPS_VALUE[k]]);\n'
      b'\n'
      b'    list.emplace(EMUSUNI_PROPS_MD5[g], properties);\n'
      b'  }\n')

# The picture's buffer it keeps a pointer to forgotten with the console it belongs to: the next game (the app plays
# them all in one process, RetroArch loads the core again) drew from the freed one.
patch('stella', 'src/os/libretro/StellaLIBRETRO.cxx',
      b'  video_ready = false;\n  audio_samples = 0;\n\n  myOSystem.reset();\n',
      b'  video_ready = false;\n  audio_samples = 0;\n'
      b'  render_surface = nullptr;  // EmuSuNi: the console\'s, freed with it\n\n  myOSystem.reset();\n')

# Its file streams without iostream exceptions: a read past a file's end leaves the rest as it was (a CBS RAM Plus
# or Chetiry cart's high-score file shorter than its table: upstream caught the failure and cleared the table).
patch('stella', 'src/emucore/Serializer.cxx',
      b'  if(myFile->stream.is_open())\n'
      b'    myFile->stream.exceptions(std::ios::failbit | std::ios::badbit);\n'
      b'  else\n'
      b'    myFile.reset();\n',
      b'  if(!myFile->stream.is_open())  // EmuSuNi: no iostream exceptions (none is caught on the PS4)\n'
      b'    myFile.reset();\n')

# The libretro core's rescue points (os/libretro/emusuni_throw.cxx): the game's load, a frame, a state saved or
# loaded, a reset. A Stella error under one ends it with Stella's message.
addfile('stella', 'src/os/libretro/emusuni_throw.hxx', r'''// EmuSuNi (scripts/build-atari-cores.sh, 00.90): Stella without C++ exceptions. Included before every Stella source
// (-include). Each throw of Stella's became emusuni_stella_throw, each try an if (true) and each catch an else that never
// runs; emusuni_stella_fail logs the error and goes back to the rescue point of the libretro call that was running
// (EMUSUNI_RESCUE, os/libretro/libretro.cxx), else ends the program as an uncaught exception would.
#pragma once
#ifdef __cplusplus
#include <csetjmp>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>

[[noreturn]] void emusuni_stella_fail(const char* what);
void* emusuni_stella_nothing();

template <class E>
[[noreturn]] inline void emusuni_stella_throw(const E& e)
{
  if constexpr (std::is_base_of_v<std::exception, E>)
    emusuni_stella_fail(e.what());
  else if constexpr (std::is_convertible_v<const E&, const char*>)
    emusuni_stella_fail(e);
  else if constexpr (std::is_convertible_v<const E&, std::string_view>)
    emusuni_stella_fail(std::string(std::string_view(e)).c_str());
  else
    emusuni_stella_fail("an error of the emulator");
}

[[noreturn]] inline void emusuni_stella_rethrow()
{
  emusuni_stella_fail("(the same error again)");
}

// The exception a catch that never runs names (never read).
template <class T>
inline T& emusuni_no_exception()
{
  return *static_cast<T*>(emusuni_stella_nothing());
}

// nlohmann/json's throws (its try and catch are if (true) and if (false) without exceptions).
#define JSON_THROW_USER(exception) emusuni_stella_throw(exception)

struct EmusuniRescue
{
  jmp_buf env;
  const char* where;
  EmusuniRescue* outer;
};
void emusuni_rescue_arm(EmusuniRescue* rescue, const char* where);
void emusuni_rescue_disarm(EmusuniRescue* rescue);
// What the last rescue came back from (Stella's message).
const char* emusuni_rescued();
#endif
''')
addfile('stella', 'src/os/libretro/emusuni_throw.cxx', r'''// EmuSuNi (scripts/build-atari-cores.sh, 00.90): Stella's errors without C++ exceptions (emusuni_throw.hxx).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "libretro.h"

extern retro_log_printf_t emusuni_stella_log;

namespace
{
EmusuniRescue* g_rescue = nullptr;  // the innermost rescue point armed (one thread: the emulation's)
char g_what[512];
alignas(64) unsigned char g_nothing[256];
}  // namespace

void* emusuni_stella_nothing()
{
  return g_nothing;
}

void emusuni_rescue_arm(EmusuniRescue* rescue, const char* where)
{
  rescue->where = where;
  rescue->outer = g_rescue;
  g_rescue = rescue;
}

void emusuni_rescue_disarm(EmusuniRescue* rescue)
{
  if (g_rescue == rescue)
    g_rescue = rescue->outer;
}

const char* emusuni_rescued()
{
  return g_what;
}

[[noreturn]] void emusuni_stella_fail(const char* what)
{
  std::snprintf(g_what, sizeof(g_what), "%s", what != nullptr && what[0] != '\0' ? what : "an error of the emulator");
  // (Without its line breaks: one log line.)
  for (char* c = g_what; *c != '\0'; c++)
    if (*c == '\n' || *c == '\r')
      *c = ' ';
  EmusuniRescue* const rescue = g_rescue;
  if (emusuni_stella_log != nullptr)
    emusuni_stella_log(RETRO_LOG_ERROR, "EmuSuNi: Stella stopped %s: %s\n",
                       rescue != nullptr ? rescue->where : "outside a rescue point (the app ends)", g_what);
  else
    std::fprintf(stderr, "EmuSuNi: Stella stopped %s: %s\n",
                 rescue != nullptr ? rescue->where : "outside a rescue point", g_what);
  if (rescue == nullptr)
    std::abort();
  g_rescue = rescue->outer;
  std::longjmp(rescue->env, 1);
}
''')
# The C++ library's errors (std::exception, logic_error, runtime_error) for Stella alone, on the PS4: there the app
# takes their constructors from libc++ but their destructors, what() and tables from the system's libSceLibcInternal,
# another C++ library with another layout. One built and destroyed without being thrown (nlohmann/json's parse error
# when Stella checks a text that isn't JSON, at every game's load) ended the app.
addfile('stella', 'src/os/libretro/emusuni_stdexcept.cxx', r'''// EmuSuNi (scripts/build-atari-cores.sh, 00.90): libc++'s std::exception, logic_error and runtime_error (its
// src/include/refstring.h, src/stdexcept.cpp; libc++abi's src/stdlib_stdexcept.cpp, src/stdlib_exception.cpp), all of
// them in Stella's object, where they stay (only its libretro API is global). On the PS4 the app would mix libc++'s
// constructors with the destructors, what() and tables of the system's C++ library (libSceLibcInternal: another
// layout), and an error destroyed without being thrown would end it.
#if defined(__ORBIS__) && defined(_LIBCPP_VERSION) && !defined(_LIBCPP_ABI_VCRUNTIME)
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>

namespace
{
// Before the text: its length, its capacity and how many more copies share it.
struct Rep
{
  std::size_t len;
  std::size_t cap;
  int count;
};

Rep* rep_from_data(const char* data)
{
  return reinterpret_cast<Rep*>(const_cast<char*>(data) - sizeof(Rep));
}

void release(const char* data)
{
  Rep* const rep = rep_from_data(data);
  if (__atomic_add_fetch(&rep->count, -1, __ATOMIC_SEQ_CST) < 0)
    ::operator delete(rep);
}
}  // namespace

_LIBCPP_BEGIN_NAMESPACE_STD
__libcpp_refstring::__libcpp_refstring(const char* msg)
{
  const size_t len = strlen(msg);
  Rep* const rep = static_cast<Rep*>(::operator new(sizeof(Rep) + len + 1));
  rep->len = len;
  rep->cap = len;
  rep->count = 0;
  char* const data = reinterpret_cast<char*>(rep) + sizeof(Rep);
  memcpy(data, msg, len + 1);
  __imp_ = data;
}

__libcpp_refstring::__libcpp_refstring(const __libcpp_refstring& s) noexcept : __imp_(s.__imp_)
{
  __atomic_add_fetch(&rep_from_data(__imp_)->count, 1, __ATOMIC_SEQ_CST);
}

__libcpp_refstring& __libcpp_refstring::operator=(const __libcpp_refstring& s) noexcept
{
  const char* const old = __imp_;
  __imp_ = s.__imp_;
  __atomic_add_fetch(&rep_from_data(__imp_)->count, 1, __ATOMIC_SEQ_CST);
  release(old);
  return *this;
}

__libcpp_refstring::~__libcpp_refstring()
{
  release(__imp_);
}

bool __libcpp_refstring::__uses_refcount() const
{
  return true;
}
_LIBCPP_END_NAMESPACE_STD

namespace std
{
exception::~exception() noexcept {}
const char* exception::what() const noexcept { return "std::exception"; }

logic_error::logic_error(const string& msg) : __imp_(msg.c_str()) {}
logic_error::logic_error(const char* msg) : __imp_(msg) {}
logic_error::logic_error(const logic_error& e) noexcept : exception(e), __imp_(e.__imp_) {}
logic_error& logic_error::operator=(const logic_error& e) noexcept
{
  __imp_ = e.__imp_;
  return *this;
}
logic_error::~logic_error() noexcept {}
const char* logic_error::what() const noexcept { return __imp_.c_str(); }

runtime_error::runtime_error(const string& msg) : __imp_(msg.c_str()) {}
runtime_error::runtime_error(const char* msg) : __imp_(msg) {}
runtime_error::runtime_error(const runtime_error& e) noexcept : exception(e), __imp_(e.__imp_) {}
runtime_error& runtime_error::operator=(const runtime_error& e) noexcept
{
  __imp_ = e.__imp_;
  return *this;
}
runtime_error::~runtime_error() noexcept {}
const char* runtime_error::what() const noexcept { return __imp_.c_str(); }

domain_error::~domain_error() noexcept {}
invalid_argument::~invalid_argument() noexcept {}
length_error::~length_error() noexcept {}
out_of_range::~out_of_range() noexcept {}
range_error::~range_error() noexcept {}
overflow_error::~overflow_error() noexcept {}
underflow_error::~underflow_error() noexcept {}
}  // namespace std
#endif
''')
RESCUE = (b'// EmuSuNi: a Stella error under it comes back here (emusuni_throw.hxx), then does what follows where.\n'
          b'struct EmusuniDisarm\n'
          b'{\n'
          b'  EmusuniRescue* rescue;\n'
          b'  ~EmusuniDisarm() { emusuni_rescue_disarm(rescue); }\n'
          b'};\n'
          b'#define EMUSUNI_RESCUE(where, ...)                            \\\n'
          b'  EmusuniRescue emusuni_rescue;                                \\\n'
          b'  if(setjmp(emusuni_rescue.env) != 0) { __VA_ARGS__; }         \\\n'
          b'  emusuni_rescue_arm(&emusuni_rescue, where);                  \\\n'
          b'  const EmusuniDisarm emusuni_disarm{&emusuni_rescue}\n'
          b'retro_log_printf_t emusuni_stella_log;\n'
          b'static bool emusuni_stopped;  // a frame failed: the game ends (RETRO_ENVIRONMENT_SHUTDOWN)\n'
          b'\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'static bool system_reset;\n',
      b'static bool system_reset;\n\n' + RESCUE)
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  log_cb = fallback_log;\n'
      b'  if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))\n'
      b'    log_cb = log.log;\n',
      b'  log_cb = fallback_log;\n'
      b'  if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))\n'
      b'    log_cb = log.log;\n'
      b'  emusuni_stella_log = log_cb;  // EmuSuNi: its errors in the log\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  string emusuni_rom_path = info->path ? info->path : "";\n',
      b'  emusuni_stopped = false;\n'
      b'  EMUSUNI_RESCUE("loading the game", {\n'
      b'    const string message = string("Stella: ") + emusuni_rescued();\n'
      b'    post_message(message.c_str(), RETRO_LOG_ERROR, 5000);\n'
      b'    return false;\n'
      b'  });\n'
      b'  string emusuni_rom_path = info->path ? info->path : "";\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'void retro_run()\n'
      b'{\n'
      b'  bool updated = false;\n',
      b'void retro_run()\n'
      b'{\n'
      b'  // EmuSuNi: a frame that failed ends the game (its message, then RETRO_ENVIRONMENT_SHUTDOWN).\n'
      b'  if(emusuni_stopped)\n'
      b'    return;\n'
      b'  EMUSUNI_RESCUE("running the game", {\n'
      b'    emusuni_stopped = true;\n'
      b'    const string message = string("Stella: ") + emusuni_rescued();\n'
      b'    post_message(message.c_str(), RETRO_LOG_ERROR, 5000);\n'
      b'    environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);\n'
      b'    return;\n'
      b'  });\n'
      b'  bool updated = false;\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'bool retro_serialize(void *data, size_t size)\n'
      b'{\n',
      b'bool retro_serialize(void *data, size_t size)\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("saving a state", return false);\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'bool retro_unserialize(const void *data, size_t size)\n'
      b'{\n',
      b'bool retro_unserialize(const void *data, size_t size)\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("loading a state", return false);\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'size_t retro_serialize_size()\n'
      b'{\n',
      b'size_t retro_serialize_size()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("measuring a state", return 0);\n')
# (The console's end - its EEPROMs written - too: a failure leaves what was left of it, never the app's end.)
patch('stella', 'src/os/libretro/libretro.cxx',
      b'void retro_unload_game()\n'
      b'{\n'
      b'  stella.destroy();\n',
      b'void retro_unload_game()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("ending the game", return);\n'
      b'  stella.destroy();\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'void retro_deinit()\n'
      b'{\n'
      b'  stella.destroy();\n',
      b'void retro_deinit()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("closing the core", return);\n'
      b'  stella.destroy();\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'void retro_reset()\n'
      b'{\n'
      b'  stella.reset();\n',
      b'void retro_reset()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("restarting the game", {\n'
      b'    emusuni_stopped = true;\n'
      b'    return;\n'
      b'  });\n'
      b'  stella.reset();\n')
# (01.03) Online play's rollbacks (src/Netplay.cpp's RulesFor): what the paddles and the driving controller keep outside
# Stella's states - the positions the left sticks move (static in update_input), the Paddles' and Driving's own (the
# stick's last value, the digital positions and speed-up, the wheel's counters, the autofire's delays) - and the input
# window (below) go after Stella's state, with "EPD1" at its end: a state loaded again went on otherwise with paddles
# (local/herramientas/prueba-atari: the paddle test cart, HARNESS_NETPLAY_ANALOG). A state without it (an older save)
# loads as before.
patch('stella', 'src/emucore/Paddles.hxx',
      b'    bool setMouseControl(Controller::Type xtype, int xid,\n'
      b'                         Controller::Type ytype, int yid) override;\n',
      b'    bool setMouseControl(Controller::Type xtype, int xid,\n'
      b'                         Controller::Type ytype, int yid) override;\n'
      b'\n'
      b'    // EmuSuNi: what Stella\'s states don\'t keep of the pair (os/libretro\'s state tail): 34 bytes\n'
      b'    void emusuniSave(uInt8* out) const;\n'
      b'    void emusuniLoad(const uInt8* in);\n')
patch('stella', 'src/emucore/Paddles.cxx',
      b'void Paddles::update()\n',
      b'// EmuSuNi (scripts/build-atari-cores.sh): what Stella\'s states don\'t keep of the pair, for os/libretro\'s state tail.\n'
      b'void Paddles::emusuniSave(uInt8* out) const\n'
      b'{\n'
      b'  std::memcpy(out, &myLastAxisX, 4);\n'
      b'  std::memcpy(out + 4, &myLastAxisY, 4);\n'
      b'  std::memcpy(out + 8, myPosition.data(), 8);\n'
      b'  std::memcpy(out + 16, &myPaddleRepeatA, 4);\n'
      b'  std::memcpy(out + 20, &myPaddleRepeatB, 4);\n'
      b'  out[24] = myKeyRepeatA ? 1 : 0;\n'
      b'  out[25] = myKeyRepeatB ? 1 : 0;\n'
      b'  std::memcpy(out + 26, &myFireDelay, 4);\n'
      b'  std::memcpy(out + 30, &myFireDelayP1, 4);\n'
      b'}\n'
      b'\n'
      b'void Paddles::emusuniLoad(const uInt8* in)\n'
      b'{\n'
      b'  std::memcpy(&myLastAxisX, in, 4);\n'
      b'  std::memcpy(&myLastAxisY, in + 4, 4);\n'
      b'  std::memcpy(myPosition.data(), in + 8, 8);\n'
      b'  std::memcpy(&myPaddleRepeatA, in + 16, 4);\n'
      b'  std::memcpy(&myPaddleRepeatB, in + 20, 4);\n'
      b'  myKeyRepeatA = in[24] != 0;\n'
      b'  myKeyRepeatB = in[25] != 0;\n'
      b'  std::memcpy(&myFireDelay, in + 26, 4);\n'
      b'  std::memcpy(&myFireDelayP1, in + 30, 4);\n'
      b'}\n'
      b'\n'
      b'// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n'
      b'void Paddles::update()\n')
patch('stella', 'src/emucore/Paddles.cxx', b'#include <cmath>\n', b'#include <cmath>\n#include <cstring>\n')
patch('stella', 'src/emucore/Driving.hxx',
      b'    string name() const override { return "Driving"; }\n',
      b'    string name() const override { return "Driving"; }\n'
      b'\n'
      b'    // EmuSuNi: what Stella\'s states don\'t keep of the wheel (os/libretro\'s state tail): 28 bytes\n'
      b'    void emusuniSave(uInt8* out) const;\n'
      b'    void emusuniLoad(const uInt8* in);\n')
patch('stella', 'src/emucore/Driving.cxx',
      b'void Driving::update()\n',
      b'// EmuSuNi (scripts/build-atari-cores.sh): what Stella\'s states don\'t keep of the wheel, for os/libretro\'s state tail.\n'
      b'void Driving::emusuniSave(uInt8* out) const\n'
      b'{\n'
      b'  const Int32 values[7] = {myCounter, myCounterHires, myLastCCWEvent, myLastCWEvent,\n'
      b'                           static_cast<Int32>(myGrayIndex), myLastYaxis, myFireDelay};\n'
      b'  std::memcpy(out, values, sizeof(values));\n'
      b'}\n'
      b'\n'
      b'void Driving::emusuniLoad(const uInt8* in)\n'
      b'{\n'
      b'  Int32 values[7];\n'
      b'  std::memcpy(values, in, sizeof(values));\n'
      b'  myCounter = values[0];\n'
      b'  myCounterHires = values[1];\n'
      b'  myLastCCWEvent = values[2];\n'
      b'  myLastCWEvent = values[3];\n'
      b'  myGrayIndex = static_cast<uInt32>(values[4]) & 3;  // (always 0-3: graytable\'s index)\n'
      b'  myLastYaxis = values[5];\n'
      b'  myFireDelay = values[6];\n'
      b'}\n'
      b'\n'
      b'// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -\n'
      b'void Driving::update()\n')
patch('stella', 'src/emucore/Driving.cxx', b'#include "Driving.hxx"\n', b'#include <cstring>\n\n#include "Driving.hxx"\n')
# The input window (Event.hxx: the inputs a poll brings, spread across the coming frame from the last frame's length on
# the system clock) isn't in Stella's states either: the first frame after a load read the input from before the load
# (Jr. Pac-Man's and Space Invaders' SWCHA, the debug core's read log: local/herramientas/prueba-atari). Its values,
# transitions (64 at most: a frame's poll brings two for each input that changed) and place on the clock: in the tail.
patch('stella', 'src/emucore/Event.hxx', b'#include <atomic>\n', b'#include <atomic>\n#include <cstring>\n')
patch('stella', 'src/emucore/Event.hxx',
      b'    /**\n'
      b'      Clears the event array (resets to initial state).\n',
      b'    // EmuSuNi (scripts/build-atari-cores.sh): the input window, which Stella\'s states don\'t keep - its values,\n'
      b'    // its transitions and its place on the system clock (os/libretro\'s state tail): EMUSUNI_SIZE bytes.\n'
      b'    static constexpr size_t EMUSUNI_TRANSITIONS = 64;\n'
      b'    static constexpr size_t EMUSUNI_SIZE = sizeof(Int32) * LastType + 8 + 8 + 1 + 4 + EMUSUNI_TRANSITIONS * 17;\n'
      b'    void emusuniSave(uInt8* out) const {\n'
      b'      const std::scoped_lock lock(myMutex);\n'
      b'      uInt8* p = out;\n'
      b'      std::memcpy(p, myValues.data(), sizeof(Int32) * LastType);\n'
      b'      p += sizeof(Int32) * LastType;\n'
      b'      std::memcpy(p, &myWindowStartCycle, 8);\n'
      b'      std::memcpy(p + 8, &myCyclesLastWindow, 8);\n'
      b'      p[16] = myHasTransitions ? 1 : 0;\n'
      b'      p += 17;\n'
      b'      const uInt32 n = myTransitions.size() < EMUSUNI_TRANSITIONS ? static_cast<uInt32>(myTransitions.size())\n'
      b'                                                                   : static_cast<uInt32>(EMUSUNI_TRANSITIONS);\n'
      b'      std::memcpy(p, &n, 4);\n'
      b'      p += 4;\n'
      b'      for(uInt32 i = 0; i < EMUSUNI_TRANSITIONS; ++i, p += 17)\n'
      b'      {\n'
      b'        const Transition t = i < n ? myTransitions[i] : Transition{};\n'
      b'        const Int32 type = static_cast<Int32>(t.type);\n'
      b'        std::memcpy(p, &type, 4);\n'
      b'        std::memcpy(p + 4, &t.pos, 8);\n'
      b'        std::memcpy(p + 12, &t.value, 4);\n'
      b'        p[16] = t.pending ? 1 : 0;\n'
      b'      }\n'
      b'    }\n'
      b'    void emusuniLoad(const uInt8* in) {\n'
      b'      const std::scoped_lock lock(myMutex);\n'
      b'      const uInt8* p = in;\n'
      b'      std::memcpy(myValues.data(), p, sizeof(Int32) * LastType);\n'
      b'      p += sizeof(Int32) * LastType;\n'
      b'      std::memcpy(&myWindowStartCycle, p, 8);\n'
      b'      std::memcpy(&myCyclesLastWindow, p + 8, 8);\n'
      b'      myHasTransitions = p[16] != 0;\n'
      b'      p += 17;\n'
      b'      uInt32 n = 0;\n'
      b'      std::memcpy(&n, p, 4);\n'
      b'      p += 4;\n'
      b'      if(n > EMUSUNI_TRANSITIONS) n = EMUSUNI_TRANSITIONS;\n'
      b'      myTransitions.clear();\n'
      b'      for(uInt32 i = 0; i < n; ++i, p += 17)\n'
      b'      {\n'
      b'        Int32 type = 0;\n'
      b'        Transition t;\n'
      b'        std::memcpy(&type, p, 4);\n'
      b'        t.type = type > 0 && type < static_cast<Int32>(LastType) ? static_cast<Type>(type) : NoType;\n'
      b'        std::memcpy(&t.pos, p + 4, 8);\n'
      b'        std::memcpy(&t.value, p + 12, 4);\n'
      b'        t.pending = p[16] != 0;\n'
      b'        myTransitions.push_back(t);\n'
      b'      }\n'
      b'      myRecording = false;\n'
      b'    }\n'
      b'\n'
      b'    /**\n'
      b'      Clears the event array (resets to initial state).\n')
patch('stella', 'src/os/libretro/StellaLIBRETRO.hxx', b'#include "Paddles.hxx"\n',
      b'#include "Paddles.hxx"\n#include "Driving.hxx"\n')
# A state whose TIA sound has nothing summed (mySumCt 0) was refused: one saved before the first frame or right after a
# reset (online play's first state, the host's: the guest's load failed), or right after a sample. It's legitimate - the
# next tick adds a clock or more before createSample() divides by it; 0 with something summed is still refused.
patch('stella', 'src/emucore/tia/Audio.cxx',
      b'    if (sumCt == 0 || sumCt > MAX_SUM_CT ||\n'
      b'        sumChannel0 / sumCt > 15 || sumChannel1 / sumCt > 15)\n'
      b'      return false;\n',
      b'    // EmuSuNi: 0 with nothing summed is legitimate (scripts/build-atari-cores.sh)\n'
      b'    if ((sumCt == 0 && (sumChannel0 != 0 || sumChannel1 != 0)) || sumCt > MAX_SUM_CT ||\n'
      b'        (sumCt != 0 && (sumChannel0 / sumCt > 15 || sumChannel1 / sumCt > 15)))\n'
      b'      return false;\n')
patch('stella', 'src/os/libretro/StellaLIBRETRO.hxx',
      b'    Controller::Type getRightControllerType() const {\n'
      b'      return myOSystem->console().rightController().type();\n'
      b'    }\n',
      b'    Controller::Type getRightControllerType() const {\n'
      b'      return myOSystem->console().rightController().type();\n'
      b'    }\n'
      b'\n'
      b'    // EmuSuNi (scripts/build-atari-cores.sh): what a port\'s paddles or wheel keep outside Stella\'s states,\n'
      b'    // EMUSUNI_PORT bytes: saved into save, or loaded from load (libretro.cxx\'s state tail).\n'
      b'    static constexpr size_t EMUSUNI_PORT = 40;\n'
      b'    void emusuniPort(int port, uInt8* save, const uInt8* load) const {\n'
      b'      Controller& c = port == 0 ? myOSystem->console().leftController()\n'
      b'                                : myOSystem->console().rightController();\n'
      b'      switch(c.type())\n'
      b'      {\n'
      b'        case Controller::Type::Paddles:\n'
      b'        case Controller::Type::PaddlesIAxis:\n'
      b'        case Controller::Type::PaddlesIAxDr:\n'
      b'          if(save) static_cast<const Paddles&>(c).emusuniSave(save);\n'
      b'          else static_cast<Paddles&>(c).emusuniLoad(load);\n'
      b'          break;\n'
      b'        case Controller::Type::Driving:\n'
      b'          if(save) static_cast<const Driving&>(c).emusuniSave(save);\n'
      b'          else static_cast<Driving&>(c).emusuniLoad(load);\n'
      b'          break;\n'
      b'        default:\n'
      b'          break;\n'
      b'      }\n'
      b'    }\n'
      b'\n'
      b'    // EmuSuNi: the input window (Event::emusuniSave), in the same tail.\n'
      b'    void emusuniEvents(uInt8* save, const uInt8* load) const {\n'
      b'      Event& e = const_cast<Event&>(myOSystem->eventHandler().event());\n'
      b'      if(save) e.emusuniSave(save);\n'
      b'      else e.emusuniLoad(load);\n'
      b'    }\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'static Controller::Type input_type[2];\n',
      b'static Controller::Type input_type[2];\n'
      b'// EmuSuNi: the paddles\' positions the left sticks move (they were static in update_input), [port][paddle]: in\n'
      b'// the state\'s tail (emusuni_tail_save).\n'
      b'static int32_t emusuni_paddle[2][2];\n')
for side, port in ((b'Left', b'0'), (b'Right', b'1')):
    patch('stella', 'src/os/libretro/libretro.cxx',
          b'      static int32_t paddle_a = 0;\n'
          b'      static int32_t paddle_b = 0;\n'
          b'\n'
          b'      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);\n'
          b'      EVENT(Event::' + side + b'PaddleAAnalog, paddle_a);\n',
          b'      int32_t& paddle_a = emusuni_paddle[' + port + b'][0];  // EmuSuNi: in the state\'s tail\n'
          b'      int32_t& paddle_b = emusuni_paddle[' + port + b'][1];\n'
          b'\n'
          b'      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);\n'
          b'      EVENT(Event::' + side + b'PaddleAAnalog, paddle_a);\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'size_t retro_serialize_size()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("measuring a state", return 0);\n',
      b'// EmuSuNi (scripts/build-atari-cores.sh): after Stella\'s own state, the paddles\' positions (emusuni_paddle),\n'
      b'// each port\'s paddles\' or wheel\'s own (StellaLIBRETRO::emusuniPort) and the input window\'s (Event), then "EPD1":\n'
      b'// a state loaded again goes on as the one saved did (online play\'s rollbacks). One without it (an older save)\n'
      b'// loads as before.\n'
      b'static constexpr size_t EMUSUNI_EVENTS = sizeof(emusuni_paddle) + 2 * StellaLIBRETRO::EMUSUNI_PORT;\n'
      b'static constexpr size_t EMUSUNI_TAIL = EMUSUNI_EVENTS + Event::EMUSUNI_SIZE + 4;\n'
      b'\n'
      b'static void emusuni_tail_save(uInt8* tail)\n'
      b'{\n'
      b'  memset(tail, 0, EMUSUNI_TAIL);\n'
      b'  memcpy(tail, emusuni_paddle, sizeof(emusuni_paddle));\n'
      b'  for(int port = 0; port < 2; port++)\n'
      b'    stella.emusuniPort(port, tail + sizeof(emusuni_paddle) + port * StellaLIBRETRO::EMUSUNI_PORT, nullptr);\n'
      b'  stella.emusuniEvents(tail + EMUSUNI_EVENTS, nullptr);\n'
      b'  memcpy(tail + EMUSUNI_TAIL - 4, "EPD1", 4);\n'
      b'}\n'
      b'\n'
      b'static bool emusuni_tail_found(const void* data, size_t size)\n'
      b'{\n'
      b'  return size >= EMUSUNI_TAIL && memcmp(static_cast<const uInt8*>(data) + size - 4, "EPD1", 4) == 0;\n'
      b'}\n'
      b'\n'
      b'static void emusuni_tail_load(const uInt8* tail)\n'
      b'{\n'
      b'  memcpy(emusuni_paddle, tail, sizeof(emusuni_paddle));\n'
      b'  for(int port = 0; port < 2; port++)\n'
      b'    stella.emusuniPort(port, nullptr, tail + sizeof(emusuni_paddle) + port * StellaLIBRETRO::EMUSUNI_PORT);\n'
      b'  stella.emusuniEvents(nullptr, tail + EMUSUNI_EVENTS);\n'
      b'}\n'
      b'\n'
      b'size_t retro_serialize_size()\n'
      b'{\n'
      b'  EMUSUNI_RESCUE("measuring a state", return 0);\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  return stella.getStateSize();\n',
      b'  const size_t size = stella.getStateSize();  // EmuSuNi: and its tail\n'
      b'  return size ? size + EMUSUNI_TAIL : 0;\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  return stella.saveState(data, size);\n',
      b'  // EmuSuNi: Stella\'s state, then its tail at the end\n'
      b'  if(size < EMUSUNI_TAIL || !stella.saveState(data, size - EMUSUNI_TAIL))\n'
      b'    return false;\n'
      b'  emusuni_tail_save(static_cast<uInt8*>(data) + size - EMUSUNI_TAIL);\n'
      b'  return true;\n')
patch('stella', 'src/os/libretro/libretro.cxx',
      b'  return stella.loadState(data, size);\n',
      b'  // EmuSuNi: with its tail, or an older state without one\n'
      b'  if(!emusuni_tail_found(data, size))\n'
      b'    return stella.loadState(data, size);\n'
      b'  if(!stella.loadState(data, size - EMUSUNI_TAIL))\n'
      b'    return false;\n'
      b'  emusuni_tail_load(static_cast<const uInt8*>(data) + size - EMUSUNI_TAIL);\n'
      b'  return true;\n')


# ---- ProSystem ----
# 1942 (Extended), header 0x0846 (SuperGame, RAM at $4000, POKEY at $0450, YM2151), ran garbage: its header read as a
# plain cart's (below), and any SuperGame cart over 128K taken for a 144K one (bank 8 at $C000). Now SuperGame carts
# bigger than 144K (256K, 512K) have all their banks, the last fixed at $C000, and 32K of RAM at $4000 in two banks,
# picked by a write to $FFFF (bit 0: the Dragonfly cart's way, 1942 (Extended)) or, header bit 5, by bit 5 of the bank
# number, as A7800DS does. The other bank goes in the state, in what its fixed size leaves. (No YM2151 in ProSystem.)
patch('prosystem', 'core/Cartridge.c',
      b'static uint32_t cartridge_GetBankOffset(uint8_t bank)\n',
      b'/* EmuSuNi (scripts/build-atari-cores.sh, 00.90): a SuperGame cart\'s banks: 8 (64K ones fold, cartridge_GetBankOffset),\n'
      b' * or all of a cart bigger than 144K (256K, 512K), the last fixed at $C000 as in MAME and A7800DS. */\n'
      b'static uint8_t cartridge_BankCount(void)\n'
      b'{\n'
      b'   return cartridge_size > 147456 ? (uint8_t)(cartridge_size / 16384) : 8;\n'
      b'}\n'
      b'\n'
      b'/* EmuSuNi: a SuperGame cart\'s second 16K of RAM at $4000 (32K in two banks: 1942). memory_ram holds the bank in\n'
      b' * use, cartridge_ram_other the other one. */\n'
      b'uint8_t cartridge_ram_other[16384];\n'
      b'uint8_t cartridge_ram_bank;\n'
      b'static bool cartridge_ram_banked; /* header bit 5: the RAM\'s bank in bit 5 of the bank number */\n'
      b'\n'
      b'static void cartridge_SwapRam(uint8_t bank)\n'
      b'{\n'
      b'   uint32_t index;\n'
      b'\n'
      b'   if(bank == cartridge_ram_bank)\n'
      b'      return;\n'
      b'   for(index = 0; index < 16384; index++)\n'
      b'   {\n'
      b'      uint8_t byte               = memory_ram[16384 + index];\n'
      b'      memory_ram[16384 + index]  = cartridge_ram_other[index];\n'
      b'      cartridge_ram_other[index] = byte;\n'
      b'   }\n'
      b'   cartridge_ram_bank = bank;\n'
      b'}\n'
      b'\n'
      b'static uint32_t cartridge_GetBankOffset(uint8_t bank)\n')
patch('prosystem', 'core/Cartridge.c',
      b'      if(cartridge_size > 131072)\n'
      b'         cartridge_type = CARTRIDGE_TYPE_SUPERCART_LARGE;\n',
      b'      if(cartridge_size > 131072 && cartridge_size <= 147456) /* EmuSuNi: only 144K ones (cartridge_BankCount) */\n'
      b'         cartridge_type = CARTRIDGE_TYPE_SUPERCART_LARGE;\n')
patch('prosystem', 'core/Cartridge.c',
      b'      else if(b & 0x02)             /* bit 1: SuperGame bankswitched */\n'
      b'         cartridge_type = CARTRIDGE_TYPE_SUPERCART;\n'
      b'      else\n'
      b'         cartridge_type = CARTRIDGE_TYPE_NORMAL;\n',
      b'      else if(b & 0x02)             /* bit 1: SuperGame bankswitched */\n'
      b'         cartridge_type = CARTRIDGE_TYPE_SUPERCART;\n'
      b'      else\n'
      b'         cartridge_type = CARTRIDGE_TYPE_NORMAL;\n'
      b'      if((b & 0x20) && cartridge_type != CARTRIDGE_TYPE_SUPERCART_LARGE) /* EmuSuNi: bit 5, banked RAM */\n'
      b'      {\n'
      b'         cartridge_type       = CARTRIDGE_TYPE_SUPERCART_RAM;\n'
      b'         cartridge_ram_banked = true;\n'
      b'      }\n')
patch('prosystem', 'core/Cartridge.c',
      b'   if(cartridge_HasHeader(header))\n'
      b'   {\n'
      b'      cartridge_ReadHeader(header);\n',
      b'   cartridge_ram_banked = false; /* EmuSuNi */\n'
      b'   if(cartridge_HasHeader(header))\n'
      b'   {\n'
      b'      cartridge_ReadHeader(header);\n')
patch('prosystem', 'core/Cartridge.c',
      b'      case CARTRIDGE_TYPE_SUPERCART:\n'
      b'         if(cartridge_GetBankOffset(7) < cartridge_size)\n'
      b'            memory_WriteROM(49152, 16384, cartridge_buffer + cartridge_GetBankOffset(7));\n'
      b'         break;\n',
      b'      case CARTRIDGE_TYPE_SUPERCART:\n'
      b'         if(cartridge_GetBankOffset(cartridge_BankCount() - 1) < cartridge_size)\n'
      b'            memory_WriteROM(49152, 16384, cartridge_buffer + cartridge_GetBankOffset(cartridge_BankCount() - 1));\n'
      b'         break;\n')
patch('prosystem', 'core/Cartridge.c',
      b'      case CARTRIDGE_TYPE_SUPERCART_RAM:\n'
      b'         if(cartridge_GetBankOffset(7) < cartridge_size)\n'
      b'         {\n'
      b'            memory_WriteROM(49152, 16384, cartridge_buffer + cartridge_GetBankOffset(7));\n'
      b'            memory_ClearROM(16384, 16384);\n',
      b'      case CARTRIDGE_TYPE_SUPERCART_RAM:\n'
      b'         if(cartridge_GetBankOffset(cartridge_BankCount() - 1) < cartridge_size)\n'
      b'         {\n'
      b'            memory_WriteROM(49152, 16384, cartridge_buffer + cartridge_GetBankOffset(cartridge_BankCount() - 1));\n'
      b'            memory_ClearROM(16384, 16384);\n'
      b'            memset(cartridge_ram_other, 0, sizeof(cartridge_ram_other)); /* EmuSuNi */\n'
      b'            cartridge_ram_bank = 0;\n')
patch('prosystem', 'core/Cartridge.c',
      b'      case CARTRIDGE_TYPE_SUPERCART_ROM:\n'
      b'         if(address >= 32768 && address < 49152 && data < 9)\n'
      b'            cartridge_StoreBank(data);\n',
      b'      case CARTRIDGE_TYPE_SUPERCART_ROM:\n'
      b'         /* EmuSuNi: the RAM\'s bank (cartridge_SwapRam); every bank of a cart bigger than 144K. */\n'
      b'         if(cartridge_type == CARTRIDGE_TYPE_SUPERCART_RAM)\n'
      b'         {\n'
      b'            if(cartridge_ram_banked && address >= 32768 && address < 49152)\n'
      b'            {\n'
      b'               cartridge_SwapRam((data >> 5) & 1);\n'
      b'               data &= 0x1f;\n'
      b'            }\n'
      b'            else if(address == 0xffff)\n'
      b'               cartridge_SwapRam(data & 1);\n'
      b'         }\n'
      b'         if(address >= 32768 && address < 49152 && data < cartridge_BankCount() + 1)\n'
      b'            cartridge_StoreBank(data);\n')
patch('prosystem', 'core/Cartridge.h',
      b'extern uint8_t cartridge_souper_ram_page_bank[2];\n',
      b'extern uint8_t cartridge_souper_ram_page_bank[2];\n'
      b'extern uint8_t cartridge_ram_other[16384]; /* EmuSuNi: a SuperGame cart\'s other 16K of RAM */\n'
      b'extern uint8_t cartridge_ram_bank;\n')
patch('prosystem', 'core/ProSystem.c',
      b'         buffer[size + index] = memory_ram[16384 + index];\n'
      b'      size += 16384;\n',
      b'         buffer[size + index] = memory_ram[16384 + index];\n'
      b'      size += 16384;\n'
      b'      /* EmuSuNi: its other RAM bank (Cartridge.c): 49214 bytes of the 49221 a state has. */\n'
      b'      buffer[size++] = cartridge_ram_bank;\n'
      b'      for(index = 0; index < 16384; index++)\n'
      b'         buffer[size + index] = cartridge_ram_other[index];\n'
      b'      size += 16384;\n')
patch('prosystem', 'core/ProSystem.c',
      b'         memory_ram[16384 + index] = buffer[offset + index];\n'
      b'      offset += 16384;',
      b'         memory_ram[16384 + index] = buffer[offset + index];\n'
      b'      offset += 16384;\n'
      b'      cartridge_ram_bank = buffer[offset++] & 1; /* EmuSuNi */\n'
      b'      for(index = 0; index < 16384; index++)\n'
      b'         cartridge_ram_other[index] = buffer[offset + index];\n'
      b'      offset += 16384;')
# The header's high byte with only sound chips' bits (POKEY at $0440 or $0800, YM2151 at $0460: 1942's 0x0846) said
# "a plain cart" (it only knew 1, 2, 16 there): its SuperGame bits decide, as when it's 0.
patch('prosystem', 'core/Cartridge.c',
      b'   if(header[53] == 0)\n',
      b'   if((header[53] & 0x73) == 0) /* EmuSuNi: no Activision, Absolute, Souper or bankset bits */\n')
# (01.03) Online play (src/Netplay.cpp's RulesFor; the app says "fast savestates", GET_AUDIO_VIDEO_ENABLE's 4, in a 7800
# match): the fast state also holds the RIOT's timer and the cycles left over from the frame's last line - without them
# a loaded state went on otherwise than the one it was saved from (1942 (Extended): local/herramientas/prueba-atari's
# HARNESS_NETPLAY). The normal state (the player's saves) stays as it was.
patch('prosystem', 'core/Riot.h',
      b'extern void riot_UpdateTimer(uint8_t cycles);\n',
      b'extern void riot_UpdateTimer(uint8_t cycles);\n'
      b'extern uint32_t riot_SaveState(uint8_t *buffer);       /* EmuSuNi: 11 bytes */\n'
      b'extern uint32_t riot_LoadState(const uint8_t *buffer);\n')
patch('prosystem', 'core/Riot.c',
      b'void riot_Reset(void)\n',
      b'/* EmuSuNi: the timer in a fast state (ProSystem.c): 11 bytes. */\n'
      b'uint32_t riot_SaveState(uint8_t *buffer)\n'
      b'{\n'
      b'   const uint32_t time = (uint32_t)riot_currentTime;\n'
      b'   buffer[0]  = riot_timing ? 1 : 0;\n'
      b'   buffer[1]  = riot_timer & 0xFF;\n'
      b'   buffer[2]  = riot_timer >> 8;\n'
      b'   buffer[3]  = riot_intervals;\n'
      b'   buffer[4]  = riot_elapsed ? 1 : 0;\n'
      b'   buffer[5]  = time & 0xFF;\n'
      b'   buffer[6]  = (time >> 8) & 0xFF;\n'
      b'   buffer[7]  = (time >> 16) & 0xFF;\n'
      b'   buffer[8]  = time >> 24;\n'
      b'   buffer[9]  = riot_clocks & 0xFF;\n'
      b'   buffer[10] = riot_clocks >> 8;\n'
      b'   return 11;\n'
      b'}\n'
      b'\n'
      b'uint32_t riot_LoadState(const uint8_t *buffer)\n'
      b'{\n'
      b'   riot_timing      = buffer[0] != 0;\n'
      b'   riot_timer       = (uint16_t)(buffer[1] | (buffer[2] << 8));\n'
      b'   riot_intervals   = buffer[3];\n'
      b'   riot_elapsed     = buffer[4] != 0;\n'
      b'   riot_currentTime = (int)(buffer[5] | ((uint32_t)buffer[6] << 8) | ((uint32_t)buffer[7] << 16) |\n'
      b'                            ((uint32_t)buffer[8] << 24));\n'
      b'   riot_clocks      = (uint16_t)(buffer[9] | (buffer[10] << 8));\n'
      b'   return 11;\n'
      b'}\n'
      b'\n'
      b'void riot_Reset(void)\n')
patch('prosystem', 'core/ProSystem.c',
      b'      buffer[size++] = pokey_filterSample[1];\n'
      b'   }\n',
      b'      buffer[size++] = pokey_filterSample[1];\n'
      b'      /* EmuSuNi: the RIOT\'s timer and the cycles left over (15 bytes) */\n'
      b'      size += riot_SaveState((uint8_t*)buffer + size);\n'
      b'      save_uint32_to_buffer(buffer, &size, prosystem_cycles);\n'
      b'   }\n')
patch('prosystem', 'core/ProSystem.c',
      b'      pokey_filterSample[1] = buffer[offset++];\n'
      b'   }\n',
      b'      pokey_filterSample[1] = buffer[offset++];\n'
      b'      /* EmuSuNi: the RIOT\'s timer and the cycles left over */\n'
      b'      offset += riot_LoadState((const uint8_t*)buffer + offset);\n'
      b'      prosystem_cycles = read_uint32_from_buffer(buffer, &offset);\n'
      b'   }\n')
# (01.03) No BIOS in an online match (the same "fast savestates"): a console with the player's 7800 BIOS starts through
# it, one without straight into the game - their states would differ from the first frame; and none left from an earlier
# game (bios_enabled was only ever set).
patch('prosystem', 'core/libretro.c',
      b'   if (bios_Load(biospath))\n'
      b'      bios_enabled = true;\n',
      b'   bios_enabled = false;  /* EmuSuNi: none in an online match, none left from the last game */\n'
      b'   if (!get_fast_savestates() && bios_Load(biospath))\n'
      b'      bios_enabled = true;\n')


# ---- Virtual Jaguar ----
# No Atari BIOS in it: upstream builds in both console boot ROMs and both CD BIOSes (src/bios, 1.5 MB of Atari's code).
# Each becomes one byte, never read: a cartridge boots through its HLE BIOS (upstream's default), or through the
# player's own boot ROM from the system folder (Custom: jagboot.rom, "[BIOS] Atari Jaguar (World).j64"... - the app's
# Jaguar BIOS setting); where upstream copied a built-in one, nothing (zeros) - and a cartridge boot with no boot ROM
# of the player's goes through the HLE BIOS instead (a GPU-only demo needs the real one: it shows nothing without it).
for path, header, name in (('src/bios/jagbios.c', 'jagbios.h', 'jaguarBootROM'),
                           ('src/bios/jagbios_m.c', 'jagbios_m.h', 'jaguarBootROM_M'),
                           ('src/bios/jagcdbios.c', 'jagcdbios.h', 'jaguarCDBootROM'),
                           ('src/bios/jagdevcdbios.c', 'jagdevcdbios.h', 'jaguarDevCDBootROM')):
    original('vjaguar', path)  # (still there)
    patched[('vjaguar', path)] = ('/* EmuSuNi (scripts/build-atari-cores.sh): no Atari BIOS is built in - the player\'s own, '
                                  'from the system folder\n   (libretro.c), or the HLE BIOS. Never read. */\n'
                                  '#include "%s"\n\nuint8_t %s[1];\n' % (header, name)).encode()
patch('vjaguar', 'libretro.c',
      b'      memcpy(external_cd_bios, jaguarDevCDBootROM, 0x40000);\n',
      b'      memset(external_cd_bios, 0, 0x40000);  /* EmuSuNi: none built in */\n')
patch('vjaguar', 'libretro.c',
      b'   memcpy(external_cd_bios, jaguarCDBootROM, 0x40000);\n',
      b'   memset(external_cd_bios, 0, 0x40000);  /* EmuSuNi: none built in */\n')
patch('vjaguar', 'libretro.c',
      b'      memcpy(jagMemSpace + 0xE00000, jaguarBootROM, 0x20000);\n',
      b'      memset(jagMemSpace + 0xE00000, 0, 0x20000);  /* EmuSuNi: none built in */\n'
      b'      emusuni_no_boot_rom = true;\n')
patch('vjaguar', 'libretro.c',
      b'   memcpy(jagMemSpace + 0xE00000, src, 0x20000);\n',
      b'   (void)src;\n'
      b'   memset(jagMemSpace + 0xE00000, 0, 0x20000);  /* EmuSuNi: none built in */\n'
      b'   emusuni_no_boot_rom = true;\n')
patch('vjaguar', 'libretro.c',
      b'static void cart_bios_type_from_path(const char *path)\n',
      b'/* EmuSuNi: no boot ROM of the player\'s was staged (stage_cart_boot_rom): the HLE BIOS boots the cartridge. */\n'
      b'static bool emusuni_no_boot_rom = false;\n'
      b'/* EmuSuNi: the memory space on the heap (vjag_memory.c) */\n'
      b'int EmusuniJagMemSpaceInit(void);\n\n'
      b'static void cart_bios_type_from_path(const char *path)\n')
patch('vjaguar', 'libretro.c',
      b'   JaguarInit();                                             // set up hardware\n'
      b'   CrashDetectReset();                                       // zero per-game watchdog state\n'
      b'   stage_cart_boot_rom();\n',
      b'   JaguarInit();                                             // set up hardware\n'
      b'   CrashDetectReset();                                       // zero per-game watchdog state\n'
      b'   emusuni_no_boot_rom = false;\n'
      b'   stage_cart_boot_rom();\n'
      b'   /* EmuSuNi: no boot ROM of the player\'s for a cartridge that asked for the real one: the HLE BIOS instead */\n'
      b'   if (emusuni_no_boot_rom && !jaguar_cd_mode && vjs.useJaguarBIOS)\n'
      b'   {\n'
      b'      LOG_WRN("[BOOT] EmuSuNi: no boot ROM of the player\'s (jagboot.rom in the system folder): the HLE BIOS "\n'
      b'              "boots the cartridge\\n");\n'
      b'      vjs.useJaguarBIOS = false;\n'
      b'      bootConfig.showBootROM = false;\n'
      b'   }\n')

# Its memory space (15.1 MiB: main RAM, the cartridge, TOM's and JERRY's registers and RAM) and TOM's three colour tables
# (768 KiB) on the heap from the core's first retro_init (and kept), not static arrays of the app's image
# (which the PS4 charges from the app's start, a Jaguar game or not). Their registers' pointers into it (vjag_memory.c's
# initialisers, constant addresses until now) are set when it is made.
JAG_MEM = 'src/core/vjag_memory.c'
text = original('vjaguar', JAG_MEM).decode('latin-1')
assigns = []
lines = []
DECL = re.compile(r'^(?P<decl>[A-Za-z_][\w ]*\*\s*(?P<name>\w+))\s*=\s*(?P<expr>[^;]*jagMemSpace\[[^;]*);(?P<rest>.*)$')
for line in text.split('\n'):
    m = DECL.match(line)
    if m:
        lines.append('%s = NULL;%s' % (m.group('decl'), m.group('rest')))
        assigns.append('   %s = %s;' % (m.group('name'), m.group('expr').strip()))
    else:
        lines.append(line)
text = '\n'.join(lines)
OLD_SPACE = 'uint8_t jagMemSpace[JAG_MEMSPACE_BYTES] = { 0 };'
assert text.count(OLD_SPACE) == 1 and len(assigns) > 60, len(assigns)
text = text.replace(OLD_SPACE, '/* EmuSuNi: on the heap (EmusuniJagMemSpaceInit, below) */\nuint8_t *jagMemSpace = NULL;')
text += ('\n/* EmuSuNi (scripts/build-atari-cores.sh): the memory space and TOM\'s colour tables on the heap, made by the\n'
         '   first retro_init and kept (a pointer into one may be kept elsewhere); the registers\' pointers into the\n'
         '   space, the 68000\'s opcode table (m68kinterface.c). 0: no memory. */\n'
         '#include <stdlib.h>\n'
         'extern uint32_t *RGB16ToRGB32, *CRY16ToRGB32, *MIX16ToRGB32;\n'
         'extern void **cpuFunctionTable;  /* (m68kinterface.c: cpuop_func **) */\n'
         'int EmusuniJagMemSpaceInit(void)\n{\n'
         '   if (!jagMemSpace)\n      jagMemSpace = (uint8_t *)calloc(1, JAG_MEMSPACE_BYTES);\n'
         '   if (!RGB16ToRGB32)\n      RGB16ToRGB32 = (uint32_t *)calloc(0x10000, sizeof(uint32_t));\n'
         '   if (!CRY16ToRGB32)\n      CRY16ToRGB32 = (uint32_t *)calloc(0x10000, sizeof(uint32_t));\n'
         '   if (!MIX16ToRGB32)\n      MIX16ToRGB32 = (uint32_t *)calloc(0x10000, sizeof(uint32_t));\n'
         '   if (!cpuFunctionTable)\n      cpuFunctionTable = (void **)calloc(65536, sizeof(void *));\n'
         '   if (!jagMemSpace || !RGB16ToRGB32 || !CRY16ToRGB32 || !MIX16ToRGB32 || !cpuFunctionTable)\n'
         '      return 0;\n'
         + '\n'.join(assigns) + '\n   return 1;\n}\n')
patched[('vjaguar', JAG_MEM)] = text.encode('latin-1')
patch('vjaguar', 'src/core/vjag_memory.h', b'extern uint8_t jagMemSpace[];\n',
      b'extern uint8_t *jagMemSpace;  /* EmuSuNi: on the heap */\n')
patch('vjaguar', 'src/core/jaguar.c', b'extern uint8_t jagMemSpace[];\n',
      b'extern uint8_t *jagMemSpace;  /* EmuSuNi: on the heap */\n')
patch('vjaguar', 'src/tom/tom.c',
      b'uint32_t RGB16ToRGB32[0x10000];\nuint32_t CRY16ToRGB32[0x10000];\nuint32_t MIX16ToRGB32[0x10000];\n',
      b'/* EmuSuNi: on the heap (vjag_memory.c, EmusuniJagMemSpaceInit) */\n'
      b'uint32_t *RGB16ToRGB32;\nuint32_t *CRY16ToRGB32;\nuint32_t *MIX16ToRGB32;\n')
for path in ('src/debug/vjdebug.c', 'src/tom/shadowfb.c', 'src/tom/texdump.c'):
    patch('vjaguar', path, b'extern uint32_t CRY16ToRGB32[0x10000];\n',
          b'extern uint32_t *CRY16ToRGB32;  /* EmuSuNi: on the heap */\n')
patch('vjaguar', 'src/tom/texdump.c', b'extern uint32_t RGB16ToRGB32[0x10000];\n',
      b'extern uint32_t *RGB16ToRGB32;  /* EmuSuNi: on the heap */\n')
patch('vjaguar', 'libretro.c',
      b'void retro_init(void)\n{\n',
      b'void retro_init(void)\n{\n'
      b'   /* EmuSuNi: the memory space on the heap (vjag_memory.c) */\n'
      b'   EmusuniJagMemSpaceInit();\n')
patch('vjaguar', 'libretro.c',
      b'bool retro_load_game(const struct retro_game_info *info)\n{\n',
      b'bool retro_load_game(const struct retro_game_info *info)\n{\n'
      b'   /* EmuSuNi: no memory for the console (vjag_memory.c) */\n'
      b'   if (!EmusuniJagMemSpaceInit())\n'
      b'      return false;\n')

# Its Jaguar CD's buffers (the boot stub's three copies of 600 KiB, the sectors it is read from - 1.1 MiB -, the disc's
# table of 400 KiB), the 68000's opcode table (512 KiB) and the core options' amended copy (290 KiB) on the heap too,
# made at their first use: 5.5 MiB of the app's image otherwise, the CD's never used (the app plays cartridges).
def heap_block_static(core, path, decl, name, count):
    # A static array declared in a block (decl, once in the file): a pointer to an array of the same type and size,
    # made at that point the first time; the block's uses of its name read through it (sizeof included).
    key = (core, path)
    if key not in patched:
        patched[key] = original(core, path)
    s = patched[key].decode('latin-1')
    assert s.count(decl) == 1, (path, decl)
    at = s.index(decl)
    end = at + len(decl)
    depth = 0
    while depth >= 0:
        c = s[end]
        depth += 1 if c == '{' else -1 if c == '}' else 0
        end += 1
    ctype = decl.split()[1]
    body = re.sub(r'\b%s\b' % name, '(*emusuni_%s)' % name, s[at + len(decl):end - 1])
    heap = ('static %s (*emusuni_%s)[%s];  /* EmuSuNi: on the heap */\n'
            '   if (!emusuni_%s)\n      emusuni_%s = calloc(1, sizeof(*emusuni_%s));' %
            (ctype, name, count, name, name, name))
    patched[key] = (s[:at] + heap + body + s[end - 1:]).encode('latin-1')

def heap_file_static(core, path, decl, name, ctype, count):
    # A static array or struct of the file (decl, once): made on the heap at its first use; every later use of its
    # name reads it there (a macro).
    key = (core, path)
    if key not in patched:
        patched[key] = original(core, path)
    s = patched[key].decode('latin-1')
    assert s.count(decl) == 1, (path, decl)
    dims = '[%s]' % count if count else ''
    heap = ('/* EmuSuNi: on the heap, made at its first use */\n'
            'static %s (*emusuni_%s)%s;\n'
            'static %s (*emusuni_heap_%s(void))%s\n{\n'
            '   if (!emusuni_%s)\n      emusuni_%s = calloc(1, sizeof(*emusuni_%s));\n'
            '   return emusuni_%s;\n}\n'
            '#define %s (*emusuni_heap_%s())' % (ctype, name, dims, ctype, name, dims, name, name, name, name, name,
                                                 name))
    patched[key] = s.replace(decl, heap).encode('latin-1')

heap_block_static('vjaguar', 'src/cd/jagcd_bios.c', 'static uint8_t stub[600 * 1024];', 'stub', '600 * 1024')
heap_block_static('vjaguar', 'src/cd/jagcd_hle.c', 'static uint8_t stubBuf[600 * 1024];', 'stubBuf', '600 * 1024')
heap_block_static('vjaguar', 'libretro.c', 'static uint8_t stub[600 * 1024];', 'stub', '600 * 1024')
heap_block_static('vjaguar', 'src/cd/cdintf.c', 'static uint8_t raw[2352 * 256];', 'raw', '2352 * 256')
heap_block_static('vjaguar', 'src/cd/cdintf.c', 'static uint8_t swapped[sizeof((*emusuni_raw))];', 'swapped',
                  '2352 * 256')
heap_file_static('vjaguar', 'src/cd/cdintf.c', 'static struct CDIntfDisc disc;', 'disc', 'struct CDIntfDisc', '')
heap_file_static('vjaguar', 'libretro.c', 'static struct retro_core_option_v2_definition\n'
                 '                       explain_defs[EXPLAIN_DEFS_MAX];', 'explain_defs',
                 'struct retro_core_option_v2_definition', 'EXPLAIN_DEFS_MAX')
# The CD BIOS's place (256 KiB) and the Memory Track's flash (128 KiB): the Jaguar CD's, made at their first use.
patch('vjaguar', 'libretro.c',
      b'uint8_t external_cd_bios[0x40000];  /* 256 KB */\n',
      b'/* EmuSuNi: the CD BIOS (256 KiB) on the heap, made at its first use (a Jaguar CD\'s only) */\n'
      b'static uint8_t (*emusuni_external_cd_bios)[0x40000];\n'
      b'uint8_t (*emusuni_heap_external_cd_bios(void))[0x40000]\n{\n'
      b'   if (!emusuni_external_cd_bios)\n'
      b'      emusuni_external_cd_bios = calloc(1, sizeof(*emusuni_external_cd_bios));\n'
      b'   return emusuni_external_cd_bios;\n}\n'
      b'#define external_cd_bios (*emusuni_heap_external_cd_bios())\n')
patch('vjaguar', 'src/cd/jagcd_bios.c',
      b'extern uint8_t external_cd_bios[0x40000];\n',
      b'uint8_t (*emusuni_heap_external_cd_bios(void))[0x40000];  /* EmuSuNi: on the heap (libretro.c) */\n'
      b'#define external_cd_bios (*emusuni_heap_external_cd_bios())\n')
patch('vjaguar', 'src/core/memtrack.h',
      b'extern uint8_t mtMem[MT_MEM_SIZE];\n',
      b'uint8_t (*emusuni_heap_mtMem(void))[MT_MEM_SIZE];  /* EmuSuNi: on the heap (memtrack.c) */\n'
      b'#define mtMem (*emusuni_heap_mtMem())\n')
patch('vjaguar', 'src/core/memtrack.c',
      b'uint8_t mtMem[0x20000];\n',
      b'/* EmuSuNi: the flash (128 KiB) on the heap, made at its first use (a Jaguar CD\'s only) */\n'
      b'#include <stdlib.h>\n'
      b'#undef mtMem\n'
      b'static uint8_t (*emusuni_mtMem)[MT_MEM_SIZE];\n'
      b'uint8_t (*emusuni_heap_mtMem(void))[MT_MEM_SIZE]\n{\n'
      b'   if (!emusuni_mtMem)\n'
      b'      emusuni_mtMem = calloc(1, sizeof(*emusuni_mtMem));\n'
      b'   return emusuni_mtMem;\n}\n'
      b'#define mtMem (*emusuni_heap_mtMem())\n')
# No libchdr (CHD images are Jaguar CD discs: 240 KiB of code) nor miniz (texture dumps and texture packs): these answer
# "not there" in their place.
addfile('vjaguar', 'emusuni_nochd.c', r'''/* EmuSuNi (scripts/build-atari-cores.sh): no libchdr - its CHD images are Jaguar CD discs, which EmuSuNi doesn't
   play - nor miniz (texture dumps, texture packs): what Virtual Jaguar calls of them answers "not there". */
#include <stdlib.h>
#include <libchdr/chd.h>

chd_error chd_open(const char *filename, int mode, chd_file *parent, chd_file **chd)
{
   (void)filename; (void)mode; (void)parent;
   if (chd)
      *chd = NULL;
   return CHDERR_FILE_NOT_FOUND;
}

chd_error chd_open_file(FILE *file, int mode, chd_file *parent, chd_file **chd)
{
   (void)file;
   return chd_open(NULL, mode, parent, chd);
}

chd_error chd_open_core_file_callbacks(const core_file_callbacks *callbacks, const void *user_data, int mode,
                                       chd_file *parent, chd_file **chd)
{
   (void)callbacks; (void)user_data;
   return chd_open(NULL, mode, parent, chd);
}

void chd_close(chd_file *chd)
{
   (void)chd;
}

const char *chd_error_string(chd_error err)
{
   (void)err;
   return "CHD images aren't supported";
}

const chd_header *chd_get_header(chd_file *chd)
{
   (void)chd;
   return NULL;
}

chd_error chd_read(chd_file *chd, uint32_t hunknum, void *buffer)
{
   (void)chd; (void)hunknum; (void)buffer;
   return CHDERR_INVALID_FILE;
}

chd_error chd_get_metadata(chd_file *chd, uint32_t searchtag, uint32_t searchindex, void *output, uint32_t outputlen,
                           uint32_t *resultlen, uint32_t *resulttag, uint8_t *resultflags)
{
   (void)chd; (void)searchtag; (void)searchindex; (void)output; (void)outputlen; (void)resultlen; (void)resulttag;
   (void)resultflags;
   return CHDERR_METADATA_NOT_FOUND;
}

void *tdefl_write_image_to_png_file_in_memory_ex(const void *pImage, int w, int h, int num_chans, size_t *pLen_out,
                                                 unsigned int level, int flip)
{
   (void)pImage; (void)w; (void)h; (void)num_chans; (void)level; (void)flip;
   if (pLen_out)
      *pLen_out = 0;
   return NULL;
}

void *tinfl_decompress_mem_to_heap(const void *pSrc_buf, size_t src_buf_len, size_t *pOut_len, int flags)
{
   (void)pSrc_buf; (void)src_buf_len; (void)flags;
   if (pOut_len)
      *pOut_len = 0;
   return NULL;
}

void mz_free(void *p)
{
   free(p);
}
''')

# (The 68000's table: made with the others, EmusuniJagMemSpaceInit; the CPU's dispatch reads it as before.)
patch('vjaguar', 'src/m68000/m68kinterface.c', b'cpuop_func * cpuFunctionTable[65536];\n',
      b'cpuop_func ** cpuFunctionTable;  /* EmuSuNi: on the heap (vjag_memory.c, EmusuniJagMemSpaceInit) */\n')

# Its sources: Makefile.common's (the C ones, its SSE2 blitter, libretro-common's), libchdr's unity build apart.
jag_make = original('vjaguar', 'Makefile.common').decode()
jag_core = re.findall(r'\$\(CORE_DIR\)/((?:libretro\.c|src/[\w/]+\.c))', jag_make.split('SIMD-accelerated blitter')[0])
jag_common = re.findall(r'\$\(LIBRETRO_COMM_DIR\)/([\w/]+\.c)', jag_make)
jag_files = sorted(set(jag_core)) + ['src/tom/blitter_simd_sse2.c'] + ['libretro-common/' + p for p in sorted(set(jag_common))]
for p in jag_files:
    original('vjaguar', p)  # (each one is there)
jag_files.append('emusuni_nochd.c')
with open(os.path.join(out_dir, 'jaguar-sources.txt.new'), 'w') as f:
    f.write('\n'.join(jag_files) + '\n')
# The emulation's own files at -O3 (the 68000, TOM - its GPU, blitter, Object Processor -, JERRY - its DSP, sound -,
# the bus and its events); the rest - the libretro glue and its options, the CD, the debugger, the link cable and
# voice modem, cheats, the title database, texture dumps, libretro-common - at -Os: half the size, no frame slower.
JAG_HOT = ('src/m68000/', 'src/tom/gpu.c', 'src/tom/blitter.c', 'src/tom/blitter_simd_sse2.c', 'src/tom/blit_memo.c',
           'src/tom/blitter_mmio.c', 'src/tom/op.c', 'src/tom/tom.c', 'src/tom/shadowfb.c', 'src/jerry/dsp.c',
           'src/jerry/jerry.c', 'src/jerry/dac.c', 'src/jerry/eeprom.c', 'src/jerry/joystick.c', 'src/core/jaguar.c',
           'src/core/vjag_memory.c', 'src/core/event.c', 'src/core/bus_arbiter.c')
with open(os.path.join(out_dir, 'jaguar-cold-sources.txt.new'), 'w') as f:
    f.write('\n'.join(p for p in jag_files if not p.startswith(JAG_HOT)) + '\n')
version = re.search(r'^CORE_BASE_VERSION\s*:?=\s*(\S+)', original('vjaguar', 'Makefile').decode(), re.M).group(1)
rev = subprocess.run(['git', '-C', roots['vjaguar'], 'rev-parse', '--short=7', 'HEAD'], capture_output=True,
                     text=True).stdout.strip()
os.makedirs(os.path.join(out_dir, 'jaguar-include'), exist_ok=True)
with open(os.path.join(out_dir, 'jaguar-include', 'version.h.new'), 'w') as f:
    f.write('/* Generated by scripts/build-atari-cores.sh (as upstream\'s scripts/gen-version-h.sh). */\n'
            '#ifndef VJAG_VERSION_H\n#define VJAG_VERSION_H\n#define CORE_BASE_VERSION "%s"\n'
            '#define GIT_VERSION " %s"\n#define CORE_VERSION CORE_BASE_VERSION GIT_VERSION\n#endif\n' % (version, rev))

# ---- Virtual Jaguar: the fast blitter's A1 window with BKGWREN, as the accurate engine's (00.96) ----
# The accurate engine (BlitterMidsummer2) tests the A1 window after the pixel's data and, with BKGWREN, still writes
# that data (only its Z write is held back); the fast one (blitter_generic) wrote DSTDATA there: Alien vs Predator's
# HUD bar (cmd 11800E41: 8bpp, CLIP_A1, BKGWREN) a pixel short on every line. Now with BKGWREN, no comparator inhibit
# and no Z write, a pixel outside the window is written as any other. (With DSTWRZ, as before.)
patch('vjaguar', 'src/tom/blitter.c',
      b'            if (CLIPA1)\n'
      b'            {\n'
      b'               inhibit |= (((a1_x >> 16) < a1_clip_x && (a1_x >> 16) >= 0\n'
      b'                        && (a1_y >> 16) < a1_clip_y && (a1_y >> 16) >= 0) ? 0 : 1);\n'
      b'            }\n',
      b'            /* EmuSuNi: with BKGWREN the accurate engine writes a pixel outside the A1 window as any other\n'
      b'             * (scripts/build-atari-cores.sh). */\n'
      b'            if (CLIPA1 && !(BKGWREN && !inhibit && !DSTWRZ))\n'
      b'            {\n'
      b'               inhibit |= (((a1_x >> 16) < a1_clip_x && (a1_x >> 16) >= 0\n'
      b'                        && (a1_y >> 16) < a1_clip_y && (a1_y >> 16) >= 0) ? 0 : 1);\n'
      b'            }\n', count=2)

# ---- Virtual Jaguar: the fast blitter's SRCDATA register at the destination's pixel size, as the accurate's (00.98) ----
# Without SRCEN the source data is the SRCDATA register, a phrase written to the destination: the accurate engine takes
# its pixels at the destination's size and place in the phrase; the fast one took them at the source window's (A2 when
# A1 is written). Wolfenstein 3D clears its screen with cmd 01800000 (A1 16bpp, A2 8bpp, SRCDATA DEDEDEDE...): 00DE a
# pixel, not DEDE (the only difference in 449024 blits of a game).
patch('vjaguar', 'src/tom/blitter.c',
      b'            else\t// Use SRCDATA register...\n'
      b'            {\n'
      b'               srcdata = READ_RDATA(SRCDATA, a2, a2_flags, a2_phrase_mode);\n',
      b'            else\t// Use SRCDATA register...\n'
      b'            {\n'
      b'               /* EmuSuNi: at the destination\'s pixel size (scripts/build-atari-cores.sh). */\n'
      b'               srcdata = READ_RDATA(SRCDATA, a1, a1_flags, a1_phrase_mode);\n')
patch('vjaguar', 'src/tom/blitter.c',
      b'            else\n'
      b'            {\n'
      b'               srcdata = READ_RDATA(SRCDATA, a1, a1_flags, a1_phrase_mode);\n'
      b'               if (cmd & 0x001C020)',
      b'            else\n'
      b'            {\n'
      b'               /* EmuSuNi: at the destination\'s pixel size (scripts/build-atari-cores.sh). */\n'
      b'               srcdata = READ_RDATA(SRCDATA, a2, a2_flags, a2_phrase_mode);\n'
      b'               if (cmd & 0x001C020)')

# ---- Virtual Jaguar: blits of under 8 bits a pixel on the accurate engine, the fast one chosen (00.96) ----
# The fast engine's 1-bit pixels with DSTEN aren't the accurate engine's (Alien vs Predator's "GAME OVER", cmd 01800609:
# a 1bpp font, a pixel a line; the only difference in 430540 blits). Such blits are few and small: on the accurate
# engine, the fast blitter's games (Emulator.cpp's JAGUAR_TUNES) are the accurate engine's picture for picture.
patch('vjaguar', 'src/tom/blitter_mmio.c',
      b'         if (vjs.useFastBlitter)\n'
      b'            blitter_blit(GET32(blitter_ram, COMMAND));\n'
      b'         else\n'
      b'            BlitterMidsummer2();\n',
      b'         /* EmuSuNi: under 8 bits a pixel (A1 or A2), the accurate engine (scripts/build-atari-cores.sh). */\n'
      b'         if (vjs.useFastBlitter && ((GET32(blitter_ram, A1_FLAGS) >> 3) & 7) >= 3\n'
      b'               && ((GET32(blitter_ram, A2_FLAGS) >> 3) & 7) >= 3)\n'
      b'            blitter_blit(GET32(blitter_ram, COMMAND));\n'
      b'         else\n'
      b'            BlitterMidsummer2();\n')

# ---- Virtual Jaguar: EmuSuNi's options declared (00.96) ----
# virtualjaguar_emusuni_jit and _jit_dsp (libretro.c, the recompilers) among the core's options, so the frontend gives
# them as any other (EmuSuNi's log said "isn't one the core declared"). Off unless the frontend sets them.
patch('vjaguar', 'libretro_core_options.h',
      b'   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },\n',
      b'   {\n'
      b'      "virtualjaguar_emusuni_jit",\n'
      b'      "GPU Recompiler (EmuSuNi)",\n'
      b'      NULL,\n'
      b'      "The GPU\'s code in its local RAM recompiled to x86-64: the same pictures, sound and save states as its '
      b'interpreter, faster.",\n'
      b'      NULL,\n'
      b'      "speed",\n'
      b'      {\n'
      b'         { "disabled", NULL },\n'
      b'         { "enabled",  NULL },\n'
      b'         { NULL, NULL },\n'
      b'      },\n'
      b'      "disabled"\n'
      b'   },\n'
      b'   {\n'
      b'      "virtualjaguar_emusuni_jit_dsp",\n'
      b'      "DSP Recompiler (EmuSuNi)",\n'
      b'      NULL,\n'
      b'      "The DSP\'s code in its local RAM recompiled to x86-64, with the GPU Recompiler on: the same pictures, '
      b'sound and save states as its interpreter, faster.",\n'
      b'      NULL,\n'
      b'      "speed",\n'
      b'      {\n'
      b'         { "disabled", NULL },\n'
      b'         { "enabled",  NULL },\n'
      b'         { NULL, NULL },\n'
      b'      },\n'
      b'      "disabled"\n'
      b'   },\n'
      b'   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },\n')

# ---- Virtual Jaguar: the fast blitter compiled for the commands the games spend it on (00.96) ----
# blitter_generic decides every pixel's work from cmd's bits. For the commands the games tried blit most with the fast
# engine (local/herramientas/prueba-atari: a per-command count of the time) it is compiled once more with cmd a
# constant, so the compiler drops every test it decides: the same code, the same pixels, fewer steps a pixel. Any other
# command as before. Their share of the fast engine's time: Alien vs Predator 41802801 81% (textures, shaded),
# 49800801 12% (sprites: transparent, shaded), 00010800 4%, 01800001 2%; Tempest 2000 00010200 46-64% (a fill),
# 00011040 22% (gouraud), 41802F41 19-24%, 01800005 3-6%, 09800741 3-7%, 00011000 2%; Doom 41802F01 44%, 41802801 26%,
# 01800601 12%, 01800200 11%. (00.98) Iron Soldier's 3D 00010040 41% (2.8 million spans of ~21 pixels), 01800001 34%,
# 00010200 7%, 00011200 6%, 09800F01 3%, 01800801 2%; Wolfenstein 3D 01800F01 51% (its walls), 01800000 49% (its clear).
patch('vjaguar', 'src/tom/blitter.c',
      b'static void blitter_generic(uint32_t cmd)\n{\n',
      b'static BLITTER_ALWAYS_INLINE void blitter_generic_body(uint32_t cmd)\n{\n')
patch('vjaguar', 'src/tom/blitter.c',
      b'void blitter_blit(uint32_t cmd)\n{\n',
      b'/* EmuSuNi: blitter_generic_body with cmd a constant for the commands games spend the fast engine on\n'
      b' * (scripts/build-atari-cores.sh), else as it is. */\n'
      b'#define EMUSUNI_BLIT_HOT(X) X(41802801) X(49800801) X(00010800) X(01800001) X(00010200) X(00011040) \\\n'
      b'   X(41802F41) X(01800005) X(09800741) X(00011000) X(41802F01) X(01800601) X(01800200) \\\n'
      b'   X(00010040) X(00011200) X(09800F01) X(01800801) X(01800F01) X(01800000)\n'
      b'#define EMUSUNI_BLIT_ONE(c) static void blitter_generic_##c(void) { blitter_generic_body(0x##c##u); }\n'
      b'EMUSUNI_BLIT_HOT(EMUSUNI_BLIT_ONE)\n'
      b'static void blitter_generic(uint32_t cmd)\n'
      b'{\n'
      b'   switch (cmd)\n'
      b'   {\n'
      b'#define EMUSUNI_BLIT_CASE(c) case 0x##c##u: blitter_generic_##c(); return;\n'
      b'      EMUSUNI_BLIT_HOT(EMUSUNI_BLIT_CASE)\n'
      b'      default: blitter_generic_body(cmd); return;\n'
      b'   }\n'
      b'}\n'
      b'\n'
      b'void blitter_blit(uint32_t cmd)\n{\n')

# ---- Virtual Jaguar: the GPU's recompiler (00.96, scripts/vjaguar-jit/emusuni_gpu_jit.h: what it does and keeps) ----
# Included at the end of gpu.c; gpu.c tells it of every write to the GPU's local RAM (the blocks of its code there are
# compiled again), of a reset and a state load (none is kept), and runs a block at the top of GPUExec's loop when it
# can. The frontend's virtualjaguar_emusuni_jit turns it on (libretro.c). The PC's comparison: EMUSUNI_GPU_TRACE.
with open(os.path.join(sys.argv[5], 'vjaguar-jit', 'emusuni_gpu_jit.h')) as f:
    addfile('vjaguar', 'src/tom/emusuni_gpu_jit.h', f.read())
patch('vjaguar', 'src/tom/gpu.c',
      b'static uint8_t gpu_ram_8[0x1000];\n',
      b'static uint8_t gpu_ram_8[0x1000];\n'
      b'/* EmuSuNi: the recompiler (emusuni_gpu_jit.h, at the end): every 16-byte line of local RAM counts its writes;\n'
      b' * a write to a word with compiled code in it also stops the block running. */\n'
      b'#if defined(__x86_64__) && !defined(VJ_TRACE)\n'
      b'#define EMUSUNI_GPU_JIT 1\n'
      b'static uint64_t gpu_jit_line_version[256];\n'
      b'static uint8_t  gpu_jit_code_word[2048];\n'
      b'static uint8_t  gpu_jit_code_hit;\n'
      b'static uint64_t gpu_jit_code_gen;   /* (00.98) words with compiled code written: every block checked again */\n'
      b'#define GPU_JIT_WROTE(off) do { uint32_t o_ = (uint32_t)(off) & 0xFFF; gpu_jit_line_version[o_ >> 4]++; \\\n'
      b'      if (gpu_jit_code_word[o_ >> 1]) { gpu_jit_code_word[o_ >> 1] = 0; gpu_jit_code_hit = 1; \\\n'
      b'         gpu_jit_code_gen++; } } while (0)\n'
      b'static int GPUJitRun(int32_t *cycles, int idle_skip, uint32_t **ldx_armed, uint32_t *ldx_val,\n'
      b'                     uint32_t *ldx_pc);\n'
      b'static void GPUJitFlush(void);\n'
      b'static void GPUJitReset(void);\n'
      b'extern int gpu_jit_wanted;\n'
      b'#ifndef __ORBIS__\n'
      b'static void GPUJitTraceSlice(int32_t cycles);\n'
      b'#endif\n'
      b'#else\n'
      b'#define GPU_JIT_WROTE(off) do { } while (0)\n'
      b'#endif\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'      gpu_ram_8[offset & 0xFFF] = data;\n',
      b'      gpu_ram_8[offset & 0xFFF] = data;\n'
      b'      GPU_JIT_WROTE(offset);\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'      gpu_ram_8[(offset+1) & 0xFFF] = data & 0xFF;//*/\n',
      b'      gpu_ram_8[(offset+1) & 0xFFF] = data & 0xFF;//*/\n'
      b'      GPU_JIT_WROTE(offset);\n'
      b'      GPU_JIT_WROTE(offset + 1);\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'      SET32(gpu_ram_8, offset, data);\n',
      b'      SET32(gpu_ram_8, offset, data);\n'
      b'      GPU_JIT_WROTE(offset);\n'
      b'      GPU_JIT_WROTE(offset + 1);\n'
      b'      GPU_JIT_WROTE(offset + 3);\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'   gpu_ldx_slot         = NULL;\n',
      b'   gpu_ldx_slot         = NULL;\n'
      b'#ifdef EMUSUNI_GPU_JIT\n'
      b'   GPUJitReset();\n'
      b'#endif\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'   STATE_LOAD_BUF(buf, gpu_ram_8, sizeof(gpu_ram_8));\n',
      b'   STATE_LOAD_BUF(buf, gpu_ram_8, sizeof(gpu_ram_8));\n'
      b'#ifdef EMUSUNI_GPU_JIT\n'
      b'   GPUJitFlush();\n'
      b'#endif\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'   while ((cycles > 0 || ldxArmed) && GPU_RUNNING)\n'
      b'   {\n',
      b'   while ((cycles > 0 || ldxArmed) && GPU_RUNNING)\n'
      b'   {\n'
      b'#ifdef EMUSUNI_GPU_JIT\n'
      b'      /* EmuSuNi: recompiled blocks from here, when nothing the loop does below is pending for this\n'
      b'       * instruction (emusuni_gpu_jit.h: one after another, the idle probe after each jr); after them, what\n'
      b'       * the loop does after a single step. */\n'
      b'      if (gpu_jit_wanted && !ldxArmed && !pipeTiming && !gdbArmedSlice && !gpuFlagsRetireDelay\n'
      b'          && !gpuFlagsIRQPending && !busArbiter.enabled && !blitMemoRecording\n'
      b'          && GPUJitRun(&cycles, idleSkipActive, &ldxArmed, &ldxVal, &ldxPc))\n'
      b'      {\n'
      b'         if (gpu_control & 0x08)\n'
      b'            break;\n'
      b'         continue;\n'
      b'      }\n'
      b'#endif\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'   gpu_in_exec--;\n'
      b'\n'
      b'   VJP_LEAVE(VJP_GPU);\n',
      b'#if defined(EMUSUNI_GPU_JIT) && !defined(__ORBIS__)\n'
      b'   GPUJitTraceSlice(cycles);\n'
      b'#endif\n'
      b'   gpu_in_exec--;\n'
      b'\n'
      b'   VJP_LEAVE(VJP_GPU);\n')
patch('vjaguar', 'src/tom/gpu.c',
      b'   STATE_LOAD_VAR(buf, gpu_releaseTimeSlice_flag);\n\n   return (size_t)(buf - start);\n}\n',
      b'   STATE_LOAD_VAR(buf, gpu_releaseTimeSlice_flag);\n\n   return (size_t)(buf - start);\n}\n'
      b'\n'
      b'#include "emusuni_gpu_jit.h"\n')
patch('vjaguar', 'libretro.c',
      b'   GPUClockScaleReset();\n',
      b'   GPUClockScaleReset();\n'
      b'   {\n'
      b'      /* EmuSuNi: the GPU\'s and the DSP\'s recompilers (src/tom/emusuni_gpu_jit.h, src/jerry/emusuni_dsp_jit.h),\n'
      b'       * when the frontend asks for them (the DSP\'s: both options). */\n'
      b'      extern int gpu_jit_wanted, dsp_jit_wanted;\n'
      b'      var.key = "virtualjaguar_emusuni_jit";\n'
      b'      var.value = NULL;\n'
      b'      gpu_jit_wanted = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value\n'
      b'                    && strcmp(var.value, "enabled") == 0;\n'
      b'      var.key = "virtualjaguar_emusuni_jit_dsp";\n'
      b'      var.value = NULL;\n'
      b'      dsp_jit_wanted = gpu_jit_wanted && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value\n'
      b'                    && strcmp(var.value, "enabled") == 0;\n'
      b'   }\n')

# ---- Virtual Jaguar: the DSP's recompiler (00.96, scripts/vjaguar-jit/emusuni_dsp_jit.h: the GPU's for the DSP) ----
# Included at the end of dsp.c; dsp.c tells it of every write to the DSP's local RAM, of a reset and a state load, and
# DSPExecUntimed's loop (dsp_exec_untimed.h, not the debugger's copy) runs a block when it can. The frontend turns it
# on with virtualjaguar_emusuni_jit and virtualjaguar_emusuni_jit_dsp (libretro.c). The PC's comparison:
# EMUSUNI_DSP_TRACE.
with open(os.path.join(sys.argv[5], 'vjaguar-jit', 'emusuni_dsp_jit.h')) as f:
    addfile('vjaguar', 'src/jerry/emusuni_dsp_jit.h', f.read())
patch('vjaguar', 'src/jerry/dsp.c',
      b'static uint8_t dsp_ram_8[0x2000];\n',
      b'static uint8_t dsp_ram_8[0x2000];\n'
      b'/* EmuSuNi: the recompiler (emusuni_dsp_jit.h, at the end): every 16-byte line of local RAM counts its writes;\n'
      b' * a write to a word with compiled code in it also stops the block running. */\n'
      b'#if defined(__x86_64__) && !defined(VJ_TRACE)\n'
      b'#define EMUSUNI_DSP_JIT 1\n'
      b'static uint64_t dsp_jit_line_version[512];\n'
      b'static uint8_t  dsp_jit_code_word[4096];\n'
      b'static uint8_t  dsp_jit_code_hit;\n'
      b'static uint64_t dsp_jit_code_gen;   /* (00.98) words with compiled code written: every block checked again */\n'
      b'#define DSP_JIT_WROTE(off) do { uint32_t o_ = (uint32_t)(off) & 0x1FFF; dsp_jit_line_version[o_ >> 4]++; \\\n'
      b'      if (dsp_jit_code_word[o_ >> 1]) { dsp_jit_code_word[o_ >> 1] = 0; dsp_jit_code_hit = 1; \\\n'
      b'         dsp_jit_code_gen++; } } while (0)\n'
      b'static int DSPJitRun(int32_t *cycles, int idle_skip, uint32_t **ldx_armed, uint32_t *ldx_val,\n'
      b'                     uint32_t *ldx_pc);\n'
      b'static void DSPJitFlush(void);\n'
      b'static void DSPJitReset(void);\n'
      b'extern int dsp_jit_wanted;\n'
      b'#ifndef __ORBIS__\n'
      b'static void DSPJitTraceSlice(int32_t cycles);\n'
      b'#endif\n'
      b'#else\n'
      b'#define DSP_JIT_WROTE(off) do { } while (0)\n'
      b'#endif\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'      dsp_ram_8[offset] = data;\n',
      b'      dsp_ram_8[offset] = data;\n'
      b'      DSP_JIT_WROTE(offset);\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'      dsp_ram_8[offset+1] = data & 0xFF;\n',
      b'      dsp_ram_8[offset+1] = data & 0xFF;\n'
      b'      DSP_JIT_WROTE(offset);\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'      SET32(dsp_ram_8, offset, data);\n',
      b'      SET32(dsp_ram_8, offset, data);\n'
      b'      DSP_JIT_WROTE(offset);\n'
      b'      DSP_JIT_WROTE(offset + 2);\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'\t\t\tSET16(dsp_ram_8, offset, 0);\n',
      b'\t\t\tSET16(dsp_ram_8, offset, 0);\n'
      b'\t\t\tDSP_JIT_WROTE(offset);\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'         SET32(dsp_ram_8, offset, 0);\n',
      b'         SET32(dsp_ram_8, offset, 0);\n'
      b'         DSP_JIT_WROTE(offset);\n'
      b'         DSP_JIT_WROTE(offset + 2);\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'\tdsp_reset_stats();\n',
      b'\tdsp_reset_stats();\n'
      b'#ifdef EMUSUNI_DSP_JIT\n'
      b'\tDSPJitReset();\n'
      b'#endif\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'   STATE_LOAD_BUF(buf, dsp_ram_8, sizeof(dsp_ram_8));\n',
      b'   STATE_LOAD_BUF(buf, dsp_ram_8, sizeof(dsp_ram_8));\n'
      b'#ifdef EMUSUNI_DSP_JIT\n'
      b'   DSPJitFlush();\n'
      b'#endif\n')
patch('vjaguar', 'src/jerry/dsp.c',
      b'\tdsp_in_exec--;\n\n\tVJP_LEAVE(VJP_DSP);\n}\n',
      b'\tdsp_in_exec--;\n\n\tVJP_LEAVE(VJP_DSP);\n}\n'
      b'\n'
      b'#include "emusuni_dsp_jit.h"\n')
patch('vjaguar', 'src/jerry/dsp_exec_untimed.h',
      b'\t\t{\n\t\t\tcycles = 0;\n\t\t\tbreak;\n\t\t}\n',
      b'\t\t{\n\t\t\tcycles = 0;\n\t\t\tbreak;\n\t\t}\n'
      b'\n'
      b'#if defined(EMUSUNI_DSP_JIT) && !DSP_UNTIMED_GDB\n'
      b'\t\t/* EmuSuNi: recompiled blocks from here, when nothing the loop does below is pending for this\n'
      b'\t\t * instruction (emusuni_dsp_jit.h: one after another, the idle probe after each jr). */\n'
      b'\t\tif (dsp_jit_wanted && !ldxArmed && !divArmed && !dspFlagsRetireDelay && !IMASKCleared\n'
      b'\t\t    && !blitMemoRecording && DSPJitRun(&cycles, idleSkipActive, &ldxArmed, &ldxVal, &ldxPc))\n'
      b'\t\t\tcontinue;\n'
      b'#endif\n')
patch('vjaguar', 'src/jerry/dsp_exec_untimed.h',
      b'\tdsp_in_exec--;\n\n\tVJP_LEAVE(VJP_DSP);\n}\n',
      b'#if defined(EMUSUNI_DSP_JIT) && !defined(__ORBIS__) && !DSP_UNTIMED_GDB\n'
      b'\tDSPJitTraceSlice(cycles);\n'
      b'#endif\n'
      b'\tdsp_in_exec--;\n\n\tVJP_LEAVE(VJP_DSP);\n}\n')

STELLA_FILES = ['src/' + p for p in compiled] + ['src/os/libretro/emusuni_throw.cxx',
                                                 'src/os/libretro/emusuni_stdexcept.cxx']
with open(os.path.join(out_dir, 'stella-sources.txt.new'), 'w') as f:
    f.write('\n'.join(STELLA_FILES) + '\n')
STELLA_C = sorted(set(re.findall(r'\$\(CORE_DIR\)/([\w/+.-]+\.c)\b', makefile)))
with open(os.path.join(out_dir, 'stella-c-sources.txt.new'), 'w') as f:
    f.write('\n'.join('src/' + p for p in STELLA_C) + '\n')

written = 0
for (core, path), text in list(patched.items()) + list(added.items()):
    full = roots[core] + '/' + path
    try:
        with open(full, 'rb') as f:
            same = f.read() == text
    except FileNotFoundError:
        same = False
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
print("Stella: %d files without C++ exceptions; Virtual Jaguar: no BIOS, its memory on the heap (%d files changed)" %
      (rewritten, written))
PATCH
for list in stella-sources.txt stella-c-sources.txt jaguar-sources.txt jaguar-cold-sources.txt \
    jaguar-include/version.h; do
  if cmp -s "$OUT/$list.new" "$OUT/$list"; then rm -f "$OUT/$list.new"; else mv "$OUT/$list.new" "$OUT/$list"; fi
done

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
# The cores: name, its API's prefix, its sources.
CORES="a2600 a7800 jaguar"
declare -A PREFIX=([a2600]=stella [a7800]=prosystem [jaguar]=vjaguar)
declare -A NAME=([a2600]="Stella" [a7800]="ProSystem" [jaguar]="Virtual Jaguar")
declare -A SRC=([a2600]="$STELLA_SRC" [a7800]="$PROSYSTEM_SRC" [jaguar]="$JAGUAR_SRC")
for core in $CORES; do
  {
    echo "// Generated by scripts/build-atari-cores.sh: the ${NAME[$core]} core's libretro API, renamed."
    echo "#pragma once"
    for f in $API; do echo "#define $f ${PREFIX[$core]}_$f"; done
    # (Stella: without C++ exceptions, emusuni_throw.hxx, before everything.)
    [ "$core" = a2600 ] && echo '#include "emusuni_throw.hxx"'
  } > "$OUT/prefix_$core.h.new"
  # Rewritten only when different: every source depends on it.
  if cmp -s "$OUT/prefix_$core.h.new" "$OUT/prefix_$core.h"; then
    rm -f "$OUT/prefix_$core.h.new"
  else
    mv "$OUT/prefix_$core.h.new" "$OUT/prefix_$core.h"
  fi
done

# The cores' CMake: what their Makefile.common compiles, with their Makefile's defines.
mkdir -p "$OUT/cmake"
cat > "$OUT/cmake/CMakeLists.txt.new" <<'CMAKE'
# Generated by scripts/build-atari-cores.sh: Stella and ProSystem as static libraries.
cmake_minimum_required(VERSION 3.20)
project(atari C CXX)

# Stella (the Atari 2600): its libretro Makefile's sources and defines (-D__LIB_RETRO__ -DSOUND_SUPPORT -DZIP_SUPPORT
# -DHAVE_STDINT_H, C++23 without RTTI), without exceptions (emusuni_throw.hxx).
set(S ${STELLA_DIR}/src)
file(STRINGS ${OUT_DIR}/stella-sources.txt STELLA_CXX)
file(STRINGS ${OUT_DIR}/stella-c-sources.txt STELLA_C)
list(TRANSFORM STELLA_CXX PREPEND ${STELLA_DIR}/)
list(TRANSFORM STELLA_C PREPEND ${STELLA_DIR}/)
add_library(a2600 STATIC ${STELLA_CXX} ${STELLA_C})
target_include_directories(a2600 PRIVATE ${S} ${S}/os/libretro ${S}/emucore ${S}/emucore/elf ${S}/emucore/tia
  ${S}/common ${S}/common/audio ${S}/common/tv_filters ${S}/common/repository/sqlite ${S}/lib ${S}/lib/sqlite
  ${S}/lib/zlib ${S}/lib/json ${S}/lib/libretro-common/include)
target_compile_definitions(a2600 PRIVATE __LIB_RETRO__ SOUND_SUPPORT ZIP_SUPPORT HAVE_STDINT_H
  GIT_VERSION="${STELLA_REVISION}")
set_target_properties(a2600 PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(a2600 PRIVATE -w -include ${OUT_DIR}/prefix_a2600.h)

# ProSystem (the Atari 7800): core/, BupBoop's CoreTone, libretro-common (its Makefile.common).
set(P ${PROSYSTEM_DIR})
set(PL ${P}/libretro-common)
add_library(a7800 STATIC
  ${P}/core/libretro.c ${P}/core/Bios.c ${P}/core/BupChip.c ${P}/core/Cartridge.c ${P}/core/Database.c
  ${P}/core/Hash.c ${P}/core/Maria.c ${P}/core/Memory.c ${P}/core/Palette.c ${P}/core/Pokey.c ${P}/core/ProSystem.c
  ${P}/core/Region.c ${P}/core/Riot.c ${P}/core/Sally.c ${P}/core/Tia.c
  ${P}/bupboop/coretone/channel.c ${P}/bupboop/coretone/coretone.c ${P}/bupboop/coretone/music.c
  ${P}/bupboop/coretone/sample.c
  ${PL}/compat/compat_posix_string.c ${PL}/compat/compat_strcasestr.c ${PL}/compat/compat_snprintf.c
  ${PL}/compat/compat_strl.c ${PL}/compat/fopen_utf8.c ${PL}/encodings/encoding_utf.c ${PL}/file/file_path.c
  ${PL}/file/file_path_io.c ${PL}/streams/file_stream.c ${PL}/streams/file_stream_transforms.c
  ${PL}/string/stdstring.c ${PL}/time/rtime.c ${PL}/vfs/vfs_implementation.c)
target_include_directories(a7800 PRIVATE ${P}/core ${PL}/include)
target_compile_definitions(a7800 PRIVATE __LIBRETRO__ GIT_VERSION="${PROSYSTEM_REVISION}")
target_compile_options(a7800 PRIVATE -w -include ${OUT_DIR}/prefix_a7800.h)

# Virtual Jaguar (the Jaguar): its Makefile.common's sources (-D__LIBRETRO__ -DINLINE=inline, the SSE2 blitter), its
# version.h made here, without libchdr (emusuni_nochd.c: its CHD images are Jaguar CD discs).
set(J ${JAGUAR_DIR})
file(STRINGS ${OUT_DIR}/jaguar-sources.txt JAGUAR_C)
list(TRANSFORM JAGUAR_C PREPEND ${J}/)
add_library(jaguar STATIC ${JAGUAR_C})
target_include_directories(jaguar PRIVATE ${OUT_DIR}/jaguar-include ${J} ${J}/src ${J}/src/core ${J}/src/tom
  ${J}/src/jerry ${J}/src/cd ${J}/src/bios ${J}/src/m68000 ${J}/src/debug ${J}/libretro-common/include
  ${J}/deps/libchdr/include ${J}/deps/libchdr/deps/lzma-25.01/include ${J}/deps/libchdr/deps/miniz-3.1.1
  ${J}/deps/libchdr/deps/zstd-1.5.7)
target_compile_definitions(jaguar PRIVATE __LIBRETRO__ INLINE=inline BLITTER_SIMD_SSE2)
target_compile_options(jaguar PRIVATE -w -msse2 -include ${OUT_DIR}/prefix_jaguar.h)
file(STRINGS ${OUT_DIR}/jaguar-cold-sources.txt JAGUAR_COLD)
list(TRANSFORM JAGUAR_COLD PREPEND ${J}/)
set_source_files_properties(${JAGUAR_COLD} PROPERTIES COMPILE_OPTIONS -Os)
# (Its PGO: the profile, or instrumented for a new one; see JAGUAR_PGO below.)
separate_arguments(JAGUAR_PGO_FLAGS UNIX_COMMAND "${JAGUAR_PGO}")
target_compile_options(jaguar PRIVATE ${JAGUAR_PGO_FLAGS})
CMAKE
if cmp -s "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"; then
  rm -f "$OUT/cmake/CMakeLists.txt.new"
else
  mv "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O3, the PS4's CPU, a section for each
# function and variable (the app's link drops the unused ones), no unwind tables (nothing unwinds through them); the
# C++ files without exceptions or RTTI.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -fPIC"
# (AddressSanitizer: its globals registered by each file, not through the section the -r link and the symbols made
# local would confuse.)
[ "$ASAN" = 1 ] && FLAGS_CORE+=" -fsanitize=address -fno-omit-frame-pointer -fno-sanitize-address-globals-dead-stripping"
# Stella, the only C++ core here, at -Os: 1.3 MB of code, not 1.8 (-O3), and as fast (the PC, its own test ROMs: 128.bin
# 1.17 ms a frame against 1.13, Draconian 0.71 against 0.71); ATARI_STELLA_OPT=-O3 to measure again.
FLAGS_CXX="${FLAGS_CORE/-O3/${ATARI_STELLA_OPT:--Os}} -fno-exceptions -fno-rtti"
# Virtual Jaguar's PGO (profile-guided optimisation, 00.90): scripts/jaguar-core.profdata.xz, a profile of the PC build
# with the app's options (local/herramientas/prueba-atari, HARNESS_PLAY) running the owner's games as the app runs them
# (Emulator.cpp's JAGUAR_TUNES: Doom with the fast blitter and GPU/DSP at half, Tempest 2000, Alien vs Predator and
# (00.98) Iron Soldier and Wolfenstein 3D with the fast blitter; 00.96: the GPU's and DSP's recompilers on) - Doom,
# Tempest 2000, Rayman, Wolfenstein 3D and Alien vs Predator (the final game and its v0.93 beta): their attract modes
# and a scripted start (3600 frames each), Iron Soldier's briefing and 3D demo -, and Virtual Jaguar's test ROMs NICCC
# and yarc (3600), used by both builds when it's there
# (ATARI_JAGUAR_PGO=off: without it). On the PC: Doom 14-15% faster, Rayman 25%, NICCC 12%, yarc 5% (00.90-00.94's); a
# game not in it gains too (NICCC 10% with Doom's alone); in it, Alien vs Predator's game 18% more (00.95), the others
# the same (+-2%). A profile older than the sources only optimises less (a function that changed gets none: the fast
# blitter's change of 00.96 made Tempest 2000 7-17% slower until the profile was made again). A new one:
# ATARI_CORE_HOST=1 ATARI_JAGUAR_PGO=generate build-atari-cores.sh <dir> (instrumented), the harness linked with
# -fprofile-instr-generate and run on the games with LLVM_PROFILE_FILE=<dir>/%p.profraw, then
# llvm-profdata-21 merge -o jaguar-core.profdata <dir>/*.profraw; xz -9 it into scripts/.
PROFILE="${ATARI_JAGUAR_PROFILE:-$HERE/jaguar-core.profdata.xz}"
PGO="none"
JAGUAR_PGO=""
case "${ATARI_JAGUAR_PGO:-use}" in
  generate)
    if [ "$HOST" != 1 ]; then
      echo "Virtual Jaguar: ATARI_JAGUAR_PGO=generate is for the PC build (ATARI_CORE_HOST=1)"
      exit 1
    fi
    JAGUAR_PGO="-fprofile-instr-generate"
    PGO="generate" ;;
  off) ;;
  *)
    if [ -f "$PROFILE" ]; then
      xz -dc "$PROFILE" > "$OUT/jaguar-core.profdata.new"
      if cmp -s "$OUT/jaguar-core.profdata.new" "$OUT/jaguar-core.profdata"; then
        rm -f "$OUT/jaguar-core.profdata.new"
      else
        mv "$OUT/jaguar-core.profdata.new" "$OUT/jaguar-core.profdata"
      fi
      # (The code no profiled game ran keeps -O3: not made small as cold, -pgso=false; another game may need it.)
      JAGUAR_PGO="-fprofile-instr-use=$OUT/jaguar-core.profdata -mllvm -pgso=false"
      PGO="profile $(sha256sum "$PROFILE" | cut -c1-16)"
    fi ;;
esac
REV() { echo " $(git -C "$1" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"; }
OPTIONS=(-DSTELLA_DIR="$STELLA_SRC" -DPROSYSTEM_DIR="$PROSYSTEM_SRC" -DJAGUAR_DIR="$JAGUAR_SRC" -DOUT_DIR="$OUT"
  "-DSTELLA_REVISION=$(REV "$STELLA_SRC")" "-DPROSYSTEM_REVISION=$(REV "$PROSYSTEM_SRC")"
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CXX" "-DJAGUAR_PGO=$JAGUAR_PGO")
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 "$@"
  }
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
COMMITS=""
for core in $CORES; do COMMITS+=" $(git -C "${SRC[$core]}" rev-parse HEAD 2>/dev/null || echo unknown)"; done
WANT="$STELLA_SRC $PROSYSTEM_SRC $JAGUAR_SRC |$COMMITS | host $HOST | pgo $PGO | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Atari cores: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$OUT/cmake" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Atari cores: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" $CORES >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "Atari cores: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"
[ "${PGO%% *}" = profile ] && echo "Virtual Jaguar: PGO with $(basename "$PROFILE") (${PGO#profile })"

if [ "$HOST" != 1 ]; then
  # What the PS4 libraries the app links give, and the app (operator new/delete: port/ps4_new.cpp, compiled here as
  # the app compiles it, only to list its symbols).
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores.)
  # (emusuni_jit_arena: port/ps4_n64.c's shared JIT arena, the Jaguar GPU's recompiler's code.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" | awk 'NF >= 3 {print $3}'; echo __dso_handle
                 echo emusuni_jit_arena; } |
               sort -u)
  rm -f "$OUT/check_ps4_new.o"
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
  # What the system's C++ library gives (libSceLibcInternal: not libc++, another layout for the same classes). The app
  # takes from it what libc++ gives too, so a core needing one of these would mix both (emusuni_stdexcept.cxx).
  SONY_CXX=$(llvm-nm-21 -D --defined-only "$OPENORBIS/lib/libSceLibcInternal.so" | awk '$NF ~ /^_Z/ {print $NF}' |
             sort -u)
fi

# One object for each core: only its libretro API (renamed) stays global. Replaced only when it changed (the app isn't
# linked again for nothing).
for core in $CORES; do
  P="${PREFIX[$core]}"
  N="${NAME[$core]}"
  ld.lld-21 -r --whole-archive "$BUILD/lib$core.a" -o "$OUT/${core}_core_all.o"
  llvm-objcopy-21 --wildcard --keep-global-symbol="${P}_retro_*" "$OUT/${core}_core_all.o" "$OUT/${core}_core.o.new"
  rm -f "$OUT/${core}_core_all.o"
  if [ "$PGO" = generate ] && [ "$core" = jaguar ]; then
    # (The instrumented PC build for a new profile: its counters and the profile runtime's hooks are no app's object.)
    echo "Virtual Jaguar: instrumented for PGO (ATARI_JAGUAR_PGO=generate): the object's checks left out"
    mv "$OUT/${core}_core.o.new" "$OUT/${core}_core.o"
    continue
  fi

  # The checks.
  GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/${core}_core.o.new" | awk '{print $3}' | sort)
  WANT_GLOBALS=$(for f in $API; do echo "${P}_$f"; done | sort)
  if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
    echo "$N: the object's global symbols aren't exactly the renamed libretro API:"
    diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
    exit 1
  fi
  UNDEFINED=$(llvm-nm-21 -u "$OUT/${core}_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
  # No weak references left unresolved: the app would export them as imports nobody gives.
  # (AddressSanitizer's own, on the PC, are the sanitizer runtime's.)
  WEAK=$(llvm-nm-21 -u "$OUT/${core}_core.o.new" | awk '$1 != "U" {print $NF}' | grep -v '_asan_globals$' | sort -u ||
         true)
  if [ -n "$WEAK" ]; then
    echo "$N: the object has weak references nothing defines:"
    echo "$WEAK"
    exit 1
  fi
  if llvm-nm-21 --defined-only "$OUT/${core}_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
    echo "$N: the object defines a C++ operator new/delete of its own"
    exit 1
  fi
  # No C++ exceptions: nothing thrown, caught or unwound through (an exception ends the app without a word).
  if echo "$UNDEFINED" | grep -E '^(__cxa_(throw|allocate_exception|begin_catch|end_catch|rethrow)|__gxx_personality|_Unwind_)'; then
    echo "$N: the object throws or catches C++ exceptions"
    exit 1
  fi
  # No static array of 1 MiB or more: the PS4 charges the app's whole static memory from its start, used or not.
  # (Not with AddressSanitizer: its red zones make Stella's 0.9 MiB game properties bigger.)
  [ "$ASAN" = 1 ] || BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/${core}_core.o.new" |
        awk '($3 == "b" || $3 == "B" || $3 == "d" || $3 == "D") && $2 + 0 >= 1048576 {print $4}')
  if [ -n "${BIG:-}" ]; then
    echo "$N: the object has static arrays of 1 MiB or more:"
    echo "$BIG"
    exit 1
  fi
  if [ "$HOST" != 1 ]; then
    MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
    if [ -n "$MISSING" ]; then
      echo "$N: the object needs symbols the PS4 libraries and the app don't have:"
      echo "$MISSING"
      exit 1
    fi
    # (std::terminate: it ends the app, whichever library's.)
    MIXED=$(comm -12 <(echo "$UNDEFINED") <(echo "$SONY_CXX") | comm -23 - <(echo "$APP_GIVES") |
            grep -vx '_ZSt9terminatev' || true)
    if [ -n "$MIXED" ]; then
      echo "$N: the object needs C++ library pieces the app would take from the system's C++ library, not libc++:"
      echo "$MIXED"
      exit 1
    fi
    NEW_DELETE=$(echo "$UNDEFINED" | grep -cE '^_Z(nw|na|dl|da)' || true)
    echo "$N: $(echo "$UNDEFINED" | sed '/^$/d' | wc -l) symbols needed, all in the PS4 libraries and the app" \
         "($NEW_DELETE operator new/delete, port/ps4_new.cpp's)"
  fi
  if cmp -s "$OUT/${core}_core.o.new" "$OUT/${core}_core.o"; then
    rm -f "$OUT/${core}_core.o.new"
  else
    mv "$OUT/${core}_core.o.new" "$OUT/${core}_core.o"
  fi
done
llvm-size-21 "$OUT/a2600_core.o" "$OUT/a7800_core.o" "$OUT/jaguar_core.o"
