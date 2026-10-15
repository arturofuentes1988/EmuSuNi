// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Settings.h (Dolphin Emulator Project, GPL-2.0+).

// User settings, shared by the menu (Menu.cpp) and the in-game menu (IngameMenu.cpp). The user's
// choices are kept in data_dir/settings.ini: a [global] section and one section per game for games
// set to "This Game". A setting never changed falls back to the built-in default below.

#pragma once

#include <string>
#include <vector>

namespace Settings
{
struct Choice
{
  const char* value;           // stored in settings.ini
  const char* label;           // shown in the menus
  const char* help = nullptr;  // what it does (shown under the settings list)
};

struct Setting
{
  const char* key;
  const char* label;
  const char* category;
  std::vector<Choice> choices;
  const char* fallback;
  bool live;  // takes effect while a game runs (else: next game start)
  const char* help;
};

const std::vector<Setting>& All();
const std::vector<const char*>& Categories();
const Setting* Find(const std::string& key);

// settings.ini
void Load(const std::string& data_dir);
void Save();

// Effective value: the game's own (custom games), else global, else the default.
std::string Get(const std::string& key, const std::string& game_id = {});
int GetInt(const std::string& key, const std::string& game_id = {});
bool GetOn(const std::string& key, const std::string& game_id = {});
// Stores a value globally (game_id empty) or for a custom game.
void Set(const std::string& key, const std::string& value, const std::string& game_id = {});
std::vector<std::pair<std::string, std::string>> GlobalValues();
bool IsCustom(const std::string& game_id);
// Custom on: the game starts with a copy of the effective global values.
void SetCustom(const std::string& game_id, bool custom);
// A custom game's values become the global ones (every game uses them); the game follows them.
void CopyGameToGlobal(const std::string& game_id);

void ResetGlobal(const std::string& key);
void ResetGame(const std::string& game_id);
void ResetAll();

std::string LabelOf(const Setting& setting, const std::string& value);
const Choice* FindChoice(const Setting& setting, const std::string& value);
std::string Step(const Setting& setting, const std::string& value, int direction);
}  // namespace Settings
