#!/usr/bin/env bash
# Packages the built eboot.bin with the assets into a PS4 fake PKG (out/).
#   package.sh [version XX.YY]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
VERSION="${1:-${EMUSUNI_VERSION:-00.10}}"
BUILD="${EMUSUNI_BUILD:-$HOME/emusuni-build}"
TITLE_ID=EMSN00001; TITLE="EmuSuNi"; LABEL=EMUSUNI
STAGE="$BUILD/stage"
OUT="$BUILD/out"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys" "$STAGE/xmb" "$OUT"
cp "$BUILD/emusuni_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$SRC/sce_sys/icon0.png" "$STAGE/sce_sys/icon0.png"
[ -f "$SRC/sce_sys/pic1.png" ] && cp "$SRC/sce_sys/pic1.png" "$STAGE/sce_sys/pic1.png"
[ -f "$SRC/sce_sys/pic0.png" ] && cp "$SRC/sce_sys/pic0.png" "$STAGE/sce_sys/pic0.png"
cp -r "$SRC/assets/xmb/." "$STAGE/xmb/"
rm -rf "$STAGE/xmb/lang/src" "$STAGE/xmb/lang/keys.txt"
# (00.90) xmb's small files in one, xmb/assets.zip: each file takes 64 KiB of the package at least, and these took 3.6
# MB more than their bytes. src/AssetPack.h reads them from it (a loose file first). (01.26) Deflated where that makes a
# file smaller: the languages and the ratings' indexes 4 MB less. (01.27) The fonts' .ttf too (src/Menu.cpp and
# src/PS4Lang.cpp read them through AssetPack): 1.6 MB less; EMUSUNI_FONTS_LOOSE=1 leaves them loose, as before. Their
# licences, the logo, the QR code, cacert.pem and netplay stay loose.
PACK=(bezels ratings lang sounds)
[ "${EMUSUNI_FONTS_LOOSE:-0}" = 1 ] || PACK+=("fonts/*.ttf")
python3 -I "$HERE/make-asset-pack.py" "$STAGE/xmb" "${PACK[@]}"
# The PSP core's own files (PPSSPP's: the PSP's fonts, its system dialogs, compat.ini...), which it
# reads from /app0/ppsspp/PPSSPP (src/Emulator.cpp: its system folder) and never writes.
PPSSPP_ASSETS="${PPSSPP_DIR:-$HOME/src/ppsspp}/assets"
if [ -d "$PPSSPP_ASSETS" ]; then
    mkdir -p "$STAGE/ppsspp/PPSSPP"
    for f in compat.ini compatvr.ini knownfuncs.ini langregion.ini infra-dns.json ppge_atlas.zim ppge_atlas.meta \
             flash0 vfpu; do
        cp -r "$PPSSPP_ASSETS/$f" "$STAGE/ppsspp/PPSSPP/"
    done
    # (00.90) Of lang and shaders only what the app can make it read (each file takes 64 KiB of the package at
    # least; the 84 left out took 7 MB): its dialogs' languages, those src/Emulator.cpp's BuildPspVariables can give
    # it (the others are English; Chinese kept for when it gives it), and the texture shaders Settings offers (PSP
    # Texture Upscaling: 2xBRZ, 4xBRZ, MMPX). The post-processing shaders are never set. A file missing here (a newer
    # PPSSPP renamed it) stops the packaging.
    mkdir -p "$STAGE/ppsspp/PPSSPP/lang" "$STAGE/ppsspp/PPSSPP/shaders"
    for f in en_US es_ES fr_FR de_DE it_IT pt_PT ja_JP ru_RU ko_KR nl_NL zh_CN zh_TW; do
        cp "$PPSSPP_ASSETS/lang/$f.ini" "$STAGE/ppsspp/PPSSPP/lang/"
    done
    for f in defaultshaders.ini tex_2xbrz.csh tex_4xbrz.csh tex_mmpx.csh; do
        cp "$PPSSPP_ASSETS/shaders/$f" "$STAGE/ppsspp/PPSSPP/shaders/"
    done
    # (01.27) All of them in one zip, /app0/ppsspp/PPSSPP.zip (deflated where that makes a file smaller), which the
    # PSP core reads first (scripts/build-psp-core.sh, as PPSSPP on Android its APK's assets): 2 MB less. Only with a
    # core that reads it (its log line in the eboot); EMUSUNI_PPSSPP_LOOSE=1 leaves the files loose, as before.
    if [ "${EMUSUNI_PPSSPP_LOOSE:-0}" = 1 ]; then
        echo "PPSSPP's files loose (EMUSUNI_PPSSPP_LOOSE=1)"
    elif ! LC_ALL=C grep -q -a -F "EmuSuNi: PPSSPP.zip" "$STAGE/eboot.bin"; then
        echo "PPSSPP's files loose: this eboot's PSP core doesn't read PPSSPP.zip"
    else
        mapfile -t PPSSPP_FILES < <(ls "$STAGE/ppsspp/PPSSPP")
        python3 -I "$HERE/make-asset-pack.py" -o "$STAGE/ppsspp/PPSSPP.zip" "$STAGE/ppsspp/PPSSPP" "${PPSSPP_FILES[@]}"
        rmdir "$STAGE/ppsspp/PPSSPP"
    fi
fi
# The licences that must ship with their code, verbatim (FinalBurn Neo's), from assets/licenses.
cp -r "$SRC/assets/licenses" "$STAGE/licenses"
# OpenBOR's (BSD-3) only in a build that has its core (build.sh with-openbor: its object in CMake's cache).
if ! grep -q "^OPENBOR_CORE_OBJECT:FILEPATH=." "$BUILD/CMakeCache.txt" 2>/dev/null; then
    rm -f "$STAGE/licenses/openbor-license.txt"
fi
# (01.68) MUGEN's engine files in /app0/mugen (Ikemen GO's own Lua scripts, common states and debug font, which the
# core takes before a game's copies: port/mugen/ikemen/emusuni_retro.go) and its licences (Ikemen GO's MIT, Go's and
# its modules'), only in a build that has its core (build.sh with-mugen): from beside its object, where
# scripts/build-mugen-core.sh puts them. No MUGEN content: a game brings its own.
MUGEN_OBJECT="$(grep "^MUGEN_CORE_OBJECT:FILEPATH=." "$BUILD/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
if [ -n "$MUGEN_OBJECT" ]; then
    MUGEN_OUT="$(dirname "$MUGEN_OBJECT")"
    if [ ! -f "$MUGEN_OUT/mugen-engine/external/script/main.lua" ] || [ ! -f "$MUGEN_OUT/ikemen-go-license.txt" ]; then
        echo "package: no mugen-engine or ikemen-go-license.txt beside $MUGEN_OBJECT: build-mugen-core.sh again" >&2
        exit 1
    fi
    cp -r "$MUGEN_OUT/mugen-engine" "$STAGE/mugen"
    cp "$MUGEN_OUT/ikemen-go-license.txt" "$STAGE/licenses/ikemen-go-license.txt"
fi
# FinalBurn Neo's high-score table (hiscore.dat: the games keep their scores, with Hiscores enabled),
# which the app copies to /data/EmuSuNi/system/fbneo at the first arcade game (src/Emulator.cpp).
FBNEO_METADATA="${FBNEO_DIR:-$HOME/src/FBNeo}/metadata"
if [ -f "$FBNEO_METADATA/hiscore.dat" ]; then
    mkdir -p "$STAGE/fbneo"
    cp "$FBNEO_METADATA/hiscore.dat" "$STAGE/fbneo/hiscore.dat"
fi
# (01.57) Mupen64Plus's cheat list (mupencheat.txt, GPL-2.0-or-later, 1.2 MB): the N64's cheats found by the
# cartridge's header, whatever its file's name (src/CheatDownload.h). From the core's own tree, as build-n64-core.sh's.
N64_CHEATS="${MUPEN64PLUS_DIR:-$HOME/src/mupen64plus-next}/mupen64plus-core/data/mupencheat.txt"
if [ -f "$N64_CHEATS" ]; then
    mkdir -p "$STAGE/n64"
    cp "$N64_CHEATS" "$STAGE/n64/mupencheat.txt"
else
    echo "package: no $N64_CHEATS - the N64's cheats by the cartridge are left out" >&2
fi
echo "$VERSION" > "$STAGE/xmb/version.txt"
# The Sony Piglet modules aren't used (the display is Mesa's RADV): an empty module folder.
mkdir -p "$BUILD/no-modules"
# Plain param.sfo values: the homebrew identity RADV needs (see build.sh).
SFO_STYLE=plain DOLPHIN_PS4_MODULES_DIR="$BUILD/no-modules" "$HERE/make-pkg.sh" "$STAGE" "$TITLE_ID" "$TITLE" "$VERSION" "$LABEL" "$OUT"
ls -la "$OUT"
