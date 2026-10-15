// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Tour.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

#include <orbis/Pad.h>

#include "Display.h"
#include "Image.h"
#include "Log.h"
#include "Settings.h"

namespace Tour
{
namespace
{
std::string s_data_dir;
std::vector<std::string> s_lines;
size_t s_next = 0;
float s_wait = 0.0f;
bool s_active = false;
// Global settings as they were before the tour changed them ("" = not set).
std::map<std::string, std::string> s_saved;

uint32_t Buttons(const std::string& names)
{
  static const std::map<std::string, uint32_t> BUTTONS = {
      {"CROSS", ORBIS_PAD_BUTTON_CROSS},     {"CIRCLE", ORBIS_PAD_BUTTON_CIRCLE},
      {"SQUARE", ORBIS_PAD_BUTTON_SQUARE},   {"TRIANGLE", ORBIS_PAD_BUTTON_TRIANGLE},
      {"UP", ORBIS_PAD_BUTTON_UP},           {"DOWN", ORBIS_PAD_BUTTON_DOWN},
      {"LEFT", ORBIS_PAD_BUTTON_LEFT},       {"RIGHT", ORBIS_PAD_BUTTON_RIGHT},
      {"L1", ORBIS_PAD_BUTTON_L1},           {"R1", ORBIS_PAD_BUTTON_R1},
      {"L2", ORBIS_PAD_BUTTON_L2},           {"R2", ORBIS_PAD_BUTTON_R2},
      {"L3", ORBIS_PAD_BUTTON_L3},           {"R3", ORBIS_PAD_BUTTON_R3},
      {"OPTIONS", ORBIS_PAD_BUTTON_OPTIONS}, {"TOUCHPAD", ORBIS_PAD_BUTTON_TOUCH_PAD},
  };
  uint32_t bits = 0;
  std::stringstream in(names);
  std::string name;
  while (std::getline(in, name, '+'))
  {
    const auto it = BUTTONS.find(name);
    if (it != BUTTONS.end())
      bits |= it->second;
  }
  return bits;
}
}  // namespace

void Load(const std::string& data_dir)
{
  s_data_dir = data_dir;
  std::istringstream in(ReadTextFile(data_dir + "/tour.txt"));
  std::string line;
  while (std::getline(in, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (!line.empty() && line[0] != '#')
      s_lines.push_back(line);
  }
  s_active = !s_lines.empty();
  if (s_active)
  {
    MakeDirs(data_dir + "/screenshots");
    LogF("tour: %zu steps", s_lines.size());
  }
}

bool Active()
{
  return s_active;
}

Step Next(float dt)
{
  Step step;
  if (!s_active)
    return step;
  if (s_wait > 0.0f)
  {
    s_wait -= dt;
    return step;
  }
  if (s_next >= s_lines.size())
  {
    Finish();
    return step;
  }
  const std::string line = s_lines[s_next++];
  LogLine("tour: " + line);
  const size_t space = line.find(' ');
  const std::string word = line.substr(0, space);
  const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
  if (word == "wait")
  {
    s_wait = static_cast<float>(std::atof(rest.c_str()));
  }
  else if (word == "press")
  {
    step.buttons = Buttons(rest);
  }
  else if (word == "shot")
  {
    step.shot = rest;
  }
  else if (word == "set")
  {
    const size_t gap = rest.find(' ');
    const std::string key = rest.substr(0, gap);
    if (!s_saved.count(key))
    {
      std::string before;
      for (const auto& [global_key, value] : Settings::GlobalValues())
      {
        if (global_key == key)
          before = value;
      }
      s_saved[key] = before;
    }
    Settings::Set(key, gap == std::string::npos ? std::string() : rest.substr(gap + 1));
  }
  else if (word == "end")
  {
    Finish();
  }
  else
  {
    step.command = line;
  }
  return step;
}

void SaveShot(Display& display, const std::string& name)
{
  ImageRGBA image;
  if (!display.Capture(&image.pixels))
    return;
  image.width = Display::WIDTH;
  image.height = Display::HEIGHT;
  const std::string path = s_data_dir + "/screenshots/" + name + ".png";
  LogLine(SavePng(path, image) ? "tour: saved " + path : "tour: couldn't save " + path);
}

void Finish()
{
  if (!s_active)
    return;
  s_active = false;
  for (const auto& [key, value] : s_saved)
  {
    if (value.empty())
      Settings::ResetGlobal(key);
    else
      Settings::Set(key, value);
  }
  s_saved.clear();
  const std::string path = s_data_dir + "/tour.txt";
  std::remove((path + ".done").c_str());
  std::rename(path.c_str(), (path + ".done").c_str());
  LogLine("tour: done");
}
}  // namespace Tour
