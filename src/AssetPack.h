// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The package's small files in one (00.90): a PKG keeps each file in whole 64 KiB blocks, so the hundred or so small
// files of xmb - the built-in bezels, the star ratings' indexes, the languages, the sounds - took about 3.6 MB more
// than their bytes. scripts/package.sh puts them in <assets>/xmb/assets.zip (scripts/make-asset-pack.py: stored, not
// compressed, named from xmb: "bezels/tv.png", "lang/es.ini") and leaves their folders out of the package.
//
// A path of the app's own files (<assets>/xmb/...) is read here as it always was: the loose file first (the PC's tests,
// a folder put back by hand), else the zip's entry. The zip's index (its central directory: each stored entry's place)
// is read once, at the first file not found loose; each entry is read with the zip opened again (nothing shared while
// reading: any thread). A zip that can't be read is no pack (logged): the loose files only. Nothing here throws.

#pragma once

#include <cstdio>
#include <string>

namespace AssetPack
{
// Whether the file is there, loose or in the zip.
bool Exists(const std::string& path);
// The whole file (false: not there, or not read).
bool Read(const std::string& path, std::string* data);
// The file to read as a FILE: the loose file, or a stream over the zip entry's bytes (fclose frees them). nullptr: not
// there (errno says why).
FILE* Open(const std::string& path);
}  // namespace AssetPack
