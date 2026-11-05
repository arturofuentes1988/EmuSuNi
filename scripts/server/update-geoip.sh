#!/usr/bin/env bash
# The country database of emusuni-netd's anonymous statistics: DB-IP's IP to Country Lite (https://db-ip.com, CC BY 4.0
# - "IP Geolocation by DB-IP"; its CSV: "first,last,CC" a line, IPv4 and IPv6), downloaded once a month by
# emusuni-netd-geoip.timer (deploy.sh). This month's file, else last month's (a month's comes out in its first days).
# The server reads it again by itself when the file changes; an old one stays when the download fails.
#   update-geoip.sh [folder]   (/var/lib/emusuni-netd)
set -euo pipefail
DIR=${1:-/var/lib/emusuni-netd}
OUT=$DIR/dbip-country-lite.csv
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for MONTH in "$(date -u +%Y-%m)" "$(date -u -d "$(date -u +%Y-%m-15) -1 month" +%Y-%m)"; do
  if curl -fsSL -m 300 -o "$TMP/db.csv.gz" "https://download.db-ip.com/free/dbip-country-lite-$MONTH.csv.gz" &&
     gunzip -c "$TMP/db.csv.gz" > "$TMP/db.csv"; then
    RANGES=$(grep -c '^[0-9]*\.[0-9]*\.[0-9]*\.[0-9]*,' "$TMP/db.csv" || true)
    if [ "${RANGES:-0}" -gt 100000 ]; then
      cp "$TMP/db.csv" "$OUT.new"
      chmod 640 "$OUT.new"
      mv -f "$OUT.new" "$OUT"
      echo "DB-IP's IP to Country Lite $MONTH: $RANGES IPv4 ranges -> $OUT"
      exit 0
    fi
  fi
done
echo "DB-IP's IP to Country Lite couldn't be downloaded: the file of before stays" >&2
exit 1
