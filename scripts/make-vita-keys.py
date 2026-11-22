#!/usr/bin/env python3
# EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#
# The PS Vita's keys file, for an EmuSuNi built with-vita-nokeys (scripts/build.sh): that build's core has no key
# of Sony's inside, and reads them at a game's start from
#     /data/EmuSuNi/system/vita3k/vita-keys.txt
# which the player puts there, as a console's BIOS. This script makes that file out of Vita3K's own source code
# (https://github.com/Vita3K/Vita3K, cloned with its submodules: psvpfstools holds some of them), where the keys
# are written. No key is in this script, in EmuSuNi's source, in its package nor on any server of its own.
#
#   make-vita-keys.py <Vita3K's source folder>... <out file>   makes the file, says its SHA-256
#   make-vita-keys.py --sha256 <source>...                     the SHA-256 alone (the build: what the app and the
#                                                              core take the file by)
#   make-vita-keys.py --scan <binary> <source>...              looks for every key in a binary (an object, an
#                                                              eboot.bin): 1 when any is there, 0 when none
# <source>: Vita3K's source folder (more than one when the submodule is somewhere else), or a file made earlier.
# For --scan, FFmpeg's source folder may be one more: its OpenMG keys (libavformat/omadec.c) are looked for too.
#
# The file: lines of text, '#' for a comment.
#   sce <set> <key type> <SCE type> <revision> <key> <iv> <first version> <last version> <SELF type>
#   table <name> <bytes>
# Its SHA-256 here is of its lines without their spaces, comments and line ends, one '\n' after each: the same
# file with other line ends or another comment is the same file. The app (src/Vita.cpp) and the core
# (port/ps4_vita_keys.cpp) take it only when that SHA-256 is the one their build was made with.
import hashlib
import os
import re
import sys

# Where each table of bytes is in Vita3K's source: the file's name, the table's name there, its size.
TABLES = [
    ('pkg_vita_2', 'pkg.h', 'pkg_vita_2', 16),
    ('pkg_vita_3', 'pkg.h', 'pkg_vita_3', 16),
    ('pkg_vita_4', 'pkg.h', 'pkg_vita_4', 16),
    ('vci_bind_key', 'vci.h', 'BIND_KEY', 16),
    ('registry_xor', 'regmgr.cpp', 'xorKey', 16),
    ('pfs_hmac_key0', 'PfsKeys.cpp', 'hmac_key0', 20),
    ('pfs_hmac_key1', 'PfsKeys.cpp', 'hmac_key1', 20),
    ('pfs_iv0', 'PfsKeys.cpp', 'iv0', 16),
    ('pfs_contract_key0', 'F00DNativeKeyEncryptor.cpp', 'contract_key0', 16),
    ('pfs_sealedkey_retail_key', 'LocalKeys.cpp', 'sealedkey_retail_key', 16),
    ('pfs_keystone_hmac_secret', 'LocalKeys.cpp', 'keystone_hmac_secret', 32),
    ('pfs_passcode_hmac_secret', 'LocalKeys.cpp', 'passcode_hmac_secret', 32),
    ('pfs_enc_key', 'LocalKeys.cpp', 'PFS_EncKey', 16),
]
SCE_FILE = 'sce_utils.cpp'
HEADER = ("# The PS Vita's keys, for EmuSuNi (with-vita-nokeys): /data/EmuSuNi/system/vita3k/vita-keys.txt\n"
          "# Made by scripts/make-vita-keys.py out of Vita3K's source code. Not a part of EmuSuNi.\n")


def find_file(folders, name):
    found = []
    for folder in folders:
        for at, dirs, files in os.walk(folder):
            dirs[:] = [d for d in dirs if d not in ('.git', 'build')]
            if name in files:
                found.append(os.path.join(at, name))
    if not found:
        sys.exit("make-vita-keys: %s isn't under %s (Vita3K's source, with its submodules)" % (name, ', '.join(folders)))
    return found


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def from_source(folders):
    """The file's lines, out of Vita3K's source."""
    lines = []
    # The keys of the system's modules and packages: every SCE_KEYS.register_keys(...) of register_keys, under its
    # "case 0:" (the retail set) or "case 1:".
    texts = [read(p) for p in find_file(folders, SCE_FILE)]
    texts = [t for t in texts if 'void register_keys(' in t]
    if len(texts) != 1:
        sys.exit("make-vita-keys: expected one %s with register_keys, found %d" % (SCE_FILE, len(texts)))
    text = texts[0]
    marks = [(m.start(), 'case', m.group(1)) for m in re.finditer(r'\bcase (\d+):', text)]
    marks += [(m.start(), 'call', m.group(1)) for m in re.finditer(r'SCE_KEYS\.register_keys\(([^;]*?)\);', text)]
    key_set = None
    for _, kind, value in sorted(marks):
        if kind == 'case':
            key_set = int(value)
            continue
        args = [a.strip() for a in value.split(',')]
        if len(args) == 8 and not args[3].startswith('"'):
            continue  # (not a key written there: the call of EmuSuNi's own patch, which passes the file's)
        if key_set is None or len(args) != 8:
            sys.exit("make-vita-keys: a register_keys call of %s isn't as expected: %s" % (SCE_FILE, value[:60]))
        key_type, sce_type, revision, key, iv, first, last, self_type = args
        names = [key_type, sce_type, self_type]
        for i, (name, prefix) in enumerate(zip(names, ('KeyType::', 'SceType::', 'SelfType::'))):
            if not name.startswith(prefix) or not re.fullmatch(r'[A-Z0-9_]+', name[len(prefix):]):
                sys.exit("make-vita-keys: %s: not a %s name: %s" % (SCE_FILE, prefix, name))
            names[i] = name[len(prefix):]
        for hexes in (key, iv):
            if not re.fullmatch(r'"(?:[0-9A-Fa-f]{2})+"', hexes):
                sys.exit("make-vita-keys: %s: not a key in hexadecimal" % SCE_FILE)
        lines.append('sce %d %s %s %d %s %s 0x%X 0x%X %s' % (
            key_set, names[0], names[1], int(revision, 0), key.strip('"').upper(), iv.strip('"').upper(),
            int(first, 0), int(last, 0), names[2]))
    if not lines:
        sys.exit("make-vita-keys: no register_keys call in %s" % SCE_FILE)
    # The tables of bytes.
    for name, file_name, table, size in TABLES:
        values = set()
        for path in find_file(folders, file_name):
            for m in re.finditer(r'\b%s\b[^;=(){}]*=\s*\{([^{};]*)\}' % re.escape(table), read(path)):
                data = bytes(int(b, 16) for b in re.findall(r'0[xX]([0-9A-Fa-f]{1,2})\b', m.group(1)))
                if len(data) == size:  # (not the "{0}" of EmuSuNi's own patch, in a source it has patched)
                    values.add(data)
        if len(values) != 1 or len(next(iter(values))) != size:
            sys.exit("make-vita-keys: %s of %s isn't as expected (%d bytes, once)" % (table, file_name, size))
        lines.append('table %s %s' % (name, next(iter(values)).hex().upper()))
    return lines


def from_file(path):
    """The lines of a file made earlier (no comments, no empty lines; the spaces as they are)."""
    lines = []
    for line in read(path).splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            lines.append(line)
    return lines


def lines_of(sources):
    files = [s for s in sources if os.path.isfile(s)]
    folders = [s for s in sources if os.path.isdir(s)]
    if len(files) + len(folders) != len(sources) or not sources:
        sys.exit("make-vita-keys: not there: %s" % ', '.join(s for s in sources if not os.path.exists(s)))
    if files and (folders or len(files) != 1):
        sys.exit("make-vita-keys: Vita3K's source folders, or one file made earlier")
    return from_file(files[0]) if files else from_source(folders)


def digest(lines):
    h = hashlib.sha256()
    for line in lines:
        h.update(b''.join(line.encode('ascii').split()) + b'\n')
    return h.hexdigest()


def values_of(lines):
    """Every key of the file as bytes, with what it is."""
    values = []
    for number, line in enumerate(lines):
        words = line.split()
        if words[0] == 'sce' and len(words) == 10:
            values.append(('sce key %d' % number, bytes.fromhex(words[5])))
            values.append(('sce iv %d' % number, bytes.fromhex(words[6])))
        elif words[0] == 'table' and len(words) == 3:
            values.append((words[1], bytes.fromhex(words[2])))
        else:
            sys.exit("make-vita-keys: not a line of the keys file: %s..." % line[:24])
    return values


def ffmpeg_values(sources):
    """For the scan alone: the keys FFmpeg has in its demuxer of Sony's OMA files (OpenMG's), when one of the
    folders is FFmpeg's source - 64-bit numbers, as a binary of the PS4 holds them. Nothing reads them from the
    player's file: the build without keys has zeros there (scripts/build-vita-core.sh)."""
    values = []
    for folder in (s for s in sources if os.path.isdir(s)):
        for at, dirs, files in os.walk(folder):
            dirs[:] = [d for d in dirs if d != '.git']
            if 'omadec.c' not in files:
                continue
            for m in re.finditer(r'\bleaf_table\[\]\s*=\s*\{([^{};]*)\}', read(os.path.join(at, 'omadec.c'))):
                for i, number in enumerate(re.findall(r'0[xX][0-9A-Fa-f]{16}\b', m.group(1))):
                    values.append(('ffmpeg_oma_leaf_%d' % i, int(number, 16).to_bytes(8, 'little')))
    return values


def scan(binary, lines, more=()):
    """Every key in the binary: as its bytes, as hexadecimal text in capitals and in small letters; whole, and
    each of its 8-byte pieces (a table the compiler turned into constants of the code)."""
    try:
        with open(binary, 'rb') as f:
            data = f.read()
    except OSError as error:
        sys.exit("make-vita-keys: %s can't be read: %s" % (binary, error.strerror))
    found = {}
    seen = set()
    for name, value in values_of(lines) + list(more):
        if not any(value) or value in seen:  # (a debug key of zeros isn't one; a key twice is looked for once)
            continue
        seen.add(value)
        pieces = [('whole', value)] + [('8 bytes at %d' % i, value[i:i + 8]) for i in range(0, len(value) - 7, 8)]
        for piece_name, piece in pieces:
            forms = (('bytes', piece), ('HEX', piece.hex().upper().encode()), ('hex', piece.hex().encode()))
            for form, needle in forms:
                count = data.count(needle)
                if count:
                    found.setdefault(name, []).append('%s as %s x%d' % (piece_name, form, count))
    return len(seen), found


def main(argv):
    if len(argv) >= 2 and argv[0] == '--sha256':
        print(digest(lines_of(argv[1:])))
        return 0
    if len(argv) >= 3 and argv[0] == '--scan':
        looked, found = scan(argv[1], lines_of(argv[2:]), ffmpeg_values(argv[2:]))
        if found:
            print("make-vita-keys: %s has %d of the %d keys inside:" % (argv[1], len(found), looked))
            for name in sorted(found):
                print("  %s: %s" % (name, ', '.join(found[name])))
            return 1
        print("make-vita-keys: %s has none of the %d keys inside" % (argv[1], looked))
        return 0
    if len(argv) >= 2 and not argv[0].startswith('-'):
        lines = lines_of(argv[:-1])
        if os.path.isdir(argv[-1]):
            sys.exit("make-vita-keys: the last word is the file to make, not a folder")
        with open(argv[-1], 'w', encoding='ascii', newline='\n') as f:
            f.write(HEADER + '\n'.join(lines) + '\n')
        print("make-vita-keys: %s: %d keys of the system's modules, %d tables" % (
            argv[-1], sum(l.startswith('sce ') for l in lines), sum(l.startswith('table ') for l in lines)))
        print("make-vita-keys: its SHA-256 (as EmuSuNi takes it): %s" % digest(lines))
        return 0
    print(__doc__ or "make-vita-keys.py <Vita3K's source folder>... <out file> | --sha256 <source>... | "
          "--scan <binary> <source>...")
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
