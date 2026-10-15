// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4Settings.cpp (Dolphin Emulator Project, GPL-2.0+).

#include "Settings.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "Log.h"

namespace Settings
{
namespace
{
// The categories and their order follow Dolphin for PS4 (Video, Graphics, Performance, System);
// Emulation holds what only the Super Nintendo has.
const std::vector<Setting> SETTINGS = {
    // Video
    {"aspect", "Aspect Ratio", "Video",
     {{"auto", "Auto (4:3)", "As on a TV of the time: a little wider than the console's own pixels."},
      {"pixel", "Pixel Perfect (8:7)", "Square pixels, exactly as the console draws them (a little narrower)."},
      {"wide", "16:9", "Stretched to a widescreen TV (everything looks wider)."},
      {"stretch", "Stretch", "Fills the whole screen, whatever its shape."}},
     "auto", true, "The picture's shape on the TV. 4:3 is how a Super Nintendo looked on a TV."},
    {"resolution", "Resolution", "Video",
     {{"1", "Native (224p)", "The console's own 256x224 picture, enlarged by the TV (the fastest)."},
      {"2", "2x (448p)", "Redrawn at 512x448 before it is fitted to the TV."},
      {"3", "3x (720p)", "Redrawn at 768x672: for 720p TVs."},
      {"4", "4x (900p)", "Redrawn at 1024x896."},
      {"5", "5x (1080p)", "Redrawn at 1280x1120: the closest to a 1080p (Full HD) TV."},
      {"6", "6x (1440p)", "Redrawn at 1536x1344, then made smaller for the TV (a little smoother)."}},
     "1", true,
     "Redraws the game bigger with the Upscaling Filter (Graphics) before it is fitted to the TV: "
     "3x for 720p, 5x for 1080p. Uses CPU time: lower it if a game slows down."},
    {"scaling", "Picture Size", "Video",
     {{"fit", "Fit to Screen", "As big as the TV allows."},
      {"integer", "Integer Scale (sharpest)",
       "The biggest whole multiple of the console's lines (4x on a 1080p TV): every pixel the same "
       "size, with a black border."},
      {"2", "2x", "Twice the console's size, small in the middle of the TV."},
      {"3", "3x", "Three times the console's size."},
      {"4", "4x", "Four times the console's size."}},
     "fit", true,
     "How big the picture is drawn. Integer Scale keeps every pixel the same size (no uneven "
     "rows)."},
    {"zoom", "Letterbox Zoom", "Video",
     {{"off", "Off", "The whole picture."},
      {"105", "105%", "A little bigger: the edges are cut."},
      {"110", "110%", "Bigger: more of the edges are cut."},
      {"120", "120%", "Much bigger: fills a widescreen TV more, the edges cut."}},
     "off", true, "Zooms in a little to fill more of the TV (the edges are cut)."},
    {"overscan", "Crop Overscan", "Video",
     {{"enabled", "On", "224 lines: the empty lines at the top and bottom are hidden."},
      {"disabled", "Off", "All 239 lines (some games show a black stripe)."},
      {"auto", "Auto", "Hidden when the game doesn't draw on them."}},
     "enabled", true, "Hides the empty lines at the top and bottom (224 lines instead of 239)."},
    {"fps_limit", "Frame Rate Limit", "Video",
     {{"auto", "Auto (game speed)", "NTSC games at 60 and PAL games at 50 frames per second."},
      {"60", "60 FPS", "Every game at 60 (PAL games run faster than on their console)."},
      {"50", "50 FPS", "Every game at 50 (NTSC games run slower)."},
      {"0", "Unlimited", "As fast as the PS4 can (up to 4x)."}},
     "auto", true,
     "Auto runs NTSC games at 60 and PAL games at 50 frames per second, as on the console."},
    {"vsync", "V-Sync", "Video",
     {{"on", "On", "Smooth movement, no tearing."},
      {"off", "Off", "A little less input delay; the picture may tear."}},
     "on", true, "Waits for the TV's refresh (no tearing). Off can lower the input delay a little."},
    {"show_fps", "FPS Counter", "Video", {{"off", "Off"}, {"on", "On"}}, "off", true,
     "Frames and emulation speed in the corner."},
    // Graphics
    {"upscaler", "Upscaling Filter", "Graphics",
     {{"xbrz", "xBRZ (smooth)", "Rounded edges and clean lines: the most HD look."},
      {"hqx", "HQx", "Softer edges, closer to the original pixels (up to 4x)."},
      {"pixels", "Sharp Pixels", "Bigger square pixels: crisp and even, like the original."}},
     "xbrz", true,
     "How Resolution redraws the pixels: xBRZ and HQx round the edges for an HD look; Sharp "
     "Pixels keeps them square. HQx goes up to 4x."},
    {"filter", "Texture Filtering", "Graphics",
     {{"sharp", "Sharp (nearest)", "Every pixel crisp (at Native some pixels can look wider than others)."},
      {"smooth", "Smooth (linear)", "Soft, blended pixels."}},
     "sharp", true,
     "Sharp keeps the pixels crisp; Smooth softens them. Above Native resolution the picture is "
     "always smooth."},
    {"ntsc_filter", "NTSC Filter", "Graphics",
     {{"disabled", "Off", "A clean picture."},
      {"composite", "Composite", "The yellow video cable: colours bleed, soft details."},
      {"s-video", "S-Video", "Sharper than composite, a little colour bleeding."},
      {"rgb", "RGB", "The best cable of the time: sharp, clean colours."},
      {"rf", "RF", "The aerial cable: the blurriest, with noise."},
      {"monochrome", "Monochrome", "A black and white TV."}},
     "disabled", true, "The look of an old TV cable (blargg's NTSC filter). Costs CPU time."},
    {"scanlines", "Scanlines", "Graphics",
     {{"off", "Off", "No lines."},
      {"light", "Light", "Faint lines."},
      {"medium", "Medium", "Clear lines."},
      {"strong", "Strong", "Dark lines, like an arcade monitor."}},
     "off", true, "Dark lines between the picture's rows, like a CRT TV."},
    {"hires_blend", "Hi-Res Blending", "Graphics",
     {{"disabled", "Off", "The hi-res mode's columns as they are."},
      {"merge", "Merge", "Pairs of columns averaged: the see-through effects look as on a TV."},
      {"blur", "Blur", "The columns softened."}},
     "disabled", true,
     "Blends the hi-res mode's columns (pseudo-transparency in Kirby's Dream Land 3, Jurassic "
     "Park)."},
    {"sprite_flicker", "Reduce Sprite Flicker", "Graphics",
     {{"disabled", "Off", "As the console: sprites flicker when too many share a line."},
      {"enabled", "On", "Up to 128 sprites per line: less flicker (a few games glitch)."}},
     "disabled", true, "Draws more sprites per line than the console could (less flicker)."},
    {"transparency", "Transparency Effects", "Graphics",
     {{"enabled", "On", "Water, shadows and fades as on the console."},
      {"disabled", "Off", "No colour math (to find graphic glitches)."}},
     "enabled", true, "Colour math: water, shadows, fades. Off only to diagnose glitches."},
    // Performance
    {"cpu_clock", "Emulated CPU Clock", "Performance",
     {{"disabled", "100%", "The console's own speed."},
      {"light", "Light Overclock", "A little faster: less slowdown, safe in most games."},
      {"compatible", "Overclock", "Faster: removes most slowdown; some games glitch."},
      {"max", "Max Overclock", "Much faster; more games glitch."}},
     "disabled", true,
     "Overclocking the console's CPU removes slowdown in busy scenes (Gradius III, Super R-Type); "
     "a few games glitch."},
    {"superfx_clock", "SuperFX Clock", "Performance",
     {{"100", "100%", "The chip's own speed."},
      {"150", "150%", "A faster chip: smoother 3D."},
      {"200", "200%", "Twice as fast: much smoother 3D."},
      {"300", "300%", "Three times as fast (some scenes may run too fast)."},
      {"400", "400%", "Four times as fast (some scenes may run too fast)."}},
     "100", true, "Faster SuperFX chip: smoother Star Fox, Stunt Race FX, Yoshi's Island."},
    {"speed", "Emulation Speed", "Performance",
     {{"100", "100%"}, {"50", "50%"}, {"75", "75%"}, {"150", "150%"}, {"200", "200%"},
      {"300", "300%"}},
     "100", true, "The game's speed. 100% is the console's."},
    {"fast_forward", "Fast Forward (R2)", "Performance",
     {{"3", "3x"}, {"2", "2x"}, {"5", "5x"}, {"10", "10x"}, {"off", "Off", "R2 does nothing in the game."}},
     "3", true, "Speed while R2 is held."},
    {"frameskip", "Frame Skip", "Performance",
     {{"off", "Off", "Every frame drawn."},
      {"auto", "Auto", "Skips drawing a frame only when the PS4 falls behind."},
      {"1", "1", "Draws every other frame."},
      {"2", "2", "Draws one frame in three."}},
     "off", true, "Skips drawing some frames when the PS4 can't keep up (the game keeps its speed)."},
    {"perf_overlay", "Performance Graphs", "Performance", {{"off", "Off"}, {"on", "On"}}, "off",
     true, "Frame time and speed graphs on screen."},
    // Emulation
    {"region", "Region", "Emulation",
     {{"auto", "Auto", "As the game's own cartridge."},
      {"ntsc", "NTSC (60 Hz)", "As an American or Japanese console."},
      {"pal", "PAL (50 Hz)", "As a European console: the game and its music run slower."}},
     "auto", false, "Auto uses the game's own region."},
    {"block_vram", "Block Invalid VRAM Access", "Emulation",
     {{"enabled", "On", "As the console."}, {"disabled", "Off", "For hacks and translations that need it."}},
     "enabled", true, "As the console does. A few hacks and translations need Off."},
    {"echo_hack", "Echo Buffer Hack", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", true, "Fixes the sound in a few hacks (Super Mario World ROM hacks)."},
    {"up_down", "Allow Opposing Directions", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", true, "Up+Down or Left+Right at once (some games glitch with it)."},
    {"randomize_memory", "Randomize Memory", "Emulation", {{"disabled", "Off"}, {"enabled", "On"}},
     "disabled", false, "Starts with random RAM, as a real console. Some games need it."},
    // System
    {"language", "Language", "System",
     {{"en", "English"},
      {"es", "Español"},
      {"fr", "Français"},
      {"de", "Deutsch"},
      {"it", "Italiano"},
      {"pt", "Português"},
      {"nl", "Nederlands"},
      {"pl", "Polski"},
      {"tr", "Türkçe"},
      {"ru", "Русский"},
      {"ja", "日本語"},
      {"zh", "中文 (简体)"},
      {"ko", "한국어"}},
     "en", false, "The menus' language."},
    {"volume", "Game Volume", "System",
     {{"100", "100"}, {"90", "90"}, {"80", "80"}, {"70", "70"}, {"60", "60"}, {"50", "50"},
      {"40", "40"}, {"30", "30"}, {"20", "20"}, {"10", "10"}, {"0", "0"}},
     "100", true, "Volume of the games' sound."},
    {"audio_interpolation", "Audio Interpolation", "System",
     {{"gaussian", "Gaussian (console)", "The console's own sound filter: warm, a little muffled."},
      {"cubic", "Cubic", "Crisper high notes."},
      {"sinc", "Sinc", "The crispest and cleanest (more CPU time)."},
      {"linear", "Linear", "Simple, a little harsh."},
      {"none", "None", "The raw samples: sharp and noisy."}},
     "gaussian", true, "Gaussian sounds like the console; Cubic and Sinc are crisper."},
    {"audio_buffer", "Audio Buffer", "System",
     {{"64", "64 ms", "The usual balance."},
      {"48", "48 ms", "Less delay; may crackle when the PS4 is busy."},
      {"96", "96 ms", "Smoother sound in heavy games, a little more delay."},
      {"128", "128 ms", "The smoothest sound, the most delay."}},
     "64", false,
     "Larger is smoother when a game runs below full speed; smaller has less delay."},
    {"multitap", "Multitap (3-5 Players)", "System",
     {{"off", "Off", "Two players."}, {"on", "On", "Up to five players with more DualShock 4s."}},
     "off", true, "The Super Multitap in port 2, for Bomberman and other 3-5 player games."},
    {"resume", "Resume Where You Left Off", "System",
     {{"off", "Off", "Each start begins from the game's own start (or its battery save)."},
      {"on", "On", "Quit Game keeps the moment you left; the next start goes on from there."}},
     "off", false, "Quit Game saves the game's state, and the next start continues from there."},
};

const std::vector<const char*> CATEGORIES = {"Video", "Graphics", "Performance", "Emulation",
                                             "System"};

std::mutex s_mutex;
std::string s_path;
std::map<std::string, std::map<std::string, std::string>> s_sections;  // "global", game IDs

std::string Trim(std::string text)
{
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
    text.pop_back();
  return text;
}

std::string Fallback(const std::string& key)
{
  const Setting* setting = Find(key);
  return setting ? setting->fallback : "";
}

std::string GetLocked(const std::string& key, const std::string& game_id)
{
  if (!game_id.empty())
  {
    auto game = s_sections.find(game_id);
    if (game != s_sections.end() && game->second.count("custom") &&
        game->second.at("custom") == "on")
    {
      auto value = game->second.find(key);
      if (value != game->second.end())
        return value->second;
    }
  }
  auto global = s_sections.find("global");
  if (global != s_sections.end())
  {
    auto value = global->second.find(key);
    if (value != global->second.end())
      return value->second;
  }
  return Fallback(key);
}

void SaveLocked()
{
  if (s_path.empty())
    return;
  std::ostringstream file;
  file << "# EmuSuNi settings, written by the menu and the in-game menu.\n"
          "# [global] applies to every game; a [game] section with custom=on overrides it.\n";
  auto write = [&](const std::string& name, const std::map<std::string, std::string>& values) {
    file << "\n[" << name << "]\n";
    for (const auto& [key, value] : values)
      file << key << "=" << value << "\n";
  };
  if (s_sections.count("global"))
    write("global", s_sections["global"]);
  for (const auto& [name, values] : s_sections)
  {
    if (name != "global")
      write(name, values);
  }
  WriteTextFile(s_path, file.str());
}
}  // namespace

const std::vector<Setting>& All()
{
  return SETTINGS;
}

const std::vector<const char*>& Categories()
{
  return CATEGORIES;
}

const Setting* Find(const std::string& key)
{
  for (const Setting& setting : SETTINGS)
  {
    if (key == setting.key)
      return &setting;
  }
  return nullptr;
}

void Load(const std::string& data_dir)
{
  std::lock_guard lock(s_mutex);
  s_path = data_dir + "/settings.ini";
  s_sections.clear();
  std::ifstream file(s_path);
  std::string line, section = "global";
  while (std::getline(file, line))
  {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';')
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

void Save()
{
  std::lock_guard lock(s_mutex);
  SaveLocked();
}

std::string Get(const std::string& key, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  return GetLocked(key, game_id);
}

int GetInt(const std::string& key, const std::string& game_id)
{
  return std::atoi(Get(key, game_id).c_str());
}

bool GetOn(const std::string& key, const std::string& game_id)
{
  const std::string value = Get(key, game_id);
  return value == "on" || value == "enabled";
}

void Set(const std::string& key, const std::string& value, const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections[game_id.empty() ? "global" : game_id][key] = value;
  SaveLocked();
}

std::vector<std::pair<std::string, std::string>> GlobalValues()
{
  std::lock_guard lock(s_mutex);
  std::vector<std::pair<std::string, std::string>> values;
  auto global = s_sections.find("global");
  if (global != s_sections.end())
    values.assign(global->second.begin(), global->second.end());
  return values;
}

bool IsCustom(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  auto game = s_sections.find(game_id);
  return game != s_sections.end() && game->second.count("custom") &&
         game->second.at("custom") == "on";
}

void SetCustom(const std::string& game_id, bool custom)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  auto& section = s_sections[game_id];
  if (custom && section.size() <= 1)
  {
    // First time: start from what the game uses now.
    for (const Setting& setting : SETTINGS)
      section[setting.key] = GetLocked(setting.key, "");
  }
  section["custom"] = custom ? "on" : "off";
  SaveLocked();
}

void CopyGameToGlobal(const std::string& game_id)
{
  if (game_id.empty())
    return;
  std::lock_guard lock(s_mutex);
  auto game = s_sections.find(game_id);
  if (game == s_sections.end())
    return;
  for (const Setting& setting : SETTINGS)
  {
    if (std::string(setting.key) == "language")
      continue;
    const auto value = game->second.find(setting.key);
    if (value != game->second.end())
      s_sections["global"][setting.key] = value->second;
  }
  game->second["custom"] = "off";
  SaveLocked();
}

void ResetGlobal(const std::string& key)
{
  std::lock_guard lock(s_mutex);
  auto global = s_sections.find("global");
  if (global != s_sections.end())
    global->second.erase(key);
  SaveLocked();
}

void ResetGame(const std::string& game_id)
{
  std::lock_guard lock(s_mutex);
  s_sections.erase(game_id);
  SaveLocked();
}

void ResetAll()
{
  std::lock_guard lock(s_mutex);
  // The language stays: the menus would switch back to English.
  std::string language;
  if (s_sections.count("global") && s_sections["global"].count("language"))
    language = s_sections["global"]["language"];
  s_sections.clear();
  if (!language.empty())
    s_sections["global"]["language"] = language;
  SaveLocked();
}

std::string LabelOf(const Setting& setting, const std::string& value)
{
  for (const Choice& choice : setting.choices)
  {
    if (value == choice.value)
      return choice.label;
  }
  return value;
}

const Choice* FindChoice(const Setting& setting, const std::string& value)
{
  for (const Choice& choice : setting.choices)
  {
    if (value == choice.value)
      return &choice;
  }
  return nullptr;
}

std::string Step(const Setting& setting, const std::string& value, int direction)
{
  const int count = static_cast<int>(setting.choices.size());
  int index = 0;
  for (int i = 0; i < count; i++)
  {
    if (value == setting.choices[i].value)
      index = i;
  }
  index = (index + direction % count + count) % count;
  return setting.choices[index].value;
}
}  // namespace Settings
