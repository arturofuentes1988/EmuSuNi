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
};

// Same positions as on the SNES pad: B bottom, A right, Y left, X top.
const std::vector<Binding> BINDINGS = {
    {"b", "B", "Cross", false},          {"a", "A", "Circle", false},
    {"y", "Y", "Square", false},         {"x", "X", "Triangle", false},
    {"l", "L", "L1", false},             {"r", "R", "R1", false},
    {"start", "START", "Options", false}, {"select", "SELECT", "Touchpad", false},
    {"dpad", "D-Pad", "D-Pad + Left Stick", true},
    {"fast_forward", "Fast Forward (hold)", "R2", false},
};

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
  file << "# EmuSuNi controls: the DualShock 4 control for each Super Nintendo button.\n"
          "# [global] is the normal layout; a [game] section is that game's own.\n";
  for (const auto& [name, values] : s_sections)
  {
    file << "\n[" << name << "]\n";
    for (const auto& [key, value] : values)
      file << key << "=" << value << "\n";
  }
  WriteTextFile(s_path, file.str());
}

std::string GetLocked(const Binding& binding, const std::string& game_id)
{
  for (const std::string& section : {game_id, std::string("global")})
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

const std::vector<Binding>& Bindings()
{
  return BINDINGS;
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
}

std::string Get(const Binding& binding, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  return GetLocked(binding, game_id);
}

void Set(const Binding& binding, const std::string& value, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections[game_id.empty() ? "global" : game_id][binding.key] = value;
  SaveLocked();
}

bool HasGameProfile(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  return !game_id.empty() && s_sections.count(game_id) && !s_sections[game_id].empty();
}

void CreateGameProfile(const std::string& game_id)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  auto& section = s_sections[game_id];
  for (const Binding& binding : BINDINGS)
    section[binding.key] = GetLocked(binding, "");
  SaveLocked();
}

void DeleteGameProfile(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections.erase(game_id);
  SaveLocked();
}

void ResetToDefault(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  auto& section = s_sections[game_id.empty() ? "global" : game_id];
  for (const Binding& binding : BINDINGS)
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

Layout LayoutFor(const std::string& game_id)
{
  Layout layout;
  std::lock_guard lock(s_mutex);
  auto get = [&](const char* key) {
    for (const Binding& binding : BINDINGS)
    {
      if (std::string(binding.key) == key)
        return GetLocked(binding, game_id);
    }
    return std::string();
  };
  layout.button_mask[ID_B] = MaskOf(get("b"));
  layout.button_mask[ID_A] = MaskOf(get("a"));
  layout.button_mask[ID_Y] = MaskOf(get("y"));
  layout.button_mask[ID_X] = MaskOf(get("x"));
  layout.button_mask[ID_L] = MaskOf(get("l"));
  layout.button_mask[ID_R] = MaskOf(get("r"));
  layout.button_mask[ID_START] = MaskOf(get("start"));
  layout.button_mask[ID_SELECT] = MaskOf(get("select"));
  layout.fast_forward = MaskOf(get("fast_forward"));
  const std::string dpad = get("dpad");
  layout.dpad = dpad != "Left Stick";
  layout.left_stick = dpad != "D-Pad";
  // A D-pad direction bound to a button (Pad N...) still works as the D-pad too.
  return layout;
}

uint16_t SnesButtons(const Layout& layout, const OrbisPadData& data)
{
  uint16_t out = 0;
  const uint32_t buttons = data.buttons;
  for (int id : {ID_B, ID_A, ID_Y, ID_X, ID_L, ID_R, ID_START, ID_SELECT})
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
