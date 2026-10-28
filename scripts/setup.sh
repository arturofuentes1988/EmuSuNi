#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# One-time build environment on Ubuntu 24.04 (WSL2 works), as root:
#   - clang 21 (apt.llvm.org), CMake, Ninja, Python, glslang
#   - PacBrew's OpenOrbis PS4 toolchain (/opt/pacbrew/ps4/openorbis), with OpenSSL 1.1 for its
#     PkgTool.Core
#   - DolphinPS4's PS4 runtime: libc++ 21 and Mesa's RADV Vulkan driver on GNM
#     (~/dolphinps4-build), built with its own scripts
#   - Snes9x 1.63, FCEUmm (libretro-fceumm), Mupen64Plus-Next, mGBA (libretro's fork), SwanStation,
#     melonDS DS (with its dependencies in ~/src/melonds-ds-deps), PPSSPP 1.20.4 (with its
#     submodules), Flycast 2.7 (with the submodules its libretro core needs), Genesis Plus GX,
#     FinalBurn Neo and Dear ImGui 1.92.2b sources (~/src), and Azahar 2126.2's unified source
#     (~/src/azahar: every build but build.sh no-3ds uses it); YabaSanshiro 3.4.2 (~/src/yabasanshiro: only
#     build.sh with-saturn uses it); Beetle PCE Fast (~/src/beetle-pce-fast-libretro)
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
SWANSTATION_COMMIT=e436e9b85374def58d211df7f78880f0f4a917b4   # libretro/swanstation, 2026-10-15
MELONDSDS_COMMIT=f394adbacb5722ee97c1b37c8064da9a25818310   # JesseTG/melonds-ds, release 1.4.0
# What melonDS DS's CMake would download (cmake/FetchDependencies.cmake), at the commits its
# version names: scripts/build-nds-core.sh builds them from these clones.
MELONDSDS_DEPS="
melonDS          https://github.com/JesseTG/melonDS                16f127dbb587a73371827ce79689c5d237c4b59b
libretro-common  https://github.com/libretro/libretro-common       fa8a1b572a444fcb552007019f8caf68281108aa
embed-binaries   https://github.com/andoalon/embed-binaries        078b62beba97e8192c99bfb16d5e17220cfc7598
glm              https://github.com/g-truc/glm                     6f14f4792a0cde5d0cf2c910506724d61cb95834
libslirp         https://github.com/JesseTG/libslirp-mirror        e61dbd459c8c06607b3a84694489427e8ec60f17
pntr             https://github.com/robloach/pntr                  a47fe167703b0ac3bfad00785b20b0dce6712509
fmt              https://github.com/fmtlib/fmt                     1be298e1bd68957e4cd352e1f676f00e07dcfb57
yamc             https://github.com/yohhoy/yamc                    4e015a7e8eb0d61c34e6928676c8c78881a72d73
span-lite        https://github.com/martinmoene/span-lite          f63fe2974943b5f555b928b10d910e457fbb93d4
date             https://github.com/HowardHinnant/date             179a6b921ccd6179b5b04dce812a89f71a51b4df
zlib             https://github.com/madler/zlib                    da607da739fa6047df13e66a2af6b8bec7c2a498
"
PPSSPP_COMMIT=fa50bb1976065c4f8b1b47af227d367fe9771555   # hrydgard/ppsspp, release v1.20.4
# The submodules its libretro core is built from (at the commits v1.20.4 pins; ffmpeg is PPSSPP's
# FFmpeg 3.0.2, with prebuilt libraries of other platforms: ~1.5 GB), pspautotests for the PC tests.
# Not the iOS/macOS/Android-only ones nor the web debugger.
PPSSPP_SUBMODULES="libretro/libretro-common ffmpeg ext/armips ext/glslang ext/SPIRV-Cross ext/zstd
  ext/libchdr ext/rcheevos ext/lua ext/cpu_features ext/aemu_postoffice ext/rapidjson ext/naett
  ext/miniupnp ext/discord-rpc ext/freetype ext/nanosvg ext/OpenXR-SDK pspautotests"
FLYCAST_COMMIT=5aa091fde632fb332c8d8c34e280d62dc951954c   # flyinghead/flycast, release v2.7
# The submodules its libretro core is built from (at the commits v2.7 pins; tinygettext's own
# tinycmmc too, which its CMake needs). Not SDL, breakpad, Lua, rcheevos and the other ones of
# Flycast's own (standalone) builds.
FLYCAST_SUBMODULES="core/deps/libchdr core/deps/Vulkan-Headers core/deps/VulkanMemoryAllocator
  core/deps/glslang core/deps/xbyak core/deps/asio core/deps/tinygettext"
GPGX_COMMIT=aa74d79634cd230f65cce4180e92688e53883d2f   # libretro/Genesis-Plus-GX, 2026-10-16 (v1.7.4)
# The romsets must match this commit's DATs (dats/, v1.0.0.03): a newer FBNeo wants newer romsets.
FBNEO_COMMIT=2dbbac7623fbae1c7894a6437769e8a3adb78610   # libretro/FBNeo, 2026-10-19 (v1.0.0.03)
# Azahar (Nintendo 3DS): the release's unified source tarball (every dependency in it; GPL-2.0-or-later up to
# this version), checked by its SHA-256. scripts/build-3ds-core.sh patches it, so it is committed once.
AZAHAR_VERSION=2126.2   # azahar-emu/azahar, 2026-10-08 (GIT-COMMIT 219d743a1e698101fe1ab8bef43e6563126c3c27)
AZAHAR_SHA256=85c8b56fac703df04720125044b8ff887997911858a6cde545dfb1107ba4cd26
# YabaSanshiro 3.4.2 (Sega Saturn): only for build.sh with-saturn (00.64's speed test, scripts/build-saturn-core.sh).
YABASANSHIRO_COMMIT=09ed8e5b2e97e7a848ea2514545c34c7b809e399   # libretro/yabause, branch yabasanshiro, 2026-08-23
# Beetle PCE Fast (the PC Engine, 00.82; a Mednafen core, GPL-2.0, with its own libchdr, zlib and Tremor in deps/),
# built by scripts/build-beetle-cores.sh. (00.80 to 00.82 also pinned PicoDrive - the 32X - and Beetle NeoPop and Cygne
# - the Neo Geo Pockets, the WonderSwans -: gone in 00.83 with those consoles.)
BEETLE_PCE_FAST_COMMIT=4eee99c451f85ebe66d1591de88b892015b23059   # libretro/beetle-pce-fast-libretro, 2026-10-23
# RetroAchievements (00.73): rcheevos, its own library (MIT), and libchdr (BSD-3) for hashing CHD images.
RCHEEVOS_COMMIT=1433173220a7eaede6a9ed7a18e94117be1821e0   # RetroAchievements/rcheevos, release v12.5.0, 2026-09-13
LIBCHDR_COMMIT=47d794151b2b64f6aba60304e6480f4f5a2bc24b    # rtissera/libchdr, 2026-10-11
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

step "Snes9x, FCEUmm, Mupen64Plus-Next, mGBA, SwanStation, melonDS DS, PPSSPP, Flycast, Genesis Plus GX, FinalBurn Neo, Azahar and Dear ImGui"
checkout https://github.com/snes9xgit/snes9x.git "$SRC_ROOT/snes9x" "$SNES9X_COMMIT"
checkout https://github.com/libretro/libretro-fceumm.git "$SRC_ROOT/fceumm" "$FCEUMM_COMMIT"
checkout https://github.com/libretro/mupen64plus-libretro-nx.git "$SRC_ROOT/mupen64plus-next" "$MUPEN64PLUS_COMMIT"
# mGBA (Game Boy, Game Boy Color, Game Boy Advance): built with its own CMake by
# scripts/build-gba-core.sh (CMake, Ninja and clang 21 from above; nothing else).
checkout https://github.com/libretro/mgba.git "$SRC_ROOT/mgba" "$MGBA_COMMIT"
# SwanStation (PlayStation): built with its own CMake by scripts/build-ps1-core.sh, which patches
# it each time (nothing else to install).
checkout https://github.com/libretro/swanstation.git "$SRC_ROOT/swanstation" "$SWANSTATION_COMMIT"
# melonDS DS (Nintendo DS): built with its own CMake by scripts/build-nds-core.sh, which patches it
# (and melonDS, libretro-common) each time; its dependencies from ~/src/melonds-ds-deps, not downloaded
# by CMake.
checkout https://github.com/JesseTG/melonds-ds.git "$SRC_ROOT/melonds-ds" "$MELONDSDS_COMMIT"
echo "$MELONDSDS_DEPS" | while read -r name url commit; do
    [ -n "$name" ] && checkout "$url" "$SRC_ROOT/melonds-ds-deps/$name" "$commit"
done
# PPSSPP (PSP): its libretro core built with its own CMake by scripts/build-psp-core.sh, which
# patches it each time and builds its FFmpeg first; the submodules at the commits it pins.
checkout https://github.com/hrydgard/ppsspp.git "$SRC_ROOT/ppsspp" "$PPSSPP_COMMIT"
# shellcheck disable=SC2086
git -C "$SRC_ROOT/ppsspp" submodule update --init --depth 1 -- $PPSSPP_SUBMODULES
git -C "$SRC_ROOT/ppsspp/ext/armips" submodule update --init --depth 1 -- ext/filesystem
# Flycast (Dreamcast): its libretro core built with its own CMake by scripts/build-dc-core.sh, which
# patches it each time; the submodules at the commits it pins.
checkout https://github.com/flyinghead/flycast.git "$SRC_ROOT/flycast" "$FLYCAST_COMMIT"
# shellcheck disable=SC2086
git -C "$SRC_ROOT/flycast" submodule update --init --depth 1 -- $FLYCAST_SUBMODULES
git -C "$SRC_ROOT/flycast/core/deps/tinygettext" submodule update --init --depth 1
# Genesis Plus GX (SG-1000, Master System, Game Gear, Mega Drive): its libretro core built by
# scripts/build-gpgx-core.sh (a CMake of its own made there), which patches it each time.
checkout https://github.com/libretro/Genesis-Plus-GX.git "$SRC_ROOT/Genesis-Plus-GX" "$GPGX_COMMIT"
# FinalBurn Neo (Arcade): its libretro core built with its own Makefile by scripts/build-arcade-core.sh
# in a copy of the sources, which it patches each time (make and clang 21 from above; nothing else).
# scripts/package.sh ships its metadata/hiscore.dat.
checkout https://github.com/libretro/FBNeo.git "$SRC_ROOT/FBNeo" "$FBNEO_COMMIT"
# Azahar (Nintendo 3DS): its libretro core built with its own CMake by scripts/build-3ds-core.sh (unless
# build.sh no-3ds), which patches it each time from the files as git has them: the tarball is checked,
# unpacked and committed once (the submodules' files too, git add -A -f). Nothing else to install.
if ! git -C "$SRC_ROOT/azahar" rev-parse -q --verify HEAD >/dev/null 2>&1; then
    wget -q "https://github.com/azahar-emu/azahar/releases/download/$AZAHAR_VERSION/azahar-unified-source-$AZAHAR_VERSION.tar.xz" \
        -O /tmp/azahar-source.tar.xz
    echo "$AZAHAR_SHA256  /tmp/azahar-source.tar.xz" | sha256sum -c -
    rm -rf "$SRC_ROOT/azahar"
    mkdir -p "$SRC_ROOT/azahar"
    tar xJf /tmp/azahar-source.tar.xz -C "$SRC_ROOT/azahar" --strip-components=1
    rm -f /tmp/azahar-source.tar.xz
    git -C "$SRC_ROOT/azahar" init -q
    git -C "$SRC_ROOT/azahar" config core.autocrlf false
    git -C "$SRC_ROOT/azahar" add -A -f . >/dev/null
    git -C "$SRC_ROOT/azahar" -c user.name=EmuSuNi -c user.email=emusuni@localhost \
        commit -q -m "Azahar $AZAHAR_VERSION unified source (GIT-COMMIT $(cat "$SRC_ROOT/azahar/GIT-COMMIT"))"
fi
# YabaSanshiro (Sega Saturn, only build.sh with-saturn): built by scripts/build-saturn-core.sh (a CMake of its own
# made there, without a renderer), which patches it each time from the files as git has them.
checkout https://github.com/libretro/yabause.git "$SRC_ROOT/yabasanshiro" "$YABASANSHIRO_COMMIT"
# Beetle PCE Fast (the PC Engine): built by scripts/build-beetle-cores.sh (a CMake of its own made there), which
# patches it each time from the files as git has them.
checkout https://github.com/libretro/beetle-pce-fast-libretro.git "$SRC_ROOT/beetle-pce-fast-libretro" "$BEETLE_PCE_FAST_COMMIT"
checkout https://github.com/ocornut/imgui.git "$SRC_ROOT/imgui" "$IMGUI_COMMIT"
# RetroAchievements (00.73): built by CMakeLists.txt as they are (no patches).
checkout https://github.com/RetroAchievements/rcheevos.git "$SRC_ROOT/rcheevos" "$RCHEEVOS_COMMIT"
checkout https://github.com/rtissera/libchdr.git "$SRC_ROOT/libchdr" "$LIBCHDR_COMMIT"
# Mbed TLS 2.16.6, PacBrew's source tarball (its PKGBUILD's URL and SHA-256): scripts/build-mbedtls.sh rebuilds its
# bignum.c with upstream's fix (00.73: HTTPS to RSA servers).
MBEDTLS_SHA256=66455e23a6190a30142cdc1113f7418158839331a9d8e6b0778631d077281770
if ! echo "$MBEDTLS_SHA256  $SRC_ROOT/mbedtls-2.16.6-apache.tgz" | sha256sum -c --quiet - 2>/dev/null; then
    wget -q https://sources.buildroot.net/mbedtls/mbedtls-2.16.6-apache.tgz -O /tmp/mbedtls-2.16.6-apache.tgz
    echo "$MBEDTLS_SHA256  /tmp/mbedtls-2.16.6-apache.tgz" | sha256sum -c -
    mv /tmp/mbedtls-2.16.6-apache.tgz "$SRC_ROOT/mbedtls-2.16.6-apache.tgz"
fi

step "Ready: now scripts/build.sh and scripts/package.sh <version>"
