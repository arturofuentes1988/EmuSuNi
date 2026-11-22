"""The package's small files in one (00.90): a PKG keeps each file in whole 64 KiB blocks, so xmb's hundred or so small
files took about 3.6 MB more than their bytes. The folders named here go into <xmb>/assets.zip, named from xmb
("bezels/tv.png", "lang/es.ini"), in the same order and with the same date each time (the same files give the same
zip); then the folders are deleted. (01.26) Each file deflated (level 9) when that makes it at least 2% smaller, else
stored: the languages 4.1 -> 1.2 MB, the ratings' indexes 1.2 -> 0.5, the sounds and the bezels a little
(src/AssetPack.cpp inflates them). (01.27) A folder's files of one kind ("fonts/*.ttf": its own files only, not its
subfolders') go in alone, the rest of that folder stays (the fonts' licences); a file named alone goes in too; and
-o <zip> writes another zip than <folder>/assets.zip (PPSSPP's files: <stage>/ppsspp/PPSSPP.zip, read by the PSP core
as on Android). scripts/package.sh runs it on the package's copies (never on assets/xmb):
    python3 -I scripts/make-asset-pack.py <stage>/xmb bezels ratings lang sounds 'fonts/*.ttf'
    python3 -I scripts/make-asset-pack.py -o <stage>/ppsspp/PPSSPP.zip <stage>/ppsspp/PPSSPP compat.ini flash0 ..."""
import fnmatch
import os
import shutil
import sys
import zipfile
import zlib

BLOCK = 65536


def rounded(size):
    return (size + BLOCK - 1) // BLOCK * BLOCK


def main():
    args = sys.argv[1:]
    out = None
    if args[:1] == ["-o"]:
        out, args = args[1], args[2:]
    if len(args) < 2:
        sys.exit("make-asset-pack.py: [-o <zip>] <folder> <what>...")
    xmb, folders = args[0], args[1:]
    if out is None:
        out = os.path.join(xmb, "assets.zip")
    names = []
    picked = []  # (01.27) the files taken alone or from "folder/*.ext"
    for folder in folders:
        if "*" in folder:
            folder, pattern = folder.rsplit("/", 1)
            top = os.path.join(xmb, folder)
            if not os.path.isdir(top):
                sys.exit("make-asset-pack.py: no folder " + top)
            files = sorted(f for f in os.listdir(top) if fnmatch.fnmatch(f, pattern)
                           and os.path.isfile(os.path.join(top, f)))
            if not files:
                sys.exit("make-asset-pack.py: no %s in %s" % (pattern, top))
            for f in files:
                names.append(folder + "/" + f)
                picked.append(names[-1])
            continue
        top = os.path.join(xmb, folder)
        if os.path.isfile(top):
            names.append(folder)
            picked.append(folder)
            continue
        if not os.path.isdir(top):
            sys.exit("make-asset-pack.py: no folder " + top)
        for root, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                names.append(os.path.relpath(os.path.join(root, f), xmb).replace(os.sep, "/"))
    loose = deflated = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for name in names:
            with open(os.path.join(xmb, name), "rb") as f:
                data = f.read()
            loose += rounded(len(data))
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            smaller = len(data) > 0 and len(zlib.compress(data, 9)) < len(data) * 0.98
            info.compress_type = zipfile.ZIP_DEFLATED if smaller else zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            z.writestr(info, data, compresslevel=9 if smaller else None)
            deflated += smaller
    for folder in folders:
        if "*" not in folder and os.path.isdir(os.path.join(xmb, folder)):
            shutil.rmtree(os.path.join(xmb, folder))
    for name in picked:
        os.remove(os.path.join(xmb, name))
    packed = rounded(os.path.getsize(out))
    print("%s: %d files of %s (%d deflated), %.2f MB in the package instead of %.2f MB (%.2f MB less)"
          % (os.path.basename(out), len(names), ", ".join(folders), deflated, packed / 1e6, loose / 1e6,
             (loose - packed) / 1e6))


if __name__ == "__main__":
    main()
