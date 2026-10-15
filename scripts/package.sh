#!/usr/bin/env bash
# Packages the built eboot.bin with the assets into a PS4 fake PKG (out/).
#   package.sh [version XX.YY]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
BUILD="${EMUSUNI_BUILD:-$HOME/emusuni-build}"
VERSION="${1:-${EMUSUNI_VERSION:-00.10}}"
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
echo "$VERSION" > "$STAGE/xmb/version.txt"
# The Sony Piglet modules aren't used (the display is Mesa's RADV): an empty module folder.
mkdir -p "$BUILD/no-modules"
# Plain param.sfo values: the homebrew identity RADV needs (see build.sh).
SFO_STYLE=plain DOLPHIN_PS4_MODULES_DIR="$BUILD/no-modules" "$HERE/make-pkg.sh" "$STAGE" EMSN00001 "EmuSuNi" "$VERSION" EMUSUNI "$OUT"
ls -la "$OUT"
