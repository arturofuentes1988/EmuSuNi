#!/usr/bin/env bash
# GitHub's download counts of EmuSuNi's releases (each one's .pkg), once a day into emusuni-netd's statistics
# (downloads.txt: the report shows each version's total and how many more in the days asked for). GitHub's public API,
# no token (60 requests an hour are allowed; this makes up to 5). Run daily by emusuni-netd-downloads.timer (deploy.sh).
#   record-downloads.sh [stats folder]   (/var/lib/emusuni-netd/stats; EMUSUNI_REPO=owner/name for another repository)
set -euo pipefail
REPO=${EMUSUNI_REPO:-arturofuentes1988/EmuSuNi}
DIR=${1:-/var/lib/emusuni-netd/stats}
ALL=""
for PAGE in 1 2 3 4 5; do
  BODY=$(curl -fsSL -m 60 -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/$REPO/releases?per_page=100&page=$PAGE")
  [ "$(printf '%s' "$BODY" | tr -d '[:space:]')" = "[]" ] && break
  ALL="$ALL$BODY"
done
printf '%s' "$ALL" | /opt/emusuni-netd/emusuni-netd record-downloads --stats-dir "$DIR"
