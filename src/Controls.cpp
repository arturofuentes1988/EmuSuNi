// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Controls.h"

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "Log.h"

namespace Controls
{
namespace
{
// libretro joypad ids (libretro.h RETRO_DEVICE_ID_JOYPAD_*).
enum JoypadId
{
  ID_B = 0,
  ID_Y = 1,
  ID_SELECT = 2,
  ID_START = 3,
  ID_UP = 4,
  ID_DOWN = 5,
  ID_LEFT = 6,
  ID_RIGHT = 7,
  ID_A = 8,
  ID_X = 9,
  ID_L = 10,
  ID_R = 11,
  ID_L2 = 12,
  ID_R2 = 13,
  ID_L3 = 14,
  ID_R3 = 15,
};

// Same positions as on the SNES pad: B bottom, A right, Y left, X top.
const std::vector<Binding> SNES_BINDINGS = {
    {"b", "B", "Cross", false},          {"a", "A", "Circle", false},
    {"y", "Y", "Square", false},         {"x", "X", "Triangle", false},
    {"l", "L", "L1", false},             {"r", "R", "R1", false},
    {"start", "START", "Options", false}, {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The NES pad: B left, A right as on the SNES pad; turbo B and turbo A (Settings -> Emulation ->
// Turbo Buttons) where Y and X are; FCEUmm's Famicom Disk System buttons on L and R.
const std::vector<Binding> NES_BINDINGS = {
    {"b", "B", "Cross", false},                 {"a", "A", "Circle", false},
    {"turbo_b", "Turbo B", "Square", false},    {"turbo_a", "Turbo A", "Triangle", false},
    {"start", "START", "Options", false},       {"select", "SELECT", "Touchpad", false},
    {"l", "Disk Side (FDS)", "L1", false},      {"r", "Insert/Eject Disk (FDS)", "R1", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The N64 pad: the left stick is its control stick and the right stick its C buttons (both
// analog); A and B where they sit on its pad (B up and left of A); Z on both triggers (GoldenEye
// fires and Zelda targets with either), R and L on R1 and L1; C Up and C Down also on Triangle and
// Circle. Fast forward on the touchpad: R2 is Z here.
const std::vector<Binding> N64_BINDINGS = {
    {"n64_a", "A", "Cross", false},          {"n64_b", "B", "Square", false},
    {"z", "Z", "L2", false},                 {"z2", "Z (second trigger)", "R2", false},
    {"n64_r", "R", "R1", false},             {"n64_l", "L", "L1", false},
    {"start", "START", "Options", false},
    {"c_up", "C Up", "Triangle", false},     {"c_down", "C Down", "Circle", false},
    {"c_left", "C Left", "None", false},     {"c_right", "C Right", "None", false},
    {"dpad", "D-Pad", "D-Pad", true},
    {"fast_forward", "Fast Forward (hold)", "Touchpad", false},
};
// The PlayStation pad is the DualShock 4's own: every button where it is, both sticks the
// DualShock's (analog). No fast forward: the touchpad is SELECT.
const std::vector<Binding> PS1_BINDINGS = {
    {"ps_cross", "Cross", "Cross", false},       {"ps_circle", "Circle", "Circle", false},
    {"ps_square", "Square", "Square", false},    {"ps_triangle", "Triangle", "Triangle", false},
    {"l", "L1", "L1", false},                    {"r", "R1", "R1", false},
    {"l2", "L2", "L2", false},                   {"r2", "R2", "R2", false},
    {"l3", "L3", "L3", false},                   {"r3", "R3", "R3", false},
    {"start", "START", "Options", false},        {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The Nintendo DS: its buttons where the SNES pad has them (A right, B bottom, X top, Y left), SELECT
// on L2 (the touchpad is its touch screen, a trackpad: the finger moves the stylus, a tap or the touchpad's click
// touches - src/Touchpad.h);
// L3 blows into its microphone (melonDS DS), R3 swaps the two screens (the app's view,
// src/Emulator.cpp NdsSwapped).
const std::vector<Binding> NDS_BINDINGS = {
    {"b", "B", "Cross", false},                  {"a", "A", "Circle", false},
    {"y", "Y", "Square", false},                 {"x", "X", "Triangle", false},
    {"l", "L", "L1", false},                     {"r", "R", "R1", false},
    {"start", "START", "Options", false},        {"select", "SELECT", "L2", false},
    {"l3", "Microphone (blow)", "L3", false},    {"r3", "Swap Screens", "R3", false},
    {"tap", "Tap the Screen", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The PSP: its buttons where the DualShock 4 has them, its stick the left stick (analog), SELECT on
// the touchpad, fast forward on R2.
const std::vector<Binding> PSP_BINDINGS = {
    {"ps_cross", "Cross", "Cross", false},       {"ps_circle", "Circle", "Circle", false},
    {"ps_square", "Square", "Square", false},    {"ps_triangle", "Triangle", "Triangle", false},
    {"l", "L", "L1", false},                     {"r", "R", "R1", false},
    {"start", "START", "Options", false},        {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The Dreamcast: A bottom, B right, X left, Y top (where the DualShock 4 has them), START on Options,
// its analog triggers on L2 / R2 (src/Emulator.cpp: their pressure), its stick the left stick
// (Flycast reads the RetroPad's B as A, A as B, Y as X, X as Y).
const std::vector<Binding> DC_BINDINGS = {
    {"dc_a", "A", "Cross", false},         {"dc_b", "B", "Circle", false},
    {"dc_x", "X", "Square", false},        {"dc_y", "Y", "Triangle", false},
    {"l2", "L Trigger", "L2", false},      {"r2", "R Trigger", "R2", false},
    {"start", "START", "Options", false},
    {"dpad", "D-Pad", "D-Pad", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The Mega Drive's six-button pad: A, B and C along the bottom (Square, Cross, Circle), X, Y and Z
// above them (L1, Triangle, R1), MODE on the touchpad. A game for the three-button pad has only A,
// B, C and START.
const std::vector<Binding> MD_BINDINGS = {
    {"md_a", "A", "Square", false},             {"md_b", "B", "Cross", false},
    {"md_c", "C", "Circle", false},             {"md_x", "X", "L1", false},
    {"md_y", "Y", "Triangle", false},           {"md_z", "Z", "R1", false},
    {"start", "START", "Options", false},       {"mode", "MODE", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The Master System's and the SG-1000's pad: buttons 1 and 2 (Cross, Circle); the console's PAUSE
// button on Options.
const std::vector<Binding> SMS_BINDINGS = {
    {"button_1", "Button 1", "Cross", false},   {"button_2", "Button 2", "Circle", false},
    {"start", "PAUSE", "Options", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The Game Gear: the same buttons 1 and 2, and its START.
const std::vector<Binding> GG_BINDINGS = {
    {"button_1", "Button 1", "Cross", false},   {"button_2", "Button 2", "Circle", false},
    {"start", "START", "Options", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The Arcade (FinalBurn Neo's "Modern" RetroPad): the DualShock 4's buttons as they are. Buttons 1-4
// on Cross, Circle, Square, Triangle, 5-8 on R2, R1, L2, L1, 9 and 10 on R3 and L3 (a Neo Geo game's
// A, B, C, D on Square, Cross, Triangle, Circle, as on its own pad; a six-button fighting game's three
// punches on Square, Triangle, R1 and its kicks on Cross, Circle, R2), START on Options, the coin slot
// on the touchpad. No fast forward: every button is a cabinet's.
const std::vector<Binding> ARCADE_BINDINGS = {
    {"b", "Button 1", "Cross", false},        {"a", "Button 2", "Circle", false},
    {"y", "Button 3", "Square", false},       {"x", "Button 4", "Triangle", false},
    {"r2", "Button 5", "R2", false},          {"r", "Button 6", "R1", false},
    {"l2", "Button 7", "L2", false},          {"l", "Button 8", "L1", false},
    {"r3", "Button 9", "R3", false},          {"l3", "Button 10", "L3", false},
    {"start", "START", "Options", false},     {"select", "COIN", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The NAOMI (Flycast's arcade board, as its libretro core reads the RetroPad: B, A, Y, X, R, L, R2, L2 are buttons 1
// to 8, SELECT the coin slot, L3 and R3 the board's TEST and SERVICE buttons): buttons 1 to 4 on Cross, Circle,
// Square, Triangle, 5 and 6 on R1 and L1, 7 and 8 on R2 and L2 (a driving game's accelerator and brake are those
// triggers' pressure: src/Emulator.cpp), START on Options, the coin slot on the touchpad. TEST and SERVICE (the
// operator menu) on no button: the in-game menu presses them (Arcade Button), or the player puts them on one. A
// light gun game aims with the left stick and reloads with Gun Reload (R1: the gun games use no button 5). No
// fast forward: every button is a cabinet's.
const std::vector<Binding> NAOMI_BINDINGS = {
    {"b", "Button 1", "Cross", false},       {"a", "Button 2", "Circle", false},
    {"y", "Button 3", "Square", false},      {"x", "Button 4", "Triangle", false},
    {"r", "Button 5", "R1", false},          {"l", "Button 6", "L1", false},
    {"r2", "Button 7", "R2", false},         {"l2", "Button 8", "L2", false},
    {"start", "START", "Options", false},    {"select", "COIN", "Touchpad", false},
    {"l3", "TEST", "None", false},           {"r3", "SERVICE", "None", false},
    {"reload", "Gun Reload", "R1", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The Atomiswave: the same, with its five buttons (the core gives L, L2 and R2 no button on this board; a driving
// game's pedals are the triggers' pressure).
const std::vector<Binding> ATOMISWAVE_BINDINGS = {
    {"b", "Button 1", "Cross", false},       {"a", "Button 2", "Circle", false},
    {"y", "Button 3", "Square", false},      {"x", "Button 4", "Triangle", false},
    {"r", "Button 5", "R1", false},
    {"start", "START", "Options", false},    {"select", "COIN", "Touchpad", false},
    {"l3", "TEST", "None", false},           {"r3", "SERVICE", "None", false},
    {"reload", "Gun Reload", "R1", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The Nintendo 3DS: its buttons where the SNES pad has them (A right, B bottom, X top, Y left), L and R on
// L1 and R1, ZL and ZR (the New 3DS's: Old mode has no use for them, but the Circle Pad Pro games read them)
// on L2 and R2, START on Options, SELECT on L3, the Circle Pad the left stick and the C-stick the right
// stick (both analog). The touchpad is the touch screen, as the DS's (a trackpad: the finger moves the stylus,
// a tap or the touchpad's click touches), and R3 swaps the two screens (the core's swap option, src/Emulator.cpp). No
// HOME: L3 is its button and the core's own screen swap in Azahar, so the app never passes L3 on. No fast
// forward by default (R2 is ZR): most 3D games run below full speed on the PS4.
const std::vector<Binding> THREEDS_BINDINGS = {
    {"b", "B", "Cross", false},            {"a", "A", "Circle", false},
    {"y", "Y", "Square", false},           {"x", "X", "Triangle", false},
    {"l", "L", "L1", false},               {"r", "R", "R1", false},
    {"l2", "ZL", "L2", false},             {"r2", "ZR", "R2", false},
    {"start", "START", "Options", false},  {"select", "SELECT", "L3", false},
    {"r3", "Swap Screens", "R3", false},   {"tap", "Tap the Screen", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad", true},
    {"fast_forward", "Fast Forward (hold)", "None", false},
};
// The Game Boy and Game Boy Color: B left, A right as on the NES pad; mGBA's turbo B and turbo A
// (on Y and X) where Y and X are.
const std::vector<Binding> GB_BINDINGS = {
    {"b", "B", "Cross", false},                 {"a", "A", "Circle", false},
    {"turbo_b", "Turbo B", "Square", false},    {"turbo_a", "Turbo A", "Triangle", false},
    {"start", "START", "Options", false},       {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};
// The Game Boy Advance: the same, with its shoulder buttons on L1 and R1.
const std::vector<Binding> GBA_BINDINGS = {
    {"b", "B", "Cross", false},                 {"a", "A", "Circle", false},
    {"turbo_b", "Turbo B", "Square", false},    {"turbo_a", "Turbo A", "Triangle", false},
    {"l", "L", "L1", false},                    {"r", "R", "R1", false},
    {"start", "START", "Options", false},       {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};

// The libretro joypad id each binding key presses (turbo B and A: Y and X for FCEUmm; the N64's
// buttons as Mupen64Plus-Next reads the RetroPad with its Independent C-button Controls - the
// alt-map option src/Emulator.cpp sets: A on B, B on Y, Z on L2, L on SELECT, R on R2, the C
// buttons on X, A, L and R; the Sega pads' as Genesis Plus GX reads it: A on Y, B on B, C on A, X on
// L, Y on X, Z on R, MODE on SELECT, buttons 1 and 2 on B and A). Two keys on one id (Z) press it
// with either control.
const std::pair<const char*, int> KEY_IDS[] = {
    {"b", ID_B},       {"a", ID_A},       {"y", ID_Y},         {"x", ID_X},
    {"l", ID_L},       {"r", ID_R},       {"start", ID_START}, {"select", ID_SELECT},
    {"turbo_b", ID_Y}, {"turbo_a", ID_X},
    {"n64_a", ID_B},   {"n64_b", ID_Y},   {"z", ID_L2},        {"z2", ID_L2},
    {"n64_l", ID_SELECT}, {"n64_r", ID_R2}, {"c_up", ID_X},    {"c_down", ID_A},
    {"c_left", ID_L},  {"c_right", ID_R},
    {"ps_cross", ID_B}, {"ps_circle", ID_A}, {"ps_square", ID_Y}, {"ps_triangle", ID_X},
    {"l2", ID_L2},     {"r2", ID_R2},     {"l3", ID_L3},       {"r3", ID_R3},
    {"dc_a", ID_B},    {"dc_b", ID_A},    {"dc_x", ID_Y},      {"dc_y", ID_X},
    {"md_a", ID_Y},    {"md_b", ID_B},    {"md_c", ID_A},      {"md_x", ID_L},
    {"md_y", ID_X},    {"md_z", ID_R},    {"mode", ID_SELECT},
    {"button_1", ID_B}, {"button_2", ID_A},
};

// The N64's normal layout until 00.30 is dropped once: the core read the buttons another way then
// (R pressed its C-button mode, C Up, C Down and L did nothing), so a layout set around that goes.
constexpr const char* VERSION_SECTION = "controls-version";
constexpr const char* N64_LAYOUT = "2";

// The section of a console's normal layout ([global] stays the Super Nintendo's).
std::string GlobalSection(SystemId system)
{
  return system == SystemId::SNES ? "global" : GetSystem(system).key;
}

const char* const STICK_CHOICES[] = {"D-Pad + Left Stick", "D-Pad", "Left Stick"};

struct Control
{
  const char* name;
  uint32_t mask;
};
const Control CONTROLS[] = {
    {"Cross", ORBIS_PAD_BUTTON_CROSS},       {"Circle", ORBIS_PAD_BUTTON_CIRCLE},
    {"Square", ORBIS_PAD_BUTTON_SQUARE},     {"Triangle", ORBIS_PAD_BUTTON_TRIANGLE},
    {"L1", ORBIS_PAD_BUTTON_L1},             {"R1", ORBIS_PAD_BUTTON_R1},
    {"L2", ORBIS_PAD_BUTTON_L2},             {"R2", ORBIS_PAD_BUTTON_R2},
    {"L3", ORBIS_PAD_BUTTON_L3},             {"R3", ORBIS_PAD_BUTTON_R3},
    {"Options", ORBIS_PAD_BUTTON_OPTIONS},   {"Touchpad", ORBIS_PAD_BUTTON_TOUCH_PAD},
    {"Pad N", ORBIS_PAD_BUTTON_UP},          {"Pad S", ORBIS_PAD_BUTTON_DOWN},
    {"Pad W", ORBIS_PAD_BUTTON_LEFT},        {"Pad E", ORBIS_PAD_BUTTON_RIGHT},
};

std::mutex s_mutex;
std::string s_path;
std::map<std::string, std::map<std::string, std::string>> s_sections;  // "global", game ids

uint32_t MaskOf(const std::string& name)
{
  for (const Control& control : CONTROLS)
  {
    if (name == control.name)
      return control.mask;
  }
  return 0;
}

void SaveLocked()
{
  std::ostringstream file;
  file << "# EmuSuNi controls: the DualShock 4 control for each console button.\n"
          "# [global] is the Super Nintendo's normal layout, [nes], [n64], [gb]... each other console's; a [game] section is\n"
          "# that game's own.\n";
  for (const auto& [name, values] : s_sections)
  {
    file << "\n[" << name << "]\n";
    for (const auto& [key, value] : values)
      file << key << "=" << value << "\n";
  }
  WriteTextFile(s_path, file.str());
}

std::string GetLocked(const Binding& binding, const std::string& game_id, SystemId system)
{
  for (const std::string& section : {game_id, GlobalSection(system)})
  {
    if (section.empty())
      continue;
    auto it = s_sections.find(section);
    if (it == s_sections.end())
      continue;
    auto value = it->second.find(binding.key);
    if (value != it->second.end())
      return value->second;
  }
  return binding.fallback;
}
}  // namespace

const std::vector<Binding>& Bindings(SystemId system)
{
  switch (system)
  {
  case SystemId::NES:
    return NES_BINDINGS;
  case SystemId::N64:
    return N64_BINDINGS;
  case SystemId::GB:
  case SystemId::GBC:
    return GB_BINDINGS;
  case SystemId::GBA:
    return GBA_BINDINGS;
  case SystemId::PS1:
    return PS1_BINDINGS;
  case SystemId::NDS:
    return NDS_BINDINGS;
  case SystemId::PSP:
    return PSP_BINDINGS;
  case SystemId::DC:
    return DC_BINDINGS;
  case SystemId::SG1000:
  case SystemId::SMS:
    return SMS_BINDINGS;
  case SystemId::GG:
    return GG_BINDINGS;
  case SystemId::MD:
    return MD_BINDINGS;
  case SystemId::ARCADE:
    return ARCADE_BINDINGS;
  case SystemId::THREEDS:
    return THREEDS_BINDINGS;
  case SystemId::NAOMI:
    return NAOMI_BINDINGS;
  case SystemId::ATOMISWAVE:
    return ATOMISWAVE_BINDINGS;
  default:
    return SNES_BINDINGS;
  }
}

std::vector<std::string> StickChoices()
{
  return {std::begin(STICK_CHOICES), std::end(STICK_CHOICES)};
}

void Load(const std::string& data_dir)
{
  std::lock_guard lock(s_mutex);
  s_path = data_dir + "/controls.ini";
  s_sections.clear();
  std::ifstream file(s_path);
  std::string line, section = "global";
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq != std::string::npos)
      s_sections[section][line.substr(0, eq)] = line.substr(eq + 1);
  }
  auto& version = s_sections[VERSION_SECTION];
  if (version["n64"] != N64_LAYOUT)
  {
    if (s_sections.erase("n64"))
      LogLine("controls: the N64's old normal layout dropped (the new one is used)");
    version["n64"] = N64_LAYOUT;
    SaveLocked();
  }
}

std::string Get(const Binding& binding, const std::string& game_id, SystemId system)
{
  std::lock_guard lock(s_mutex);
  return GetLocked(binding, game_id, system);
}

void Set(const Binding& binding, const std::string& value, const std::string& game_id, SystemId system)
{
  std::lock_guard lock(s_mutex);
  s_sections[game_id.empty() ? GlobalSection(system) : game_id][binding.key] = value;
  SaveLocked();
}

bool HasGameProfile(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  return !game_id.empty() && s_sections.count(game_id) && !s_sections[game_id].empty();
}

void CreateGameProfile(const std::string& game_id, SystemId system)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  auto& section = s_sections[game_id];
  for (const Binding& binding : Bindings(system))
    section[binding.key] = GetLocked(binding, "", system);
  SaveLocked();
}

void DeleteGameProfile(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections.erase(game_id);
  SaveLocked();
}

void ResetToDefault(const std::string& game_id, SystemId system)
{
  std::lock_guard lock(s_mutex);
  auto& section = s_sections[game_id.empty() ? GlobalSection(system) : game_id];
  for (const Binding& binding : Bindings(system))
    section[binding.key] = binding.fallback;
  SaveLocked();
}

std::string Describe(const std::string& value)
{
  if (value == "Pad N")
    return "D-Pad Up";
  if (value == "Pad S")
    return "D-Pad Down";
  if (value == "Pad W")
    return "D-Pad Left";
  if (value == "Pad E")
    return "D-Pad Right";
  if (value.empty() || value == "None")
    return "None";
  return value;
}

std::string FastForwardButton(const std::string& game_key, SystemId system)
{
  for (const Binding& binding : Bindings(system))
  {
    if (std::string(binding.key) == "fast_forward")
      return Describe(Get(binding, game_key, system));
  }
  return "None";
}

std::string FastForwardHelp(const std::string& game_key, SystemId system)
{
  const std::string button = FastForwardButton(game_key, system);
  if (button == "None")
  {
    return HasGameProfile(game_key) ?
               "How fast the game runs while the Fast Forward button is held. No button does it in this game's own "
               "controls yet: choose one in Controls." :
               "How fast the game runs while the Fast Forward button is held. No button does it on this console yet: "
               "choose one in Controls.";
  }
  return "Speed while " + (button == "Touchpad" ? std::string("the touchpad") : button) +
         " is held (change the button in Controls).";
}

bool IsAtRest(const OrbisPadData& data)
{
  return (data.buttons & ~0u) == 0 && data.analogButtons.l2 < 40 && data.analogButtons.r2 < 40;
}

std::string Capture(const OrbisPadData& data)
{
  for (const Control& control : CONTROLS)
  {
    if (data.buttons & control.mask)
      return control.name;
  }
  return "";
}

Layout LayoutFor(const std::string& game_id, SystemId system)
{
  Layout layout;
  std::lock_guard lock(s_mutex);
  std::string dpad;
  for (const Binding& binding : Bindings(system))
  {
    const std::string key = binding.key, value = GetLocked(binding, game_id, system);
    if (key == "dpad")
      dpad = value;
    else if (key == "fast_forward")
      layout.fast_forward = MaskOf(value);
    else if (key == "tap")
      layout.tap = MaskOf(value);
    else if (key == "reload")
      layout.reload = MaskOf(value);
    for (const auto& [name, id] : KEY_IDS)
    {
      if (key == name)
        layout.button_mask[id] |= MaskOf(value);
    }
  }
  layout.dpad = dpad != "Left Stick";
  layout.left_stick = dpad != "D-Pad";
  // A D-pad direction bound to a button (Pad N...) still works as the D-pad too.
  return layout;
}

uint16_t PadButtons(const Layout& layout, const OrbisPadData& data)
{
  uint16_t out = 0;
  const uint32_t buttons = data.buttons;
  for (int id : {ID_B, ID_A, ID_Y, ID_X, ID_L, ID_R, ID_START, ID_SELECT, ID_L2, ID_R2, ID_L3, ID_R3})
  {
    if (layout.button_mask[id] && (buttons & layout.button_mask[id]))
      out |= 1u << id;
  }
  if (layout.dpad)
  {
    if (buttons & ORBIS_PAD_BUTTON_UP)
      out |= 1u << ID_UP;
    if (buttons & ORBIS_PAD_BUTTON_DOWN)
      out |= 1u << ID_DOWN;
    if (buttons & ORBIS_PAD_BUTTON_LEFT)
      out |= 1u << ID_LEFT;
    if (buttons & ORBIS_PAD_BUTTON_RIGHT)
      out |= 1u << ID_RIGHT;
  }
  if (layout.left_stick)
  {
    // A dead zone around the centre (128); diagonals count when both axes are past it.
    constexpr int DEAD = 50;
    const int x = data.leftStick.x - 128, y = data.leftStick.y - 128;
    if (y < -DEAD)
      out |= 1u << ID_UP;
    if (y > DEAD)
      out |= 1u << ID_DOWN;
    if (x < -DEAD)
      out |= 1u << ID_LEFT;
    if (x > DEAD)
      out |= 1u << ID_RIGHT;
  }
  return out;
}

OrbisPadData Untouched(const OrbisPadData& data)
{
  OrbisPadData out = data;
  out.buttons = 0;
  out.leftStick.x = out.leftStick.y = 128;
  out.rightStick.x = out.rightStick.y = 128;
  out.analogButtons.l2 = out.analogButtons.r2 = 0;
  out.touch.fingers = 0;
  return out;
}

OrbisPadData MenuRelease::Pad(const OrbisPadData& data, bool connected, int* hidden_passes)
{
  if (hidden_passes)
    *hidden_passes = 0;
  if (!m_waiting)
    return data;
  // (A pass without the pad - not read, or the screenshot tour's - neither ends the wait nor counts in it: the
  // game gets nothing from the pad then anyway.)
  if (!connected)
    return Untouched(data);
  if ((data.buttons & PAD_BUTTONS) == 0)
  {
    m_waiting = false;
    if (hidden_passes)
      *hidden_passes = m_passes;
    return data;
  }
  m_passes++;
  return Untouched(data);
}
}  // namespace Controls
