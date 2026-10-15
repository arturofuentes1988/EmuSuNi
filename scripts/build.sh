#!/usr/bin/env bash
# Builds EmuSuNi's eboot.bin in WSL (Ubuntu): clang 21, the PacBrew OpenOrbis toolchain in
# /opt/pacbrew/ps4/openorbis, libc++ 21 and Mesa's RADV built by DolphinPS4's scripts
# (toolchain/build-libcxx.sh, toolchain/build-runtime.sh, scripts/build-mesa.sh) in
# ~/dolphinps4-build, Snes9x in /root/src/snes9x and Dear ImGui 1.92.2b in /root/src/imgui.
# Vulkan (RADV on GNM) runs under the plain homebrew identity (program authority ID
# 0x3800000000000035, plain param.sfo: 4.5 GiB of direct memory), as DolphinPS4 packages it; the
# system identity (0x3100000000000002, RetroArch's) is only for Sony's OpenGL (Piglet).
#   build.sh [setup]   (setup: configure from scratch)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
BUILD="${EMUSUNI_BUILD:-$HOME/emusuni-build}"
source "$HERE/ps4-env.sh"
if [ "${1:-}" = setup ] || [ ! -f "$BUILD/build.ninja" ]; then
    rm -rf "$BUILD"
    ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/toolchain/ps4-love-modern.cmake" \
        -DEMUSUNI_VERSION="${EMUSUNI_VERSION:-00.10}" \
        -DPS4_PAID="${PS4_PAID:-0x3800000000000035}"
elif [ -n "${EMUSUNI_VERSION:-}" ] && \
     ! grep -q "^EMUSUNI_VERSION:STRING=${EMUSUNI_VERSION}\$" "$BUILD/CMakeCache.txt"; then
    # A new version number only: configure again, without building everything again.
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -DEMUSUNI_VERSION="$EMUSUNI_VERSION" "$BUILD" > /dev/null
fi
ninja -C "$BUILD"
ls -la "$BUILD/emusuni_eboot/eboot.bin"
