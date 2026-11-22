// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.81, the master: a player renamed 920 Mega Drive games in the app - Triangle -> Rename, names.ini - and wants the
// files to have those names, to move them or share the list) Rename Files With My Names: each of a console's games
// with a name of the player's own gets it as its file's name, in its folder, with its extension - and what goes by the
// game's key (its file's name) goes with it in the player's data folder: its battery saves, save states, cheats,
// covers (and the one kept from before), its bezel. The menu (src/Menu.cpp) does the rest: settings, controls, names,
// favorites, played, the default game, My Stats. Apart from the menu, as GameDelete: the PC test runs it as it is.
//
// Only the consoles of one file a game whose saves go by its key (cartridges; the N64, the DS, the 3DS): never an
// Arcade, NAOMI or Atomiswave zip (its name is its romset's), MUGEN, OpenBOR or the PS Vita (folders), nor a disc
// (a .cue's tracks, an .m3u's discs, a .gdi - and the consoles of discs, whose cores keep saves of their own by name).
// Nothing is ever written over: a name taken - by a file, or another game of the console - takes " (2)", " (3)"...

#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Systems.h"

namespace GameRename
{
// The console's games can take their names as their files'.
bool ConsoleTakes(SystemId system);
// A name as a file's on a USB drive's FAT32 or exFAT and on the PS4: ": " " - ", the characters they don't take
// (? / \ * " < > | and controls) out or as '-', no space or dot at the ends, 180 bytes at most (a whole UTF-8 character
// cut). "": nothing usable is left.
std::string FileName(const std::string& name);

struct Wanted
{
  std::string path;   // the game's file
  std::string key;    // its key (Systems.h: the console's prefix and the file's name without its extension)
  std::string name;   // the player's name for it
  size_t files = 1;   // its files (Library::GameFiles): more than one keeps its names
};
struct Plan
{
  std::string from, to;          // the game's file now and its new path
  std::string old_key, new_key;  // its key now and its new one
};
// Why a game keeps its file's name.
enum class Kept
{
  Files,    // several files (a disc's tracks, discs)
  Missing,  // its file isn't there
  Name,     // its name leaves nothing for a file's
  Same,     // its file has that name already
};
// The plans, in the order wanted; `ids` the lower-case names (without extension) of the console's games - every one,
// these too: a new name is never another game's key, nor one with saves, states or cheats already in the player's
// data folder `user_dir` (a game gone, whose data stays: the renamed game's own data couldn't follow it there). Each
// kept game's reason in `kept` (key, why).
std::vector<Plan> MakePlans(const std::vector<Wanted>& wanted, std::set<std::string> ids, const std::string& user_dir,
                            std::vector<std::pair<std::string, Kept>>* kept);
// A key has saves, states or cheats in the player's data folder.
bool HasData(const std::string& user_dir, const std::string& key);

struct Moved
{
  std::vector<std::pair<std::string, std::string>> files;  // each file moved: from, to
  std::vector<std::string> left;                           // a data file left where it was: its new name was taken
  std::string error;                                       // the game's own file couldn't be renamed: why
};
// The game's file renamed - its data stays as it is when it can't be -, then each file of its key in the player's data
// folder (`user_dir`) and in the covers' places (the internal storage's and a USB drive's covers folders).
bool Apply(const Plan& plan, const std::string& user_dir, const std::vector<std::string>& cover_places, Moved* moved);
}  // namespace GameRename
