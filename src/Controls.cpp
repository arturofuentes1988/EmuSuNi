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
// on L2 (the touchpad is its touch screen); L3 blows into its microphone, R3 swaps the screens'
// layout (melonDS DS).
const std::vector<Binding> NDS_BINDINGS = {
    {"b", "B", "Cross", false},                  {"a", "A", "Circle", false},
    {"y", "Y", "Square", false},                 {"x", "X", "Triangle", false},
    {"l", "L", "L1", false},                     {"r", "R", "R1", false},
    {"start", "START", "Options", false},        {"select", "SELECT", "L2", false},
    {"l3", "Microphone (blow)", "L3", false},    {"r3", "Swap Screens", "R3", false},
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
// buttons on X, A, L and R). Two keys on one id (Z) press it with either control.
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
}  // namespace Controls
