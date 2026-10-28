// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The consoles EmuSuNi plays, and what differs between them: the core, the game files, the game
// folder, the libretro thumbnails' folder, the tab and the badge on the covers. The Nintendo 64
// is in builds with EMUSUNI_N64, the Game Boy, Game Boy Color and Game Boy Advance (mGBA) in
// builds with EMUSUNI_GBA, the PlayStation (SwanStation) in builds with EMUSUNI_PS1, the Nintendo
// DS (melonDS DS) in builds with EMUSUNI_NDS, the PSP (PPSSPP) in builds with EMUSUNI_PSP, the
// Dreamcast (Flycast) in builds with EMUSUNI_DC, the SG-1000, Master System, Game Gear and Mega
// Drive (Genesis Plus GX) in builds with EMUSUNI_SEGA, the Arcade (FinalBurn Neo) in builds with
// EMUSUNI_ARCADE, the Nintendo 3DS (Azahar) in builds with EMUSUNI_3DS (scripts/build.sh's); Sega's NAOMI
// and Sammy's Atomiswave, Flycast's arcade boards, with the Dreamcast (EMUSUNI_DC); the Sega Saturn
// (YabaSanshiro with its software renderer, experimental) in builds with EMUSUNI_SATURN (with-saturn); the PC Engine
// (Beetle PCE Fast: scripts/build-beetle-cores.sh) in builds with EMUSUNI_PCE (00.82). (00.80 to 00.82 also had the Sega
// CD, the 32X, the PC Engine CD, the Neo Geo Pockets and the WonderSwans: gone in 00.83, the owner's choice. Their keys -
// megacd, 32x, pcecd, ngp, ngpc, wswan, wswanc - in a player's files are no console's now: SystemByKey gives nullptr
// (a saved Console Order, Consoles Shown and the bezels' downloads leave them out), no game of theirs is in the Library,
// and their files - saves, states, covers, settings sections, Roms folders - are left as they are.)
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
  SG1000 = 10,
  SMS = 11,
  GG = 12,
  MD = 13,
  ARCADE = 14,
  THREEDS = 15,
  NAOMI = 16,       // Sega's NAOMI (and NAOMI 2): Flycast, the Dreamcast's core
  ATOMISWAVE = 17,  // Sammy's Atomiswave: Flycast too
  SATURN = 18,      // the Sega Saturn: YabaSanshiro (builds with EMUSUNI_SATURN)
  PCE = 19,         // the PC Engine / TurboGrafx-16: Beetle PCE Fast (builds with EMUSUNI_PCE; 21 in 00.82)
};
// The most there can be: Systems() has those of this build (at most 32: the settings' console masks are 32-bit,
// Settings.h FOR_ALL).
constexpr int SYSTEM_COUNT = 20;

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
  // (The Arcade's by_path game is the .zip itself, named after the core's driver: src/Arcade.h; the NAOMI's
  // and the Atomiswave's after Flycast's set: src/Naomi.h.)
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
// The SG-1000, Master System, Game Gear and Mega Drive: one core (Genesis Plus GX).
inline bool IsGpgx(SystemId id)
{
  return id == SystemId::SG1000 || id == SystemId::SMS || id == SystemId::GG || id == SystemId::MD;
}
// A handheld's LCD (the grid between its dots, LCD Ghosting): the Game Boys' and the Game Gear's
// (not IsHandheld: another core, and its dots aren't square).
inline bool HasLcd(SystemId id)
{
  return IsHandheld(id) || id == SystemId::GG;
}
// The NAOMI and the Atomiswave: Flycast's arcade boards (src/Naomi.h), the Dreamcast's hardware in a cabinet.
inline bool IsFlycastBoard(SystemId id)
{
  return id == SystemId::NAOMI || id == SystemId::ATOMISWAVE;
}
// Flycast's machines: the Dreamcast and its arcade boards (one core; the same Quality levels as the Dreamcast).
inline bool IsFlycast(SystemId id)
{
  return id == SystemId::DC || IsFlycastBoard(id);
}
// The consoles whose game is a romset: a .zip (or .7z) named after the set, never opened by the app (the core
// reads it), found by its name, not its extension: the Arcade's (FinalBurn Neo's drivers) and the NAOMI's and
// Atomiswave's (Flycast's sets).
inline bool IsRomsetSystem(SystemId id)
{
  return id == SystemId::ARCADE || IsFlycastBoard(id);
}
// The console whose ROM files have this name's extension (nullptr: none, or a .zip).
const System* SystemOfFile(const std::string& name);
// "snes" -> SNES; nullptr for an unknown key.
const System* SystemByKey(const std::string& key);
// The console of a game key ("nes/Contra (U)" -> NES, "Contra (U)" -> SNES).
const System& SystemOfKey(const std::string& game_key);
