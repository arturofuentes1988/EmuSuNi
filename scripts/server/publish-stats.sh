#!/usr/bin/env bash
# The public page's numbers, pushed every 5 minutes by emusuni-netd-publish.timer to the public statistics repository
# (arturofuentes1988/emusuni-stats), which the page (site/, GitHub Pages) reads from raw.githubusercontent.com:
#   stats.json      emusuni-netd public-json: aggregates only - no id, address, nickname, room code or file name; a
#                   game's title only from the lists and only once 3 installs played it
#   rankings.json   emusuni-netd public-profiles: the public profiles' nicknames, countries and hours
#   p/<name>.bin    one file an opt-in profile, its name and bytes made from its code (only the code opens it)
# Each push is ONE commit with no parent, forced: the repository keeps no history and never grows. The work tree is
# kept between runs, so a push sends only the files that changed (a profile not played since: nothing).
# Its SSH deploy key (deploy.sh made it: /var/lib/emusuni-netd/publish/deploy_key) writes to that repository only.
# No port is opened: an outgoing SSH connection to github.com, its host keys from GitHub's API (deploy.sh).
#   publish-stats.sh [--dry-run]   (env: EMUSUNI_STATS_REPO=git@github.com:owner/name.git, EMUSUNI_STATE=folder)
set -euo pipefail
STATE=${EMUSUNI_STATE:-/var/lib/emusuni-netd}
REPO=${EMUSUNI_STATS_REPO:-git@github.com:arturofuentes1988/emusuni-stats.git}
BIN=${EMUSUNI_NETD:-/opt/emusuni-netd/emusuni-netd}
KEY=$STATE/publish/deploy_key
WORK=$STATE/publish/work
mkdir -p "$WORK"
"$BIN" public-json --stats-dir "$STATE/stats" --titles "$STATE/titles.txt" > "$WORK/stats.json.new"
[ "$(wc -c < "$WORK/stats.json.new")" -gt 200 ] || { echo "public-json gave nothing: not published" >&2; exit 1; }
mv -f "$WORK/stats.json.new" "$WORK/stats.json"
"$BIN" public-profiles --stats-dir "$STATE/stats" --titles "$STATE/titles.txt" --out "$WORK"
cat > "$WORK/README.md" << 'TXT'
# EmuSuNi's public statistics

The numbers of EmuSuNi's page (https://emusuni.com), made every 5 minutes by its online
server from the app's anonymous statistics (see EmuSuNi's README, "Anonymous statistics"):

- `stats.json`: aggregates only.
- `rankings.json`: the profiles their players made public (nickname, country, hours).
- `p/`: the opt-in online profiles, one file each, readable only with that profile's code.

This repository keeps one commit: each update replaces it.

Countries: IP Geolocation by DB-IP (https://db-ip.com, CC BY 4.0). Game titles: libretro-database (No-Intro,
Redump; CC BY-SA 4.0) and FinalBurn Neo.
TXT
if [ "${1:-}" = "--dry-run" ]; then
  echo "dry run: $WORK ($(du -sh "$WORK" --exclude=.git | cut -f1)), not pushed to $REPO"
  exit 0
fi
[ -f "$KEY" ] || { echo "no deploy key ($KEY): deploy.sh makes it" >&2; exit 1; }
cd "$WORK"
[ -d .git ] || git init -q
printf 'push.log\n.last-gc\n*.new\n*.tmp\n' > .git/info/exclude
git add -A
TREE=$(git write-tree)
COMMIT=$(git -c user.name="EmuSuNi statistics" -c user.email="stats@emusuni.invalid" commit-tree "$TREE" \
  -m "EmuSuNi public statistics $(date -u +%Y-%m-%dT%H:%MZ)")
HOSTS=$STATE/publish/known_hosts
CHECK="-o StrictHostKeyChecking=yes"
[ -s "$HOSTS" ] || CHECK="-o StrictHostKeyChecking=accept-new"
export GIT_SSH_COMMAND="ssh -i $KEY -o IdentitiesOnly=yes -o BatchMode=yes -o UserKnownHostsFile=$HOSTS $CHECK"
if ! git push -q --force "$REPO" "$COMMIT:refs/heads/main" 2> push.log; then
  echo "the push to $REPO failed (is the deploy key added to it, with write access?): $(tail -2 push.log)" >&2
  exit 1
fi
# (The pushed commit kept as a branch: the next push sends only what changed since. The older ones, unreachable,
# are swept once a day.)
git update-ref refs/heads/published "$COMMIT"
if [ ! -f .last-gc ] || [ -n "$(find .last-gc -mmin +1440 2> /dev/null)" ]; then
  git reflog expire --expire=now --all 2> /dev/null || true
  git gc -q --prune=now 2> /dev/null || true
  touch .last-gc
fi
echo "published $(wc -c < stats.json) bytes of stats.json, $(ls p 2> /dev/null | wc -l) profiles to $REPO"
