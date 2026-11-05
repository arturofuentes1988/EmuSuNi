#!/usr/bin/env bash
# The log reports' certificate (server/emusuni-reports.py): an ECDSA P-256 key (the PS4's Mbed TLS 2.16 handshakes with
# ECDSA servers), self-signed for 30 years, made on the owner's PC - never on GitHub, never in the package:
#
#   bash scripts/server/make-reports-key.sh <folder>      (e.g. local/secretos/reportes; it must not exist yet)
#
# It writes <folder>/key.pem (private: the server's /etc/emusuni-reports/key.pem, and a copy kept off the PC), cert.pem
# (the server's /etc/emusuni-reports/cert.pem) and pin.txt: the key's pin, "sha256//<base64 of the SHA-256 of its
# public key>", which src/LogReport.cpp's PINS carries - the app trusts that key only, whatever the server's address
# or the certificate's dates (curl's CURLOPT_PINNEDPUBLICKEY). The app carries a second pin, of a spare key made the
# same way and kept off the PC unused: if the first key is lost or exposed, the server gets the spare one and the
# app's versions out there still send (then a new spare goes in the next version).
set -euo pipefail
OUT="${1:-}"
if [ -z "$OUT" ] || [ -e "$OUT" ]; then
  echo "usage: make-reports-key.sh <new folder>"
  exit 2
fi
mkdir -p "$OUT"
chmod 700 "$OUT"
umask 077
openssl ecparam -name prime256v1 -genkey -noout -out "$OUT/key.pem"
openssl req -new -x509 -key "$OUT/key.pem" -out "$OUT/cert.pem" -days 10950 -sha256 \
  -subj "/CN=EmuSuNi reports" -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=serverAuth"
chmod 644 "$OUT/cert.pem"
PIN="sha256//$(openssl pkey -in "$OUT/key.pem" -pubout -outform der | openssl dgst -sha256 -binary | base64)"
echo "$PIN" > "$OUT/pin.txt"
chmod 644 "$OUT/pin.txt"
echo "made $OUT/key.pem (private), cert.pem, pin.txt:"
echo "$PIN"
