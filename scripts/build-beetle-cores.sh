#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds libretro's Beetle PCE Fast (Mednafen's pce_fast) for the PS4 into one object that scripts/build.sh links into
# EmuSuNi. It is GPL-2.0 (the app's licence):
#   - Beetle PCE Fast (libretro/beetle-pce-fast-libretro, ~/src/beetle-pce-fast-libretro): the PC Engine /
#     TurboGrafx-16 (HuCards, .pce), with libchdr (its LZMA, zstd and FLAC decoders), its zlib and Tremor - the core's
#     CD support, built as its Makefile builds it; the app gives it no CD game since 00.83. No SuperGrafx: PCE Fast has
#     none (its .sgx is Beetle SuperGrafx's).                                    -> <out>/pce_core.o, beetle_pce_retro_*
# (00.82 also built Beetle NeoPop and Beetle Cygne here, the Neo Geo Pockets' and the WonderSwans' cores: gone in 00.83
# with those consoles and the PC Engine CD; their sources stay in ~/src.)
# It has no CMake of its own: the one made here lists the sources its Makefile.common compiles for unix (with its
# Makefile's settings: HAVE_CHD, NEED_TREMOR, NEED_BLIP, USE_CHEATS) and their defines, compiled with the PS4 toolchain
# (toolchain/ps4-love-modern.cmake). The app hands it the HuCards from memory (GET_GAME_INFO_EXT). No threads.
# Not as their unix builds:
#   - No physical CD drive code (HAVE_CDROM: Linux's SCSI ioctls), no link-time optimisation.
#   - Every function and variable in its own section (-ffunction-sections -fdata-sections), so the app's link
#     (--gc-sections) leaves out what nothing calls; -O3 for the PS4's CPU (btver2).
# Changes to its sources (applied each time to the file as git has it, checked):
#   - The HuCard/CD memory space (ROMSpace, 1.07 MiB) on the heap from the first game to retro_deinit, not a static
#     array of the image the PS4 charges from the app's start.
#   - The frame's sound buffer 32 KiB, not 128 (a frame has ~735 samples). No core options' translations
#     (HAVE_NO_LANGEXTRA: the app sets the options itself; 2.6 MiB of data).
#   - A frame the frontend won't show isn't drawn (GET_AUDIO_VIDEO_ENABLE: the app's Frame Skip), the way its own
#     frameskip option skips (spec.skip, video_cb(NULL)).
#   (00.82's System Card by the app's path, EMUSUNI_PCE_BIOS, is gone with the PC Engine CD in 00.83.)
# Its libretro API becomes beetle_pce_retro_* and everything else in it private, as for FCEUmm (CMakeLists.txt). The
# object is checked at the end: only its renamed API defined
# globally, no weak references left, no static array of 1 MiB or more, no C++ exceptions (nothing that throws or
# unwinds: an exception in a core ends the app without a word on the PS4), no operator new/delete of its own, and
# every symbol it needs found in the PS4 libraries the app links (OpenOrbis' libc, libkernel, libSceLibcInternal,
# clang's builtins, libc++) or in the app (operator new/delete).
# Only what changed is compiled again (ninja); everything when the flags or this script change.
#   build-beetle-cores.sh <out dir> [clean]
#   BEETLE_CORE_HOST=1 build-beetle-cores.sh <out dir>   the same objects for this PC (Linux, clang 21), for the PC
#                                                        tests: no PS4 checks (BEETLE_CORE_ASAN=1: with AddressSanitizer)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
PCE_SRC="${BEETLE_PCE_DIR:-$HOME/src/beetle-pce-fast-libretro}"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
BUILD="$OUT/build"
LOG="$OUT/beetle-cores.log"
HOST="${BEETLE_CORE_HOST:-0}"
ASAN="${BEETLE_CORE_ASAN:-0}"
[ "$HOST" = 1 ] || ASAN=0

python3 -I - "$PCE_SRC" <<'PATCH'
import subprocess
import sys
roots = {'pce': sys.argv[1]}
patched = {}
def patch(core, path, old, new):
    # On the file as git has it (the commit setup.sh pins), so a changed patch replaces the old one.
    key = (core, path)
    if key not in patched:
        patched[key] = subprocess.run(['git', '-C', roots[core], 'show', 'HEAD:' + path], check=True,
                                      capture_output=True).stdout
    text = patched[key]
    if b'\r\n' in text:
        old = old.replace(b'\n', b'\r\n')
        new = new.replace(b'\n', b'\r\n')
    assert text.count(old) == 1, (core, path, text.count(old), old[:70])
    patched[key] = text.replace(old, new)

# ---- PCE Fast ----
# The HuCard/CD memory space on the heap (1.07 MiB not in the app's image): made by the first game's load, freed by
# retro_deinit (the memory descriptors and RETRO_MEMORY_ROM/SAVE_RAM point into it while a game is loaded).
patch('pce', 'libretro.c',
      b'// Statically allocated for speed...or something.\n'
      b'uint8 ROMSpace[0x88 * 8192 + 8192];\t// + 8192 for PC-as-pointer safety padding\n',
      b'/* EmuSuNi: on the heap (0x88 * 8192 + 8192 bytes, + 8192 for PC-as-pointer safety padding), made by the first\n'
      b'   retro_load_game, freed by retro_deinit: 1.07 MiB not in the app\'s image */\n'
      b'uint8 *ROMSpace = NULL;\n')
patch('pce', 'mednafen/pce_fast/pce.h',
      b'extern uint8 ROMSpace[0x88 * 8192 + 8192];',
      b'extern uint8 *ROMSpace; /* EmuSuNi: on the heap (libretro.c) */')
patch('pce', 'libretro.c',
      b'   if (failed_init)\n      return false;\n',
      b'   if (failed_init)\n      return false;\n'
      b'\n'
      b'   /* EmuSuNi: the HuCard/CD memory space on the heap, kept until retro_deinit */\n'
      b'   if (!ROMSpace && !(ROMSpace = (uint8 *)calloc(1, 0x88 * 8192 + 8192)))\n'
      b'   {\n'
      b'      if (log_cb)\n'
      b'         log_cb(RETRO_LOG_ERROR, "EmuSuNi: no memory for the PC Engine\'s memory space\\n");\n'
      b'      return false;\n'
      b'   }\n')
patch('pce', 'libretro.c',
      b'void retro_deinit(void)\n{\n',
      b'void retro_deinit(void)\n{\n'
      b'   /* EmuSuNi: the memory space retro_load_game made */\n'
      b'   if (ROMSpace)\n'
      b'      free(ROMSpace);\n'
      b'   ROMSpace = NULL;\n'
      b'\n')
# A frame the frontend won't show isn't drawn (the VDC's spec.skip, video_cb(NULL): its frameskip option's way).
FRAMESKIP_HOOK = (b'   /* If frameskip settings have changed, update\n'
                  b'    * frontend audio latency */\n')
AV_SKIP = (b'   /* EmuSuNi: a frame the frontend won\'t show isn\'t drawn (GET_AUDIO_VIDEO_ENABLE: its Frame Skip) */\n'
           b'   if (!skip_frame)\n'
           b'   {\n'
           b'      int av_enable = 3;\n'
           b'      if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av_enable) && !(av_enable & 1))\n'
           b'         skip_frame = 1;\n'
           b'   }\n'
           b'\n')
patch('pce', 'libretro.c', FRAMESKIP_HOOK, AV_SKIP + FRAMESKIP_HOOK)
# The frame's sound buffer 32 KiB, not 128 (a frame has ~735 stereo samples, the Blip buffers hold 50 ms; Emulate
# reads at most what fits, from its size).
patch('pce', 'libretro.c',
      b'static int16_t sound_buf[0x10000];\n',
      b'static int16_t sound_buf[0x4000]; /* EmuSuNi: 32 KiB of .bss, not 128 (a frame: ~735 stereo samples) */\n')

written = 0
for (core, path), text in patched.items():
    full = roots[core] + '/' + path
    with open(full, 'rb') as f:
        same = f.read() == text
    if not same:
        with open(full, 'wb') as f:
            f.write(text)
        written += 1
print("Beetle PCE Fast: patches applied (%d files changed)" % written)
PATCH

API="retro_set_environment retro_set_video_refresh retro_set_audio_sample
  retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state retro_init retro_deinit
  retro_api_version retro_get_system_info retro_get_system_av_info retro_set_controller_port_device
  retro_reset retro_run retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
  retro_cheat_set retro_load_game retro_load_game_special retro_unload_game retro_get_region
  retro_get_memory_data retro_get_memory_size"
# The core: name, its API's prefix, its sources (00.82 had three).
CORES="pce"
declare -A PREFIX=([pce]=beetle_pce)
declare -A NAME=([pce]="Beetle PCE Fast")
declare -A SRC=([pce]="$PCE_SRC")
for core in $CORES; do
  {
    echo "// Generated by scripts/build-beetle-cores.sh: the ${NAME[$core]} core's libretro API, renamed."
    echo "#pragma once"
    for f in $API; do echo "#define $f ${PREFIX[$core]}_$f"; done
  } > "$OUT/prefix_$core.h.new"
  # Rewritten only when different: every source depends on it.
  if cmp -s "$OUT/prefix_$core.h.new" "$OUT/prefix_$core.h"; then
    rm -f "$OUT/prefix_$core.h.new"
  else
    mv "$OUT/prefix_$core.h.new" "$OUT/prefix_$core.h"
  fi
done

# The core's CMake: what its Makefile.common compiles for unix, with the Makefile's defines.
mkdir -p "$OUT/cmake"
cat > "$OUT/cmake/CMakeLists.txt.new" <<'CMAKE'
# Generated by scripts/build-beetle-cores.sh: Beetle PCE Fast as a static library.
cmake_minimum_required(VERSION 3.20)
project(beetle C CXX)
set(COMMON_DEFINES MEDNAFEN_VERSION_NUMERIC=931 STDC_HEADERS __STDC_LIMIT_MACROS __LIBRETRO__ INLINE=inline NDEBUG
  FRONTEND_SUPPORTS_RGB565 HAVE_NO_LANGEXTRA)

# PC Engine (Makefile: HAVE_CHD, NEED_TREMOR, NEED_BLIP, NEED_CD, USE_CHEATS; no HAVE_CDROM, no CACHE_CD)
set(P ${PCE_DIR})
set(PL ${P}/libretro-common)
set(PD ${P}/deps)
file(GLOB PCE_TREMOR ${P}/mednafen/tremor/*.c)
list(FILTER PCE_TREMOR EXCLUDE REGEX "ivorbisfile_example\\.c$")
add_library(pce STATIC
  ${P}/mednafen/hw_misc/arcade_card/arcade_card.c ${P}/mednafen/pce_fast/pcecd_drive.c ${P}/mednafen/pce_fast/pcecd.c
  ${P}/mednafen/pce_fast/psg.c ${P}/mednafen/pce_fast/huc6280.c ${P}/mednafen/pce_fast/input.c
  ${P}/mednafen/pce_fast/vdc.c ${P}/mednafen/sound/Blip_Buffer.c
  ${P}/mednafen/cdrom/CDAccess.c ${P}/mednafen/cdrom/CDAccess_Image.c ${P}/mednafen/cdrom/CDAccess_CCD.c
  ${P}/mednafen/cdrom/audioreader.c ${P}/mednafen/cdrom/cdromif.c ${P}/mednafen/cdrom/CDUtility.c
  ${P}/mednafen/cdrom/lec.c ${P}/mednafen/cdrom/galois.c ${P}/mednafen/cdrom/l-ec.c ${P}/mednafen/cdrom/edc_crc32.c
  ${P}/mednafen/cdrom/recover-raw.c ${P}/mednafen/cdrom/CDAccess_CHD.c
  ${PD}/lzma-19.00/src/Alloc.c ${PD}/lzma-19.00/src/Bra86.c ${PD}/lzma-19.00/src/BraIA64.c
  ${PD}/lzma-19.00/src/CpuArch.c ${PD}/lzma-19.00/src/Delta.c ${PD}/lzma-19.00/src/LzFind.c
  ${PD}/lzma-19.00/src/Lzma86Dec.c ${PD}/lzma-19.00/src/LzmaDec.c ${PD}/lzma-19.00/src/LzmaEnc.c
  ${PD}/libchdr/src/libchdr_bitstream.c ${PD}/libchdr/src/libchdr_cdrom.c ${PD}/libchdr/src/libchdr_chd.c
  ${PD}/libchdr/src/libchdr_flac.c ${PD}/libchdr/src/libchdr_huffman.c
  ${PD}/zstd/lib/common/entropy_common.c ${PD}/zstd/lib/common/error_private.c
  ${PD}/zstd/lib/common/fse_decompress.c ${PD}/zstd/lib/common/zstd_common.c ${PD}/zstd/lib/common/xxhash.c
  ${PD}/zstd/lib/decompress/huf_decompress.c ${PD}/zstd/lib/decompress/zstd_ddict.c
  ${PD}/zstd/lib/decompress/zstd_decompress.c ${PD}/zstd/lib/decompress/zstd_decompress_block.c
  ${PD}/zlib-1.2.11/adler32.c ${PD}/zlib-1.2.11/crc32.c ${PD}/zlib-1.2.11/inffast.c ${PD}/zlib-1.2.11/inflate.c
  ${PD}/zlib-1.2.11/inftrees.c ${PD}/zlib-1.2.11/zutil.c
  ${PCE_TREMOR}
  ${P}/libretro.c ${P}/mednafen/general.c ${P}/mednafen/cdstream.c ${P}/mednafen/mempatcher.c
  ${P}/mednafen/okiadpcm.c ${P}/mednafen/file.c ${P}/mednafen/settings.c ${P}/mednafen/state.c
  ${P}/mednafen/mednafen-endian.c
  ${PL}/streams/file_stream.c ${PL}/streams/file_stream_transforms.c ${PL}/file/file_path.c
  ${PL}/file/retro_dirent.c ${PL}/lists/string_list.c ${PL}/lists/dir_list.c ${PL}/compat/compat_strl.c
  ${PL}/compat/compat_snprintf.c ${PL}/compat/compat_posix_string.c ${PL}/compat/compat_strcasestr.c
  ${PL}/compat/fopen_utf8.c ${PL}/encodings/encoding_utf.c ${PL}/encodings/encoding_crc32.c
  ${PL}/memmap/memalign.c ${PL}/string/stdstring.c ${PL}/time/rtime.c ${PL}/vfs/vfs_implementation.c)
target_include_directories(pce PRIVATE ${P} ${P}/mednafen ${P}/mednafen/include ${P}/mednafen/hw_sound
  ${P}/mednafen/hw_cpu ${P}/mednafen/hw_misc ${PL}/include ${PD}/lzma-19.00/include ${PD}/libchdr/include
  ${PD}/zstd/lib ${PD}/zlib-1.2.11)
target_compile_definitions(pce PRIVATE ${COMMON_DEFINES} GIT_VERSION="${PCE_REVISION}" WANT_PCE_FAST_EMU
  _LOW_ACCURACY_ NEED_CD NEED_TREMOR USE_CHEATS HAVE_CHD _7ZIP_ST ZSTD_DISABLE_ASM ZSTD_TRACE=0)
target_compile_options(pce PRIVATE -w -include ${OUT_DIR}/prefix_pce.h)
CMAKE
if cmp -s "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"; then
  rm -f "$OUT/cmake/CMakeLists.txt.new"
else
  mv "$OUT/cmake/CMakeLists.txt.new" "$OUT/cmake/CMakeLists.txt"
fi

# The Release flags (CMAKE_C_FLAGS would replace the toolchain's target flags): -O3, the PS4's CPU, a section for each
# function and variable (the app's link drops the unused ones), no unwind tables (nothing unwinds through them); the
# C++ files without exceptions or RTTI (nothing in them throws or catches; an exception would end the app on the PS4).
FLAGS_CORE="-O3 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables"
# (On the PC: Jaguar's instructions but AMD's SSE4A, which Intel CPUs don't have.)
[ "$HOST" = 1 ] && FLAGS_CORE+=" -mno-sse4a -fPIC"
# (AddressSanitizer: its globals registered by each file, not through the section the -r link and the symbols made
# local would confuse.)
[ "$ASAN" = 1 ] && FLAGS_CORE+=" -fsanitize=address -fno-omit-frame-pointer -fno-sanitize-address-globals-dead-stripping"
FLAGS_CXX="$FLAGS_CORE -fno-exceptions -fno-rtti"
REV() { echo " $(git -C "$1" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"; }
OPTIONS=(-DPCE_DIR="$PCE_SRC" -DOUT_DIR="$OUT" "-DPCE_REVISION=$(REV "$PCE_SRC")"
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS_CORE" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS_CXX")
if [ "$HOST" = 1 ]; then
  configure() {
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
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
WANT="$PCE_SRC |$COMMITS | host $HOST | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
: > "$LOG"
if [ "${2:-}" = clean ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ] || [ ! -f "$BUILD/build.ninja" ]; then
  echo "Beetle PCE Fast: configuring and compiling everything (new flags, a new version of this script, or clean)"
  rm -rf "$BUILD" "$STAMP"
  if ! configure -S "$OUT/cmake" -B "$BUILD" -G Ninja "${OPTIONS[@]}" >> "$LOG" 2>&1; then
    tail -30 "$LOG"
    echo "Beetle PCE Fast: configuring failed (see $LOG)"
    exit 1
  fi
fi
# (At most 4 compilers at once: the PC's load, scripts/build.sh.)
if ! ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}" pce >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "Beetle PCE Fast: build failed (see $LOG)"
  exit 1
fi
printf '%s' "$WANT" > "$STAMP"

if [ "$HOST" != 1 ]; then
  # What the PS4 libraries the app links give, and the app (operator new/delete: port/ps4_new.cpp, compiled here as
  # the app compiles it, only to list its symbols).
  PS4C="-target x86_64-pc-freebsd12-elf -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4"
  PS4C+=" -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -isysroot $OPENORBIS -isystem $PS4_BUILD_ROOT/sysroot/libc-overlay"
  PS4C+=" -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -march=btver2"
  clang++-21 -std=c++23 -nostdinc++ -isystem "$PS4_BUILD_ROOT/sysroot/include/c++/v1" $PS4C -O2 \
    -c "$PROJECT/port/ps4_new.cpp" -o "$OUT/check_ps4_new.o"
  # (__dso_handle: ld.lld defines it for the program, as for the other C++ cores.)
  APP_GIVES=$( { llvm-nm-21 --defined-only "$OUT/check_ps4_new.o" | awk 'NF >= 3 {print $3}'; echo __dso_handle; } |
               sort -u)
  rm -f "$OUT/check_ps4_new.o"
  PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
    $OPENORBIS/lib/libclang_rt.builtins-x86_64.a $PS4_BUILD_ROOT/sysroot/lib/libc++.a"
  PROVIDED=$( { for lib in $PS4LIBS; do
      case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
    done | awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVvi]$/ {print $NF}'; echo "$APP_GIVES"; } | sort -u)
fi

# One object for each core: only its libretro API (renamed) stays global. Replaced only when it changed (the app isn't
# linked again for nothing).
for core in $CORES; do
  P="${PREFIX[$core]}"
  N="${NAME[$core]}"
  ld.lld-21 -r --whole-archive "$BUILD/lib$core.a" -o "$OUT/${core}_core_all.o"
  llvm-objcopy-21 --wildcard --keep-global-symbol="${P}_retro_*" "$OUT/${core}_core_all.o" "$OUT/${core}_core.o.new"
  rm -f "$OUT/${core}_core_all.o"

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
  BIG=$(llvm-nm-21 --print-size --radix=d "$OUT/${core}_core.o.new" |
        awk '($3 == "b" || $3 == "B" || $3 == "d" || $3 == "D") && $2 + 0 >= 1048576 {print $4}')
  if [ -n "$BIG" ]; then
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
# (00.82's ngp_core.o and wswan_core.o, left in <out> by an older build, are removed: nothing links them.)
rm -f "$OUT/ngp_core.o" "$OUT/wswan_core.o" "$OUT/prefix_ngp.h" "$OUT/prefix_wswan.h"
llvm-size-21 "$OUT/pce_core.o"
