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
// The DualShock 4 control that fast-forwards a game (its own layout, else its console's normal one), as the menus name
// it ("R2", "Touchpad", "D-Pad Up"), or "None". Every console has the Fast Forward (hold) row: on those whose buttons
// are all the console's (the PlayStation, the arcade boards, the Dreamcast, the 3DS) it starts at None.
std::string FastForwardButton(const std::string& game_key, SystemId system);
// The Fast Forward setting's help for a game's or a console's page (game_key empty: the console's normal layout):
// the button that does it now - the player's own if they changed it - or that none does yet. English, for PS4Lang::T.
std::string FastForwardHelp(const std::string& game_key, SystemId system);
// A button capture: true when nothing is pressed; the first pressed control's name, or "".
bool IsAtRest(const OrbisPadData& data);
std::string Capture(const OrbisPadData& data);

// While a game runs: the console's buttons (libretro RETRO_DEVICE_ID_JOYPAD_* bits) for a pad.
struct Layout
{
  uint32_t button_mask[16] = {};  // per libretro joypad id: the DS4 button bits that press it
  bool dpad = true, left_stick = true;
  uint32_t fast_forward = 0;
  uint32_t tap = 0;  // the DS's stylus down (Tap the Screen)
  uint32_t reload = 0;  // a NAOMI or Atomiswave light gun game's reload (Gun Reload: the gun shot off the screen)
};
Layout LayoutFor(const std::string& game_key, SystemId system);
uint16_t PadButtons(const Layout& layout, const OrbisPadData& data);

// Every button of the DualShock 4 (the pad's other bits aren't the player's).
constexpr uint32_t PAD_BUTTONS = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3 | ORBIS_PAD_BUTTON_OPTIONS |
                                 ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_DOWN |
                                 ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2 | ORBIS_PAD_BUTTON_L1 |
                                 ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_TRIANGLE | ORBIS_PAD_BUTTON_CIRCLE |
                                 ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_TOUCH_PAD;
// The pad as nobody touches it: no button, the sticks in the middle, the triggers up, no finger on the
// touchpad (the motion sensors as they are).
OrbisPadData Untouched(const OrbisPadData& data);
// After the in-game menu (whatever took the game back: Resume, Circle, L3+R3, a state loaded, Reset, a disc
// changed; src/Emulator.cpp), player 1's pad reaches the game again only once every button is up: the
// press that closed the menu - Cross, Circle, or L3+R3 and the sticks pushed with them - isn't the game's.
// Until then the game sees the pad untouched.
class MenuRelease
{
public:
  void Start()  // the menu has just closed
  {
    m_waiting = true;
    m_passes = 0;
  }
  bool Waiting() const { return m_waiting; }
  // Player 1's pad for the game this pass (`connected`: read this pass, and not the screenshot tour's). The
  // wait ends at the first pass the pad is read with every button up (the pad goes to the game as it is then);
  // `hidden_passes` then says how many passes it kept the pad untouched, else 0.
  OrbisPadData Pad(const OrbisPadData& data, bool connected, int* hidden_passes = nullptr);

private:
  bool m_waiting = false;
  int m_passes = 0;
};

// The buttons that open the in-game menu (00.68, GitHub issue #5: Settings "menu_buttons", for every game, chosen in
// Settings -> Controls and in the in-game menu's Controls): L3 + R3 pressed together (the default), R3 or the
// touchpad held for a second, or Options + Touchpad (START + SELECT on most consoles); since 00.69 also L1 + R1 or
// L2 + R2 held together for a second (the game sees them meanwhile). The PS button is the PS4's own.
// On the DS and the 3DS, where the touchpad is the touch screen, a touchpad's choice is L3 + R3. L3 + R3 always open
// the menu too (MenuOpener), so a choice can't leave a player without it.
struct MenuButtons
{
  uint32_t mask = 0;   // the DualShock 4 buttons, all down
  float hold = 0.0f;   // seconds they are held (0: pressed together)
};
MenuButtons MenuButtonsFor(const std::string& choice, SystemId system);
// Whether this pass opens the menu (player 1's buttons down now, and those that went down this pass). A hold counts
// again only once its buttons were let go: after the menu closed (Closed()), or a hold that opened it.
class MenuOpener
{
public:
  bool Update(const MenuButtons& buttons, uint32_t held, uint32_t pressed, float dt);
  void Closed();

private:
  float m_time = 0.0f;
  bool m_spent = false;  // the buttons held are a hold that already opened the menu (or were down as it closed)
};
}  // namespace Controls
