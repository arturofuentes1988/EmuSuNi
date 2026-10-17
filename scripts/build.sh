#!/usr/bin/env bash
# Builds EmuSuNi's eboot.bin in WSL (Ubuntu): clang 21, the PacBrew OpenOrbis toolchain in
# /opt/pacbrew/ps4/openorbis, libc++ 21 and Mesa's RADV built by DolphinPS4's scripts
# (toolchain/build-libcxx.sh, toolchain/build-runtime.sh, scripts/build-mesa.sh) in
# ~/dolphinps4-build, Snes9x in /root/src/snes9x, FCEUmm in /root/src/fceumm and Dear ImGui 1.92.2b
# in /root/src/imgui.
# Vulkan (RADV on GNM) runs under the plain homebrew identity (program authority ID
# 0x3800000000000035, plain param.sfo: 4.5 GiB of direct memory), as DolphinPS4 packages it; the
# system identity (0x3100000000000002, RetroArch's) is only for Sony's OpenGL (Piglet).
# The Nintendo 64's core, Mupen64Plus-Next, is built first by scripts/build-n64-core.sh (in its
# own source tree: again only what changed) and linked in; so is the Game Boy / Game Boy Color /
# Game Boy Advance core, mGBA (/root/src/mgba), by scripts/build-gba-core.sh (in <build>/gba).
#   build.sh [setup] [no-n64]   (setup: configure from scratch; no-n64: without the Nintendo 64)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
SETUP=0
N64=ON
for arg in "$@"; do
    case "$arg" in
        setup) SETUP=1 ;;
        no-n64) N64=OFF ;;
    esac
done
BUILD="${EMUSUNI_BUILD:-$HOME/emusuni-build}"
if [ "$N64" = ON ]; then
    bash "$HERE/build-n64-core.sh" "$BUILD/n64"
    EXTRA="-DEMUSUNI_N64=ON -DN64_CORE_OBJECT=$BUILD/n64/n64_core.o"
else
    EXTRA="-DEMUSUNI_N64=OFF"
fi
bash "$HERE/build-gba-core.sh" "$BUILD/gba"
GBA_OBJECT="$BUILD/gba/gba_core.o"
EXTRA="$EXTRA -DGBA_CORE_OBJECT=$GBA_OBJECT"
source "$HERE/ps4-env.sh"
# From scratch also when the Nintendo 64 comes in or goes out.
if [ "$SETUP" = 1 ] || [ ! -f "$BUILD/build.ninja" ] || \
   ! grep -q "^EMUSUNI_N64:BOOL=$N64\$" "$BUILD/CMakeCache.txt"; then
    rm -rf "$BUILD/CMakeFiles" "$BUILD/CMakeCache.txt" "$BUILD/build.ninja"
    ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/toolchain/ps4-love-modern.cmake" \
        -DEMUSUNI_VERSION="${EMUSUNI_VERSION:-00.10}" \
        -DPS4_PAID="${PS4_PAID:-0x3800000000000035}" $EXTRA
else
    # A new version number, or the GBA core's object not yet in the cache (a build directory from
    # before mGBA): configure again, without building everything again.
    AGAIN=()
    if [ -n "${EMUSUNI_VERSION:-}" ] && \
       ! grep -q "^EMUSUNI_VERSION:STRING=${EMUSUNI_VERSION}\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DEMUSUNI_VERSION="$EMUSUNI_VERSION")
    fi
    if ! grep -q "^GBA_CORE_OBJECT:FILEPATH=$GBA_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DGBA_CORE_OBJECT="$GBA_OBJECT")
    fi
    if [ ${#AGAIN[@]} -gt 0 ]; then
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS cmake "${AGAIN[@]}" "$BUILD" > /dev/null
    fi
fi
ninja -C "$BUILD"
ls -la "$BUILD/emusuni_eboot/eboot.bin"
