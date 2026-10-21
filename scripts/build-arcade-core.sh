#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds FinalBurn Neo (libretro/FBNeo, ~/src/FBNeo: its own non-commercial licence, see
# assets/licenses/fbneo-license.txt) for the PS4 into one object that scripts/build.sh links into
# EmuSuNi: the Arcade (every arcade board it emulates, the Neo Geo, CPS-1/2/3, Cave, PGM, Toaplan,
# Konami...). Its own Makefile (src/burner/libretro/Makefile, platform=unix: 7z and CHD support, its
# zlib, no dynarec) is run with the PS4 compiler and flags of toolchain/ps4-love-modern.cmake in a copy of
# the sources (the Makefile writes the objects next to them), with:
#   - -DGBAPPU_WORKER_DISABLE: the Game Boy Advance console driver's sound worker is a pthread that
#     doesn't compile for the PS4 (and the app runs no thread of the core's). (Moot while that driver is
#     left out, below.)
#   - -O3, for the PS4's CPU (btver2), as its own unix build.
#   - A section for each function and variable (-ffunction-sections -fdata-sections), so the app's link
#     (--gc-sections) drops what nothing uses.
#   - Unwind tables only for the functions that can throw (-fno-unwind-tables -fno-asynchronous-unwind-tables,
#     not the toolchain's -funwind-tables: a frame for every function, 1.9 MB of .eh_frame here): C++
#     exceptions still work (mpeg_audio.cpp's try/catch); the app's crash log reads the stack itself.
# The console drivers are left out (the Mega Drive, PC Engine, SG-1000, Master System / Game Gear,
# ColecoVision, MSX, ZX Spectrum, NES / Famicom, Super Nintendo, Game Boy Advance, Channel F, Neo Geo
# Pocket, and the Astrocade's home games: about 20,900 of its 29,500 sets, the ones the app never lists, see
# src/Arcade.cpp's CONSOLE_HARDWARE; what stays is what its "Arcade only" DAT lists). Their sources aren't
# compiled and the driver list (dep/generated/driverlist.h: the declaration of each driver, the array of
# them, and the table of their source files, which only a DAT generator reads) lacks them: 28 MB of the
# program image (the core's share went from 67 to 39 MB), most of it the 24-byte relocation each pointer
# in a driver's data costs.
# Changes to its sources (applied to the copy, from the files as git has them at the commit setup.sh
# pins, checked):
#   - A game that can't start fails: retro_load_game returned true even when the romset was unknown or
#     had missing files, and drew its own error screen (uGUI) at the next frames. The load returns false
#     now, and the screen's text is kept for the app (fbneo_emusuni_error), which shows its own message;
#     the screen itself isn't made (its frame buffer was never freed). The list of missing ROMs that text
#     holds starts afresh at every load: it was a static that grew with every refused game (RetroArch
#     unloads the core between games and never saw it; the second message listed the first game's ROMs
#     too, and outgrew its 2 KB buffer). A game refused before its driver started (files missing) isn't
#     exited as one that ran: the Exit of some drivers (the Mega Drive boards') crashes when their Init
#     never ran, and RetroArch unloaded the core right after.
#   - fbneo_emusuni_list: every driver's short name (the zip's), full name, parent, BIOS set, year,
#     maker, board, flags and hardware code, for the app's Library (a game is a .zip named after a
#     driver). Callable while no game runs.
#   - The TMS34010's memory maps (64 MB: Midway's Mortal Kombat, NBA Jam...), the E1-32XS's page
#     tables (16 MB) and the Philips XA's and the TLCS-900's (1 MB each, 2 for the XA: no game of the
#     list uses either CPU, but the program kept them) are allocated when such a CPU starts and given
#     back at its exit, not static arrays: 83 MB less of the app's static memory, all of which the PS4
#     charges from the app's start.
#   - The uGUI error screen's 4.5 MB Chinese font is left out (its 8x8 one instead): the screen is never
#     shown in the app.
#   - Its driver list can be started and closed again: BurnLibInit copies every driver's names and points
#     the drivers at the copies, BurnLibExit freed them without putting the originals back or forgetting
#     the freed arrays, and the next BurnLibInit freed them again (RetroArch unloads the core between
#     games and never saw it; the app starts and closes the core for every game, and lists the drivers).
#     The wide-character copies of the names, 30 MB of the 41 the copies hold while a game runs, aren't
#     made: nothing reads them without _UNICODE.
# Its libretro API becomes fbneo_retro_* and everything else in it private, as for FCEUmm
# (CMakeLists.txt); the two helpers stay global too. The object is checked at the end: only those
# defined globally, no weak references left, no C++ operator new or delete of its own (the app's heap
# gives them, port/ps4_new.cpp), and every symbol it needs found in the PS4 libraries the app links
# (OpenOrbis' libc, libkernel, libSceLibcInternal, libc++ 21, clang's builtins).
# Only what changed is compiled again (make, with its own dependency files; a build cut short goes on
# from where it was; a changed or removed patch recompiles its file); everything when the flags change.
#   build-arcade-core.sh <out dir> [clean]
#   FBNEO_CORE_HOST=1 build-arcade-core.sh <out dir>   the same object for this PC (Linux, clang 21), for
#                                                      the PC tests (local/herramientas/prueba-arcade):
#                                                      no PS4 checks
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
SRC="${FBNEO_DIR:-$HOME/src/FBNeo}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/arcade-core.log"
HOST="${FBNEO_CORE_HOST:-0}"
JOBS="${EMUSUNI_JOBS:-4}"

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
# The helpers the app calls besides the API (patched in below).
HELPERS="fbneo_emusuni_list fbneo_emusuni_error"
{
  echo "// Generated by scripts/build-arcade-core.sh: the FinalBurn Neo core's libretro API, renamed."
  echo "#pragma once"
  for f in $API; do echo "#define $f fbneo_$f"; done
} > "$OUT/prefix_fbneo.h.new"
# Rewritten only when different: every source depends on it.
if cmp -s "$OUT/prefix_fbneo.h.new" "$OUT/prefix_fbneo.h"; then
  rm -f "$OUT/prefix_fbneo.h.new"
else
  mv "$OUT/prefix_fbneo.h.new" "$OUT/prefix_fbneo.h"
fi

# The compilers as the Makefile runs them ($(CC) / $(CXX)): the PS4 target and headers as
# toolchain/ps4-love-modern.cmake (libc++ 21 ahead of OpenOrbis' C headers), the PS4's CPU, a section for
# each function and variable, the renamed API, no worker thread in the GBA driver, no warnings in the log.
if [ "$HOST" = 1 ]; then
  # (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
  TARGET_FLAGS="-fPIC -march=btver2 -mno-sse4a"
  CC_CMD="clang-21 $TARGET_FLAGS"
  CXX_CMD="clang++-21 $TARGET_FLAGS"
else
  # shellcheck disable=SC1091
  source "$HERE/ps4-env.sh"
  TARGET_FLAGS="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  TARGET_FLAGS+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS"
  TARGET_FLAGS+=" -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay -isystem $OPENORBIS/include -I$OPENORBIS/usr/include"
  TARGET_FLAGS+=" -march=btver2"
  CC_CMD="clang-21 $TARGET_FLAGS"
  CXX_CMD="clang++-21 -nostdinc++ -isystem $PS4_BUILD_ROOT/sysroot/include/c++/v1 $TARGET_FLAGS"
fi
COMMON_FLAGS="-w -DGBAPPU_WORKER_DISABLE -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -include $OUT/prefix_fbneo.h"
CC_CMD+=" $COMMON_FLAGS"
CXX_CMD+=" $COMMON_FLAGS"
COMMIT="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_SHORT="$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DATE="$(git -C "$SRC" log -1 --format=%ad --date=format:%y%m%d 2>/dev/null || echo 000000)"

# The console drivers left out of the copy (see the top): their directories under burn/drv, and single files
# under burn/ (the Neo Geo Pocket's driver, the Astrocade's, and the PC Engine's video chip, which nothing
# else uses). A directory or file that also has arcade games keeps its sources (the NES driver's VS.
# UniSystem games, the Mega Drive's Mega-Play games, the Astrocade's arcade games): only its console
# drivers leave the list.
export FBNEO_CONSOLE_DIRS="channelf coleco gba megadrive msx nes pce sg1000 sms snes spectrum"
export FBNEO_CONSOLE_FILES="drv/pst90s/d_ngp.cpp drv/pre90s/d_astrocde.cpp devices/vdc.cpp"

STAMP="$OUT/.emusuni-flags"
# (The patches aren't in it: they are applied to the copy each time, and make recompiles a changed file.
# The console drivers left out are: the copy goes back to git's when they change.)
WANT="$SRC | $COMMIT | host $HOST | $CC_CMD | $CXX_CMD | without $FBNEO_CONSOLE_DIRS $FBNEO_CONSOLE_FILES"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -d "$BUILD/src/burner/libretro" ]; then
  echo "FinalBurn Neo: copying the sources and compiling everything (new flags, or clean)"
  # The sources as git has them at the commit setup.sh pins (the patches take their originals from git too):
  # a tree with changes of its own, or objects built in it, isn't what the pin says.
  if [ "$COMMIT" = unknown ] || [ -n "$(git -C "$SRC" status --porcelain -- src)" ]; then
    echo "FinalBurn Neo: $SRC is not setup.sh's clean git checkout (git status shows changes under src, or it is no checkout)" >&2
    exit 1
  fi
  rm -rf "$BUILD" "$STAMP"
  mkdir -p "$BUILD"
  # (The sources only: the Makefile writes its objects and dependency files next to them.)
  cp -a "$SRC/src" "$BUILD/src"
  # The copy is this script's and these flags' from here on: a build cut short (the PC rebooted) goes on
  # with make's own dependency files instead of copying everything again.
  printf '%s' "$WANT" > "$STAMP"
fi

python3 -I - "$SRC" "$BUILD" <<'PATCH'
import os
import re
import subprocess
import sys
checkout, build = sys.argv[1], sys.argv[2] + '/src'
patched = {}
def original(path):
    # The file as git has it (the commit setup.sh pins), not as the working tree has it: a changed patch
    # replaces the old one, and an edit made by hand there isn't taken for the original; as bytes: its CRLF
    # line ends and Latin-1 comments stay as they are.
    return subprocess.run(['git', '-C', checkout, 'show', 'HEAD:src/' + path], check=True,
                          capture_output=True).stdout
def current(path):
    if path not in patched:
        patched[path] = original(path)
    return patched[path]
def patch(path, old, new):
    text = current(path)
    assert text.count(old) == 1, (path, text.count(old), old[:70])
    patched[path] = text.replace(old, new)
def patch_re(path, pattern, new):
    # The same, at the one place a pattern matches (lines whose spacing isn't worth pinning down).
    text = current(path)
    patched[path], count = re.subn(pattern, new, text, flags=re.DOTALL)
    assert count == 1, (path, count, pattern[:70])

LR = 'burner/libretro/libretro.cpp'
# The error screen's text kept for the app, the screen itself not made (nothing runs after a failed load,
# and its frame buffer, made at each call and never freed, would leak with every refused game); a failed
# load returns false. The list of missing ROMs starts afresh at every load: it was a static that grew with
# every refused game (RetroArch unloads the core between games and never saw it; here the second message
# listed the first game's ROMs too, and the text outgrew its 2 KB buffer).
patch_re(LR,
         rb'static void SetUguiError\(const char\* error\)\n\{\n.*?\n\}\n',
         b'// EmuSuNi: the error screen\'s text, for the app (it shows its own message; the screen isn\'t made).\n'
         b'static char emusuni_error_text[4096];\n'
         b'static bool emusuni_driver_init_run; // EmuSuNi: BurnDrvInit was called, so the driver has something to exit\n'
         b'extern "C" const char* fbneo_emusuni_error(void) { return emusuni_error_text; }\n\n'
         b'static void SetUguiError(const char* error)\n{\n'
         b'\tstrncpy(emusuni_error_text, error, sizeof(emusuni_error_text) - 1); // EmuSuNi\n'
         b'}\n')
patch(LR,
      b'\tgui_show = false;\n\n#ifdef FBNEO_DEBUG\n\tfor (nBurnDrvActive=0',
      b'\tgui_show = false;\n\temusuni_error_text[0] = 0; // EmuSuNi\n\ttext_missing_files[0] = 0; // EmuSuNi\n'
      b'\temusuni_driver_init_run = false; // EmuSuNi\n\n'
      b'#ifdef FBNEO_DEBUG\n\tfor (nBurnDrvActive=0')
# A game refused before its driver started (BIOS, files missing) is not exited as one that ran: the Exit of some
# drivers (the Mega Drive boards') crashes when their Init never ran, and in RetroArch the core was unloaded after.
patch(LR,
      b'\t\t// Initialize game driver\n\t\tif(BurnDrvInit() == 0)\n',
      b'\t\t// Initialize game driver\n\t\temusuni_driver_init_run = true; // EmuSuNi\n\t\tif(BurnDrvInit() == 0)\n')
patch(LR,
      b'end:\n\tif (nBurnDrvActive != ~0U) {\n\t\tBurnDrvExit();\n\t\tnBurnDrvActive = ~0U;\n\t}\n',
      b'end:\n\tif (nBurnDrvActive != ~0U) {\n\t\tif (emusuni_driver_init_run) BurnDrvExit(); // EmuSuNi: not for a driver that never started\n'
      b'\t\tnBurnDrvActive = ~0U;\n\t}\n')
patch(LR,
      b'\tRomDataExit();\n\tIpsPatchExit();\n\n\treturn true;\n}\n',
      b'\tRomDataExit();\n\tIpsPatchExit();\n\n\treturn false; // EmuSuNi: a game that can\'t start fails (fbneo_emusuni_error says why)\n}\n')
# Every driver for the app's Library.
patched[LR] += (
    b'\n// EmuSuNi: every driver for the app\'s Library (the zip\'s name, the title, parent and BIOS sets, year, maker,\n'
    b'// board, flags, hardware code), while no game runs: the library is started for it when it isn\'t.\n'
    b'extern "C" unsigned fbneo_emusuni_list(void (*add)(void* user, const char* name, const char* title,\n'
    b'\tconst char* parent, const char* bios, const char* year, const char* maker, const char* system,\n'
    b'\tunsigned flags, unsigned hardware), void* user)\n'
    b'{\n'
    b'\tconst bool init = (nBurnDrvCount == 0);\n'
    b'\tif (init) BurnLibInit();\n'
    b'\tconst UINT32 old = nBurnDrvActive;\n'
    b'\tfor (UINT32 i = 0; i < nBurnDrvCount; i++) {\n'
    b'\t\tnBurnDrvActive = i;\n'
    b'\t\tadd(user, BurnDrvGetTextA(DRV_NAME), BurnDrvGetTextA(DRV_FULLNAME), BurnDrvGetTextA(DRV_PARENT),\n'
    b'\t\t\tBurnDrvGetTextA(DRV_BOARDROM), BurnDrvGetTextA(DRV_DATE), BurnDrvGetTextA(DRV_MANUFACTURER),\n'
    b'\t\t\tBurnDrvGetTextA(DRV_SYSTEM), (unsigned)BurnDrvGetFlags(), (unsigned)BurnDrvGetHardwareCode());\n'
    b'\t}\n'
    b'\tnBurnDrvActive = old;\n'
    b'\tconst unsigned count = nBurnDrvCount;\n'
    b'\tif (init) BurnLibExit();\n'
    b'\treturn count;\n'
    b'}\n')
# The TMS34010's memory maps on the heap while such a game runs (a 64 MB static array).
TMS = 'cpu/tms34_intf.cpp'
patch(TMS,
      b'static TMS34010MemoryMap MapStore[MAX_CPUS];\n',
      b'static TMS34010MemoryMap *MapStore = NULL; // EmuSuNi: on the heap while a TMS340x0 game runs (64 MB static otherwise)\n')
patch(TMS,
      b'\tif (nCpu == 0) {\n\t\tmemset(&MapStore, 0, sizeof(MapStore));\n\t}\n',
      b'\tif (nCpu == 0) {\n'
      b'\t\tif (MapStore == NULL) MapStore = (TMS34010MemoryMap*)malloc(sizeof(TMS34010MemoryMap) * MAX_CPUS); // EmuSuNi\n'
      b'\t\tmemset(MapStore, 0, sizeof(TMS34010MemoryMap) * MAX_CPUS);\n\t}\n')
patch(TMS,
      b'\ttotal_cpus = 0;\n\tactive_cpu = -1;\n}\n',
      b'\ttotal_cpus = 0;\n\tactive_cpu = -1;\n\tfree(MapStore); // EmuSuNi\n\tMapStore = NULL;\n\tg_mmap = NULL;\n}\n')
# The E1-32XS's page tables on the heap while such a game runs (16 MB static).
E1 = 'cpu/e132xs/e132xs.cpp'
patch(E1,
      b'static UINT8 *mem[2][0x100000];\n',
      b'static UINT8 **mem[2] = { NULL, NULL }; // EmuSuNi: the two 8 MB page tables on the heap while an E1-32XS game runs\n')
patch(E1,
      b'void E132XSInit(INT32 , INT32 type, INT32 )\n{\n\tmemset (mem, 0, sizeof(mem));\n',
      b'void E132XSInit(INT32 , INT32 type, INT32 )\n{\n'
      b'\tfor (INT32 i = 0; i < 2; i++) { // EmuSuNi\n'
      b'\t\tif (mem[i] == NULL) mem[i] = (UINT8**)malloc(0x100000 * sizeof(UINT8*));\n'
      b'\t\tmemset(mem[i], 0, 0x100000 * sizeof(UINT8*));\n\t}\n')
patch(E1,
      b'void E132XSExit()\n{\n\n}\n',
      b'void E132XSExit()\n{\n\tfor (INT32 i = 0; i < 2; i++) { // EmuSuNi\n\t\tfree(mem[i]);\n\t\tmem[i] = NULL;\n\t}\n}\n')
# The Philips XA's page tables (program and data space, 1 MB each) on the heap while it runs; a map made
# before xaInit is dropped (xaInit cleared it anyway).
XA = 'cpu/xa_intf.cpp'
patch(XA,
      b'static UINT8 *prog_mem[2][XA_PAGE_COUNT]; // 0 = read/fetch, 1 = write\n'
      b'static UINT8 *data_mem[2][XA_PAGE_COUNT];\n',
      b'// EmuSuNi: on the heap while the CPU runs (xaInit to xaExit), 1 MB each static otherwise\n'
      b'static UINT8 *(*prog_mem)[XA_PAGE_COUNT] = NULL; // 0 = read/fetch, 1 = write\n'
      b'static UINT8 *(*data_mem)[XA_PAGE_COUNT] = NULL;\n')
patch(XA,
      b'void xaMapMemory(UINT8 *ptr, UINT32 start, UINT32 end, INT32 flags)\n{\n',
      b'void xaMapMemory(UINT8 *ptr, UINT32 start, UINT32 end, INT32 flags)\n{\n'
      b'\tif (prog_mem == NULL) return; // EmuSuNi: before xaInit (which clears the maps)\n')
patch(XA,
      b'void xaMapDataMemory(UINT8 *ptr, UINT32 start, UINT32 end, INT32 flags)\n{\n',
      b'void xaMapDataMemory(UINT8 *ptr, UINT32 start, UINT32 end, INT32 flags)\n{\n'
      b'\tif (data_mem == NULL) return; // EmuSuNi: before xaInit (which clears the maps)\n')
patch(XA,
      b'INT32 xaInit(INT32, INT32 clock)\n{\n\tmemset(prog_mem, 0, sizeof(prog_mem));\n\tmemset(data_mem, 0, sizeof(data_mem));\n',
      b'INT32 xaInit(INT32, INT32 clock)\n{\n'
      b'\tif (prog_mem == NULL) prog_mem = (UINT8 *(*)[XA_PAGE_COUNT])malloc(2 * sizeof(*prog_mem)); // EmuSuNi\n'
      b'\tif (data_mem == NULL) data_mem = (UINT8 *(*)[XA_PAGE_COUNT])malloc(2 * sizeof(*data_mem));\n'
      b'\tmemset(prog_mem, 0, 2 * sizeof(*prog_mem));\n\tmemset(data_mem, 0, 2 * sizeof(*data_mem));\n')
patch(XA,
      b'\tif (xacpu) { delete xacpu; xacpu = NULL; }\n\n\tmemset(prog_mem, 0, sizeof(prog_mem));\n\tmemset(data_mem, 0, sizeof(data_mem));\n',
      b'\tif (xacpu) { delete xacpu; xacpu = NULL; }\n\n'
      b'\tfree(prog_mem); // EmuSuNi\n\tfree(data_mem);\n\tprog_mem = NULL;\n\tdata_mem = NULL;\n')
# The TLCS-900's page tables (1 MB) on the heap while it runs; a map made before tlcs900Init is dropped (it
# cleared the maps anyway).
TLCS = 'cpu/tlcs900/tlcs900.cpp'
patch(TLCS,
      b'static UINT8 *mem[2][MEM_SIZE >> MEM_PAGE_SHIFT];\n',
      b'static UINT8 *(*mem)[MEM_SIZE >> MEM_PAGE_SHIFT] = NULL; // EmuSuNi: on the heap while the CPU runs, 1 MB static otherwise\n')
patch(TLCS,
      b'void tlcs900MapMemory(UINT8 *ptr, UINT32 start, UINT32 end, UINT32 flags)\n{\n',
      b'void tlcs900MapMemory(UINT8 *ptr, UINT32 start, UINT32 end, UINT32 flags)\n{\n'
      b'\tif (mem == NULL) return; // EmuSuNi: before tlcs900Init (which clears the maps)\n')
patch(TLCS,
      b'void tlcs900Init(INT32)\n{\n',
      b'void tlcs900Init(INT32)\n{\n'
      b'\tif (mem == NULL) mem = (UINT8 *(*)[MEM_SIZE >> MEM_PAGE_SHIFT])calloc(2, sizeof(*mem)); // EmuSuNi\n')
patch(TLCS,
      b'void tlcs900Exit()\n{\n}\n',
      b'void tlcs900Exit()\n{\n\tfree(mem); // EmuSuNi\n\tmem = NULL;\n}\n')
# The driver list started and closed again (BurnLibInit / BurnLibExit, for every game and for the app's
# list): the drivers' own name strings kept and put back at exit, the freed arrays forgotten.
BURN = 'burn/burn.cpp'
patch_re(BURN,
         rb'(static wchar_t\*\*\s*pszFullNameW\s*=\s*NULL;[^\n]*\n)',
         rb'\1'
         rb'static char**    pszShortName0 = NULL; // EmuSuNi: the original strings (BurnGameListInit points the drivers at copies)\n'
         rb'static char**    pszFullNameA0 = NULL;\n'
         rb'static wchar_t** pszFullNameW0 = NULL;\n')
patch_re(BURN,
         rb'(\n(\s*)pszFullNameW = \(wchar_t\*\*\)malloc\(nBurnDrvCount \* sizeof\(wchar_t\*\)\);\n)',
         rb'\1'
         rb'\2pszShortName0 = (char**   )malloc(nBurnDrvCount * sizeof(char*)); // EmuSuNi\n'
         rb'\2pszFullNameA0 = (char**   )malloc(nBurnDrvCount * sizeof(char*));\n'
         rb'\2pszFullNameW0 = (wchar_t**)malloc(nBurnDrvCount * sizeof(wchar_t*));\n')
patch_re(BURN,
         rb'\n(\s*)(pszShortName\[i\] = \(char\*\s*\)malloc\(100\s*\* sizeof\(char\)\);\n)',
         rb'\n\1pszShortName0[i] = pDriver[i]->szShortName; // EmuSuNi: back at exit\n'
         rb'\1pszFullNameA0[i] = pDriver[i]->szFullNameA;\n'
         rb'\1pszFullNameW0[i] = pDriver[i]->szFullNameW;\n'
         rb'\1\2')
# The wide-character copies of the names (260 wchar_t each: 30 MB of the 41 the copies take for the 29,338
# drivers, held while a game runs) only where something reads them: without _UNICODE nothing does.
patch_re(BURN,
         rb'\n([ \t]*)(pszFullNameW\[i\] = \(wchar_t\*\)malloc\([^;]*;)',
         rb'\n#if defined (_UNICODE)\n\1\2\n#else\n'
         rb'\1pszFullNameW[i] = NULL; // EmuSuNi: without _UNICODE nothing reads the wide names (30 MB of them)\n'
         rb'#endif')
patch_re(BURN,
         rb'(memset\(pszFullNameA\[i\], [^;]*;)\n([ \t]*)(memset\(pszFullNameW\[i\], [^;]*;)',
         rb'\1\n#if defined (_UNICODE)\n\2\3\n#endif')
patch_re(BURN,
         rb'static void BurnGameListExit\(\)\n\{\n.*?\n\}\n',
         rb'static void BurnGameListExit()\n{\n'
         rb'\t// Release of storage space\n'
         rb'\t// EmuSuNi: the original strings back and the arrays forgotten, so the next BurnLibInit (the app starts the\n'
         rb'\t// core for every game, and lists the drivers) copies them again instead of freeing them again.\n'
         rb'\tfor (UINT32 i = 0; i < nBurnDrvCount; i++) {\n'
         rb'\t\tif ((NULL != pszShortName) && (NULL != pszShortName[i])) { if (NULL != pszShortName0) pDriver[i]->szShortName = pszShortName0[i]; free(pszShortName[i]); }\n'
         rb'\t\tif ((NULL != pszFullNameA) && (NULL != pszFullNameA[i])) { if (NULL != pszFullNameA0) pDriver[i]->szFullNameA = pszFullNameA0[i]; free(pszFullNameA[i]); }\n'
         rb'\t\tif ((NULL != pszFullNameW) && (NULL != pszFullNameW[i])) {\n'
         rb'#if defined (_UNICODE)\n'
         rb'\t\t\tif (NULL != pszFullNameW0) pDriver[i]->szFullNameW = pszFullNameW0[i];\n'
         rb'#endif\n'
         rb'\t\t\tfree(pszFullNameW[i]);\n'
         rb'\t\t}\n'
         rb'\t}\n'
         rb'\tif (NULL != pszShortName) free(pszShortName);\n'
         rb'\tif (NULL != pszFullNameA) free(pszFullNameA);\n'
         rb'\tif (NULL != pszFullNameW) free(pszFullNameW);\n'
         rb'\tfree(pszShortName0); free(pszFullNameA0); free(pszFullNameW0);\n'
         rb'\tpszShortName = NULL; pszFullNameA = NULL; pszFullNameW = NULL;\n'
         rb'\tpszShortName0 = NULL; pszFullNameA0 = NULL; pszFullNameW0 = NULL;\n'
         rb'}\n')
# The error screen's 8x8 font instead of the 4.5 MB Chinese one, which isn't compiled.
patch('burner/libretro/ugui_tools.c',
      b'   UG_FontSelect(FONT_SourceHanSansCN_Medium_13X17);\n',
      b'   UG_FontSelect(FONT_8X8); /* EmuSuNi: the CJK font is left out (the app shows the errors itself) */\n')
patch('burner/libretro/Makefile.common',
      b'\t$(UGUI_DIR)/SourceHanSansCN_Medium_13X17.c \\\n',
      b'')

# The console drivers left out: their sources are taken from the copy (a copy of git's tree each time,
# the stamp above sees to that), and their drivers from the list the core is made with. The list is
# generated (dep/generated/driverlist.h, by dep/scripts/gamelist.pl from the driver sources): the
# declarations, the array pDriver and the table of source files (BurnDrvGetSourcefile, which only the
# DAT generators call: left empty; 1.4 MB of the image for relocations alone). What stays is what the
# core's own "Arcade only" DAT (dats/, the commit's) lists: the arcade games, the BIOS sets, the Neo Geo
# CD. Some of those live in console files (Nintendo's VS. UniSystem games in the NES driver's, two Sega
# Mega-Play games in the Mega Drive's, the Astrocade's arcade games with its home ones): a directory with
# one of them is kept whole (its engine is theirs), its other drivers leave the list only; the program's
# link (--gc-sections) drops their data and code. The drivers' symbols and set names come from the
# sources as git has them (struct BurnDriver BurnDrvXxx = { "name", ...).
console_dirs = os.environ['FBNEO_CONSOLE_DIRS'].split()
console_files = os.environ['FBNEO_CONSOLE_FILES'].split()
DRIVER = re.compile(rb'struct\s+BurnDriver[DX]?\s+(\w+)\s*=\s*\{[^"]*"([^"]*)"')
dat = subprocess.run(['git', '-C', checkout, 'show', 'HEAD:dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat'],
                     check=True, capture_output=True).stdout
arcade = set(re.findall(rb'<game name="([^"]+)"', dat))
assert len(arcade) > 8000, len(arcade)
def tracked(prefix):
    names = subprocess.run(['git', '-C', checkout, 'ls-tree', '-r', '--name-only', 'HEAD', 'src/' + prefix],
                           check=True, capture_output=True).stdout.decode().split('\n')
    return [name[len('src/'):] for name in names if name]
console = set()          # symbols of the drivers that leave the list
gone = []                # sources taken from the copy
kept_dirs = []
for group in [tracked('burn/drv/' + d + '/') for d in console_dirs] + [['burn/' + f] for f in console_files]:
    drivers = []
    for path in group:
        if path.endswith(('.cpp', '.c', '.h')):
            drivers += DRIVER.findall(original(path))
    out = {sym for sym, name in drivers if name not in arcade}
    console.update(out)
    if len(out) == len(drivers):
        gone += [path for path in group if path.endswith(('.cpp', '.c'))]
    else:
        kept_dirs.append(group[0].rsplit('/', 1)[0] + ' (%d of its %d drivers are arcade games)' % (len(drivers) - len(out), len(drivers)))
for path in gone:
    try:
        os.remove(build + '/' + path)
    except FileNotFoundError:
        pass
DL = 'dep/generated/driverlist.h'
lines = []
declared = pointed = 0
in_files = False
for line in original(DL).split(b'\n'):
    if in_files:
        # (the table of source files: only its end stays)
        if line.startswith(b'};'):
            in_files = False
            lines.append(line)
        continue
    m = re.match(rb'^(?:DRV|//)\s+(\w+);', line)
    if m and m.group(1) in console:
        declared += 1
        continue
    m = re.match(rb'^(?://)?\s*&(\w+),', line)
    if m and m.group(1) in console:
        pointed += 1
        continue
    lines.append(line)
    if line.startswith(b'static game_sourcefile_entry sourcefile_table[] = {'):
        lines.append(b'\t{ "\\0", "\\0"}')
        in_files = True
# (A few symbols the sources define aren't in the list: gamelist.pl skips what it can't parse.)
assert declared == pointed and declared > len(console) - 100, (declared, pointed, len(console))
patched[DL] = b'\n'.join(lines)
print("FinalBurn Neo: %d console drivers out of the list, %d sources out of the copy; kept whole: %s" %
      (declared, len(gone), ', '.join(kept_dirs)))

# The files patched last time and not now (a patch taken out of this script) go back as git has them: make
# compiles them again with the others, and the copy never keeps what no patch here makes.
listing = os.path.dirname(build) + '/.emusuni-patched'
try:
    with open(listing) as f:
        before = f.read().split()
except FileNotFoundError:
    before = []
targets = sorted(patched)
for path in before:
    if path not in patched:
        patched[path] = original(path)
        print("FinalBurn Neo: %s is no longer patched: put back as git has it" % path)
written = 0
for path, text in patched.items():
    full = build + '/' + path
    with open(full, 'rb') as f:
        same = f.read() == text
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
with open(listing, 'w') as f:
    f.write('\n'.join(targets) + '\n')
print("FinalBurn Neo: patches applied (%d files changed)" % written)
PATCH

# The Makefile's own flags come after ours (-std=gnu++98, -O3, -ffast-math, its defines); the environment's
# CFLAGS/CXXFLAGS (ps4vars.sh's, for clang 12) would be its starting point: cleared. LD=true: its link
# into a .so is skipped (ld.lld -r makes the object below).
if ! env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
     make -C "$BUILD/src/burner/libretro" -j "$JOBS" platform=unix CC="$CC_CMD" CXX="$CXX_CMD" LD=true \
     GIT_VERSION="$GIT_SHORT" GIT_DATE="$GIT_DATE" all >> "$LOG" 2>&1; then
  grep -E "error:|Error [0-9]|\*\*\*" "$LOG" | head -30
  echo "FinalBurn Neo: build failed (see $LOG)"
  exit 1
fi

# One object: only the libretro API (renamed) and the two helpers stay global. Replaced only when it
# changed (the app isn't linked again for nothing).
find "$BUILD/src" -name '*.o' | sort > "$OUT/objects.txt"
echo "FinalBurn Neo: $(wc -l < "$OUT/objects.txt") objects"
ld.lld-21 -r @"$OUT/objects.txt" -o "$OUT/arcade_core_all.o"
llvm-objcopy-21 --wildcard --keep-global-symbol='fbneo_retro_*' --keep-global-symbol='fbneo_emusuni_*' \
  "$OUT/arcade_core_all.o" "$OUT/arcade_core.o.new"
rm -f "$OUT/arcade_core_all.o"

# The checks.
GLOBALS=$(llvm-nm-21 -g --defined-only "$OUT/arcade_core.o.new" | awk '{print $3}' | sort)
WANT_GLOBALS=$( { for f in $API; do echo "fbneo_$f"; done; for f in $HELPERS; do echo "$f"; done; } | sort)
if [ "$GLOBALS" != "$WANT_GLOBALS" ]; then
  echo "FinalBurn Neo: the object's global symbols aren't exactly the renamed libretro API and the helpers:"
  diff <(echo "$WANT_GLOBALS") <(echo "$GLOBALS") || true
  exit 1
fi
UNDEFINED=$(llvm-nm-21 -u "$OUT/arcade_core.o.new" | awk '$1 == "U" {print $NF}' | sort -u)
# No weak references left unresolved: the app would export them as imports nobody gives.
WEAK=$(llvm-nm-21 -u "$OUT/arcade_core.o.new" | awk '$1 != "U" {print $NF}' | sort -u)
if [ -n "$WEAK" ]; then
  echo "FinalBurn Neo: the object has weak references nothing defines:"
  echo "$WEAK"
  exit 1
fi
if llvm-nm-21 --defined-only "$OUT/arcade_core.o.new" | awk '{print $NF}' | grep -E '^_Z(nw|na|dl|da)'; then
  echo "FinalBurn Neo: the object defines a C++ operator new/delete of its own"
  exit 1
fi
# No static array of 1 MiB or more: the PS4 charges the app's whole static memory from its start, used or
# not (the CPUs' big page tables are the heap's while such a game runs, above).
BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/arcade_core.o.new" | awk '($3 == "b" || $3 == "B") && $2 + 0 >= 1048576 {print $4}')
if [ -n "$BIG" ]; then
  echo "FinalBurn Neo: the object has static arrays of 1 MiB or more:"
  echo "$BIG"
  exit 1
fi
if [ "$HOST" != 1 ]; then
  # What the app gives it: operator new/delete (port/ps4_new.cpp), compiled here as the app compiles it,
  # only to list its symbols; __dso_handle, which ld.lld defines for the program (as the other C++ cores).
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $TARGET_FLAGS -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" | awk 'NF >= 3 {print $3}'; echo __dso_handle; } | sort -u)
  rm -f "$OUT/check_ps4_new.o"
  NEW_DELETE=$(echo "$UNDEFINED" | grep -E '^_Z(nw|na|dl|da)' || true)
  NOT_APPS=$(comm -23 <(echo "$NEW_DELETE" | sed '/^$/d') <(echo "$APP_GIVES"))
  if [ -n "$NOT_APPS" ]; then
    echo "FinalBurn Neo: the object uses C++ operator new/delete that port/ps4_new.cpp doesn't define:"
    echo "$NOT_APPS"
    exit 1
  fi
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a
    $PS4_BUILD_ROOT/sysroot/lib/libc++abi.a $OPENORBIS/lib/libunwind.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
  MISSING=$(comm -23 <(echo "$UNDEFINED") <(echo "$PROVIDED"))
  if [ -n "$MISSING" ]; then
    echo "FinalBurn Neo: the object needs symbols the PS4 libraries and the app don't have:"
    echo "$MISSING"
    exit 1
  fi
  echo "FinalBurn Neo: $(echo "$UNDEFINED" | wc -l) symbols needed, all in the PS4 libraries and the app" \
       "($(echo "$NEW_DELETE" | sed '/^$/d' | wc -l) operator new/delete, port/ps4_new.cpp's)"
fi
if cmp -s "$OUT/arcade_core.o.new" "$OUT/arcade_core.o"; then
  rm -f "$OUT/arcade_core.o.new"
else
  mv "$OUT/arcade_core.o.new" "$OUT/arcade_core.o"
fi
ls -la "$OUT/arcade_core.o"
llvm-size-21 "$OUT/arcade_core.o"
