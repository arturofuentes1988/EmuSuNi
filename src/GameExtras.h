// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
#pragma once

// (01.22, the owner: "En el menú de triángulo en una carátula y en el menú L2 y R2 en un juego") What a game can have
// installed, managed from the game: Triangle on its cover (the menu) and the in-game menu - no longer from Settings.
//   - The 3DS: its update, its DLC (from the player's CIAs: src/ThreeDsCia.h) and its mods.
//   - The Arcade: a patched romset (01.05: FinalBurn Neo's system/fbneo/patched/<set>.zip, its CRCs unchecked, never
//     online) - here: the one in use, removed, or installed from a "patched" folder in the Arcade's game folders
//     (Roms/arcade/patched, which the Library never lists as games: src/Library.cpp).
// The 3DS core installs and removes only with no game running, and both are read at the game's start: from the in-game
// menu the game closes first (AskAfterGame), then the menu opens that game's panel on the install or removal asked
// for (its own confirmation), with Start the Game at its top.

#include <cstdint>
#include <string>
#include <vector>

#include "Systems.h"

namespace GameExtras
{
// ---- From the in-game menu to the menu ----
// What the menu does once the game has closed: the game's file and the action ("3ds:install:<CIA's path>",
// "3ds:remove_update", "3ds:remove_dlc", "arcade:install:<file's path>", "arcade:remove").
struct AfterGame
{
  std::string game_path;
  std::string action;
};
void AskAfterGame(const std::string& game_path, const std::string& action);
// The one asked (and forgotten); false: none.
bool TakeAfterGame(AfterGame* out);

// The folders the in-game menu looks into (the menu's, which knows the game folders and the drives): the 3DS's "cia"
// folders, the Arcade's "patched" ones. Set by the menu as a game starts.
void SetInstallFolders(SystemId system, const std::vector<std::string>& folders);
std::vector<std::string> InstallFolders(SystemId system);

// ---- The Arcade's patched romsets ----
struct PatchedFile
{
  std::string path;  // "": none
  std::string name;  // its file name
  uint64_t size = 0;
};
// The one FinalBurn Neo uses for a set (`dir`: <system>/fbneo/patched): <set>.zip, else <set>.7z (any letters' case).
PatchedFile PatchedInUse(const std::string& dir, const std::string& set);
// The "patched" folders (any case) of the Arcade's game folders (`make`: made in each one that has none, so the player
// sees where they go).
std::vector<std::string> PatchedFolders(const std::vector<std::string>& game_folders, bool make);
// The files of a set in those folders: <set>.zip or <set>.7z (any letters' case), by name.
std::vector<PatchedFile> PatchedFound(const std::vector<std::string>& folders, const std::string& set);
// The set's patched romsets deleted from `dir` (both extensions). False: one couldn't be (error says why).
bool RemovePatched(const std::string& dir, const std::string& set, std::string* error);

// The copy of one into `dir` as <set>.<its extension>, on a thread of its own (one at a time): through
// <set>.<ext>.part, renamed once whole; then the set's other extension removed (only the new one used). Cancelled or
// failed: the part removed, what was in use before stays.
struct CopyProgress
{
  bool busy = false;  // copying
  bool done = false;  // finished since StartCopy (below), until ForgetCopy
  bool ok = false;
  bool cancelled = false;
  uint64_t bytes = 0, total = 0;
  std::string from, to;
  std::string error;
};
bool StartCopy(const std::string& from, const std::string& dir, const std::string& set);
CopyProgress CopyStatus();
void CancelCopy();
void ForgetCopy();  // (its thread joined)

// The free space of the file system holding `folder` (bytes; -1 not known).
int64_t FreeBytes(const std::string& folder);
// "640 KB", "512 MB", "1.4 GB".
std::string SizeText(uint64_t bytes);
}  // namespace GameExtras
