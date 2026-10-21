// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Arcade (FinalBurn Neo): what the app knows of the core's drivers. A game is a .zip (or .7z)
// named after one of them (mslug.zip, as the core wants it: it reads the zip itself), its title the
// driver's full name, its covers named after it too (libretro's thumbnails are), and a vertical game's
// picture is turned by the app (SET_ROTATION). Builds with EMUSUNI_ARCADE; without it the driver list
// is empty.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Arcade
{
struct Driver
{
  std::string name;    // the set's name, the zip's ("mslug")
  std::string title;   // its full name: "Metal Slug - Super Vehicle-001"
  std::string parent;  // a clone's parent set ("sf2" for sf2j), else empty
  std::string bios;    // the BIOS set it needs ("neogeo"), else empty
  std::string year, maker, board;  // "1996", "Nazca", "Neo Geo MVS"
  unsigned flags = 0;              // FinalBurn Neo's BDF_* (a BIOS set, a clone, vertical...)
  unsigned hardware = 0;           // its hardware code
  bool arcade = false;             // an arcade board: not a BIOS set, nor a console's game
  bool vertical = false;           // its monitor stands upright (the picture is turned)
};

// Every driver of the core, read once, the first time (while no game runs: the Library's first scan).
const std::vector<Driver>& Drivers();
// A console's hardware code (the core's Mega Drive, NES, MSX... games and BIOS sets): not the Arcade's.
bool ConsoleHardware(unsigned hardware);
// The driver a zip's name (without the extension, in any letters' case) is, else nullptr.
const Driver* Find(const std::string& set);
// A clone's parent driver (nullptr for a parent, or an unknown one).
const Driver* Parent(const Driver& driver);
// The game's title in the Library: a parent's full name; a clone's parent title plus what its own
// name adds, in parentheses ("Ghouls'n Ghosts (USA)", "Ghouls'n Ghosts (Daimakaimura (Japan))").
std::string Title(const Driver& driver);
// The region a set's full name says ("USA", "Japan", "World"...), else empty.
std::string Region(const Driver& driver);
// libretro's thumbnail name of a title: the characters &*/:`<>?\|" made '_' (the server's names).
std::string CoverName(const std::string& title);
// The zips the game may need that aren't there - its BIOS set's (neogeo.zip) and its parent's -, looked
// for beside the game, in system/fbneo and in system (where the core looks); none when all are there.
// "May": the core takes the ROMs it finds from any zip by their CRCs, so a non-merged set (a clone's zip
// with its parent's files) works without them; they are what a split set lacks when the core refuses it.
std::vector<std::string> Missing(const Driver& driver, const std::string& folder, const std::string& system_dir);
// The message for a game the core refused, from its error screen's text (scripts/build-arcade-core.sh's
// fbneo_emusuni_error) and the zips Missing() found not to be there: the reason in the app's words, the
// romsets and the first few ROMs it names, and where the zips go. '\n' between the lines: each of English
// that fits the error page (the language file translates it), or data under such a line (a set's name, a
// ROM's, a path) that the page may cut at its end.
std::string LoadError(const std::string& core_text, const std::vector<std::string>& not_found,
                      const std::string& system_dir);
// A picture turned (libretro's rotation: 0 as it is, 1 = 90 degrees anticlockwise, 2 = 180, 3 = 270):
// RGB565 rows of `pitch` bytes into `out` (rows of the new width), whose size goes to *w and *h.
void Rotate(const uint16_t* in, size_t pitch, unsigned width, unsigned height, unsigned rotation,
            std::vector<uint16_t>* out, unsigned* w, unsigned* h);
}  // namespace Arcade
