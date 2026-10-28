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
};

Result Run(Display& display, const Library::Game& game, ImFont* regular, ImFont* bold,
           std::string* error);
}  // namespace Emulator
