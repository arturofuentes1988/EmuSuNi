#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Genesis Plus GX (libretro/Genesis-Plus-GX by Eke-Eke, ~/src/Genesis-Plus-GX: its own
# non-commercial licence, as Snes9x's) for the PS4 into one object that scripts/build.sh links into
# EmuSuNi: the SG-1000, Master System, Game Gear and Mega Drive. It has no CMake of its own: the one
# made here lists the sources its libretro build does (libretro/Makefile.common: the core, its CPUs,
# sound chips and cartridge hardware, the Mega-CD's, zlib, libchdr with its LZMA SDK and zstd, Tremor
# for a CD's OGG tracks, libretro-common), compiled with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake) and the defines of its unix build: RGB565 pictures
# (FRONTEND_SUPPORTS_RGB565), little-endian, the 68000 and Z80 overclock, the Nuked OPN2 and OPLL
# sound cores, its files through libretro's VFS (libretro-common's own stdio: the app answers no VFS
# interface). The app hands it the games from memory (GET_GAME_INFO_EXT), .zip ones too. Not as its
# unix build:
#   - A 10 MiB cartridge buffer (MAX_ROM_SIZE, the core's own default; the unix build's 32 MiB): it
#     holds every Mega Drive cartridge (Super Street Fighter II: 5 MiB) and the Master System and Game
#     Gear BIOSes it puts at +4 MiB. It and the rest of the cartridge's or CD unit's memory (ext) are on
#     the heap while the core runs (upstream's USE_DYNAMIC_ALLOC, as its 3DS and Wii U builds), not in
#     the app's static memory, which the PS4 charges in full from the app's start: 4 MB of it instead of
#     14 (36.7 with the unix build's buffer).
#   - The core options' translations left out (HAVE_NO_LANGEXTRA: the app sets the options itself),
#     and the per-channel volumes (USE_PER_SOUND_CHANNELS_CONFIG: 19 more options, all at 100%).
#   - No CD drive access (HAVE_CDROM) and no zstd tracing hooks (weak references nothing defines).
#   - Every function and variable in its own section (-ffunction-sections -fdata-sections), so the
#     app's link (--gc-sections) leaves out what nothing calls (the LZMA encoder files: libchdr
#     only decodes).
#   - -O3 (its emscripten build's; unix: -O2), for the PS4's CPU (btver2).
# Changes to its sources (applied each time to the file as git has it, checked):
#   - A .sc file (a cartridge of the SC-3000, the SG-1000 as a computer) is the SG-1000's, as a .sg
#     is; upstream took it for a Mega Drive ROM.
#   - A game's cheats end with it: its list is static, and the next game's cheat reset (the app's, at
#     every start) undid the ROM patches in that game's ROM - the words of the one before written into
#     it. The game's own patches are undone at its unload now, while its ROM is still there.
#   - The cartridge's or CD unit's memory (ext, with USE_DYNAMIC_ALLOC) is made at retro_init (upstream
#     makes it at the first load_rom and never gives it back) and given back at retro_deinit, with the
#     BIOSes load_bios keeps in it (the Mega-CD's, the Master System's, the Game Gear's) marked as not
#     loaded: the next session loads them again.
#   - The 68000s' opcode tables (the main CPU's and the Mega-CD's) are made when the CPU starts. The
#     source has them as two 64K-entry arrays of function pointers, and each pointer is a 24-byte
#     relocation of the program image: 4.2 MB of it. They are generated here, from those same tables,
#     as 16-bit numbers of the ~2,000 distinct handlers, and expanded in m68k_init and s68k_init.
#     (Musashi's own BUILD_TABLES builds them from a description table instead, but that path no
#     longer compiles.)
# Its libretro API becomes gpgx_retro_* and everything else in it private, as for FCEUmm
# (CMakeLists.txt). The object is checked at the end: only gpgx_retro_* defined globally, no weak
# references left, no C++ operator new or delete, and every symbol it needs found in the PS4
# libraries the app links (OpenOrbis' libc, libkernel, libSceLibcInternal, clang's builtins).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-gpgx-core.sh <out dir> [clean]
#   GPGX_CORE_HOST=1 build-gpgx-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                   the PC tests (local/herramientas/prueba-sega): no
#                                                   PS4 checks
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${GPGX_DIR:-$HOME/src/Genesis-Plus-GX}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/sega-core.log"
HOST="${GPGX_CORE_HOST:-0}"

python3 -I - "$SRC" <<'PATCH'
import re
import subprocess
import sys
root = sys.argv[1]
patched = {}
def patch(path, old, new):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one;
    # as bytes: its CRLF line ends and Latin-1 comments stay as they are.
    if path not in patched:
        patched[path] = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                                       capture_output=True).stdout
    text = patched[path]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[path] = text.replace(old, new)

# A .sc file is an SG-1000 game too (SC-3000 cartridges).
patch('core/loadrom.c',
      b'    else if (!memcmp("SG", &extension[1], 2))',
      b'    else if (!memcmp("SG", &extension[1], 2) || !memcmp("SC", &extension[1], 2)) /* EmuSuNi: .sc too */')
# The cheats go with their game (statics outlive it: the core is linked into the app).
patch('libretro/libretro.c',
      b'void retro_unload_game(void) \r\n{\r\n\t/* Clear disk interface */\r\n   int i;\r\n',
      b'void retro_unload_game(void) \r\n{\r\n\t/* Clear disk interface */\r\n   int i;\r\n'
      b'   /* EmuSuNi: the cheats end with the game, its ROM patches undone while its ROM is there */\r\n'
      b'   retro_cheat_reset();\r\n')
# The cartridge's or CD unit's memory (ext, 10 MiB with USE_DYNAMIC_ALLOC) on the heap while the core runs:
# made at retro_init (load_rom makes it too if it's missing), given back at retro_deinit. The BIOSes
# load_bios put in it (system_bios: 0x10 the Mega-CD's, the Master System's, the Game Gear's) go with it;
# the Mega Drive's (boot_rom) is a static array of its own and stays.
patch('libretro/libretro.c',
      b'   audio_latency              = 0;\r\n   update_audio_latency       = false;\r\n}\r\n\r\nvoid retro_deinit(void)\r\n',
      b'   audio_latency              = 0;\r\n   update_audio_latency       = false;\r\n'
      b'#ifdef USE_DYNAMIC_ALLOC\r\n'
      b'   /* EmuSuNi: the cartridge / CD hardware memory on the heap while the core runs (not static) */\r\n'
      b'   if (!ext)\r\n'
      b'      ext = (external_t *)calloc(1, sizeof(external_t));\r\n'
      b'#endif\r\n'
      b'}\r\n\r\nvoid retro_deinit(void)\r\n')
patch('libretro/libretro.c',
      b'   g_rom_data = NULL;\r\n   g_rom_size = 0;\r\n}\r\n\r\nvoid retro_reset(void)\r\n',
      b'   g_rom_data = NULL;\r\n   g_rom_size = 0;\r\n'
      b'#ifdef USE_DYNAMIC_ALLOC\r\n'
      b'   /* EmuSuNi: given back; the BIOSes load_bios kept in it are no longer loaded */\r\n'
      b'   free(ext);\r\n'
      b'   ext = NULL;\r\n'
      b'   system_bios &= ~(0x10 | SYSTEM_SMS | SYSTEM_GG);\r\n'
      b'#endif\r\n'
      b'}\r\n\r\nvoid retro_reset(void)\r\n')
# The 68000s' opcode table (the main CPU's and the Mega-CD's, one header for both): 16-bit numbers of the
# distinct handlers, expanded into the table when the CPU starts.
TABLE = 'core/m68k/m68ki_instruction_jump_table.h'
original = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + TABLE], check=True, capture_output=True).stdout
names = re.findall(rb'm68k_op_\w+', original)
assert len(names) == 0x10000, len(names)
distinct = list(dict.fromkeys(names))
number = {name: i for i, name in enumerate(distinct)}
assert len(distinct) < 0x10000
lines = [b'/* EmuSuNi: the opcode table made when the CPU starts (m68ki_expand_instruction_table, from m68k_init and',
         b'   s68k_init): stored as 65,536 pointers each would be a relocation of the program image (24 bytes). The',
         b'   distinct handlers, and the number of the one each opcode has. */',
         b'static void (* const m68ki_instruction_handlers[])(void) =', b'{']
for i in range(0, len(distinct), 4):
    lines.append(b'  ' + b', '.join(distinct[i:i + 4]) + b',')
lines += [b'};', b'static const unsigned short m68ki_instruction_index[0x10000] =', b'{']
for i in range(0, 0x10000, 16):
    lines.append(b'  ' + b', '.join(b'%d' % number[n] for n in names[i:i + 16]) + b',')
lines += [b'};', b'static void (*m68ki_instruction_jump_table[0x10000])(void);',
          b'static void m68ki_expand_instruction_table(void)', b'{', b'  int i;', b'  for (i = 0; i < 0x10000; i++)',
          b'    m68ki_instruction_jump_table[i] = m68ki_instruction_handlers[m68ki_instruction_index[i]];', b'}', b'']
patched[TABLE] = b'\r\n'.join(lines)  # (the header's line ends are CRLF)
patch('core/m68k/m68kcpu.c',
      b'void m68k_init(void)\r\n{\r\n',
      b'void m68k_init(void)\r\n{\r\n  m68ki_expand_instruction_table(); /* EmuSuNi: see m68ki_instruction_jump_table.h */\r\n')
patch('core/m68k/s68kcpu.c',
      b'void s68k_init(void)\n{\n',
      b'void s68k_init(void)\n{\n  m68ki_expand_instruction_table(); /* EmuSuNi: see m68ki_instruction_jump_table.h */\n')

written = 0
for path, text in patched.items():
    full = root + '/' + path
    with open(full, 'rb') as f:
        same = f.read() == text
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
print("Genesis Plus GX: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
{
  echo "// Generated by scripts/build-gpgx-core.sh: the gpgx core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f gpgx_$f"; done
} > "$OUT/prefix_gpgx.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_gpgx.h.new" "$OUT/prefix_gpgx.h"; then
  rm -f "$OUT/prefix_gpgx.h.new"
else
  mv "$OUT/prefix_gpgx.h.new" "$OUT/prefix_gpgx.h"
fi

# The core's CMake: what libretro/Makefile.common compiles for it (HAVE_CHD, no HOOK_CPU, not
# STATIC_LINKING: its libretro-common files too), with Makefile.libretro's defines.
mkdir -p "$OUT/cmake"
cat > "$OUT/cmake/CMakeLists.txt.new" <<'CMAKE'
# Generated by scripts/build-gpgx-core.sh: Genesis Plus GX's libretro core as a static library.
cmake_minimum_required(VERSION 3.20)
project(gpgx C)
set(S ${GPGX_DIR})
set(D ${S}/libretro/deps)
set(L ${S}/libretro/libretro-common)
set(CORE_DIRS core core/z80 core/m68k core/ntsc core/sound core/sound/minimp3 core/input_hw core/cd_hw
  core/cart_hw core/cart_hw/svp core/sound/tremor)
set(SOURCES)
foreach(dir ${CORE_DIRS})
  file(GLOB found ${S}/${dir}/*.c)
  list(APPEND SOURCES ${found})
endforeach()
list(APPEND SOURCES
  ${D}/lzma-19.00/src/Alloc.c ${D}/lzma-19.00/src/Bra86.c ${D}/lzma-19.00/src/BraIA64.c
  ${D}/lzma-19.00/src/CpuArch.c ${D}/lzma-19.00/src/Delta.c ${D}/lzma-19.00/src/LzFind.c
  ${D}/lzma-19.00/src/Lzma86Dec.c ${D}/lzma-19.00/src/LzmaDec.c ${D}/lzma-19.00/src/LzmaEnc.c
  ${D}/lzma-19.00/src/Sort.c
  ${D}/libchdr/src/libchdr_bitstream.c ${D}/libchdr/src/libchdr_cdrom.c ${D}/libchdr/src/libchdr_chd.c
  ${D}/libchdr/src/libchdr_flac.c ${D}/libchdr/src/libchdr_huffman.c
  ${D}/zstd/lib/common/entropy_common.c ${D}/zstd/lib/common/error_private.c
  ${D}/zstd/lib/common/fse_decompress.c ${D}/zstd/lib/common/zstd_common.c ${D}/zstd/lib/common/xxhash.c
  ${D}/zstd/lib/decompress/huf_decompress.c ${D}/zstd/lib/decompress/zstd_ddict.c
  ${D}/zstd/lib/decompress/zstd_decompress.c ${D}/zstd/lib/decompress/zstd_decompress_block.c
  ${L}/streams/file_stream.c ${L}/streams/file_stream_transforms.c ${L}/compat/fopen_utf8.c
  ${L}/compat/compat_snprintf.c ${L}/compat/compat_strl.c ${L}/compat/compat_strcasestr.c
  ${L}/compat/compat_posix_string.c ${L}/encodings/encoding_utf.c ${L}/file/file_path.c
  ${L}/file/retro_dirent.c ${L}/lists/string_list.c ${L}/lists/dir_list.c ${L}/memmap/memalign.c
  ${L}/string/stdstring.c ${L}/vfs/vfs_implementation.c
  ${D}/zlib-1.2.11/adler32.c ${D}/zlib-1.2.11/crc32.c ${D}/zlib-1.2.11/inffast.c
  ${D}/zlib-1.2.11/inflate.c ${D}/zlib-1.2.11/inftrees.c ${D}/zlib-1.2.11/zutil.c
  ${S}/libretro/libretro.c)
add_library(gpgx_libretro STATIC ${SOURCES})
set(INCLUDES)
foreach(dir ${CORE_DIRS})
  list(APPEND INCLUDES ${S}/${dir})
endforeach()
target_include_directories(gpgx_libretro PRIVATE ${D}/libchdr/include ${D}/lzma-19.00/include
  ${D}/zlib-1.2.11 ${D}/zstd/lib ${INCLUDES} ${S}/libretro ${L}/include)
target_compile_definitions(gpgx_libretro PRIVATE
  USE_LIBTREMOR USE_LIBCHDR _7ZIP_ST ZSTD_DISABLE_ASM ZSTD_TRACE=0
  USE_16BPP_RENDERING FRONTEND_SUPPORTS_RGB565 LSB_FIRST BYTE_ORDER=LITTLE_ENDIAN HAVE_ZLIB
  __LIBRETRO__ USE_LIBRETRO_VFS M68K_OVERCLOCK_SHIFT=20 Z80_OVERCLOCK_SHIFT=20 HAVE_YM3438_CORE
  HAVE_OPLL_CORE MAXROMSIZE=10485760 HAVE_NO_LANGEXTRA USE_DYNAMIC_ALLOC "INLINE=static inline")
target_compile_options(gpgx_libretro PRIVATE -w)
CMAKE
if cmp -s "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"; then
  rm -f "$OUT/cmake/CMakeLists.txt.new"
else
  mv "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O3, the PS4's CPU,
# a section for each function and variable (the app's link drops the unused ones), no unwind tables
# (it is C: nothing unwinds through it; the toolchain's -funwind-tables made 0.24 MB of them) and the
# renamed API.
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_gpgx.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -fPIC"
OPTIONS=(-DGPGX_DIR="$SRC" "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE")
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  }
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
WANT="$SRC | $(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Genesis Plus GX: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$OUT/cmake" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Genesis Plus GX: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" gpgx_libretro >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "Genesis Plus GX: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) stays global. Replaced only when it changed (the app
# isn't linked again for nothing).
ld.lld-21 -r --whole-archive "$BUILD/libgpgx_libretro.a" -o "$OUT/sega_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='gpgx_retro_*' "$OUT/sega_core_all.o" "$OUT/sega_core.o.new"
rm -f "$OUT/sega_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/sega_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$(for f in $API; do echo "gpgx_$f"; done | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "Genesis Plus GX: the object's global symbols aren't exactly the renamed libretro API:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/sega_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved (zstd's tracing hooks were): the app would export them as
# imports nobody gives.
WEAK=$(llvm-nm-21 -u "$OUT/sega_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "Genesis Plus GX: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)'; then
  echo "Genesis Plus GX: the object uses C++ operator new/delete (they must come from the app's heap)"
  exit 1
fi
# No static array of 1 MiB or more: the PS4 charges the app's whole static memory from its start, used
# or not (the cartridge's memory is the heap's while the core runs).
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/sega_core.o.new" | awk '($3 == "b" || $3 == "B") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "Genesis Plus GX: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a"
  PROVIDED=$(for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done |
    awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVi]$/ {print $NF}' | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "Genesis Plus GX: the object needs symbols the PS4 libraries don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "Genesis Plus GX: $(echo "$UNDEFINED" | wc -l) C library symbols needed, all in the PS4 libraries"
fi
if cmp -s "$OUT/sega_core.o.new" "$OUT/sega_core.o"; then
  rm -f "$OUT/sega_core.o.new"
else
  mv "$OUT/sega_core.o.new" "$OUT/sega_core.o"
fi
ls -la "$OUT/sega_core.o"
