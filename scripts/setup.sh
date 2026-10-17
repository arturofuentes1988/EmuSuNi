#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# One-time build environment on Ubuntu 24.04 (WSL2 works), as root:
#   - clang 21 (apt.llvm.org), CMake, Ninja, Python, glslang
#   - PacBrew's OpenOrbis PS4 toolchain (/opt/pacbrew/ps4/openorbis), with OpenSSL 1.1 for its
#     PkgTool.Core
#   - DolphinPS4's PS4 runtime: libc++ 21 and Mesa's RADV Vulkan driver on GNM
#     (~/dolphinps4-build), built with its own scripts
#   - Snes9x 1.63, FCEUmm (libretro-fceumm), Mupen64Plus-Next, mGBA (libretro's fork) and Dear
#     ImGui 1.92.2b sources (~/src)
# Mesa takes the longest (about half an hour). Then: scripts/build.sh and scripts/package.sh.
#
#   sudo -i bash /path/to/EmuSuNi/scripts/setup.sh
set -euo pipefail

SRC_ROOT="$HOME/src"
BUILD_ROOT="${PS4_BUILD_ROOT:-$HOME/dolphinps4-build}"
OPENORBIS=/opt/pacbrew/ps4/openorbis
# The versions EmuSuNi is built and tested with.
DOLPHINPS4_COMMIT=720f60abc42d0d160ea19b81eb506de45fa0b17b
SNES9X_COMMIT=1bcc369e89f08243e0a462882fb1f3e42e51de3a   # Snes9x 1.63
IMGUI_COMMIT=45acd5e0e82f4c954432533ae9985ff0e1aad6d5    # Dear ImGui 1.92.2b
FCEUMM_COMMIT=ac0d00a7fc01166f84c7bac01d1084ebfd2b865e   # libretro-fceumm, 2026-10-11
MUPEN64PLUS_COMMIT=12edd2c74a517ff86dfa8cfc71ad75e4c10486d5   # mupen64plus-libretro-nx
MGBA_COMMIT=7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6   # libretro/mgba, 2026-09-16
MESA_TAG=mesa-26.0.8
LIBDRM_TAG=libdrm-2.4.125
export DEBIAN_FRONTEND=noninteractive

step() { printf '\n== %s\n' "$*"; }

# Clones a repository at one commit (shallow).
checkout() {
    local url=$1 dir=$2 commit=$3
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
}

step "Packages"
apt-get update -qq
apt-get install -y -qq git wget curl ca-certificates gnupg lsb-release xz-utils cmake ninja-build \
    python3 python3-venv python3-pil pkg-config bison flex glslang-tools libxml2 nasm make binutils >/dev/null
if ! command -v clang-21 >/dev/null; then
    wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    bash /tmp/llvm.sh 21
fi

step "OpenOrbis toolchain (PacBrew 1.0.0)"
if [ ! -d "$OPENORBIS" ]; then
    # The GitHub release: PacBrew's own package server is sometimes down.
    wget -q https://github.com/PacBrew/pacbrew-packages/releases/download/1.0.0/pacbrew-ps4-openorbis-1.0.0.tar.xz \
        -O /tmp/openorbis.tar.xz
    rm -rf /tmp/openorbis && mkdir -p /tmp/openorbis
    tar xJf /tmp/openorbis.tar.xz -C /tmp/openorbis
    mkdir -p "$(dirname "$OPENORBIS")"
    mv "$(find /tmp/openorbis -maxdepth 6 -type d -name openorbis | head -1)" "$OPENORBIS"
fi
# PkgTool.Core (which makes the .pkg) needs OpenSSL 1.1, gone from Ubuntu 24.04.
if [ ! -e /usr/lib/x86_64-linux-gnu/libssl.so.1.1 ]; then
    pool=http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/
    deb=$(curl -s "$pool" | grep -o 'libssl1.1_1.1.1f-1ubuntu2[^"]*_amd64.deb' | sort -u | tail -1)
    wget -q "$pool$deb" -O /tmp/libssl11.deb
    apt-get install -y -qq /tmp/libssl11.deb >/dev/null
fi

step "DolphinPS4: libc++ 21 and the PS4 runtime"
checkout https://github.com/iHaiDeeZ/DolphinPS4 "$SRC_ROOT/DolphinPS4" "$DOLPHINPS4_COMMIT"
cd "$SRC_ROOT/DolphinPS4"
chmod +x toolchain/*.sh scripts/*.sh 2>/dev/null || true
# The 2022 PacBrew C library only declares qsort_s(): Mesa has to use its own.
grep -q UHAVE_QSORT_S scripts/build-mesa.sh ||
    sed -i "s/'-D__ORBIS__', '-DPS4',/'-D__ORBIS__', '-DPS4', '-UHAVE_QSORT_S',/" scripts/build-mesa.sh
PS4_BUILD_ROOT="$BUILD_ROOT" toolchain/build-libcxx.sh
PS4_BUILD_ROOT="$BUILD_ROOT" toolchain/build-runtime.sh

step "Mesa RADV for the PS4 ($MESA_TAG)"
mkdir -p "$BUILD_ROOT/src" "$BUILD_ROOT/host-tools/bin"
if [ ! -d "$BUILD_ROOT/src/mesa/.git" ]; then
    git clone -q --depth 1 -b "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$BUILD_ROOT/src/mesa"
    git -C "$BUILD_ROOT/src/mesa" apply "$SRC_ROOT/DolphinPS4/patches/mesa-ps4.patch"
fi
[ -d "$BUILD_ROOT/src/libdrm" ] ||
    git clone -q --depth 1 -b "$LIBDRM_TAG" https://gitlab.freedesktop.org/mesa/drm.git "$BUILD_ROOT/src/libdrm"
if [ ! -x "$BUILD_ROOT/mesa-venv/bin/meson" ]; then
    python3 -m venv "$BUILD_ROOT/mesa-venv"
    "$BUILD_ROOT/mesa-venv/bin/pip" install -q meson mako pyyaml packaging
fi
ln -sf "$(command -v glslangValidator)" "$BUILD_ROOT/host-tools/bin/glslangValidator"
PS4_BUILD_ROOT="$BUILD_ROOT" scripts/build-mesa.sh setup

step "Snes9x, FCEUmm, Mupen64Plus-Next, mGBA and Dear ImGui"
checkout https://github.com/snes9xgit/snes9x.git "$SRC_ROOT/snes9x" "$SNES9X_COMMIT"
checkout https://github.com/libretro/libretro-fceumm.git "$SRC_ROOT/fceumm" "$FCEUMM_COMMIT"
checkout https://github.com/libretro/mupen64plus-libretro-nx.git "$SRC_ROOT/mupen64plus-next" "$MUPEN64PLUS_COMMIT"
# mGBA (Game Boy, Game Boy Color, Game Boy Advance): built with its own CMake by
# scripts/build-gba-core.sh (CMake, Ninja and clang 21 from above; nothing else).
checkout https://github.com/libretro/mgba.git "$SRC_ROOT/mgba" "$MGBA_COMMIT"
checkout https://github.com/ocornut/imgui.git "$SRC_ROOT/imgui" "$IMGUI_COMMIT"

step "Ready: now scripts/build.sh and scripts/package.sh <version>"
