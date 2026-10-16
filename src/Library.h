// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game library: each console's ROMs (Systems.h: .sfc .smc .swc .fig .bs for the Super
// Nintendo, .nes .fds .unf for the NES, or one of them in a .zip) in its game folder and the
// subfolders. A game is known by its key (Systems.h): its settings, saves, states, cheats and
// cover use it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Systems.h"

namespace Library
{
struct Game
{
  std::string path;    // the ROM or .zip
  std::string id;      // file name without extension
  std::string title;   // tidied up: "The 7th Saga" for "7th Saga, The (U) [!]"
  std::string region;  // "USA", "Europe", "Japan", ... or empty
  std::string tags;    // what else the name says: "[!]", "(V1.1)", "(Proto)"
  uint64_t size = 0;
  SystemId system = SystemId::SNES;
  std::string key;     // id for the Super Nintendo, "<console>/<id>" for the others (Systems.h)
  bool custom_title = false;  // the title is a name of the user's own (Rename)
};

bool IsRomName(const std::string& name);
// One console's games in folder and its subfolders (4 levels), by title. open_zips: a .zip is
// opened to see which console's ROM it holds (needed when consoles share a folder), else every
// .zip there is taken as the console's.
std::vector<Game> Scan(const std::string& folder, SystemId system, bool open_zips = true);
// How many of a console's games a folder holds directly (the folder browser).
int CountGames(const std::string& folder, SystemId system);
// The ROM's bytes (the first ROM inside a .zip).
bool ReadRom(const std::string& path, std::vector<uint8_t>* data, std::string* error);
// The title, region and tags from a file name without its extension.
void ParseName(const std::string& stem, std::string* title, std::string* region, std::string* tags);
}  // namespace Library
