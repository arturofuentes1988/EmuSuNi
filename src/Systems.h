// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The consoles EmuSuNi plays, and what differs between them: the core, the game files, the game
// folder, the libretro thumbnails' folder, the tab and the badge on the covers. The Nintendo 64
// is in builds with EMUSUNI_N64 (scripts/build.sh's).
//
// A game is known by its key: its file name without the extension for the Super Nintendo (as
// before EmuSuNi played other consoles), "<console>/<file name>" for the others. Its settings,
// controls, favorite mark, saves, states, cheats and cover use the key, so the files of the
// other consoles' games go in subfolders (saves/nes/, covers/nes/...).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Cores.h"

enum class SystemId
{
  SNES = 0,
  NES = 1,
  N64 = 2,
};
constexpr int SYSTEM_COUNT = 3;  // the most there can be: Systems() has those of this build

struct System
{
  SystemId id;
  const char* key;             // "snes", "nes": settings and folder names
  const char* name;            // "Super Nintendo"
  const char* name_japan;      // "Super Famicom" (Japanese games)
  const char* tab;             // the Library tab: "SNES"
  const char* badge;           // on the covers: "SNES"
  const char* badge_japan;     // "SUPER FAMICOM"
  uint32_t color, color_japan; // the badges' colours (RGB)
  const char* prefix;          // the game keys' prefix: "" (Super Nintendo), "nes/"
  const char* default_folder;  // the game folder (under data_dir, or absolute) until another is chosen
  std::vector<std::string> extensions;  // ROM files (lower case, with the dot); .zip besides
  const char* thumbnails;      // libretro's thumbnail folder
  const char* thumbnails_repo; // its GitHub project (libretro-thumbnails/...)
  const CoreApi* core;
};

const std::vector<System>& Systems();
const System& GetSystem(SystemId id);
// The console whose ROM files have this name's extension (nullptr: none, or a .zip).
const System* SystemOfFile(const std::string& name);
// "snes" -> SNES; nullptr for an unknown key.
const System* SystemByKey(const std::string& key);
// The console of a game key ("nes/Contra (U)" -> NES, "Contra (U)" -> SNES).
const System& SystemOfKey(const std::string& game_key);
