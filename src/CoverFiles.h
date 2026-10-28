// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The cover pictures on disk (00.81). A covers folder - data_dir/covers, a USB drive's EmuSuNi/covers (Settings ->
// Covers Saved On) - has a folder per console named by its key: covers/snes/, covers/nes/, covers/md/... (any case).
// A cover is a PNG or a JPEG (.png, .jpg, .jpeg, any case; src/Image.h reads either by what it is) named like the
// game's file or its title (an arcade game: its full name or its parent's). The Super Nintendo's covers were right in
// covers/ before 00.81 (its game keys have no prefix, src/Systems.h): still read there, after covers/snes/, and moved
// into covers/snes/ once (MoveOldSnesCovers).

#pragma once

#include <map>
#include <string>
#include <vector>

#include "Library.h"

namespace CoverFiles
{
// A file name a cover can have: .png, .jpg or .jpeg (any case); not a Mac's ._Name.png or Find Cover's .preview1.png.
bool IsPicture(const std::string& file_name);
// The names a game's cover is looked for by, the index's way (lower case, the console's prefix, no extension): its
// key, its title, an arcade game's full name and its parent's (libretro's thumbnails are named so).
std::vector<std::string> Names(const Library::Game& game);
// Where a new cover of a game goes in a covers folder (Download Covers, Find Cover): <place>/<console>/<file name>.png,
// the console's folder as it is there in another case (a covers/SNES made by hand), else its key.
std::string NewFile(const std::string& place, const std::string& game_key);

// The covers of a covers folder: name the index's way -> file. Each console's folder (any case, all of them), and the
// Super Nintendo's old place (the covers folder itself), whose pictures lose to covers/snes/'s of the same name; in
// one folder, of two pictures of one name (a .png and a .jpg, two cases) the newest. Added to *index over what is
// there (the places listed later win).
using Index = std::map<std::string, std::string>;
void List(const std::string& place, Index* index);

// The Super Nintendo's covers right in <place> moved into <place>/snes/, once: then <place>/snes/.moved-from-covers
// says it was done. Each picture is renamed (no copy), so a stop half-way leaves each in one of the two folders, both
// read, and the next time goes on. A picture whose name is already in snes/ stays (snes/'s is the one shown).
// Logged. (A folder that can't be written: tried again next time.)
void MoveOldSnesCovers(const std::string& place);
}  // namespace CoverFiles
