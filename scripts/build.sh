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
# <build>/psp), the Dreamcast core, Flycast (/root/src/flycast), by scripts/build-dc-core.sh (in
# <build>/dc), the SG-1000 / Master System / Game Gear / Mega Drive core, Genesis Plus GX
# (/root/src/Genesis-Plus-GX), by scripts/build-gpgx-core.sh (in <build>/sega), and the Arcade core,
# FinalBurn Neo (/root/src/FBNeo), by scripts/build-arcade-core.sh (in <build>/arcade), and the Nintendo
# 3DS core, Azahar (/root/src/azahar), by scripts/build-3ds-core.sh (in <build>/3ds; experimental, in every
# build since 00.62: its ~27 MB of code and data come out of the PS4's flexible memory, which the other
# cores need too - about 93 MiB of it is left free at display ready with it, 119 without: no-3ds leaves it
# out; its recompilers' 64 MiB are mapped from the system's own flexible memory when a 3DS game starts). Only
# with with-saturn: the Sega Saturn's speed test (00.64), YabaSanshiro (/root/src/yabasanshiro) without a renderer,
# by scripts/build-saturn-core.sh (in <build>/saturn): shown in Settings only with the file /data/EmuSuNi/saturn-test.
#   build.sh [setup] [no-n64] [no-ps1] [no-nds] [no-psp] [no-dc] [no-sega] [no-arcade] [no-3ds] [with-saturn]
#                                                 (setup: configure from scratch; no-n64: without
#                                                 the Nintendo 64; no-ps1: without the PlayStation;
#                                                 no-nds: without the Nintendo DS; no-psp: without
#                                                 the PSP; no-dc: without the Dreamcast; no-sega:
#                                                 without the SG-1000, Master System, Game Gear and
#                                                 Mega Drive; no-arcade: without the Arcade;
#                                                 no-3ds: without the Nintendo 3DS; with-3ds, as
#                                                 before 00.62, is the default now; with-saturn: with
#                                                 the Saturn's speed test, out by default)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
SETUP=0
N64=ON
PS1=ON
NDS=ON
PSP=ON
DC=ON
SEGA=ON
ARCADE=ON
THREEDS=ON
SATURN=OFF
for arg in "$@"; do
    case "$arg" in
        setup) SETUP=1 ;;
        no-n64) N64=OFF ;;
        no-ps1) PS1=OFF ;;
        no-nds) NDS=OFF ;;
        no-psp) PSP=OFF ;;
        no-dc) DC=OFF ;;
        no-sega) SEGA=OFF ;;
        no-arcade) ARCADE=OFF ;;
        no-3ds) THREEDS=OFF ;;
        with-3ds) THREEDS=ON ;;
        with-saturn) SATURN=ON ;;
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
if [ "$SEGA" = ON ]; then
    bash "$HERE/build-gpgx-core.sh" "$BUILD/sega"
    SEGA_OBJECT="$BUILD/sega/sega_core.o"
else
    SEGA_OBJECT=""
fi
EXTRA="$EXTRA -DSEGA_CORE_OBJECT=$SEGA_OBJECT"
if [ "$ARCADE" = ON ]; then
    bash "$HERE/build-arcade-core.sh" "$BUILD/arcade"
    ARCADE_OBJECT="$BUILD/arcade/arcade_core.o"
else
    ARCADE_OBJECT=""
fi
EXTRA="$EXTRA -DARCADE_CORE_OBJECT=$ARCADE_OBJECT"
if [ "$THREEDS" = ON ]; then
    bash "$HERE/build-3ds-core.sh" "$BUILD/3ds"
    THREEDS_OBJECT="$BUILD/3ds/3ds_core.o"
else
    THREEDS_OBJECT=""
fi
EXTRA="$EXTRA -DTHREEDS_CORE_OBJECT=$THREEDS_OBJECT"
if [ "$SATURN" = ON ]; then
    bash "$HERE/build-saturn-core.sh" "$BUILD/saturn"
    SATURN_OBJECT="$BUILD/saturn/saturn_core.o"
else
    SATURN_OBJECT=""
fi
EXTRA="$EXTRA -DSATURN_CORE_OBJECT=$SATURN_OBJECT"
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
    # mGBA, SwanStation, melonDS DS, PPSSPP, Flycast, Genesis Plus GX, FinalBurn Neo or Azahar):
    # configure again, without building everything again.
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
    # The Sega consoles' core's object in or out (no-sega), or a build directory from before it.
    if ! grep -q "^SEGA_CORE_OBJECT:FILEPATH=$SEGA_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DSEGA_CORE_OBJECT="$SEGA_OBJECT")
    fi
    # The Arcade core's object in or out (no-arcade), or a build directory from before it.
    if ! grep -q "^ARCADE_CORE_OBJECT:FILEPATH=$ARCADE_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DARCADE_CORE_OBJECT="$ARCADE_OBJECT")
    fi
    # The Nintendo 3DS core's object in or out (no-3ds), or a build directory from before it.
    if ! grep -q "^THREEDS_CORE_OBJECT:FILEPATH=$THREEDS_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DTHREEDS_CORE_OBJECT="$THREEDS_OBJECT")
    fi
    # The Saturn's speed test's object in (with-saturn) or out, or a build directory from before it.
    if ! grep -q "^SATURN_CORE_OBJECT:FILEPATH=$SATURN_OBJECT\$" "$BUILD/CMakeCache.txt"; then
        AGAIN+=(-DSATURN_CORE_OBJECT="$SATURN_OBJECT")
    fi
    if [ ${#AGAIN[@]} -gt 0 ]; then
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS cmake "${AGAIN[@]}" "$BUILD" > /dev/null
    fi
fi
# At most 4 jobs: the PC blue-screened once under a full load.
ninja -C "$BUILD" -j "${EMUSUNI_JOBS:-4}"
ls -la "$BUILD/emusuni_eboot/eboot.bin"
# The program is taken out of the PS4's flexible memory, the games' pool too, at load: every load segment of the
# OELF whole, its .bss included (MemSiz, not FileSiz: the free memory the owner's logs show at "display ready"
# fell by code, data and .bss each time cores came in: 00.32 -> 00.33, 00.33 -> 00.36, 00.46 -> 00.48, 00.55 ->
# 00.58). About 438 - MemSiz MiB of it is free at display ready (00.55: 287 MiB, 152 free; 00.59: 384, 54; 00.60:
# 319, 119); the heap's first line in xmb-trace.log reads about 6 more. Shown here, so that a build that grows it
# is noticed.
OELF="$BUILD/emusuni_eboot/emusuni.oelf"
if [ -f "$OELF" ] && command -v llvm-readelf-21 > /dev/null; then
    file=0
    memory=0
    # (LOAD: Offset VirtAddr PhysAddr FileSiz MemSiz...; the SCE relro segment, 0x61000010: type first, then the same.)
    while read -r kind a b c d e f rest; do
        case "$kind" in
            LOAD) file=$((file + d)); memory=$((memory + e)) ;;
            "<unknown>:") if [ "$a" = 0x61000010 ]; then file=$((file + e)); memory=$((memory + f)); fi ;;
        esac
    done < <(llvm-readelf-21 -lW "$OELF" 2>/dev/null) || true
    file_tenths=$((file * 10 / 1048576))
    memory_tenths=$((memory * 10 / 1048576))
    echo "image: $((file_tenths / 10)).$((file_tenths % 10)) MiB of file, $((memory_tenths / 10)).$((memory_tenths % 10)) MiB" \
         "of memory at load (MemSiz): about $(((4380 - memory_tenths + 5) / 10)) MiB of flexible memory free at display ready"
fi
