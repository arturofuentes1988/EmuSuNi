#!/usr/bin/env bash
# The game titles of emusuni-netd's statistics (the public page's "top games", the public rooms' games): the app sends
# a game as a checksum or a code, never its file's name, and the server shows a title only from these lists -
# libretro-database's (https://github.com/libretro/libretro-database, CC BY-SA 4.0: No-Intro's cartridges, Redump's
# discs) and FinalBurn Neo's sets. Downloaded and made into titles.txt (emusuni-netd build-titles) by deploy.sh, then
# monthly with the country database (emusuni-netd-geoip.service). The lists stay on the server: nothing of them is
# in the app or the package.
#   update-titles.sh [folder]   (/var/lib/emusuni-netd: titles.txt)
set -euo pipefail
DIR=${1:-/var/lib/emusuni-netd}
BIN=${EMUSUNI_NETD:-/opt/emusuni-netd/emusuni-netd}
BASE=https://raw.githubusercontent.com/libretro/libretro-database/master/metadat
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SPECS=()
fetch() {  # console kind path-in-metadat
  local file="$TMP/$1-$(basename "$3" | tr ' ' '_')"
  if curl -fsSL -m 300 -o "$file" "$BASE/$(printf '%s' "$3" | sed 's/ /%20/g')"; then
    SPECS+=("$1:$2:$file")
  else
    echo "not downloaded (its games stay without a title): $3" >&2
  fi
}
fetch nes c "no-intro/Nintendo - Nintendo Entertainment System.dat"
fetch nes c "no-intro/Nintendo - Family Computer Disk System.dat"
fetch snes c "no-intro/Nintendo - Super Nintendo Entertainment System.dat"
fetch n64 c "no-intro/Nintendo - Nintendo 64.dat"
fetch gb c "no-intro/Nintendo - Game Boy.dat"
fetch gbc c "no-intro/Nintendo - Game Boy Color.dat"
fetch gba c "no-intro/Nintendo - Game Boy Advance.dat"
fetch sg1000 c "no-intro/Sega - SG-1000.dat"
fetch sms c "no-intro/Sega - Master System - Mark III.dat"
fetch gg c "no-intro/Sega - Game Gear.dat"
fetch md c "no-intro/Sega - Mega Drive - Genesis.dat"
fetch pce c "no-intro/NEC - PC Engine - TurboGrafx 16.dat"
fetch ps1 d "redump/Sony - PlayStation.dat"
fetch psp d "redump/Sony - PlayStation Portable.dat"
fetch arcade s "fbneo-split/FBNeo - Arcade Games.dat"
[ "${#SPECS[@]}" -gt 0 ] || { echo "no list downloaded: titles.txt stays as it was" >&2; exit 1; }
"$BIN" build-titles "$DIR/titles.txt" "${SPECS[@]}"
chmod 640 "$DIR/titles.txt"
