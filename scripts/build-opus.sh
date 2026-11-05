#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Builds Opus (Xiph.Org's speech and music codec, BSD-3-Clause) for the PS4 into a static library that
# scripts/build.sh links into EmuSuNi: the voice chat's codec (00.23, src/Voice.h: 16 kHz mono speech, 20 ms
# packets, in-band FEC, DTX). The release tarball (opus-1.5.2.tar.gz from downloads.xiph.org, checked by its SHA-256;
# scripts/setup.sh fetches it, else this script) is unpacked here and built with its own CMake and the PS4
# toolchain (toolchain/ps4-love-modern.cmake):
#   - fixed-point (OPUS_FIXED_POINT): measured on the PC with the PS4's CPU flags (btver2, SSE4.1, no AVX2), as fast as
#     the float build (encode 87 against 81 us a 20 ms frame at complexity 5, decode 10 against 10.5), whole numbers
#     in and out like the app's sound, and no float API (OPUS_ENABLE_FLOAT_API off: less code);
#   - Jaguar's SSE4.1 presumed (no run-time CPU check), no AVX2 (Jaguar has none);
#   - nothing else: no custom modes, no deep-learning parts (DRED, OSCE, deep PLC), no programs or tests, no stack
#     protector or _FORTIFY_SOURCE (the PS4's C library has no __stack_chk_* nor __*_chk);
#   - -O2, a section for each function and variable (the app's link drops what it doesn't call: the decoder of
#     multistream, the repacketizer...), no unwind tables (it is C: nothing unwinds through it).
# The library is checked at the end: no C++ operator new or delete, and every symbol it needs is in the PS4 libraries
# the app links (OpenOrbis' libc, libkernel, libSceLibcInternal, clang's builtins).
# Built once; again only when this script or the tarball changes.
#   build-opus.sh <out dir>      -> <out dir>/lib/libopus.a, <out dir>/include/opus/opus*.h
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
VERSION=1.5.2
TARBALL="${OPUS_TARBALL:-$HOME/src/opus-$VERSION.tar.gz}"
SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
LOG="$OUT/opus.log"
STAMP="$OUT/.emusuni-flags"
# shellcheck disable=SC1091
source "$HERE/ps4-env.sh"

CFLAGS_OPUS="-O2 -DNDEBUG -march=btver2 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables"
OPTIONS=(
  -DOPUS_FIXED_POINT=ON -DOPUS_ENABLE_FLOAT_API=OFF -DOPUS_CUSTOM_MODES=OFF
  -DOPUS_DRED=OFF -DOPUS_OSCE=OFF -DOPUS_DEEP_PLC=OFF
  -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
  -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
  -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF
  -DOPUS_X86_MAY_HAVE_SSE=ON -DOPUS_X86_MAY_HAVE_SSE2=ON -DOPUS_X86_MAY_HAVE_SSE4_1=ON -DOPUS_X86_MAY_HAVE_AVX2=OFF
  -DOPUS_X86_PRESUME_SSE=ON -DOPUS_X86_PRESUME_SSE2=ON -DOPUS_X86_PRESUME_SSE4_1=ON -DOPUS_X86_PRESUME_AVX2=OFF
  "-DCMAKE_C_FLAGS_RELEASE=$CFLAGS_OPUS"
)
WANT="$SHA256 | ${OPTIONS[*]} | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"
if [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ] && [ -f "$OUT/lib/libopus.a" ] && [ -f "$OUT/include/opus/opus.h" ]; then
  exit 0
fi
echo "Opus $VERSION: building it for the PS4 (fixed-point)"
if ! echo "$SHA256  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; then
  mkdir -p "$(dirname "$TARBALL")"
  wget -q "https://downloads.xiph.org/releases/opus/opus-$VERSION.tar.gz" -O "$TARBALL.part"
  mv "$TARBALL.part" "$TARBALL"
  if ! echo "$SHA256  $TARBALL" | sha256sum -c --quiet -; then
    echo "Opus: $TARBALL isn't the opus-$VERSION.tar.gz release (SHA-256)"
    exit 1
  fi
fi
rm -rf "$OUT/src" "$OUT/build" "$OUT/lib" "$OUT/include" "$STAMP"
mkdir -p "$OUT/src"
tar xzf "$TARBALL" -C "$OUT/src" --strip-components=1
: > "$LOG"
if ! ps4_cmake -S "$OUT/src" -B "$OUT/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PROJECT/toolchain/ps4-love-modern.cmake" \
     -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_INSTALL_LIBDIR=lib "${OPTIONS[@]}" >> "$LOG" 2>&1; then
  tail -30 "$LOG"
  echo "Opus: configuring failed (see $LOG)"
  exit 1
fi
# At most 4 jobs: the PC blue-screened once under a full load.
if ! ninja -C "$OUT/build" -j "${EMUSUNI_JOBS:-4}" install >> "$LOG" 2>&1; then
  grep -E "error:|FAILED" "$LOG" | head -30
  echo "Opus: build failed (see $LOG)"
  exit 1
fi
# What the configuration found (the log keeps it): fixed-point, SSE4.1 presumed, no run-time CPU detection.
grep -E "^(OPUS_FIXED_POINT|OPUS_X86_PRESUME_SSE4_1|OPUS_X86_MAY_HAVE_AVX2|OPUS_ENABLE_FLOAT_API|OPUS_VAR_ARRAYS)" \
  "$OUT/build/CMakeCache.txt" >> "$LOG" || true

# The checks.
UNDEFINED=$(llvm-nm-21 -u "$OUT/lib/libopus.a" 2>/dev/null | awk 'NF {print $NF}' | grep -v ':$' | sort -u)
DEFINED=$(llvm-nm-21 --defined-only "$OUT/lib/libopus.a" 2>/dev/null | awk 'NF >= 3 {print $3}' | sort -u)
NEEDED=$(comm -23 <(echo "$UNDEFINED") <(echo "$DEFINED"))
if echo "$NEEDED" | grep -E '^_Z(nw|na|dl|da)'; then
  echo "Opus: the library uses C++ operator new/delete"
  exit 1
fi
PS4LIBS="$OPENORBIS/lib/libc.a $OPENORBIS/lib/libkernel.so $OPENORBIS/lib/libSceLibcInternal.so
  $OPENORBIS/lib/libclang_rt.builtins-x86_64.a"
PROVIDED=$(for lib in $PS4LIBS; do
    case "$lib" in *.so) llvm-nm-21 -D --defined-only "$lib" ;; *) llvm-nm-21 --defined-only "$lib" ;; esac 2>/dev/null
  done |
  awk 'NF >= 2 && $(NF-1) ~ /^[TWDBRVi]$/ {print $NF}' | sort -u)
MISSING=$(comm -23 <(echo "$NEEDED") <(echo "$PROVIDED"))
if [ -n "$MISSING" ]; then
  echo "Opus: the library needs symbols the PS4 libraries don't have:"
  echo "$MISSING"
  exit 1
fi
echo "Opus: $(echo "$NEEDED" | grep -c .) C library symbols needed, all in the PS4 libraries"
printf '%s' "$WANT" > "$STAMP"
ls -la "$OUT/lib/libopus.a"
