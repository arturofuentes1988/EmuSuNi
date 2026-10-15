// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The screenshot tour (for the documentation): data_dir/tour.txt, when it is there at start-up, is
// played by the app itself - waits, button presses, menu commands and screenshots of the screen
// (data_dir/screenshots/<name>.png) - and renamed tour.txt.done at its end. The settings it
// changes ("set <key> <value>") are put back then.
//
//   wait 1.5                      seconds before the next line
//   press CROSS | L3+R3           DualShock 4 buttons, for one frame
//   shot 02-library               a screenshot of the next frame drawn
//   set resolution 5              a global setting, until the tour ends
//   end
//   anything else                 a command for the menu or the game (Menu.cpp, Emulator.cpp)

#pragma once

#include <cstdint>
#include <string>

class Display;

namespace Tour
{
void Load(const std::string& data_dir);
bool Active();

struct Step
{
  uint32_t buttons = 0;  // ORBIS_PAD_BUTTON_* pressed this frame
  std::string command;   // for the menu or the game
  std::string shot;      // a screenshot to take after this frame is drawn
};
// This frame's step (empty while waiting).
Step Next(float dt);
// The screen just drawn, as data_dir/screenshots/<name>.png.
void SaveShot(Display& display, const std::string& name);
// The settings back as they were, tour.txt renamed (also when the last line is played).
void Finish();
}  // namespace Tour
