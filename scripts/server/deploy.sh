#!/usr/bin/env bash
# Installs EmuSuNi's online play server (server/emusuni-netd.cpp) on a Linux VM: Ubuntu / Debian (apt) or Oracle Linux
# / RHEL / Fedora (dnf or yum), e.g. an Oracle Cloud "Always Free" VM (1/8 OCPU and 1 GB RAM are plenty: one thread,
# ~2 KiB per room, UDP only). Run it from a copy of the repository, as root:
#
#   sudo bash scripts/server/deploy.sh main [port] [region]         the main server: rooms, the public list, a relay
#   sudo bash scripts/server/deploy.sh relay <name> <region> [port] a relay-only server in another region
#   sudo bash scripts/server/deploy.sh update                       rebuilt from the repository and restarted
#
# Port: 7777 (UDP) by default. It installs g++ if needed, builds /opt/emusuni-netd/emusuni-netd, makes the system user
# emusuni-netd, /etc/emusuni-netd/{secret,env,relays.txt} and the systemd unit emusuni-netd.service (started at boot,
# restarted if it stops), and opens the port in the VM's own firewall.
#
# The servers' shared secret (/etc/emusuni-netd/secret) signs the relay tickets: every relay server needs the main
# server's file (copy it over before "deploy.sh relay", e.g. with scp; keep it private - it isn't in the app). A relay
# region is added with a line in the main server's /etc/emusuni-netd/relays.txt ("name region host:port") and
# "systemctl restart emusuni-netd" there: the consoles get the list from the main server, no app update needed.
#
# Firewall notes (the cloud's own firewall is outside the VM - this script can't open it):
#  - Oracle Cloud (OCI): the VCN's subnet Security List (or the instance's Network Security Group) needs an Ingress
#    Rule: source 0.0.0.0/0, IP protocol UDP, destination port 7777. Oracle's Ubuntu images also REJECT everything in
#    iptables but SSH: this script adds an ACCEPT before that rule and saves it (netfilter-persistent). Oracle Linux
#    images use firewalld: opened with firewall-cmd.
#  - AWS: the Security Group's inbound rule UDP 7777 from 0.0.0.0/0. Google Cloud: a VPC firewall rule, udp:7777.
#    Azure: the Network Security Group's inbound rule.
#  - Only UDP 7777 is needed (and SSH for you). No TCP port, no IPv6.
# Then tell the consoles where it is: the GitHub repository's netplay/servers.txt ("server=<public IP or name>:7777",
# read by the app at each online start - servers can move without an update), or src/Netplay.h's SERVER (built in),
# or a player's own /data/EmuSuNi/netplay.ini (server=host:port).
#
# The logs (journalctl -u emusuni-netd) have counts and consoles, never an address or a room code.
#
# The main server also keeps the app's anonymous statistics (server/emusuni-netd-stats.h) in /var/lib/emusuni-netd/stats:
# counts and countries, never an address. Its timers: DB-IP's IP to Country Lite downloaded monthly
# (emusuni-netd-geoip.timer, update-geoip.sh; CC BY 4.0, "IP Geolocation by DB-IP"), GitHub's download counts daily
# (emusuni-netd-downloads.timer, record-downloads.sh), the game titles monthly (emusuni-netd-titles.timer,
# update-titles.sh: libretro-database's and FinalBurn Neo's lists), and the public page's numbers every 5 minutes
# (emusuni-netd-publish.timer, publish-stats.sh: aggregates only, pushed as one forced commit to the public repository
# arturofuentes1988/emusuni-stats with a deploy key made here - add its public key, printed at the end, to that
# repository with write access). The report:
#   sudo emusuni-stats [--days 30] [--json]      (/usr/local/bin/emusuni-stats: emusuni-netd report)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
MODE="${1:-}"
case "$MODE" in
  main)
    PORT="${2:-7777}"
    REGION="${3:-}"
    ;;
  relay)
    NAME="${2:-}"
    REGION="${3:-}"
    PORT="${4:-7777}"
    if [ -z "$NAME" ] || [ -z "$REGION" ]; then
      echo "usage: deploy.sh relay <name> <region> [port]   (e.g. deploy.sh relay sa-saopaulo SA)"
      exit 2
    fi
    ;;
  update) PORT="" ;;
  *)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
if [ "$(id -u)" != 0 ]; then
  echo "run it as root (sudo)"
  exit 1
fi

# ---- The compiler, and SQLite (the statistics' store: the distribution's libsqlite3, kept up to date by it) ----
if ! command -v g++ > /dev/null || [ ! -f /usr/include/sqlite3.h ]; then
  if command -v apt-get > /dev/null; then
    apt-get update -q && apt-get install -y -q g++ libsqlite3-dev sqlite3
  elif command -v dnf > /dev/null; then
    dnf install -y -q gcc-c++ sqlite-devel sqlite
  else
    yum install -y -q gcc-c++ sqlite-devel sqlite
  fi
fi

# ---- The server ----
install -d -m 755 /opt/emusuni-netd
g++ -std=c++17 -O2 -Wall -o /opt/emusuni-netd/emusuni-netd.new "$REPO/server/emusuni-netd.cpp" -lsqlite3
mv -f /opt/emusuni-netd/emusuni-netd.new /opt/emusuni-netd/emusuni-netd
echo "built /opt/emusuni-netd/emusuni-netd"
# The statistics' helpers (the main server's timers run them; a relay never does).
for SCRIPT in update-geoip.sh record-downloads.sh update-titles.sh publish-stats.sh; do
  install -m 755 "$HERE/$SCRIPT" "/opt/emusuni-netd/$SCRIPT"
done
printf '#!/bin/sh\n# EmuSuNi'"'"'s anonymous statistics: emusuni-stats [--days 30] [--json]\nexec /opt/emusuni-netd/emusuni-netd report --stats-dir /var/lib/emusuni-netd/stats "$@"\n' \
  > /usr/local/bin/emusuni-stats
chmod 755 /usr/local/bin/emusuni-stats
for UNIT in emusuni-netd.service emusuni-netd-geoip.service emusuni-netd-geoip.timer emusuni-netd-downloads.service \
            emusuni-netd-downloads.timer emusuni-netd-titles.service emusuni-netd-titles.timer \
            emusuni-netd-publish.service emusuni-netd-publish.timer; do
  install -m 644 "$HERE/$UNIT" "/etc/systemd/system/$UNIT"
done
systemctl daemon-reload

# The main server's statistics: their timers (the country database and the game titles monthly, GitHub's download
# counts daily, the public page's numbers every 5 minutes) and the deploy key that publishes those numbers.
STATE=/var/lib/emusuni-netd
stats_extras() {
  if ! command -v git > /dev/null; then
    if command -v apt-get > /dev/null; then
      apt-get install -y -q git
    elif command -v dnf > /dev/null; then
      dnf install -y -q git
    else
      yum install -y -q git
    fi
  fi
  install -d -m 750 -o emusuni-netd -g emusuni-netd "$STATE"
  install -d -m 700 -o emusuni-netd -g emusuni-netd "$STATE/publish"
  if [ ! -f "$STATE/publish/deploy_key" ]; then
    runuser -u emusuni-netd -- ssh-keygen -q -t ed25519 -N "" -C "emusuni-stats publisher" -f "$STATE/publish/deploy_key"
    echo "made the statistics' deploy key: $STATE/publish/deploy_key"
  fi
  # GitHub's SSH host keys, from its API over HTTPS: the publisher checks github.com against them.
  if KEYS=$(curl -fsSL -m 30 https://api.github.com/meta | grep -o '"ssh-[a-z0-9-]* [A-Za-z0-9+/=]*"' | tr -d '"') &&
     [ -n "$KEYS" ]; then
    printf '%s\n' "$KEYS" | sed 's/^/github.com /' > "$STATE/publish/known_hosts"
    chown emusuni-netd:emusuni-netd "$STATE/publish/known_hosts"
  fi
  systemctl enable emusuni-netd-geoip.timer emusuni-netd-downloads.timer emusuni-netd-titles.timer \
    emusuni-netd-publish.timer
  systemctl start emusuni-netd-geoip.timer emusuni-netd-downloads.timer emusuni-netd-titles.timer \
    emusuni-netd-publish.timer
  [ -s "$STATE/dbip-country-lite.csv" ] ||
    systemctl start emusuni-netd-geoip.service || echo "DB-IP's file couldn't be downloaded now: countries \"??\" until it is"
  [ -s "$STATE/titles.txt" ] ||
    systemctl start emusuni-netd-titles.service || echo "the game titles couldn't be made now (tried monthly)"
  systemctl start emusuni-netd-downloads.service || echo "GitHub's download counts couldn't be read now (tried daily)"
}
stats_key_note() {
  echo
  echo "The public statistics' deploy key (add it to github.com/arturofuentes1988/emusuni-stats -> Settings -> Deploy"
  echo "keys, with write access; until then publishing fails quietly in journalctl -u emusuni-netd-publish):"
  cat "$STATE/publish/deploy_key.pub"
}

if [ "$MODE" = update ]; then
  # (A main server made before the statistics: its env gets them.)
  if grep -q -- "--name main" /etc/emusuni-netd/env 2>/dev/null && ! grep -q -- "--stats-dir" /etc/emusuni-netd/env; then
    sed -i 's|--name main|--name main --stats-dir /var/lib/emusuni-netd/stats --geoip /var/lib/emusuni-netd/dbip-country-lite.csv|' \
      /etc/emusuni-netd/env
  fi
  if grep -q -- "--name main" /etc/emusuni-netd/env 2>/dev/null; then
    stats_extras
  fi
  systemctl restart emusuni-netd
  systemctl --no-pager status emusuni-netd | head -5
  grep -q -- "--name main" /etc/emusuni-netd/env 2>/dev/null && stats_key_note
  exit 0
fi
id emusuni-netd > /dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin emusuni-netd
install -d -m 750 -o root -g emusuni-netd /etc/emusuni-netd
if [ ! -s /etc/emusuni-netd/secret ]; then
  if [ "$MODE" = relay ]; then
    echo "/etc/emusuni-netd/secret is missing: copy the main server's file here first (the same secret signs and"
    echo "checks the relay tickets), e.g.: scp main:/etc/emusuni-netd/secret /etc/emusuni-netd/secret"
    exit 1
  fi
  head -c 48 /dev/urandom | base64 > /etc/emusuni-netd/secret
  echo "made a new shared secret: /etc/emusuni-netd/secret (copy it to every relay server)"
fi
chown root:emusuni-netd /etc/emusuni-netd/secret
chmod 640 /etc/emusuni-netd/secret
if [ "$MODE" = main ]; then
  if [ ! -f /etc/emusuni-netd/relays.txt ]; then
    cat > /etc/emusuni-netd/relays.txt << 'TXT'
# EmuSuNi's relay servers besides this one, one a line: name region host:port (systemctl restart emusuni-netd after a
# change). The consoles of a room use the relay nearest both of them. Example:
# sa-saopaulo SA relay-sa.example.com:7777
TXT
  fi
  printf 'EMUSUNI_NETD_PORT=%s\nEMUSUNI_NETD_ARGS="--relays /etc/emusuni-netd/relays.txt --name main --region %s --stats-dir /var/lib/emusuni-netd/stats --geoip /var/lib/emusuni-netd/dbip-country-lite.csv"\n' \
    "$PORT" "${REGION:-?}" > /etc/emusuni-netd/env
else
  printf 'EMUSUNI_NETD_PORT=%s\nEMUSUNI_NETD_ARGS="--relay-only --name %s --region %s"\n' "$PORT" "$NAME" "$REGION" \
    > /etc/emusuni-netd/env
fi
chmod 644 /etc/emusuni-netd/env
[ -f /etc/emusuni-netd/relays.txt ] && chmod 644 /etc/emusuni-netd/relays.txt
if [ "$MODE" = main ]; then
  stats_extras
fi
systemctl enable --now emusuni-netd
systemctl restart emusuni-netd

# ---- The VM's own firewall ----
if command -v ufw > /dev/null && ufw status | grep -q "Status: active"; then
  ufw allow "$PORT/udp"
fi
if command -v firewall-cmd > /dev/null && firewall-cmd --state > /dev/null 2>&1; then
  firewall-cmd --permanent --add-port="$PORT/udp"
  firewall-cmd --reload
fi
if command -v iptables > /dev/null && iptables -S INPUT 2>/dev/null | grep -q -- "-j REJECT"; then
  # (Oracle Cloud's Ubuntu images: everything but SSH rejected - the port accepted before that rule, and kept.)
  if ! iptables -C INPUT -p udp --dport "$PORT" -j ACCEPT 2>/dev/null; then
    LINE=$(iptables -L INPUT --line-numbers | awk '/REJECT/ {print $1; exit}')
    iptables -I INPUT "${LINE:-1}" -p udp --dport "$PORT" -j ACCEPT
  fi
  if command -v netfilter-persistent > /dev/null; then
    netfilter-persistent save
  elif [ -d /etc/iptables ]; then
    iptables-save > /etc/iptables/rules.v4
  fi
fi

sleep 1
systemctl --no-pager status emusuni-netd | head -6
IP=$(curl -s -m 5 https://ifconfig.me || true)
echo
echo "emusuni-netd runs on UDP $PORT. Open UDP $PORT in the cloud's firewall too (see the notes at the top)."
if [ "$MODE" = main ]; then
  echo "For the consoles: netplay/servers.txt in the GitHub repository -> server=${IP:-<this VM's public IP>}:$PORT"
else
  echo "On the main server, add to /etc/emusuni-netd/relays.txt: $NAME $REGION ${IP:-<this VM's public IP>}:$PORT"
  echo "and run: systemctl restart emusuni-netd"
fi
if [ "$MODE" = main ]; then
  stats_key_note
fi
