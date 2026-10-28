#!/usr/bin/env bash
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# Mbed TLS for the PS4 (curl's TLS: HTTPS for the update check, the covers' GitHub fallback,
# RetroAchievements): PacBrew's own Mbed TLS 2.16.6 libraries (ps4-openorbis-mbedtls) with one object
# rebuilt, bignum.c, with upstream's fix of its x86-64 assembly (Mbed TLS 2.16.12 and 2.28.0: "Fix
# missing constraints on x86_64 and aarch64 assembly code for bignum multiplication that broke some
# bignum operations with (at least) Clang 12"). PacBrew's build, by clang 13, has the bug: every RSA
# operation fails ("RSA - The PKCS#1 verification failed", -0x4380), so the PS4 refused every
# handshake with a server whose certificate or key exchange is RSA (raw.githubusercontent.com,
# GitHub's release files, thumbnails.libretro.com); only ECDSA servers worked.
# bignum.c is compiled as PacBrew's package compiles it (the same tarball, checked by its SHA-256, the
# same clang 13 and flags of PacBrew's ps4vars.sh, the same CMake options) and put in place of PacBrew's
# in a copy of its libmbedcrypto.a; libmbedx509.a and libmbedtls.a are PacBrew's, copied. Checked: every
# other object byte for byte PacBrew's, the same symbols defined and needed (curl's libcurl.a, built
# against PacBrew's 2.16.6 headers, links them unchanged).
# Built once; again only when this script changes.
#   build-mbedtls.sh <out dir>      -> <out dir>/lib/libmbedtls.a, libmbedx509.a, libmbedcrypto.a
set -euo pipefail
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
# PacBrew's source (its PKGBUILD: https://sources.buildroot.net/mbedtls/mbedtls-2.16.6-apache.tgz), fetched by
# scripts/setup.sh.
TARBALL="${MBEDTLS_TARBALL:-$HOME/src/mbedtls-2.16.6-apache.tgz}"
SHA256=66455e23a6190a30142cdc1113f7418158839331a9d8e6b0778631d077281770
LOG="$OUT/mbedtls.log"
STAMP="$OUT/.emusuni-flags"
LIBS="libmbedcrypto libmbedx509 libmbedtls"
WANT="$SHA256 | $(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1) | $(cat "$OPENORBIS"/usr/lib/libmbed*.a | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ] && [ -f "$OUT/lib/libmbedcrypto.a" ] &&
   [ -f "$OUT/lib/libmbedx509.a" ] && [ -f "$OUT/lib/libmbedtls.a" ]; then
  exit 0
fi
echo "Mbed TLS 2.16.6: PacBrew's libraries with bignum.c's x86-64 fix"
if ! echo "$SHA256  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; then
  echo "Mbed TLS: $TARBALL is missing or not PacBrew's mbedtls-2.16.6-apache.tgz (scripts/setup.sh fetches it)"
  exit 1
fi
AR=llvm-ar-21
command -v "$AR" > /dev/null || AR="$OPENORBIS/bin/llvm-ar"
NM=llvm-nm-21
command -v "$NM" > /dev/null || NM="$OPENORBIS/bin/llvm-nm"
rm -rf "$OUT/src" "$OUT/build" "$OUT/lib" "$OUT/objects" "$STAMP"
mkdir -p "$OUT/src" "$OUT/lib" "$OUT/objects/pacbrew" "$OUT/objects/new"
tar xzf "$TARBALL" -C "$OUT/src" --strip-components=1
# The fix (2.16.12's include/mbedtls/bn_mul.h): the AMD64 MULADDC block reads s[0..15] and writes d[0..15]
# ("addq %%rax, (%%rdi)") without saying so, so clang keeps those words in registers across it.
python3 -I - "$OUT/src/include/mbedtls/bn_mul.h" <<'PATCH'
import sys
path = sys.argv[1]
text = open(path).read()
BS = "\\"
old = (
    "#define MULADDC_STOP                        " + BS + "\n"
    "        : \"+c\" (c), \"+D\" (d), \"+S\" (s)      " + BS + "\n"
    "        : \"b\" (b)                           " + BS + "\n"
    "        : \"rax\", \"rdx\", \"r8\"                " + BS + "\n"
    "    );\n"
    "\n"
    "#endif /* AMD64 */"
)
new = (
    "#define MULADDC_STOP                        " + BS + "\n"
    "        : \"+c\" (c), \"+D\" (d), \"+S\" (s), \"+m\" (*(uint64_t (*)[16]) d) " + BS + "\n"
    "        : \"b\" (b), \"m\" (*(const uint64_t (*)[16]) s)                 " + BS + "\n"
    "        : \"rax\", \"rdx\", \"r8\"                                         " + BS + "\n"
    "    );\n"
    "\n"
    "#endif /* AMD64 */"
)
if text.count(old) != 1:
    sys.exit("bn_mul.h: the AMD64 MULADDC_STOP block isn't 2.16.6's")
open(path, "w").write(text.replace(old, new))
PATCH
(
  # PacBrew's recipe (its makepkg sources ps4vars.sh, whose CFLAGS CMake takes, then runs openorbis-cmake).
  # shellcheck disable=SC1091
  source "$OPENORBIS/ps4vars.sh"
  mkdir -p "$OUT/build"
  cd "$OUT/build"
  if ! openorbis-cmake -Wno-dev -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DENABLE_ZLIB_SUPPORT=TRUE \
       -DENABLE_TESTING=FALSE -DENABLE_PROGRAMS=FALSE "$OUT/src" > "$LOG" 2>&1; then
    tail -20 "$LOG"
    echo "Mbed TLS: configuring failed (see $LOG)"
    exit 1
  fi
  # (Its crypto library only; at most 4 jobs: the PC blue-screened once under a full load.)
  if ! make -j "${EMUSUNI_JOBS:-4}" mbedcrypto >> "$LOG" 2>&1; then
    grep -E "error" "$LOG" | head -20
    echo "Mbed TLS: compiling failed (see $LOG)"
    exit 1
  fi
)
for lib in $LIBS; do
  cp "$OPENORBIS/usr/lib/$lib.a" "$OUT/lib/$lib.a"
done
(cd "$OUT/objects/new" && "$AR" x "$OUT/build/library/libmbedcrypto.a" bignum.c.obj)
"$AR" rs "$OUT/lib/libmbedcrypto.a" "$OUT/objects/new/bignum.c.obj"
# Checked: the same objects in the same order, all but bignum.c.obj PacBrew's byte for byte, and the same
# symbols defined and needed.
(cd "$OUT/objects/pacbrew" && "$AR" x "$OPENORBIS/usr/lib/libmbedcrypto.a")
rm -rf "$OUT/objects/new" && mkdir -p "$OUT/objects/new"
(cd "$OUT/objects/new" && "$AR" x "$OUT/lib/libmbedcrypto.a")
if [ "$("$AR" t "$OUT/lib/libmbedcrypto.a")" != "$("$AR" t "$OPENORBIS/usr/lib/libmbedcrypto.a")" ]; then
  echo "Mbed TLS: libmbedcrypto.a's objects aren't PacBrew's"
  exit 1
fi
for object in "$OUT"/objects/pacbrew/*; do
  name="$(basename "$object")"
  if [ "$name" = bignum.c.obj ]; then
    if cmp -s "$object" "$OUT/objects/new/$name"; then
      echo "Mbed TLS: bignum.c.obj is still PacBrew's"
      exit 1
    fi
  elif ! cmp -s "$object" "$OUT/objects/new/$name"; then
    echo "Mbed TLS: $name isn't PacBrew's"
    exit 1
  fi
done
for kind in --defined-only --undefined-only; do
  if ! cmp -s <("$NM" -g "$kind" -j "$OUT/lib/libmbedcrypto.a" 2>/dev/null | sort -u) \
              <("$NM" -g "$kind" -j "$OPENORBIS/usr/lib/libmbedcrypto.a" 2>/dev/null | sort -u); then
    echo "Mbed TLS: libmbedcrypto.a's symbols ($kind) differ from PacBrew's"
    exit 1
  fi
done
rm -rf "$OUT/objects" "$OUT/build"
echo "$WANT" > "$STAMP"
echo "Mbed TLS: $OUT/lib (PacBrew's 2.16.6, bignum.c with the x86-64 fix)"
