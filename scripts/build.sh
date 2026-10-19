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
# Game Boy Advance core, mGBA (/root/src/mgba), by scripts/build-gba-core.sh (in <build>/gba), the
# PlayStation core, SwanStation (/root/src/swanstation), by scripts/build-ps1-core.sh (in <build>/ps1),
# the Nintendo DS core, melonDS DS (/root/src/melonds-ds), by scripts/build-nds-core.sh (in
# <build>/nds), the PSP core, PPSSPP (/root/src/ppsspp), by scripts/build-psp-core.sh (in
# <build>/psp), and the Dreamcast core, Flycast (/root/src/flycast), by scripts/build-dc-core.sh (in
# <build>/dc).
#   build.sh [setup] [no-n64] [no-ps1] [no-nds] [no-psp] [no-dc]
#                                                 (setup: configure from scratch; no-n64: without
#                                                 the Nintendo 64; no-ps1: without the PlayStation;
#                                                 no-nds: without the Nintendo DS; no-psp: without
#                                                 the PSP; no-dc: without the Dreamcast)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
SETUP=0
N64=ON
PS1=ON
NDS=ON
PSP=ON
DC=ON
for arg in "$@"; do
    case "$arg" in
        setup) SETUP=1 ;;
        no-n64) N64=OFF ;;
        no-ps1) PS1=OFF ;;
        no-nds) NDS=OFF ;;
        no-psp) PSP=OFF ;;
        no-dc) DC=OFF ;;
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
if [ "$PS1" = ON ]; then
    bash "$HERE/build-ps1-core.sh" "$BUILD/ps1"
    PS1_OBJECT="$BUILD/ps1/ps1_core.o"
else
    PS1_OBJECT=""
fi
EXTRA="$EXTRA -DPS1_CORE_OBJECT=$PS1_OBJECT"
if [ "$NDS" = ON ]; then
    bash "$HERE/build-nds-core.sh" "$BUILD/nds"
    NDS_OBJECT="$BUILD/nds/nds_core.o"
else
    NDS_OBJECT=""
fi
EXTRA="$EXTRA -DNDS_CORE_OBJECT=$NDS_OBJECT"
if [ "$PSP" = ON ]; then
    bash "$HERE/build-psp-core.sh" "$BUILD/psp"
    PSP_OBJECT="$BUILD/psp/psp_core.o"
else
    PSP_OBJECT=""
fi
EXTRA="$EXTRA -DPSP_CORE_OBJECT=$PSP_OBJECT"
if [ "$DC" = ON ]; then
    bash "$HERE/build-dc-core.sh" "$BUILD/dc"
    DC_OBJECT="$BUILD/dc/dc_core.o"
else
    DC_OBJECT=""
fi
EXTRA="$EXTRA -DDC_CORE_OBJECT=$DC_OBJECT"
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
    # A new version number, or a core's object not yet in the cache (a build directory from before
    # mGBA, SwanStation, melonDS DS or PPSSPP): configure again, without building everything again.
    AGAIN=()
    if [ -n "${EMUSUNI_VERSION:-}" ] && \
       ! grep -q "^EMUSUNI_VERSION:STRING=${EMUSUNI_VERSION}\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DEMUSUNI_VERSION="$EMUSUNI_VERSION")
    fi
    if ! grep -q "^GBA_CORE_OBJECT:FILEPATH=$GBA_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DGBA_CORE_OBJECT="$GBA_OBJECT")
    fi
    # The PlayStation core's object in or out (no-ps1), or a build directory from before it.
    if ! grep -q "^PS1_CORE_OBJECT:FILEPATH=$PS1_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DPS1_CORE_OBJECT="$PS1_OBJECT")
    fi
    # The Nintendo DS core's object in or out (no-nds), or a build directory from before it.
    if ! grep -q "^NDS_CORE_OBJECT:FILEPATH=$NDS_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DNDS_CORE_OBJECT="$NDS_OBJECT")
    fi
    # The PSP core's object in or out (no-psp), or a build directory from before it.
    if ! grep -q "^PSP_CORE_OBJECT:FILEPATH=$PSP_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DPSP_CORE_OBJECT="$PSP_OBJECT")
    fi
    # The Dreamcast core's object in or out (no-dc), or a build directory from before it.
    if ! grep -q "^DC_CORE_OBJECT:FILEPATH=$DC_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DDC_CORE_OBJECT="$DC_OBJECT")
    fi
    if [ ${#AGAIN[@]} -gt 0 ]; then
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS cmake "${AGAIN[@]}" "$BUILD" > /dev/null
    fi
fi
# At most 4 jobs: the PC blue-screened once under a full load.
ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}"
ls -la "$BUILD/emusuni_eboot/eboot.bin"
