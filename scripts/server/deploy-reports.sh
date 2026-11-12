#!/usr/bin/env bash
# Installs EmuSuNi's log reports (server/emusuni-reports.py: HTTPS on TCP 443, the players' logs sent from the app)
# on the main server, beside emusuni-netd (deploy.sh main first: its country file and its group are used). Run it from
# a copy of the repository, as root:
#
#   sudo bash scripts/server/deploy-reports.sh install <cert.pem> <key.pem>   the first time (make-reports-key.sh's)
#   sudo bash scripts/server/deploy-reports.sh update                         the program again, restarted
#   sudo bash scripts/server/deploy-reports.sh panel <the server's IP>        (00.92) the owner's panel on, port 80 too
#   sudo bash scripts/server/deploy-reports.sh panel <IP> panel.emusuni.com    (00.95) the panel at its name
#
# It installs /opt/emusuni-reports/emusuni-reports.py (Python 3, as the distribution has it) with reports_panel.py and
# reports_acme.py beside it, the system user emusuni-reports, /etc/emusuni-reports/{cert.pem,key.pem} (the key
# readable by that user only), the reports' folder /var/lib/emusuni-reports, the systemd unit emusuni-reports.service
# (started at boot, restarted if it stops) and the owner's command:
#   sudo emusuni-reports list [--days 30]   |   sudo emusuni-reports show ABCDEF [crash.log]   |   ... export ABCDEF
#   sudo emusuni-reports admin-setup        the panel's password and TOTP (asked in the terminal, never elsewhere)
#   sudo emusuni-reports acme issue --ip <IP> --agree-tos [--staging]   |   sudo emusuni-reports acme status
# and opens TCP 443 (panel: and 80) in the VM's own firewall. The cloud's firewall is outside the VM: Oracle Cloud's
# subnet Security List needs an Ingress Rule too - source 0.0.0.0/0, IP protocol TCP, destination port 443 (and 80).
# The program keeps no address (no access log; the journal has codes, versions, countries and sizes):
#   journalctl -u emusuni-reports
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
MODE="${1:-}"
if [ "$(id -u)" != 0 ]; then
  echo "run it as root (sudo)"
  exit 1
fi
case "$MODE" in
  install)
    CERT="${2:-}"
    KEY="${3:-}"
    if [ ! -f "$CERT" ] || [ ! -f "$KEY" ]; then
      echo "usage: deploy-reports.sh install <cert.pem> <key.pem>"
      exit 2
    fi
    ;;
  update) ;;
  panel)
    PUBLIC="${2:-}"
    NAME="${3:-}"
    if ! [[ "$PUBLIC" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] ||
       { [ -n "$NAME" ] && ! [[ "$NAME" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$ ]]; }; then
      echo "usage: deploy-reports.sh panel <the server's public IPv4 address> [the panel's name: panel.emusuni.com]"
      exit 2
    fi
    ;;
  *)
    sed -n '2,9p' "$0"
    exit 2
    ;;
esac

command -v python3 > /dev/null || apt-get install -y -q python3
command -v openssl > /dev/null || apt-get install -y -q openssl
id emusuni-reports > /dev/null 2>&1 ||
  useradd --system --no-create-home --shell /usr/sbin/nologin emusuni-reports
getent group emusuni-netd > /dev/null || groupadd --system emusuni-netd
install -d -m 755 /opt/emusuni-reports
install -m 755 "$REPO/server/emusuni-reports.py" /opt/emusuni-reports/emusuni-reports.py
install -m 644 "$REPO/server/reports_panel.py" /opt/emusuni-reports/reports_panel.py
install -m 644 "$REPO/server/reports_acme.py" /opt/emusuni-reports/reports_acme.py
rm -rf /opt/emusuni-reports/__pycache__
install -d -m 750 -o emusuni-reports -g emusuni-reports /var/lib/emusuni-reports
# (01.07) The world chat's blocks: written by the panel in chat/blocks.tsv (644), read by emusuni-netd (--chat-blocks),
# which an ACL lets through /var/lib/emusuni-reports - not to list it nor read the reports (deploy.sh does it too, when
# it comes second). Made empty once; the panel's own file is never touched again.
install -d -m 755 -o emusuni-reports -g emusuni-reports /var/lib/emusuni-reports/chat
[ -e /var/lib/emusuni-reports/chat/blocks.tsv ] ||
  install -m 644 -o emusuni-reports -g emusuni-reports /dev/null /var/lib/emusuni-reports/chat/blocks.tsv
if id emusuni-netd > /dev/null 2>&1; then
  command -v setfacl > /dev/null || apt-get install -y -q acl
  setfacl -m u:emusuni-netd:x /var/lib/emusuni-reports
fi
if [ "$MODE" = install ]; then
  install -d -m 750 -o root -g emusuni-reports /etc/emusuni-reports
  install -m 644 -o root -g root "$CERT" /etc/emusuni-reports/cert.pem
  install -m 640 -o root -g emusuni-reports "$KEY" /etc/emusuni-reports/key.pem
fi
if [ "$MODE" = panel ]; then
  # (00.95) With the panel's name: "name,IP" - a browser at the name gets its certificate, the IP's pages go there.
  printf '# EmuSuNi log reports: the owner'"'"'s panel (deploy-reports.sh panel)\nEMUSUNI_REPORTS_ARGS="--public-host %s --http-port 80"\n' \
    "${NAME:+$NAME,}$PUBLIC" > /etc/emusuni-reports/env
  chmod 644 /etc/emusuni-reports/env
fi
printf '#!/bin/sh\n# EmuSuNi'"'"'s log reports: emusuni-reports list | show CODE [file] | export CODE | purge | admin-setup | acme issue|renew|status\n%s\n' \
  'exec runuser -u emusuni-reports -- /usr/bin/python3 /opt/emusuni-reports/emusuni-reports.py --dir /var/lib/emusuni-reports "$@"' \
  > /usr/local/bin/emusuni-reports
chmod 755 /usr/local/bin/emusuni-reports
install -m 644 "$HERE/emusuni-reports.service" /etc/systemd/system/emusuni-reports.service
systemctl daemon-reload
systemctl enable emusuni-reports.service
systemctl restart emusuni-reports.service

# The VM's own firewall: TCP 443 (as deploy.sh opens UDP 7777), and 80 with the panel (ACME's challenge).
PORTS="443"
grep -q -- "--http-port 80" /etc/emusuni-reports/env 2>/dev/null && PORTS="443 80"
for PORT in $PORTS; do
  if command -v firewall-cmd > /dev/null && firewall-cmd --state > /dev/null 2>&1; then
    firewall-cmd --permanent --add-port="$PORT/tcp"
    firewall-cmd --reload
  fi
  if command -v iptables > /dev/null && iptables -S INPUT 2>/dev/null | grep -q -- "-j REJECT"; then
    if ! iptables -C INPUT -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null; then
      LINE=$(iptables -L INPUT --line-numbers | awk '/REJECT/ {print $1; exit}')
      iptables -I INPUT "${LINE:-1}" -p tcp --dport "$PORT" -j ACCEPT
    fi
  fi
done
if command -v iptables > /dev/null; then
  if command -v netfilter-persistent > /dev/null; then
    netfilter-persistent save
  elif [ -d /etc/iptables ]; then
    iptables-save > /etc/iptables/rules.v4
  fi
fi
sleep 1
systemctl --no-pager --lines=5 status emusuni-reports.service || true
echo "Oracle Cloud: the Security List's Ingress Rules for TCP $PORTS (source 0.0.0.0/0) if they aren't there."
if [ "$MODE" = panel ] && [ -n "$NAME" ]; then
  echo "Next, once $NAME resolves to $PUBLIC: sudo emusuni-reports acme issue --name $NAME"
  echo "(the account is there already). The panel: https://$NAME/ (the IP's pages go there once its certificate is in)"
elif [ "$MODE" = panel ]; then
  echo "Next: sudo emusuni-reports admin-setup; then sudo emusuni-reports acme issue --ip $PUBLIC --agree-tos --staging"
  echo "(a try), and the same without --staging. The panel: https://$PUBLIC/"
fi
