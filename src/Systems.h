// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The consoles EmuSuNi plays, and what differs between them: the core, the game files, the game
// folder, the libretro thumbnails' folder, the tab and the badge on the covers. The Nintendo 64
// is in builds with EMUSUNI_N64, the Game Boy, Game Boy Color and Game Boy Advance (mGBA) in
// builds with EMUSUNI_GBA, the PlayStation (SwanStation) in builds with EMUSUNI_PS1, the Nintendo
// DS (melonDS DS) in builds with EMUSUNI_NDS, the PSP (PPSSPP) in builds with EMUSUNI_PSP, the
// Dreamcast (Flycast) in builds with EMUSUNI_DC (scripts/build.sh's).
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
  GB = 3,
  GBC = 4,
  GBA = 5,
  PS1 = 6,
  NDS = 7,
  PSP = 8,
  DC = 9,
};
constexpr int SYSTEM_COUNT = 10;  // the most there can be: Systems() has those of this build

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
  bool by_path = false;        // the core reads the game itself from its path (CD images): no .zip
};

const std::vector<System>& Systems();
// The console (one this build plays; else the Super Nintendo).
const System& GetSystem(SystemId id);
// Does this build play it?
bool HasSystem(SystemId id);
// The Game Boy, Game Boy Color and Game Boy Advance: one core (mGBA), a screen of their own.
inline bool IsHandheld(SystemId id)
{
  return id == SystemId::GB || id == SystemId::GBC || id == SystemId::GBA;
}
// The console whose ROM files have this name's extension (nullptr: none, or a .zip).
const System* SystemOfFile(const std::string& name);
// "snes" -> SNES; nullptr for an unknown key.
const System* SystemByKey(const std::string& key);
// The console of a game key ("nes/Contra (U)" -> NES, "Contra (U)" -> SNES).
const System& SystemOfKey(const std::string& game_key);
