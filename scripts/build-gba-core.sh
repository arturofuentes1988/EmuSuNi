#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds mGBA (libretro's fork, ~/src/mgba) for the PS4 into one object that scripts/build.sh
# links into EmuSuNi: the Game Boy, Game Boy Color and Game Boy Advance. mGBA's own CMake makes
# its libretro core as a static library (BUILD_LIBRETRO, LIBRETRO_STATIC) with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake); nothing optional (no frontends, OpenGL, debuggers, scripting,
# zip/7z/PNG/SQLite/FFmpeg... the app reads the games, ZIPs included, and hands the core the
# bytes). RGB565 pictures (COLOR_16_BIT + COLOR_5_6_5, the core's libretro default), no threads
# (its libretro build is DISABLE_THREADING). Its libretro API becomes mgba_retro_* and everything
# else in it private, as for FCEUmm (CMakeLists.txt).
# What the PS4 needs from it:
#   - CMake sees the PS4 as a "Generic" system: no POSIX layer, so files go through stdio
#     (vfs-file.c, the console builds' choice) and memory through calloc (util/memory.c), which
#     the app sends to its heap (dlmalloc). mGBA's checks for C library functions really link
#     (against the OpenOrbis C library), so they find what the PS4 has: newlocale/uselocale and
#     strtof_l (musl's, in libc.a), localtime_r (the GBA's RTC; musl: UTC unless TZ is set).
#   - The optimisation the core pins for its libretro build (-O3), for the PS4's CPU (btver2).
# (01.04) The Game Boy Advance link cable online: a second libretro API in the same object,
# gbalink_retro_* (scripts/gbalink/emusuni_gbalink.c: two GBAs joined by mGBA's cable; src/GbLink.h),
# with each GBA's battery save through gbalink_emusuni_battery_size, _ram_size, _save_battery and
# _load_battery. The file is copied into mGBA's libretro folder, whose files its CMake compiles with
# the libretro core's own defines. And four changes to mGBA's sources, so that the cable goes on the
# same after a state is loaded (a rollback, the guest's start: found with
# local/herramientas/prueba-gbalink; upstream has them too), applied each time to the file as git has
# it, checked:
#   - The lockstep driver's state load ends the event queue it rebuilds (its last event pointed into
#     the free list: events that never were, or a loop without end with more GBAs).
#   - The state holds the transfer's end time only when that event is scheduled (0x7FFFFFFF if not):
#     the old time stayed after a load and, under 0x20000, scheduled a transfer's end that never was.
#   - While a state loads (GBASIOLockstepLoading, the link core's), a GBA's mode change - its RCNT
#     written again - reaches the lockstep driver without MODE_SET events to the GBAs loaded before
#     it (the order of the loads changed the cable).
#   - SIOMLT_SEND/SIODATA8 (0x12A: what the GBA sends next) is a register of the state: it wasn't
#     (after a load the value of before was sent). (Old states load: that field is 0 in them.)
# The object is checked at the end: only mgba_retro_*, gbalink_retro_* and gbalink_emusuni_* defined
# globally, no C++ operator new or delete (EmuSuNi's go to dlmalloc, port/ps4_new.cpp), and every
# symbol it needs found in the PS4 libraries the app links (OpenOrbis' libc, libkernel,
# libSceLibcInternal, clang's builtins).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-gba-core.sh <out dir> [clean]
#   GBA_CORE_HOST=1 build-gba-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                 the PC tests (local/herramientas/prueba-gbalink):
#                                                 no PS4 checks; gbalink_emusuni_debug_read/_write
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${MGBA_DIR:-$HOME/src/mgba}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/gba-core.log"
HOST="${GBA_CORE_HOST:-0}"
if [ "$HOST" != 1 ]; then
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
fi

# (01.04) mGBA's changes for the link cable (see above), and the link core's file in its libretro folder.
python3 -I - "$SRC" "$HERE/gbalink/emusuni_gbalink.c" <<'PATCH'
import subprocess
import sys
root, link_core = sys.argv[1], sys.argv[2]
patched = {}
def original(path):
    if path not in patched:
        patched[path] = subprocess.run(['git', '-C', root, 'show', 'HEAD:' + path], check=True,
                                       capture_output=True).stdout
    return patched[path]
def patch(path, old, new):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    text = original(path)
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[path] = text.replace(old, new)

# The lockstep driver's state load: the event queue it rebuilds ended, its free list's last event too.
patch('src/gba/sio/lockstep.c',
      b'\tplayer->freeList = &player->buffer[0];\n\tplayer->queue = NULL;\n',
      b'\tplayer->buffer[MAX_LOCKSTEP_EVENTS - 1].next = NULL; /* EmuSuNi: the free list ended */\n'
      b'\tplayer->freeList = &player->buffer[0];\n\tplayer->queue = NULL;\n')
patch('src/gba/sio/lockstep.c',
      b'\t\t\tLOAD_32LE(event->finishCycle, 0, &stateEvent->finishCycle);\n\t\t\tbreak;\n\t\t}\n\t}\n',
      b'\t\t\tLOAD_32LE(event->finishCycle, 0, &stateEvent->finishCycle);\n\t\t\tbreak;\n\t\t}\n\t}\n'
      b'\t*lastEvent = NULL; /* EmuSuNi: the queue ended (it pointed into the free list) */\n')
# While a state loads, a mode change reaches the driver alone (no MODE_SET to the GBAs loaded before).
patch('src/gba/sio/lockstep.c',
      b'static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {\n'
      b'\tstruct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;\n'
      b'\tstruct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;\n'
      b'\tMutexLock(&coordinator->mutex);\n'
      b'\tstruct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);\n',
      b'/* EmuSuNi: true while the link core loads its GBAs\' states (scripts/gbalink/emusuni_gbalink.c) */\n'
      b'bool GBASIOLockstepLoading = false;\n\n'
      b'static void GBASIOLockstepDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {\n'
      b'\tstruct GBASIOLockstepDriver* lockstep = (struct GBASIOLockstepDriver*) driver;\n'
      b'\tstruct GBASIOLockstepCoordinator* coordinator = lockstep->coordinator;\n'
      b'\tMutexLock(&coordinator->mutex);\n'
      b'\tstruct GBASIOLockstepPlayer* player = TableLookup(&coordinator->players, lockstep->lockstepId);\n'
      b'\tif (GBASIOLockstepLoading) { /* EmuSuNi: the driver\'s own state comes next and has it all */\n'
      b'\t\tplayer->mode = mode;\n\t\tMutexUnlock(&coordinator->mutex);\n\t\treturn;\n\t}\n')
# The transfer's end time in the state only when that event is scheduled.
patch('src/gba/cart/gpio.c',
      b'\tSTORE_32(hw->p->sio.completeEvent.when - mTimingCurrentTime(&hw->p->timing), 0, &state->hw.sioNextEvent);\n',
      b'\tif (mTimingIsScheduled(&hw->p->timing, &hw->p->sio.completeEvent)) {\n'
      b'\t\tSTORE_32(hw->p->sio.completeEvent.when - mTimingCurrentTime(&hw->p->timing), 0, &state->hw.sioNextEvent);\n'
      b'\t} else { /* EmuSuNi: not scheduled - not an old time that a load would schedule */\n'
      b'\t\tSTORE_32(0x7FFFFFFF, 0, &state->hw.sioNextEvent);\n'
      b'\t}\n')
# SIOMLT_SEND/SIODATA8 (0x12A) a register of the state: valid (saved as read) and loaded as it is.
text = original('src/gba/io.c')
rows = [i for i, line in enumerate(text.split(b'\n')) if line.strip() == b'/*    SIO */']
assert len(rows) == 3, rows
lines = text.split(b'\n')
for row in (rows[0] + 1, rows[2] + 1):  # _isValidRegister's and _isWSpecialRegister's
    assert lines[row].strip() == b'/* 12 */ 1, 1, 1, 1, 1, 0, 0, 0,', lines[row]
    lines[row] = lines[row].replace(b'1, 1, 1, 1, 1, 0, 0, 0,', b'1, 1, 1, 1, 1, 1, 0, 0, /* EmuSuNi: 0x12A */')
patched['src/gba/io.c'] = b'\n'.join(lines)

patched['src/platform/libretro/emusuni_gbalink.c'] = open(link_core, 'rb').read()
written = 0
for path, text in patched.items():
    full = root + '/' + path
    try:
        with open(full, 'rb') as f:
            same = f.read() == text
    except FileNotFoundError:
        same = False
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
print("mGBA: the link cable's changes applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
LINK_EXTRA="gbalink_emusuni_battery_size gbalink_emusuni_ram_size gbalink_emusuni_save_battery
  gbalink_emusuni_load_battery"
{
  echo "// Generated by scripts/build-gba-core.sh: the mgba core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f mgba_$f"; done
} > "$OUT/prefix_mgba.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_mgba.h.new" "$OUT/prefix_mgba.h"; then
  rm -f "$OUT/prefix_mgba.h.new"
else
  mv "$OUT/prefix_mgba.h.new" "$OUT/prefix_mgba.h"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): the core's -O3,
# the PS4's CPU, the core options' translations left out (HAVE_NO_LANGEXTRA: 84 tables of the
# options in other languages, 1.8 MB of the program image; the app doesn't ask for a language and
# sets the options itself), a section for each function and variable (the app's link drops the
# unused ones), no unwind tables (it is C: nothing unwinds through it) and the renamed API.
CFLAGS_CORE="-O3 -DNDEBUG -march=btver2 -DHAVE_NO_LANGEXTRA -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_mgba.h"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have; and the test's memory access.)
if [ "$HOST" = 1 ]; then
  CFLAGS_CORE+=" -mno-sse4a -fPIC -DEMUSUNI_GBALINK_DEBUG"
  LINK_EXTRA+=" gbalink_emusuni_debug_read gbalink_emusuni_debug_write"
fi
OPTIONS=(
  -DBUILD_LIBRETRO=ON -DLIBRETRO_STATIC=ON -DSKIP_LIBRARY=ON -DM_CORE_GB=ON -DM_CORE_GBA=ON
  -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF
  -DBUILD_SHARED=OFF -DBUILD_STATIC=OFF -DBUILD_LTO=OFF -DBUILD_PERF=OFF -DBUILD_TEST=OFF
  -DBUILD_SUITE=OFF -DBUILD_CINEMA=OFF -DBUILD_HEADLESS=OFF -DBUILD_EXAMPLE=OFF -DBUILD_PYTHON=OFF
  -DBUILD_DOCGEN=OFF -DBUILD_MAINTAINER_TOOLS=OFF
  -DENABLE_DEBUGGERS=OFF -DENABLE_GDB_STUB=OFF -DUSE_EDITLINE=OFF -DENABLE_SCRIPTING=OFF
  -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_FREETYPE=OFF -DUSE_DISCORD_RPC=OFF -DDISABLE_DEPS=ON
  -DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_MINIZIP=OFF -DUSE_LIBZIP=OFF -DUSE_LZMA=OFF -DUSE_PNG=OFF
  -DUSE_SQLITE3=OFF -DUSE_ELF=OFF
  "-DCMAKE_C_FLAGS_RELEASE=$CFLAGS_CORE"
)
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 "$@"
  }
else
  configure() {
    ps4_cmake -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" "$@"
  }
fi
STAMP="$OUT/.emusuni-flags"
# (The link core's file in the libretro folder: mGBA's CMake lists that folder's files when it configures.)
WANT="$SRC | $(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown) | host $HOST | gbalink | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "mGBA: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$SRC" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "mGBA: configuring failed (see $LOG)"
    exit 1
  fi
fi
if ! ninja -C "$BUILD" mgba_libretro >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "mGBA: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

# One object: only the libretro APIs (renamed; the link core's) and the link core's saves stay global. Replaced only
# when it changed (the app isn't linked again for nothing).
ld.lld-21 -r --whole-archive "$BUILD/mgba_libretro.a" -o "$OUT/gba_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='mgba_retro_*' --keep-global-symbol='gbalink_retro_*' \
  --keep-global-symbol='gbalink_emusuni_*' "$OUT/gba_core_all.o" "$OUT/gba_core.o.new"
rm -f "$OUT/gba_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/gba_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$( { for f in $API; do echo "mgba_$f"; echo "gbalink_$f"; done; for f in $LINK_EXTRA; do echo "$f"; done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "mGBA: the object's global symbols aren't exactly the renamed libretro APIs and the link core's saves:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/gba_core.o.new" | awk '{print $2}' | sort -u)
if echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)'; then
  echo "mGBA: the object uses C++ operator new/delete (they must come from the app's heap)"
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
    echo "mGBA: the object needs symbols the PS4 libraries don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "mGBA: $(echo "$UNDEFINED" | wc -l) C library symbols needed, all in the PS4 libraries"
fi
if cmp -s "$OUT/gba_core.o.new" "$OUT/gba_core.o"; then
  rm -f "$OUT/gba_core.o.new"
else
  mv "$OUT/gba_core.o.new" "$OUT/gba_core.o"
fi
ls -la "$OUT/gba_core.o"
