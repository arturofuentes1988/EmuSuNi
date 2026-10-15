// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Super Nintendo controller on the DualShock 4: which DS4 control presses each SNES button,
// for every game or for one game (Settings -> Controls, or Triangle on a game -> Controls), kept in
// data_dir/controls.ini. Like Dolphin for PS4's Controls page: Cross on a row, then the new button.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <orbis/Pad.h>

namespace Controls
{
struct Binding
{
  const char* key;      // controls.ini key
  const char* label;    // the SNES button
  const char* fallback; // DS4 control (PS4Glyphs names: "Cross", "L1", "Touchpad", ...)
  bool stick_group;     // the D-Pad row: which DS4 controls move it (a list of choices)
};

const std::vector<Binding>& Bindings();
std::vector<std::string> StickChoices();

void Load(const std::string& data_dir);

// The layout edited by the menus: a game's own (game_id) or the normal one (empty).
std::string Get(const Binding& binding, const std::string& game_id);
void Set(const Binding& binding, const std::string& value, const std::string& game_id);
bool HasGameProfile(const std::string& game_id);
void CreateGameProfile(const std::string& game_id);  // starts as a copy of the normal layout
void DeleteGameProfile(const std::string& game_id);
void ResetToDefault(const std::string& game_id);

// The control's name as shown when it has no icon.
std::string Describe(const std::string& value);
// A button capture: true when nothing is pressed; the first pressed control's name, or "".
bool IsAtRest(const OrbisPadData& data);
std::string Capture(const OrbisPadData& data);

// While a game runs: the SNES buttons (libretro RETRO_DEVICE_ID_JOYPAD_* bits) for a pad.
struct Layout
{
  uint32_t button_mask[16] = {};  // per libretro joypad id: the DS4 button bits that press it
  bool dpad = true, left_stick = true;
  uint32_t fast_forward = 0;
};
Layout LayoutFor(const std::string& game_id);
uint16_t SnesButtons(const Layout& layout, const OrbisPadData& data);
}  // namespace Controls
