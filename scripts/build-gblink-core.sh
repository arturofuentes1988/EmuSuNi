#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds SameBoy (libretro/SameBoy, ~/src/SameBoy; MIT) for the PS4 into one object that scripts/build.sh links into
# EmuSuNi: the Game Boy link cable online (00.95). Two Game Boys or Game Boy Colors run in it, joined by the cable
# (its "2 Player Game Boy Link" subsystem, retro_load_game_special), and each PS4 of a match runs both: player 1 the
# host's Game Boy, player 2 the guest's (src/GbLink.h). Alone, the Game Boys stay mGBA's (scripts/build-gba-core.sh).
# Its boot ROMs are SameBoy's own (MIT, BootROMs/prebuilt: free replacements, not Nintendo's), built into the object;
# no BIOS of the player's is needed. RGB XRGB8888 pictures (the core's only format), no threads.
# It has no CMake of its own: the one made here lists what libretro/Makefile.common compiles (the core, its boot ROMs
# as C arrays - the Makefile's hexdump rule, done here by Python - and the libretro-common files of a non-static
# build) with the Makefile's defines, compiled with the PS4 toolchain (toolchain/ps4-love-modern.cmake).
# Not as its unix build: -O3 for the PS4's CPU (btver2), a section for each function and variable (the app's link
# drops the unused ones), no core options' translations (HAVE_NO_LANGEXTRA: the app sets the options itself).
# Changes to its sources (applied each time to the file as git has it, checked):
#   - The second Game Boy's LCD status callback is set on gameboy[1], not on gameboy[2]: the array has two, so the
#     upstream line wrote past its end (into whatever follows it in memory) at every load of two games.
#   - Its sound at 48 kHz, the PS4's rate (as upstream's Wii U build), not 384 kHz.
#   - Its state saved from the game's load on (upstream: only once a frame ran), as an online match starts from it.
#   - The link cable's bits on their way (libretro.c's bit_to_send1/2) in the state too (its last 16 bytes): without
#     them a rollback in the middle of a byte on the cable made the two PS4s drift apart.
#   - A state loaded keeps the PPU's object line address (upstream's sanitizing zeroed it: vram_size & ~1 as the mask),
#     so a frame run again after a rollback ends with the same state as the first time.
#   - Its own messages (GB_log) not made at all without a log callback (its libretro port sets none: they went to
#     printf, thousands a frame while both Game Boys clock the cable).
#   - Always its own boot ROMs: a dmg_boot.bin or cgb_boot.bin in the system folder isn't read (the two PS4s of a match
#     start the same Game Boys).
# Added (emusuni_gblink.c, made here): each Game Boy's battery save as mGBA keeps it - its battery RAM, then for a
# cartridge with a clock (MBC3: Pokemon Gold, Silver, Crystal) its 48 bytes in VBA-M's/BGB's format, which is mGBA's
# GBMBCRTCSaveBuffer (src/GbLink.cpp keeps that clock up to date) - read and written with SameBoy's own functions:
#   sameboy_emusuni_battery_size(gb), sameboy_emusuni_ram_size(gb), sameboy_emusuni_save_battery(gb, out, size),
#   sameboy_emusuni_load_battery(gb, data, size)        (gb: 0 the first Game Boy, 1 the second)
# Its libretro API becomes sameboy_retro_* and everything else in it private, as for FCEUmm (CMakeLists.txt); the
# additions stay global as they are named. The object is checked at the end: only those defined globally, no weak
# references left, no C++, no static array of 1 MiB or more, and every symbol it needs found in the PS4 libraries the
# app links (OpenOrbis' libc, libkernel, libSceLibcInternal, clang's builtins).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-gblink-core.sh <out dir> [clean]
#   GBLINK_CORE_HOST=1 build-gblink-core.sh <out dir>   the same object for this PC (Linux, clang 21), for the PC
#                                                       tests (local/herramientas/prueba-gblink): no PS4 checks
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SB_SRC="${SAMEBOY_DIR:-$HOME/src/SameBoy}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/gblink-core.log"
HOST="${GBLINK_CORE_HOST:-0}"

python3 -I - "$SB_SRC" <<'PATCH'
import subprocess
import sys
root = sys.argv[1]
patched = {}
def patch(path, old, new):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    if path not in patched:
        patched[path] = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                                       capture_output=True).stdout
    text = patched[path]
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[path] = text.replace(old, new)

# The second Game Boy's LCD status callback on gameboy[1] (upstream: gameboy[2], past the array's end).
patch('libretro/libretro.c',
      b'GB_set_lcd_status_callback(&gameboy[2], lcd_status_change_2);',
      b'GB_set_lcd_status_callback(&gameboy[1], lcd_status_change_2); /* EmuSuNi: [1], not [2] (past the array) */')
# The core's own messages (GB_log) made only for a log callback (its libretro port sets none: they went to printf, each
# made with vasprintf and freed - thousands a frame while a game waits on the cable as both Game Boys' clock, "Serial
# read request while using internal clock").
patch('Core/gb.c',
      b'    char *string = NULL;\n    vasprintf(&string, fmt, args);\n',
      b'    char *string = NULL;\n'
      b'    if (!gb->log_callback) return; /* EmuSuNi: no log callback, no message (not printf\'s) */\n'
      b'    vasprintf(&string, fmt, args);\n')
# Its state saved from the game's load on, not only after the first frame (upstream refuses before it): an online match
# starts from the host's state at frame 0, saved before any frame runs (src/Netplay.h).
patch('libretro/libretro.c',
      b'bool retro_serialize(void *data, size_t size)\n{\n\n    if (!initialized || !data) {\n',
      b'bool retro_serialize(void *data, size_t size)\n{\n\n'
      b'    if (!data) { /* EmuSuNi: from the load on, not only once a frame ran (online: frame 0\'s state) */\n')
# The link cable's bits on their way (bit_to_send1/2: libretro.c's, outside both Game Boys' states) in the state too, in
# 16 bytes at its end: a state saved between a bit's start and its end, loaded again (a rollback, a resync), went on with
# the bit of another moment and the two PS4s drifted apart. A state of the size before (without them) still loads.
patch('libretro/libretro.c',
      b'    if (maximum_save_size) {\n        return maximum_save_size * 2;\n    }\n',
      b'    if (maximum_save_size) {\n        return maximum_save_size * 2 + 16; /* EmuSuNi: + the cable\'s bits */\n    }\n')
patch('libretro/libretro.c',
      b'    GB_free(&temp);\n\n    return maximum_save_size * 2;\n}\n',
      b'    GB_free(&temp);\n\n    return maximum_save_size * 2 + 16; /* EmuSuNi: + the cable\'s bits */\n}\n')
patch('libretro/libretro.c',
      b'        return false;\n    }\n\n    size_t offset = 0;\n',
      b'        return false;\n    }\n\n'
      b'    /* EmuSuNi: the link cable\'s bits on their way, in the state\'s last 16 bytes */\n'
      b'    if (size == retro_serialize_size()) {\n'
      b'        uint8_t *tail = (uint8_t *)data + size - 16;\n'
      b'        memset(tail, 0, 16);\n'
      b'        tail[0] = bit_to_send1;\n'
      b'        tail[1] = bit_to_send2;\n'
      b'        size -= 16;\n'
      b'    }\n\n'
      b'    size_t offset = 0;\n')
patch('libretro/libretro.c',
      b'bool retro_unserialize(const void *data, size_t size)\n{\n',
      b'bool retro_unserialize(const void *data, size_t size)\n{\n'
      b'    /* EmuSuNi: the link cable\'s bits on their way, from the state\'s last 16 bytes (a state without them: as\n'
      b'       they are) */\n'
      b'    if (data && size == retro_serialize_size()) {\n'
      b'        const uint8_t *tail = (const uint8_t *)data + size - 16;\n'
      b'        bit_to_send1 = tail[0] != 0;\n'
      b'        bit_to_send2 = tail[1] != 0;\n'
      b'        size -= 16;\n'
      b'    }\n')
# A state loaded keeps the PPU's object line address as it was saved: upstream's sanitizing masked it with
# vram_size & ~1 (one bit: 0x2000 or 0x4000), which zeroed it at every load - a state loaded wasn't the one saved, and
# the online match's checksums of a frame run again after a rollback differed from the other console's.
patch('Core/save_state.c',
      b'    gb->object_low_line_address &= gb->vram_size & ~1;\n',
      b'    gb->object_low_line_address &= (gb->vram_size - 1) & ~1; /* EmuSuNi: within VRAM, not one bit of it */\n')
# Always its own boot ROMs, never a file of the system folder (dmg_boot.bin, cgb_boot.bin...): both PS4s of a match
# must start the same Game Boys (the boot ROM runs after the state they start from is shared).
patch('libretro/libretro.c',
      b'    if (path_is_valid(boot_rom_path)) {\n',
      b'    if (0 && path_is_valid(boot_rom_path)) { /* EmuSuNi: always the built-in boot ROMs */\n')
# The sound at 48 kHz, the PS4's own rate (the Wii U's choice upstream), not 384 kHz: 800 stereo samples a frame for
# the app to play, not 6400 to bring down to 48 kHz.
patch('libretro/libretro.c',
      b'#ifndef WIIU\n#define AUDIO_FREQUENCY 384000\n',
      b'#ifndef WIIU\n#define AUDIO_FREQUENCY 48000 /* EmuSuNi: the PS4\'s rate, not 384000 */\n')

written = 0
for path, text in patched.items():
    full = root + '/' + path
    with open(full, 'rb') as f:
        same = f.read() == text
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
print("SameBoy: patches applied (%d files changed)" % written)
PATCH

# The boot ROMs as C arrays (libretro/Makefile's rule: the prebuilt binaries, "const unsigned char X[]" and its length).
python3 -I - "$SB_SRC" "$OUT" <<'BOOT'
import os
import sys
root, out = sys.argv[1], sys.argv[2]
for name in ('dmg_boot', 'cgb_boot', 'agb_boot', 'sgb_boot', 'sgb2_boot'):
    data = open(os.path.join(root, 'BootROMs', 'prebuilt', name + '.bin'), 'rb').read()
    text = ('/* Generated by scripts/build-gblink-core.sh from SameBoy\'s BootROMs/prebuilt (MIT) */\n'
            'const unsigned char %s[] = {%s};\n'
            'const unsigned %s_length = sizeof(%s);\n') % (name, ', '.join('0x%02x' % b for b in data), name, name)
    path = os.path.join(out, name + '.c')
    old = open(path).read() if os.path.exists(path) else None
    if old != text:
        with open(path, 'w') as f:
            f.write(text)
BOOT

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
EXTRA_API="sameboy_emusuni_battery_size sameboy_emusuni_ram_size sameboy_emusuni_save_battery
  sameboy_emusuni_load_battery"
{
  echo "// Generated by scripts/build-gblink-core.sh: the SameBoy core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f sameboy_$f"; done
} > "$OUT/prefix_sameboy.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_sameboy.h.new" "$OUT/prefix_sameboy.h"; then
  rm -f "$OUT/prefix_sameboy.h.new"
else
  mv "$OUT/prefix_sameboy.h.new" "$OUT/prefix_sameboy.h"
fi

# EmuSuNi's additions: each Game Boy's battery save (its RAM, then its clock's 48 bytes) through SameBoy's own
# functions. gameboy[] is libretro.c's (a global there; made local in the object with the rest).
cat > "$OUT/emusuni_gblink.c.new" <<'C'
/* Generated by scripts/build-gblink-core.sh: EmuSuNi's additions to SameBoy (GPL-2.0-or-later). */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "Core/gb.h"

extern GB_gameboy_t gameboy[2];

/* The battery save's size of Game Boy `gb` (0 or 1): its battery RAM, and for a cartridge with a clock its 48 bytes
   (VBA-M's/BGB's format: mGBA's GBMBCRTCSaveBuffer). 0: no game in it, or nothing kept by its battery. */
size_t sameboy_emusuni_battery_size(unsigned gb)
{
    if (gb > 1 || !GB_is_inited(&gameboy[gb]) || !gameboy[gb].rom) return 0;
    const int size = GB_save_battery_size(&gameboy[gb]);
    return size > 0 ? (size_t)size : 0;
}

/* Its battery RAM's size alone (the save's first bytes; the rest is the clock). */
size_t sameboy_emusuni_ram_size(unsigned gb)
{
    if (gb > 1 || !GB_is_inited(&gameboy[gb]) || !gameboy[gb].rom || !gameboy[gb].cartridge_type->has_battery) return 0;
    return gameboy[gb].mbc_ram_size;
}

/* Its battery save into `out` (`size` at least sameboy_emusuni_battery_size). The clock's timestamp is the PC's time
   now (SameBoy's), as mGBA writes it. */
bool sameboy_emusuni_save_battery(unsigned gb, uint8_t *out, size_t size)
{
    const size_t want = sameboy_emusuni_battery_size(gb);
    if (!want || size < want) return false;
    return GB_save_battery_to_buffer(&gameboy[gb], out, want) == 0;
}

/* A battery save in Game Boy `gb`, before its first frame: its RAM, and its clock if the 48 bytes follow (a clock
   left out, or one whose time is later than now, starts from zero: SameBoy's rules). */
void sameboy_emusuni_load_battery(unsigned gb, const uint8_t *data, size_t size)
{
    if (gb > 1 || !GB_is_inited(&gameboy[gb]) || !gameboy[gb].rom || !data || !size) return;
    GB_load_battery_from_buffer(&gameboy[gb], data, size);
}

#ifdef EMUSUNI_GBLINK_DEBUG
/* The PC test's only (GBLINK_CORE_HOST=1): a byte of Game Boy `gb`'s memory read or written as its CPU would. */
uint8_t sameboy_emusuni_debug_read(unsigned gb, uint16_t addr)
{
    return gb > 1 ? 0xFF : GB_read_memory(&gameboy[gb], addr);
}
void sameboy_emusuni_debug_write(unsigned gb, uint16_t addr, uint8_t value)
{
    if (gb <= 1) GB_write_memory(&gameboy[gb], addr, value);
}
#endif
C
if cmp -s "$OUT/emusuni_gblink.c.new" "$OUT/emusuni_gblink.c"; then
  rm -f "$OUT/emusuni_gblink.c.new"
else
  mv "$OUT/emusuni_gblink.c.new" "$OUT/emusuni_gblink.c"
fi

# The core's CMake: what libretro/Makefile.common compiles (STATIC_LINKING off: its libretro-common files too).
mkdir -p "$OUT/cmake"
cat > "$OUT/cmake/CMakeLists.txt.new" <<'CMAKE'
# Generated by scripts/build-gblink-core.sh: SameBoy as a static library.
cmake_minimum_required(VERSION 3.20)
project(sameboy C)
set(S ${SB_DIR})
set(SL ${S}/libretro/libretro-common)
add_library(sameboy STATIC
  ${S}/Core/gb.c ${S}/Core/sgb.c ${S}/Core/apu.c ${S}/Core/memory.c ${S}/Core/mbc.c ${S}/Core/timing.c
  ${S}/Core/display.c ${S}/Core/symbol_hash.c ${S}/Core/camera.c ${S}/Core/sm83_cpu.c ${S}/Core/joypad.c
  ${S}/Core/save_state.c ${S}/Core/random.c ${S}/Core/rumble.c
  ${OUT_DIR}/agb_boot.c ${OUT_DIR}/cgb_boot.c ${OUT_DIR}/dmg_boot.c ${OUT_DIR}/sgb_boot.c ${OUT_DIR}/sgb2_boot.c
  ${S}/libretro/libretro.c ${OUT_DIR}/emusuni_gblink.c
  ${SL}/compat/compat_posix_string.c ${SL}/compat/compat_snprintf.c ${SL}/compat/compat_strcasestr.c
  ${SL}/compat/compat_strl.c ${SL}/compat/fopen_utf8.c ${SL}/encodings/encoding_utf.c ${SL}/file/file_path.c
  ${SL}/file/file_path_io.c ${SL}/streams/file_stream.c ${SL}/streams/file_stream_transforms.c
  ${SL}/string/stdstring.c ${SL}/time/rtime.c ${SL}/vfs/vfs_implementation.c)
target_include_directories(sameboy PRIVATE ${S} ${SL}/include)
target_compile_definitions(sameboy PRIVATE __LIBRETRO__ _GNU_SOURCE _USE_MATH_DEFINES GB_INTERNAL
  GB_DISABLE_TIMEKEEPING GB_DISABLE_REWIND GB_DISABLE_DEBUGGER GB_DISABLE_CHEATS HAVE_NO_LANGEXTRA
  GB_VERSION="${SB_VERSION}" GIT_VERSION="${SB_REVISION}")
target_compile_options(sameboy PRIVATE -std=gnu11 -w -include ${OUT_DIR}/prefix_sameboy.h)
CMAKE
if cmp -s "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"; then
  rm -f "$OUT/cmake/CMakeLists.txt.new"
else
  mv "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O3, the PS4's CPU, a section for each
# function and variable, no unwind tables (it is C: nothing unwinds through it).
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have; and the test's memory access.)
if [ "$HOST" = 1 ]; then
  FLAGS_CORE+=" -mno-sse4a -fPIC -DEMUSUNI_GBLINK_DEBUG"
  EXTRA_API+=" sameboy_emusuni_debug_read sameboy_emusuni_debug_write"
fi
SB_VERSION=$(sed -n 's/^VERSION := //p' "$SB_SRC/version.mk")
OPTIONS=(-DSB_DIR="$SB_SRC" -DOUT_DIR="$OUT" "-DSB_VERSION=$SB_VERSION"
  "-DSB_REVISION= $(git -C "$SB_SRC" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE")
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
WANT="$SB_SRC | $(git -C "$SB_SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "SameBoy: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$OUT/cmake" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "SameBoy: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" sameboy >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "SameBoy: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro API (renamed) and the additions stay global. Replaced only when it changed (the app
# isn't linked again for nothing).
ld.lld-21 -r --whole-archive "$BUILD/libsameboy.a" -o "$OUT/gblink_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='sameboy_retro_*' --keep-global-symbol='sameboy_emusuni_*' \
  "$OUT/gblink_core_all.o" "$OUT/gblink_core.o.new"
rm -f "$OUT/gblink_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/gblink_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$( { for f in $API; do echo "sameboy_$f"; done; for f in $EXTRA_API; do echo "$f"; done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "SameBoy: the object's global symbols aren't exactly the renamed libretro API and the additions:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/gblink_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
WEAK=$(llvm-nm-21 -u "$OUT/gblink_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u || true)
if [ -n "$WEAK" ]; then
  echo "SameBoy: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if echo "$UNDEFINED" | grep -E '^_Z'; then
  echo "SameBoy: the object needs C++ (it is C: nothing of it may come from the C++ library)"
  exit 1
fi
# No static array of 1 MiB or more: the PS4 charges the app's whole static memory from its start, used or not.
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/gblink_core.o.new" |
      awk '($3 == "b" || $3 == "B" || $3 == "d" || $3 == "D") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "SameBoy: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a"
  PROVIDED=$(for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}' | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "SameBoy: the object needs symbols the PS4 libraries don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "SameBoy: $(echo "$UNDEFINED" | sed '/^$/d' | wc -l) C library symbols needed, all in the PS4 libraries"
fi
if cmp -s "$OUT/gblink_core.o.new" "$OUT/gblink_core.o"; then
  rm -f "$OUT/gblink_core.o.new"
else
  mv "$OUT/gblink_core.o.new" "$OUT/gblink_core.o"
fi
llvm-size-21 "$OUT/gblink_core.o"
