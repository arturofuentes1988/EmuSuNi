// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Each console's controller on the DualShock 4: which DS4 control presses each of its buttons,
// for every game of the console or for one game (Settings -> Controls, or Triangle on a game ->
// Controls), kept in data_dir/controls.ini: [global] is the Super Nintendo's normal layout, [nes]
// the NES's, a game's key its own. Like Dolphin for PS4's Controls page: Cross on a row, then the
// new button.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <orbis/Pad.h>

#include "Systems.h"

namespace Controls
{
struct Binding
{
  const char* key;      // controls.ini key
  const char* label;    // the console's button
  const char* fallback; // DS4 control (PS4Glyphs names: "Cross", "L1", "Touchpad", ...)
  bool stick_group;     // the D-Pad row: which DS4 controls move it (a list of choices)
};

const std::vector<Binding>& Bindings(SystemId system);
std::vector<std::string> StickChoices();

void Load(const std::string& data_dir);

// The layout edited by the menus: a game's own (its key) or the console's normal one (empty).
std::string Get(const Binding& binding, const std::string& game_key, SystemId system);
void Set(const Binding& binding, const std::string& value, const std::string& game_key, SystemId system);
bool HasGameProfile(const std::string& game_key);
// Starts as a copy of the console's normal layout.
void CreateGameProfile(const std::string& game_key, SystemId system);
void DeleteGameProfile(const std::string& game_key);
void ResetToDefault(const std::string& game_key, SystemId system);

// The control's name as shown when it has no icon.
std::string Describe(const std::string& value);
// A button capture: true when nothing is pressed; the first pressed control's name, or "".
bool IsAtRest(const OrbisPadData& data);
std::string Capture(const OrbisPadData& data);

// While a game runs: the console's buttons (libretro RETRO_DEVICE_ID_JOYPAD_* bits) for a pad.
struct Layout
{
  uint32_t button_mask[16] = {};  // per libretro joypad id: the DS4 button bits that press it
  bool dpad = true, left_stick = true;
  uint32_t fast_forward = 0;
};
Layout LayoutFor(const std::string& game_key, SystemId system);
uint16_t PadButtons(const Layout& layout, const OrbisPadData& data);
}  // namespace Controls
