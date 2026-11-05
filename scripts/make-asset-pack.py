"""The package's small files in one (00.90): a PKG keeps each file in whole 64 KiB blocks, so xmb's hundred or so small
files took about 3.6 MB more than their bytes. The folders named here go into <xmb>/assets.zip, stored (not
compressed: src/AssetPack.h reads an entry's bytes where they are), named from xmb ("bezels/tv.png", "lang/es.ini"),
in the same order and with the same date each time (the same files give the same zip); then the folders are deleted.
scripts/package.sh runs it on the package's copy of xmb (never on assets/xmb):
    python3 -I scripts/make-asset-pack.py <stage>/xmb bezels ratings lang sounds"""
import os
import shutil
import sys
import zipfile

BLOCK = 65536


def rounded(size):
    return (size + BLOCK - 1) // BLOCK * BLOCK


def main():
    xmb, folders = sys.argv[1], sys.argv[2:]
    if not folders:
        sys.exit("make-asset-pack.py: which folders?")
    names = []
    for folder in folders:
        top = os.path.join(xmb, folder)
        if not os.path.isdir(top):
            sys.exit("make-asset-pack.py: no folder " + top)
        for root, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                names.append(os.path.relpath(os.path.join(root, f), xmb).replace(os.sep, "/"))
    out = os.path.join(xmb, "assets.zip")
    loose = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for name in names:
            with open(os.path.join(xmb, name), "rb") as f:
                data = f.read()
            loose += rounded(len(data))
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    for folder in folders:
        shutil.rmtree(os.path.join(xmb, folder))
    packed = rounded(os.path.getsize(out))
    print("assets.zip: %d files of %s, %.2f MB in the package instead of %.2f MB (%.2f MB less)"
          % (len(names), ", ".join(folders), packed / 1e6, loose / 1e6, (loose - packed) / 1e6))


if __name__ == "__main__":
    main()
