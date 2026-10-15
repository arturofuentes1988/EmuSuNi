// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game library: Super Nintendo ROMs (.sfc .smc .swc .fig .bs, or one of them in a .zip) in the
// game folder and its subfolders. A game is known by its file name without the extension (its
// "id"): its settings, saves, states, cheats and cover use it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
};

bool IsRomName(const std::string& name);
// The games in folder and its subfolders (4 levels), by title.
std::vector<Game> Scan(const std::string& folder);
// How many games a folder holds directly (the folder browser).
int CountGames(const std::string& folder);
// The ROM's bytes (the first ROM inside a .zip).
bool ReadRom(const std::string& path, std::vector<uint8_t>* data, std::string* error);
// The title, region and tags from a file name without its extension.
void ParseName(const std::string& stem, std::string* title, std::string* region, std::string* tags);
}  // namespace Library
