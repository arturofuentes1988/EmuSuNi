// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Settings.h (Dolphin Emulator Project, GPL-2.0+).

// User settings, shared by the menu (Menu.cpp) and the in-game menu (IngameMenu.cpp). The user's
// choices are kept in data_dir/settings.ini: a [global] section, a [console:<key>] section for a
// console set to "This Console", and one section per game set to "This Game" (its "custom" says
// where the game's settings come from: on, console or global). A setting never changed falls back
// to the built-in default below.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Systems.h"

namespace Settings
{
struct Choice
{
  const char* value;           // stored in settings.ini
  const char* label;           // shown in the menus
  const char* help = nullptr;  // what it does (shown under the settings list)
};

// The consoles a setting is for: bits of SystemId.
constexpr uint32_t FOR_ALL = (1u << SYSTEM_COUNT) - 1;
constexpr uint32_t FOR_SNES = 1u << static_cast<int>(SystemId::SNES);
constexpr uint32_t FOR_NES = 1u << static_cast<int>(SystemId::NES);
constexpr uint32_t FOR_N64 = 1u << static_cast<int>(SystemId::N64);
constexpr uint32_t FOR_GB = 1u << static_cast<int>(SystemId::GB);
constexpr uint32_t FOR_GBC = 1u << static_cast<int>(SystemId::GBC);
constexpr uint32_t FOR_GBA = 1u << static_cast<int>(SystemId::GBA);
constexpr uint32_t FOR_PS1 = 1u << static_cast<int>(SystemId::PS1);
constexpr uint32_t FOR_NDS = 1u << static_cast<int>(SystemId::NDS);
constexpr uint32_t FOR_PSP = 1u << static_cast<int>(SystemId::PSP);
constexpr uint32_t FOR_DC = 1u << static_cast<int>(SystemId::DC);
// The three Game Boys (mGBA).
constexpr uint32_t FOR_HANDHELD = FOR_GB | FOR_GBC | FOR_GBA;
// The Super Nintendo's and the NES's (the N64 draws on the GPU, its own way).
constexpr uint32_t FOR_RETRO = FOR_SNES | FOR_NES;
// The consoles whose picture the CPU hands over as pixels (Resolution, Upscaling Filter).
constexpr uint32_t FOR_PIXELS = FOR_RETRO | FOR_HANDHELD | FOR_NDS;

struct Setting
{
  const char* key;
  const char* label;
  const char* category;
  std::vector<Choice> choices;
  const char* fallback;
  bool live;  // takes effect while a game runs (else: next game start)
  const char* help;
  uint32_t systems = FOR_ALL;
};

inline bool AppliesTo(const Setting& setting, SystemId system)
{
  return (setting.systems >> static_cast<int>(system)) & 1;
}
// For a console this build plays (the N64's settings are in the N64 build only).
bool AppliesToAny(const Setting& setting);
// The console tags of a setting for one console only (" (SNES)"), else "".
std::string SystemTag(const Setting& setting);

const std::vector<Setting>& All();
const std::vector<const char*>& Categories();
const Setting* Find(const std::string& key);

// settings.ini
void Load(const std::string& data_dir);
void Save();

// Where a game's settings come from (Apply To): every game's (global), its console's, or its own.
// A game never set follows its console's settings when the console has them, else the global ones.
enum class Scope
{
  Global,
  Console,
  Game,
};
Scope ScopeOf(const std::string& game_id);
// Console, the first time: the console starts with a copy of what the game uses now; Game, the
// first time: the game starts with a copy of what it uses now.
void SetScope(const std::string& game_id, Scope scope);

// Effective value: the game's own (This Game), else its console's (This Console), else global,
// else the default.
std::string Get(const std::string& key, const std::string& game_id = {});
int GetInt(const std::string& key, const std::string& game_id = {});
bool GetOn(const std::string& key, const std::string& game_id = {});
// Stores a value globally (game_id empty), or where the game's settings come from (its scope).
void Set(const std::string& key, const std::string& value, const std::string& game_id = {});
std::vector<std::pair<std::string, std::string>> GlobalValues();
// The screenshot tour's values (Tour.cpp's "set"): above every game's, console's and global value
// until cleared, never written to settings.ini.
void SetTourValue(const std::string& key, const std::string& value);
void ClearTourValues();
bool IsCustom(const std::string& game_id);  // This Game
// A custom game's values become the global ones (every game uses them); the game follows them.
void CopyGameToGlobal(const std::string& game_id);

void ResetGlobal(const std::string& key);
void ResetGame(const std::string& game_id);
// The game's console has no settings of its own anymore (its games follow the global ones).
void ResetConsole(const std::string& game_id);
void ResetAll();

std::string LabelOf(const Setting& setting, const std::string& value);
const Choice* FindChoice(const Setting& setting, const std::string& value);
std::string Step(const Setting& setting, const std::string& value, int direction);
}  // namespace Settings
