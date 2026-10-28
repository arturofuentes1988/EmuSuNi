// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4XMB.h (Dolphin Emulator Project, GPL-2.0+).

// The launcher: Dolphin for PS4's menu for the Super Nintendo. A Library of covers (tabs switched
// with L1 / R1) or the PSP-style XMB; the games, saved data, settings and themes.

#pragma once

#include <memory>
#include <string>

#include <imgui.h>

#include "Library.h"

class Display;

class Menu
{
public:
  Menu(Display& display, const std::string& data_dir, const std::string& assets_dir);
  ~Menu();
  // Shows the menu until a game is picked (true, *game set).
  bool Run(Library::Game* game);
  // After a game: an error to show (empty: none).
  void GameEnded(const std::string& error);
  // The default game (Triangle -> Set as Default Game), when it exists.
  bool DefaultGame(Library::Game* game);
  // A game of the lists by its file (the game the start watchdog restarted EmuSuNi for: src/StartGuard.h).
  bool FindGame(const std::string& path, Library::Game* game);
  ImFont* RegularFont() const;
  ImFont* BoldFont() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};
