// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Runs a game: Snes9x (its libretro build, linked in) with the picture drawn on the shared display,
// the sound through GameAudio, the DualShock 4s as Super Nintendo controllers, battery saves, save
// states, cheats and the in-game menu (L3 + R3).

#pragma once

#include <string>

#include <imgui.h>

#include "Library.h"

class Display;

namespace Emulator
{
enum class Result
{
  Menu,    // Quit Game: back to the game list
  Failed,  // the game couldn't start (error says why)
  Retry,   // the start watchdog stopped it (no picture in time): to be started once more (src/StartGuard.h)
  // (01.57, the owner) The N64 Resolution changed in the in-game menu: the game's state was kept apart (never a
  // player's slot), the game is to start again at once - its next Run loads that state as its first thing.
  Restart,
};

Result Run(Display& display, const Library::Game& game, ImFont* regular, ImFont* bold,
           std::string* error);
// (01.27, the owner: Triangle's menu with the in-game menu's rows) Triangle -> Load State: the game's next start (its
// Run within a minute) loads that save state (1..8; 0: Continue's own) instead of Continue's, as its first thing.
void LoadStateAtStart(const std::string& game_key, int slot);
// (01.66) The game's cover (its file, from the menu's index; "" none), for its start's own screen - the PS Vita's
// loading screen. Taken by that game's next Run.
void SetStartCover(const std::string& game_key, const std::string& path);
// (01.58) An N64 ROM that N64 Widescreen knows (its header's CRCs in Emulator.cpp's list): the setting is offered for it.
bool N64WidescreenListed(const std::string& rom_path);
}  // namespace Emulator
