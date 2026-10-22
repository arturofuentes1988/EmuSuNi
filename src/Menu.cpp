// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).
// Based on DolphinPS4's DolphinNoGUI/PS4XMB.cpp (Dolphin Emulator Project, GPL-2.0+): the same
// launcher - Library and XMB styles, themes, sounds, side panels, settings lists, controls - for
// the Super Nintendo and the NES: every console's games together, and a tab for each console.

#include "Menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <sys/stat.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "Arcade.h"
#include "CheatDownload.h"
#include "Cheats.h"
#include "Controls.h"
#include "CoverDownload.h"
#include "Display.h"
#include "GameAudio.h"
#include "Image.h"
#include "Log.h"
#include "Naomi.h"
#include "PS4Glyphs.h"
#include "PS4Lang.h"
#include "Saves.h"
#include "Settings.h"
#include "Systems.h"
#include "SystemKeyboard.h"
#include "Tour.h"
#ifdef EMUSUNI_SATURN
#include "SaturnTest.h"
#endif

namespace
{
using Clock = std::chrono::steady_clock;
constexpr float W = static_cast<float>(Display::WIDTH);
constexpr float H = static_cast<float>(Display::HEIGHT);

void Log(const std::string& text)
{
  LogLine("menu: " + text);
}

ImU32 Rgba(uint32_t rgb, float alpha = 1.0f)
{
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                  static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

// The colour darker (factor < 1) or lighter.
uint32_t Shade(uint32_t rgb, float factor)
{
  auto channel = [&](int shift) {
    return static_cast<uint32_t>(std::clamp(((rgb >> shift) & 0xFF) * factor, 0.0f, 255.0f)) << shift;
  };
  return channel(16) | channel(8) | channel(0);
}

// A quad with a colour per corner (ImGui has the rectangle version only).
void QuadMultiColor(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb,
                    ImU32 cc, ImU32 cd)
{
  const ImVec2 uv = dl->_Data->TexUvWhitePixel;
  dl->PrimReserve(6, 4);
  const ImDrawIdx idx = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  dl->PrimWriteIdx(idx);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 1));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
  dl->PrimWriteIdx(idx);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 3));
  dl->PrimWriteVtx(a, uv, ca);
  dl->PrimWriteVtx(b, uv, cb);
  dl->PrimWriteVtx(c, uv, cc);
  dl->PrimWriteVtx(d, uv, cd);
}

// A five-pointed star (favorites), with an outline when `outline` isn't 0.
void DrawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 outline = 0)
{
  ImVec2 points[10];
  for (int i = 0; i < 10; i++)
  {
    const float angle = -IM_PI * 0.5f + static_cast<float>(i) * IM_PI / 5.0f;
    const float radius = i % 2 == 0 ? r : r * 0.45f;
    points[i] = ImVec2(c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius);
  }
  dl->AddConcavePolyFilled(points, 10, fill);
  if (outline)
    dl->AddPolyline(points, 10, outline, ImDrawFlags_Closed, std::max(1.0f, r * 0.12f));
}

// A textured quad with a colour per corner (the Library's cover reflections fade out).
void ImageQuadColors(ImDrawList* dl, ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d,
                     ImVec2 uv_a, ImVec2 uv_b, ImVec2 uv_c, ImVec2 uv_d, ImU32 ca, ImU32 cb,
                     ImU32 cc, ImU32 cd)
{
  dl->PushTexture(ImTextureRef(texture));
  dl->PrimReserve(6, 4);
  const ImDrawIdx idx = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  dl->PrimWriteIdx(idx);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 1));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
  dl->PrimWriteIdx(idx);
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
  dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 3));
  dl->PrimWriteVtx(a, uv_a, ca);
  dl->PrimWriteVtx(b, uv_b, cb);
  dl->PrimWriteVtx(c, uv_c, cc);
  dl->PrimWriteVtx(d, uv_d, cd);
  dl->PopTexture();
}

// The four face buttons of the Super Famicom pad (red, yellow, green, blue), the app's emblem.
constexpr uint32_t BUTTON_COLORS[4] = {0xE5484D, 0xF2C230, 0x3DB46D, 0x3F78D8};

void DrawEmblem(ImDrawList* dl, ImVec2 c, float size, float alpha)
{
  // A diamond of four buttons: X top (blue), A right (red), B bottom (yellow), Y left (green).
  const float r = size * 0.2f, d = size * 0.3f;
  const ImVec2 at[4] = {ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y),
                        ImVec2(c.x, c.y - d)};
  for (int i = 0; i < 4; i++)
  {
    dl->AddCircleFilled(ImVec2(at[i].x + r * 0.12f, at[i].y + r * 0.16f), r, IM_COL32(0, 0, 0, static_cast<int>(70 * alpha)), 24);
    dl->AddCircleFilled(at[i], r, Rgba(BUTTON_COLORS[i], alpha), 24);
    dl->AddCircleFilled(ImVec2(at[i].x - r * 0.3f, at[i].y - r * 0.3f), r * 0.32f,
                        IM_COL32(255, 255, 255, static_cast<int>(70 * alpha)), 16);
  }
}

// A Super Nintendo cartridge, upright, centred on c (h tall): the body, its label and the ridges.
void DrawCartridgeIcon(ImDrawList* dl, ImVec2 c, float h, ImU32 body, ImU32 label)
{
  const float w = h * 0.9f;
  const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
  dl->AddRectFilled(a, b, body, h * 0.08f, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilled(ImVec2(a.x + w * 0.14f, a.y + h * 0.14f), ImVec2(b.x - w * 0.14f, c.y + h * 0.18f),
                    label, h * 0.04f);
  for (int i = 0; i < 3; i++)
  {
    const float y = c.y + h * (0.27f + i * 0.07f);
    dl->AddLine(ImVec2(a.x + w * 0.1f, y), ImVec2(b.x - w * 0.1f, y), label, h * 0.025f);
  }
}

// "Played today", "Played 3 days ago", ...
std::string PlayedText(int64_t when)
{
  if (when <= 0)
    return "Not played yet";
  const std::time_t now = std::time(nullptr), then = static_cast<std::time_t>(when);
  std::tm today = {}, day = {};
  localtime_r(&now, &today);
  localtime_r(&then, &day);
  today.tm_hour = day.tm_hour = 12;
  today.tm_min = day.tm_min = today.tm_sec = day.tm_sec = 0;
  const long days = std::lround(std::difftime(std::mktime(&today), std::mktime(&day)) / 86400.0);
  if (days <= 0)
    return "Played today";
  if (days == 1)
    return "Played yesterday";
  if (days < 7)
    return "Played " + std::to_string(days) + " days ago";
  char text[48];
  std::strftime(text, sizeof(text), "Played %b %e, %Y", &day);
  return text;
}

std::string DateText(int64_t when)
{
  if (when <= 0)
    return "";
  const std::time_t then = static_cast<std::time_t>(when);
  std::tm day = {};
  localtime_r(&then, &day);
  char text[48];
  std::strftime(text, sizeof(text), "%b %e, %Y  %H:%M", &day);
  return text;
}

// Moves `value` towards `target` (frame-rate independent exponential smoothing).
void Approach(float& value, float target, float rate, float dt)
{
  value += (target - value) * (1.0f - std::exp(-rate * dt));
}

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// The names a cover put in covers/ by hand is looked for by (the keys of m_cover_files: lower case, the
// console's prefix, no .png): the game's key and its title, and an arcade game's full name and its
// parent's (libretro's thumbnails are named so, ':' and '/' made '_').
std::vector<std::string> CoverNames(const Library::Game& rom)
{
  const std::string prefix = GetSystem(rom.system).prefix;
  std::vector<std::string> names = {Lower(rom.key), prefix + Lower(rom.title)};
  for (const std::string& name : rom.cover_names)
    names.push_back(prefix + Lower(name));
  return names;
}

// ---------------------------------------------------------------------------------------------
// Sounds: 16-bit PCM WAV files mixed into one 48 kHz stereo AudioOut port.

struct Sound
{
  std::vector<int16_t> frames;  // interleaved stereo, 48 kHz
};

std::unique_ptr<Sound> LoadWav(const std::string& path)
{
  const std::string data = ReadTextFile(path);
  if (data.size() < 44 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WAVE") != 0)
    return nullptr;
  int channels = 0, rate = 0, bits = 0;
  const char* samples = nullptr;
  size_t sample_bytes = 0;
  for (size_t at = 12; at + 8 <= data.size();)
  {
    uint32_t size;
    std::memcpy(&size, data.data() + at + 4, 4);
    const std::string id = data.substr(at, 4);
    if (id == "fmt " && size >= 16)
    {
      uint16_t format, ch, bps;
      uint32_t sr;
      std::memcpy(&format, data.data() + at + 8, 2);
      std::memcpy(&ch, data.data() + at + 10, 2);
      std::memcpy(&sr, data.data() + at + 12, 4);
      std::memcpy(&bps, data.data() + at + 22, 2);
      if (format != 1)
        return nullptr;
      channels = ch;
      rate = static_cast<int>(sr);
      bits = bps;
    }
    else if (id == "data")
    {
      samples = data.data() + at + 8;
      sample_bytes = std::min<size_t>(size, data.size() - at - 8);
    }
    at += 8 + size + (size & 1);
  }
  if (!samples || bits != 16 || (channels != 1 && channels != 2) || rate <= 0)
    return nullptr;
  const size_t in_frames = sample_bytes / (2 * channels);
  auto sound = std::make_unique<Sound>();
  const double step = static_cast<double>(rate) / 48000.0;
  const size_t out_frames = static_cast<size_t>(in_frames / step);
  sound->frames.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; i++)
  {
    const size_t src = std::min(in_frames - 1, static_cast<size_t>(i * step));
    int16_t l, r;
    std::memcpy(&l, samples + src * 2 * channels, 2);
    if (channels == 2)
      std::memcpy(&r, samples + src * 4 + 2, 2);
    else
      r = l;
    sound->frames[i * 2] = l;
    sound->frames[i * 2 + 1] = r;
  }
  return sound;
}

class Audio
{
public:
  void Start()
  {
    m_stop = false;
    m_errors = 0;
    // The app's one AudioOut port, shared with the games' sound and never closed (GameAudio.h:
    // a port closed here after every visit never came back, and the 9th open failed).
    m_port = AudioPort::Get();
    if (m_port < 0)
    {
      Log("audio: no sound port (no menu sounds)");
      return;
    }
    try
    {
      m_thread = std::thread([this] { Loop(); });
    }
    catch (...)
    {
      Log("audio: the sound thread couldn't be made (no menu sounds)");
      m_port = -1;
    }
  }
  void Stop()
  {
    m_stop = true;
    if (m_thread.joinable())
      m_thread.join();
    if (m_errors > 0)
    {
      char line[96];
      snprintf(line, sizeof(line), "audio: %d sound outputs failed (last %#x)", m_errors.load(),
               static_cast<unsigned>(m_last_error.load()));
      Log(line);
    }
    m_port = -1;  // the port stays open for the games (AudioPort)
    std::lock_guard lock(m_mutex);
    m_voices.clear();
  }
  void Play(const Sound* sound)
  {
    if (!sound || !m_enabled || m_port < 0)
      return;
    std::lock_guard lock(m_mutex);
    m_voices.push_back({sound, 0});
  }
  void SetEnabled(bool enabled) { m_enabled = enabled; }
  void SetVolume(int percent) { m_volume = std::clamp(percent, 0, 100); }

private:
  static constexpr uint32_t FRAMES = AudioPort::PERIOD;  // 256 frames at 48 kHz
  struct Voice
  {
    const Sound* sound;
    size_t position;
  };
  void Loop()
  {
    AudioPort::SetSoundThread("menu");
    std::vector<int16_t> buffer(FRAMES * 2);
    std::vector<int32_t> mix(FRAMES * 2);
    while (!m_stop)
    {
      std::fill(mix.begin(), mix.end(), 0);
      {
        std::lock_guard lock(m_mutex);
        for (Voice& voice : m_voices)
        {
          const size_t total = voice.sound->frames.size() / 2;
          for (uint32_t i = 0; i < FRAMES && voice.position < total; i++, voice.position++)
          {
            mix[i * 2] += voice.sound->frames[voice.position * 2];
            mix[i * 2 + 1] += voice.sound->frames[voice.position * 2 + 1];
          }
        }
        std::erase_if(m_voices, [](const Voice& v) {
          return v.position >= v.sound->frames.size() / 2;
        });
      }
      const int volume = m_volume;
      for (size_t i = 0; i < mix.size(); i++)
        buffer[i] = static_cast<int16_t>(std::clamp(mix[i] * volume / 100, -32768, 32767));
      const int result = AudioPort::Output(buffer.data());  // waits a period after an error
      if (result < 0)
      {
        m_errors++;
        m_last_error = result;
      }
    }
    AudioPort::Drain();  // the last period played: the port is clean for the game
  }
  int m_port = -1;  // the shared port while the menu's sound runs (never closed here)
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_errors{0}, m_last_error{0};
  std::atomic<bool> m_enabled{true};
  std::atomic<int> m_volume{80};
  std::mutex m_mutex;
  std::vector<Voice> m_voices;
};

// ---------------------------------------------------------------------------------------------
// Pad: buttons with edge detection and auto-repeat for the directions (D-pad or left stick).

enum Button : uint32_t
{
  BTN_UP = 1 << 0,
  BTN_DOWN = 1 << 1,
  BTN_LEFT = 1 << 2,
  BTN_RIGHT = 1 << 3,
  BTN_CROSS = 1 << 4,
  BTN_CIRCLE = 1 << 5,
  BTN_TRIANGLE = 1 << 6,
  BTN_SQUARE = 1 << 7,
  BTN_L1 = 1 << 8,
  BTN_R1 = 1 << 9,
  BTN_OPTIONS = 1 << 10,
  BTN_L2 = 1 << 11,
  BTN_R2 = 1 << 12,
};

// DualShock 4 buttons (ORBIS_PAD_BUTTON_*) as the menu's: the screenshot tour presses them.
uint32_t MenuButtons(uint32_t b)
{
  static const std::pair<uint32_t, uint32_t> MAP[] = {
      {ORBIS_PAD_BUTTON_UP, BTN_UP},         {ORBIS_PAD_BUTTON_DOWN, BTN_DOWN},
      {ORBIS_PAD_BUTTON_LEFT, BTN_LEFT},     {ORBIS_PAD_BUTTON_RIGHT, BTN_RIGHT},
      {ORBIS_PAD_BUTTON_CROSS, BTN_CROSS},   {ORBIS_PAD_BUTTON_CIRCLE, BTN_CIRCLE},
      {ORBIS_PAD_BUTTON_TRIANGLE, BTN_TRIANGLE}, {ORBIS_PAD_BUTTON_SQUARE, BTN_SQUARE},
      {ORBIS_PAD_BUTTON_L1, BTN_L1},         {ORBIS_PAD_BUTTON_R1, BTN_R1},
      {ORBIS_PAD_BUTTON_L2, BTN_L2},         {ORBIS_PAD_BUTTON_R2, BTN_R2},
      {ORBIS_PAD_BUTTON_OPTIONS, BTN_OPTIONS},
  };
  uint32_t out = 0;
  for (const auto& [orbis, button] : MAP)
  {
    if (b & orbis)
      out |= button;
  }
  return out;
}

class Input
{
public:
  void Open(int user)
  {
    scePadInit();
    m_pad = scePadOpen(user, 0, 0, nullptr);
    if (m_pad < 0)
      Log("scePadOpen failed");
    m_held = 0xFFFFFFFF;  // a button still held from the game isn't a new press
  }
  void Close()
  {
    if (m_pad >= 0)
      scePadClose(m_pad);
    m_pad = -1;
  }
  bool Raw(OrbisPadData* out)
  {
    return m_pad >= 0 && scePadReadState(m_pad, out) == 0 && out->connected;
  }
  uint32_t Poll(float dt)
  {
    uint32_t held = 0;
    if (m_pad >= 0)
    {
      OrbisPadData data = {};
      if (scePadReadState(m_pad, &data) == 0 && data.connected)
      {
        const uint32_t b = data.buttons;
        if ((b & ORBIS_PAD_BUTTON_UP) || data.leftStick.y < 50)
          held |= BTN_UP;
        if ((b & ORBIS_PAD_BUTTON_DOWN) || data.leftStick.y > 205)
          held |= BTN_DOWN;
        if ((b & ORBIS_PAD_BUTTON_LEFT) || data.leftStick.x < 50)
          held |= BTN_LEFT;
        if ((b & ORBIS_PAD_BUTTON_RIGHT) || data.leftStick.x > 205)
          held |= BTN_RIGHT;
        if (b & ORBIS_PAD_BUTTON_CROSS)
          held |= BTN_CROSS;
        if (b & ORBIS_PAD_BUTTON_CIRCLE)
          held |= BTN_CIRCLE;
        if (b & ORBIS_PAD_BUTTON_TRIANGLE)
          held |= BTN_TRIANGLE;
        if (b & ORBIS_PAD_BUTTON_SQUARE)
          held |= BTN_SQUARE;
        if (b & ORBIS_PAD_BUTTON_L1)
          held |= BTN_L1;
        if (b & ORBIS_PAD_BUTTON_R1)
          held |= BTN_R1;
        if (b & ORBIS_PAD_BUTTON_OPTIONS)
          held |= BTN_OPTIONS;
        if ((b & ORBIS_PAD_BUTTON_L2) || data.analogButtons.l2 > 180)
          held |= BTN_L2;
        if ((b & ORBIS_PAD_BUTTON_R2) || data.analogButtons.r2 > 180)
          held |= BTN_R2;
      }
    }
    uint32_t pressed = held & ~m_held;
    constexpr uint32_t directions = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;
    if ((held & directions) && (held & directions) == (m_held & directions))
    {
      m_repeat -= dt;
      if (m_repeat <= 0.0f)
      {
        pressed |= held & directions;
        m_repeat = 0.075f;
      }
    }
    else
    {
      m_repeat = 0.35f;
    }
    m_held = held;
    return pressed;
  }

private:
  int m_pad = -1;
  uint32_t m_held = 0;
  float m_repeat = 0.35f;
};

// ---------------------------------------------------------------------------------------------
// Themes: a background gradient with animated waves (or a wallpaper), and the text colour.

struct Theme
{
  const char* name;
  uint32_t top, bottom, wave, text;
};

constexpr Theme THEMES[] = {
    {"Super Purple", 0x2A1A5E, 0x0B0620, 0xB49CFF, 0xFFFFFF},
    {"Super Famicom", 0x3A3D48, 0x111217, 0xF2C230, 0xFFFFFF},
    {"Console Grey", 0xE9E9F0, 0xB8B9C8, 0x7B5CD6, 0x1E2733},
    {"Night Sky", 0x0B2147, 0x020713, 0x5BB8FF, 0xFFFFFF},
    {"Ocean", 0x0B4060, 0x03121B, 0x62E6FF, 0xFFFFFF},
    {"Emerald", 0x0C4231, 0x02140E, 0x46F5AE, 0xFFFFFF},
    {"Lime", 0x34500F, 0x0C1504, 0xC9FF63, 0xFFFFFF},
    {"Gold", 0x4F3D10, 0x150F04, 0xFFD870, 0xFFFFFF},
    {"Sunset", 0x5E2030, 0x1B0611, 0xFFA070, 0xFFFFFF},
    {"Crimson", 0x5C0F17, 0x170305, 0xFF6474, 0xFFFFFF},
    {"Rose", 0x5C1B4A, 0x1B0616, 0xFF8AD4, 0xFFFFFF},
    {"Midnight", 0x11131F, 0x000000, 0x7D89FF, 0xFFFFFF},
};

// ---------------------------------------------------------------------------------------------
// Preferences (data_dir/xmb.ini, "key=value" lines).

struct Preferences
{
  std::string theme = THEMES[0].name;
  std::string wallpaper;  // file name in data_dir/themes, empty = the theme's waves
  bool sounds = true;
  int volume = 80;
  bool clock = true;
  // Settings -> Game Folders: each console's ("snes" -> folder); none = data_dir/<its folder>.
  std::map<std::string, std::string> games_dirs;
  std::string last_game;
  std::string style = "library";  // "library" (covers) or "xmb"
  std::string sort = "title";     // "title", "recent" (last played first), "region", "console",
                                  // "nocover" (Without Cover: this session only)
  std::string default_game;       // starts by itself when the app opens
  std::string console_order;      // Settings -> Console Order: "snes,nes,..." ("": the usual one)
  // 2: the standard order became Nintendo then Sony, the most basic first (00.43): an order saved
  // before is dropped once.
  int console_order_version = 0;

  void Load(const std::string& path)
  {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
    {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
      if (key == "theme")
        theme = value;
      else if (key == "wallpaper")
        wallpaper = value;
      else if (key == "sounds")
        sounds = value == "on";
      else if (key == "volume")
        volume = std::clamp(std::atoi(value.c_str()), 0, 100);
      else if (key == "clock")
        clock = value == "on";
      else if (key == "games_dir")
        games_dirs["snes"] = value;  // the Super Nintendo's (as before EmuSuNi played other consoles)
      else if (key.rfind("games_dir.", 0) == 0)
        games_dirs[key.substr(10)] = value;
      else if (key == "last_game")
        last_game = value;
      else if (key == "style")
        style = value == "xmb" ? "xmb" : "library";
      else if (key == "sort")
        sort = value == "nocover" ? "title" : value;  // Without Cover lasts one session
      else if (key == "default_game")
        default_game = value;
      else if (key == "console_order")
        console_order = value;
      else if (key == "console_order_version")
        console_order_version = std::atoi(value.c_str());
    }
    if (console_order_version < 2)
    {
      console_order.clear();
      console_order_version = 2;
    }
  }
  void Save(const std::string& path) const
  {
    std::string text = "# EmuSuNi menu preferences (edited by the menu)\n";
    const auto snes = games_dirs.find("snes");
    text += "theme=" + theme + "\nwallpaper=" + wallpaper + "\nsounds=" + (sounds ? "on" : "off") +
            "\nvolume=" + std::to_string(volume) + "\nclock=" + (clock ? "on" : "off") +
            "\ngames_dir=" + (snes == games_dirs.end() ? std::string() : snes->second) +
            "\nlast_game=" + last_game + "\nstyle=" + style + "\nsort=" + sort +
            "\ndefault_game=" + default_game + "\nconsole_order=" + console_order +
            "\nconsole_order_version=" + std::to_string(console_order_version) + "\n";
    for (const auto& [key, dir] : games_dirs)
    {
      if (key != "snes")
        text += "games_dir." + key + "=" + dir + "\n";
    }
    WriteTextFile(path, text);
  }
};

// ---------------------------------------------------------------------------------------------
// The menu.

enum class Category
{
  Settings,
  Games,      // every console's games
  SnesGames,  // one console's games: Games + 1 + its SystemId
  NesGames,
  N64Games,   // builds with the Nintendo 64 (EMUSUNI_N64)
  GbGames,    // builds with the Game Boys (EMUSUNI_GBA)
  GbcGames,
  GbaGames,
  Ps1Games,   // builds with the PlayStation (EMUSUNI_PS1)
  NdsGames,   // builds with the Nintendo DS (EMUSUNI_NDS)
  PspGames,   // builds with the PSP (EMUSUNI_PSP)
  DcGames,    // builds with the Dreamcast (EMUSUNI_DC)
  Sg1000Games,  // builds with the Sega consoles (EMUSUNI_SEGA)
  SmsGames,
  GgGames,
  MdGames,
  ArcadeGames,  // builds with the Arcade (EMUSUNI_ARCADE)
  ThreedsGames,  // builds with the Nintendo 3DS (EMUSUNI_3DS)
  NaomiGames,    // builds with the Dreamcast (EMUSUNI_DC): its arcade boards
  AtomiswaveGames,
  Favorites,
  SavedData,
  Themes,
};
constexpr int CATEGORY_COUNT = 23;
static_assert(static_cast<int>(Category::N64Games) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::N64));
static_assert(static_cast<int>(Category::GbaGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::GBA));
static_assert(static_cast<int>(Category::MdGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::MD));
static_assert(static_cast<int>(Category::ArcadeGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::ARCADE));
static_assert(static_cast<int>(Category::ThreedsGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::THREEDS));
static_assert(static_cast<int>(Category::NaomiGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::NAOMI));
static_assert(static_cast<int>(Category::AtomiswaveGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::ATOMISWAVE));
static_assert(static_cast<int>(Category::Favorites) == static_cast<int>(Category::Games) + 1 + SYSTEM_COUNT);
constexpr const char* CATEGORY_NAMES[CATEGORY_COUNT] = {
    "Settings",  "All Games", "Super Nintendo", "NES",        "Nintendo 64", "Game Boy",
    "Game Boy Color", "Game Boy Advance", "PlayStation", "Nintendo DS", "PSP", "Dreamcast",
    "SG-1000", "Master System", "Game Gear", "Genesis", "Arcade", "Nintendo 3DS", "NAOMI", "Atomiswave",
    "Favorites", "Saved Data", "Themes"};
constexpr const char* SORT_NAMES[][2] = {
    {"title", "A-Z"}, {"recent", "Recently Played"}, {"region", "Region"}, {"console", "Console"},
    {"nocover", "Without Cover"}};  // a view: only the games without a cover picture, A-Z

// The console of a console's game list (a console of this build), else -1.
int SystemOfList(int category)
{
  const int system = category - static_cast<int>(Category::Games) - 1;
  return system >= 0 && system < SYSTEM_COUNT && HasSystem(static_cast<SystemId>(system)) ? system : -1;
}

// The consoles' order in the Library's tabs, the XMB's columns and the Console sort (Settings ->
// Console Order, kept in xmb.ini as console_order): every console of this build once, the ones the
// saved order doesn't name after the others in their usual order.
std::vector<SystemId> s_console_order;
int s_console_order_version = 1;  // changes with the order (Tabs() and Columns() are made again)
// The consoles with a tab and an XMB column (Settings -> Consoles Shown, Library::ShownConsoles): some or all of
// the console order's, in it; empty until the menu sets it (then all). Changing it changes the version too.
std::vector<SystemId> s_shown_consoles;

// The standard order (Default Order): Nintendo's consoles, then Sony's, then Sega's, each from the
// most basic to the most powerful, then the arcade boards: the Arcade, the NAOMI (Sega's, the
// Dreamcast's hardware in a cabinet) and the Atomiswave (Sammy's, the same again).
constexpr SystemId STANDARD_ORDER[] = {SystemId::GB,  SystemId::NES, SystemId::GBC, SystemId::SNES,
                                       SystemId::GBA, SystemId::N64, SystemId::NDS, SystemId::THREEDS,
                                       SystemId::PS1, SystemId::PSP, SystemId::SG1000, SystemId::SMS,
                                       SystemId::GG,  SystemId::MD,  SystemId::DC,  SystemId::ARCADE,
                                       SystemId::NAOMI, SystemId::ATOMISWAVE};

// The consoles of this build in the standard order (any not in it after them).
std::vector<SystemId> StandardOrder()
{
  std::vector<SystemId> list;
  for (SystemId id : STANDARD_ORDER)
  {
    if (HasSystem(id))
      list.push_back(id);
  }
  for (const System& system : Systems())
  {
    if (std::find(list.begin(), list.end(), system.id) == list.end())
      list.push_back(system.id);
  }
  return list;
}

const std::vector<SystemId>& ConsoleOrder()
{
  if (s_console_order.empty())
    s_console_order = StandardOrder();
  return s_console_order;
}

// Console Order's ready-made orders: "company" (the standard one), "power" (whatever the maker:
// the most basic first; the Arcade's boards, 1970s to 2000s, after the Dreamcast, then the NAOMI and the
// Atomiswave, a Dreamcast each with more memory), "year" (when each came out, Japan's dates; the Arcade
// by the Neo Geo's 1990), "alpha" (by name, A-Z).
std::vector<SystemId> PresetOrder(const std::string& preset)
{
  std::vector<SystemId> order;
  if (preset == "power")
    order = {SystemId::GB,  SystemId::SG1000, SystemId::NES, SystemId::SMS,    SystemId::GG,
             SystemId::GBC, SystemId::MD,     SystemId::SNES, SystemId::GBA,   SystemId::PS1,
             SystemId::N64, SystemId::NDS,    SystemId::DC,   SystemId::ARCADE, SystemId::NAOMI,
             SystemId::ATOMISWAVE, SystemId::PSP, SystemId::THREEDS};
  else if (preset == "year")
    order = {SystemId::NES, SystemId::SG1000, SystemId::SMS,  SystemId::MD,  SystemId::GB,   // 1983 1983 1985 1988 1989
             SystemId::GG,  SystemId::ARCADE, SystemId::SNES, SystemId::PS1, SystemId::N64,  // 1990 1990 1990 1994 1996
             SystemId::GBC, SystemId::DC,     SystemId::NAOMI, SystemId::GBA, SystemId::ATOMISWAVE,  // 1998 1998 1998 2001 2003
             SystemId::NDS, SystemId::PSP,    SystemId::THREEDS};                             // 2004 2004 2011
  else if (preset == "alpha")
  {
    for (const System& system : Systems())
      order.push_back(system.id);
    std::sort(order.begin(), order.end(), [](SystemId a, SystemId b) {
      std::string x = GetSystem(a).name, y = GetSystem(b).name;
      std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return x < y;
    });
  }
  else
    order = StandardOrder();
  std::vector<SystemId> list;
  for (SystemId id : order)
  {
    if (HasSystem(id))
      list.push_back(id);
  }
  for (SystemId id : StandardOrder())
  {
    if (std::find(list.begin(), list.end(), id) == list.end())
      list.push_back(id);
  }
  return list;
}

void SetConsoleOrder(const std::vector<SystemId>& order)
{
  std::vector<SystemId> list;
  for (SystemId id : order)
  {
    if (HasSystem(id) && std::find(list.begin(), list.end(), id) == list.end())
      list.push_back(id);
  }
  for (SystemId id : StandardOrder())
  {
    if (std::find(list.begin(), list.end(), id) == list.end())
      list.push_back(id);
  }
  s_console_order = list;
  // The consoles shown stay the same ones, in the new order.
  if (!s_shown_consoles.empty())
  {
    std::vector<SystemId> shown;
    for (SystemId id : list)
    {
      if (std::find(s_shown_consoles.begin(), s_shown_consoles.end(), id) != s_shown_consoles.end())
        shown.push_back(id);
    }
    s_shown_consoles = shown;
  }
  s_console_order_version++;
}

// The consoles with a tab, in the console order.
const std::vector<SystemId>& ShownOrder()
{
  return s_shown_consoles.empty() ? ConsoleOrder() : s_shown_consoles;
}

// "snes,nes,n64" <-> the order.
std::vector<SystemId> ParseConsoleOrder(const std::string& keys)
{
  std::vector<SystemId> order;
  std::stringstream in(keys);
  std::string key;
  while (std::getline(in, key, ','))
  {
    if (const System* system = SystemByKey(key))
      order.push_back(system->id);
  }
  return order;
}

// The order a saved one stands for: a ready-made order saved before some consoles came (its keys are
// that order's without them) is that order, the new consoles in their places; any other as it is
// (the consoles it doesn't name go after the others: SetConsoleOrder).
std::vector<SystemId> SavedConsoleOrder(const std::string& keys)
{
  const std::vector<SystemId> saved = ParseConsoleOrder(keys);
  for (const char* preset : {"power", "year", "alpha"})
  {
    std::vector<SystemId> named;
    for (SystemId id : PresetOrder(preset))
    {
      if (std::find(saved.begin(), saved.end(), id) != saved.end())
        named.push_back(id);
    }
    if (!saved.empty() && named == saved)
      return PresetOrder(preset);
  }
  return saved;
}

std::string ConsoleOrderKeys()
{
  std::string keys;
  for (SystemId id : ConsoleOrder())
    keys += std::string(keys.empty() ? "" : ",") + GetSystem(id).key;
  return keys;
}

// The XMB style's columns, left to right: Settings, all the games, a column per console shown (in the
// console order), the favorites. Saved Data and Themes are pages of Settings in both styles (Cross
// on them in its list, Circle back).
const std::vector<int>& Columns()
{
  static std::vector<int> columns;
  static int version = 0;
  if (version != s_console_order_version)
  {
    version = s_console_order_version;
    columns = {static_cast<int>(Category::Settings), static_cast<int>(Category::Games)};
    for (SystemId id : ShownOrder())
      columns.push_back(static_cast<int>(Category::Games) + 1 + static_cast<int>(id));
    columns.push_back(static_cast<int>(Category::Favorites));
  }
  return columns;
}

// The Library style's tabs (L1 / R1, round and round), left to right: All, a tab per console shown
// (in the console order), Favorites, Settings.
const std::vector<int>& Tabs()
{
  static std::vector<int> tabs;
  static int version = 0;
  if (version != s_console_order_version)
  {
    version = s_console_order_version;
    tabs = {static_cast<int>(Category::Games)};
    for (SystemId id : ShownOrder())
      tabs.push_back(static_cast<int>(Category::Games) + 1 + static_cast<int>(id));
    tabs.push_back(static_cast<int>(Category::Favorites));
    tabs.push_back(static_cast<int>(Category::Settings));
  }
  return tabs;
}

const char* TabName(int category)
{
  if (category == static_cast<int>(Category::Games))
    return "All";
  const int system = SystemOfList(category);
  if (system >= 0)
    return GetSystem(static_cast<SystemId>(system)).tab;
  return CATEGORY_NAMES[category];
}

int ListOfSystem(SystemId system)
{
  return static_cast<int>(Category::Games) + 1 + static_cast<int>(system);
}

// Saved Data and Themes: pages of Settings.
bool IsSettingsPage(int category)
{
  return category == static_cast<int>(Category::SavedData) || category == static_cast<int>(Category::Themes);
}

// The tab / column a list is shown under: Settings for its pages, else the list's own.
int ColumnOf(int category)
{
  return IsSettingsPage(category) ? static_cast<int>(Category::Settings) : category;
}

// The columns / tabs that list games: all of them, each console's, and the favorites.
bool IsGameList(int category)
{
  return category == static_cast<int>(Category::Games) || SystemOfList(category) >= 0 ||
         category == static_cast<int>(Category::Favorites);
}

// Where a list is in the XMB row (its column, or Settings' for its pages).
int ColumnIndex(int category)
{
  const auto& columns = Columns();
  const auto it = std::find(columns.begin(), columns.end(), ColumnOf(category));
  return it == columns.end() ? 0 : static_cast<int>(it - columns.begin());
}

std::string Upper(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// The search keyboard (R2): four rows of ten keys, then Space, Delete, Clear and Done, each two
// or four columns wide.
constexpr const char* KEY_ROWS[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM'&."};
constexpr int KEY_COLUMNS = 10;
constexpr int KEY_ROW_COUNT = 5;
constexpr size_t SEARCH_MAX = 40;
struct WideKey
{
  const char* label;
  int first, span;    // columns
  const char* glyph;  // the button that does the same
};
constexpr WideKey WIDE_KEYS[] = {{"Space", 0, 3, "Triangle"},
                                 {"Delete", 3, 2, "Square"},
                                 {"Clear All", 5, 3, "L2"},
                                 {"Done", 8, 2, "R2"}};

int WideKeyAt(int column)
{
  for (int i = 0; i < static_cast<int>(std::size(WIDE_KEYS)); i++)
  {
    if (column >= WIDE_KEYS[i].first && column < WIDE_KEYS[i].first + WIDE_KEYS[i].span)
      return i;
  }
  return 0;
}

// What the search compares: lower-case letters and digits, the words separated by one space
// ("Yoshi's Island" -> "yoshis island").
std::string SearchText(const std::string& text)
{
  std::string out;
  bool space = false;
  for (const char ch : text)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\'')
      continue;
    if (std::isalnum(c))
    {
      if (space && !out.empty())
        out += ' ';
      space = false;
      out += static_cast<char>(std::tolower(c));
    }
    else
    {
      space = true;
    }
  }
  return out;
}

// What the box on a console's page says (DrawConsoleInfo): the systems it plays, the BIOS only where the player
// must bring one (the Arcade's Neo Geo games, the NAOMI's and the Atomiswave's every game; every other core has
// its own built in, and the optional real ones only confused players, so the box no longer lists them) and where
// it goes (00.64: beside the games, or the core's own folder in system), and its files when its extensions alone
// don't say it. The BIOS and Files rows fit two lines and one (the Folders rows need the room: two drives and a
// folder of the player's are four lines).
// (No " = " in a text: the language files' separator.)
struct ConsoleNote
{
  SystemId id;
  const char* plays;
  const char* bios;   // nullptr: no BIOS row
  const char* files;  // nullptr: its extensions (Systems.cpp), and a .zip or not
};
constexpr ConsoleNote CONSOLE_NOTES[] = {
    {SystemId::SNES, "Super Nintendo and Super Famicom games", nullptr, nullptr},
    {SystemId::NES, "NES, Famicom and Famicom Disk System games", nullptr, nullptr},
    {SystemId::N64, "Nintendo 64 games", nullptr, nullptr},
    {SystemId::GB, "Game Boy games", nullptr, nullptr},
    {SystemId::GBC, "Game Boy Color games", nullptr, nullptr},
    {SystemId::GBA, "Game Boy Advance games", nullptr, nullptr},
    {SystemId::PS1, "PlayStation games, on one disc or several", nullptr, nullptr},
    {SystemId::NDS, "Nintendo DS games", nullptr, nullptr},
    {SystemId::PSP, "PSP games: UMD images and PSN downloads", nullptr, nullptr},
    {SystemId::DC, "Dreamcast games, on one disc or several", nullptr, nullptr},
    {SystemId::SG1000, "SG-1000 and SC-3000 games", nullptr, nullptr},
    {SystemId::SMS, "Master System and Mark III games", nullptr, nullptr},
    {SystemId::GG, "Game Gear games", nullptr, nullptr},
    {SystemId::MD, "Genesis and Mega Drive games", nullptr, nullptr},
    // (FinalBurn Neo finds a BIOS set beside the game, in system/fbneo or in system: Arcade::Missing.)
    {SystemId::ARCADE, "Neo Geo, Capcom CPS1/CPS2/CPS3, Sega, Konami, Taito...",
     "neogeo.zip and the like in Roms/arcade or /data/EmuSuNi/system/fbneo",
     "FinalBurn Neo 1.0.0.03 sets, one .zip per game (mslug.zip)"},
    {SystemId::THREEDS, "Nintendo 3DS games and homebrew", nullptr,
     "Decrypted .3ds (.cci), .cxi, .3dsx; no .cia, no .zip"},
    // (Flycast finds a BIOS zip in system/dc or beside the game: shell/libretro/oslib.cpp's findNaomiBios. A NAOMI
    // game is a MAME set or a cartridge image, a loose .dat or .bin: src/Naomi.h.)
    {SystemId::NAOMI, "Sega's NAOMI and NAOMI 2 arcade games",
     "naomi.zip, naomi2.zip in Roms/naomi or /data/EmuSuNi/system/dc",
     "MAME sets (mvsc2.zip) or .dat / .bin images, unzipped"},
    {SystemId::ATOMISWAVE, "Sammy's Atomiswave arcade games",
     "awbios.zip in Roms/atomiswave or /data/EmuSuNi/system/dc", "MAME sets, one .zip per game (kofxi.zip)"},
};

struct Item
{
  std::string label;
  std::string detail;  // right-hand value (settings) or subtitle
  int game = -1;       // index into games (Games category)
  int theme = -1;      // index into THEMES, or -2 - wallpaper index
  int setting = -1;
  int save = -1;  // index into the saved data list
};

enum Setting
{
  SET_SOUNDS,
  SET_VOLUME,
  SET_CLOCK,
  SET_ABOUT,
  SET_RESET_ALL,
  SET_CONTROLS,    // Controls: the consoles, then a console's controls list
  SET_GAMES_DIR,   // a console's game folder: a folder browser
  SET_FOLDERS,     // Game Folders: the consoles and their folders
  SET_STYLE,       // Menu Style: Library or XMB
  SET_LANGUAGE,    // Language
  SET_SAVES,       // a game's saved data
  SET_DELETE,      // "Delete?" for a save
  SET_COVERS,      // Covers Saved On: the internal storage or a USB drive (and where to put your own)
  SET_DOWNLOAD,    // Download Covers: progress and result
  SET_DELETE_GAME,      // "Delete?" for a game (Triangle's options)
  SET_CLEANUP,          // Clean Up Games: duplicates and other regions
  SET_CLEANUP_CONFIRM,  // "Delete?" for Clean Up Games
  SET_SAVED_DATA,       // the Saved Data page (in Settings)
  SET_THEMES,           // the Themes page (in Settings)
  SET_FIND_COVER,       // Find Cover (Triangle's options): pictures to choose from
  SET_NO_COVER,         // Games Without Cover: the games without a picture, each one's Find Cover
  SET_CONSOLE_ORDER,    // Console Order: the consoles' tabs, moved up and down
  SET_CONSOLES_SHOWN,   // Consoles Shown: all, those with games, or the ones chosen (each on or off)
#ifdef EMUSUNI_SATURN
  SET_SATURN_TEST,      // Saturn (speed test): builds with EMUSUNI_SATURN, with the file saturn-test (src/SaturnTest.h)
#endif
  SET_GROUP = 100,  // + index into Settings::Categories(): an emulation settings list
};

// A row of a settings list panel.
struct SettingRow
{
  std::string label;
  std::string value;
  std::string key;      // setting key; empty for headers
  bool header = false;  // category title
  bool scope = false;   // "Apply To: All Games / This Console / This Game"
  bool note = false;    // takes effect at the next game start
  std::string glyph;    // Controls: the DualShock 4 control, drawn as its icon
  int tag = 0;          // cheats: 1 = an ON tag (green), -1 = an OFF tag (red)
  ImTextureID icon{};
};

// A PSP-style panel on the right: options to pick from, information text, or a settings list.
struct SidePanel
{
  bool open = false;
  std::string title;
  std::vector<std::string> options;
  std::vector<std::string> lines;
  std::vector<SettingRow> rows;
  std::string category;  // settings list: one category, or all (game settings)
  std::string game_id;   // settings list: the game's own settings (its key), else global
  int system = -1;       // settings list: its console's settings only; -1 = every console's
  int selected = 0;
  int setting = -1;
  float slide = 0.0f;
  float scroll = 0.0f;
};

struct Game
{
  Library::Game rom;
  int64_t played = 0;
};
}  // namespace

// ---------------------------------------------------------------------------------------------

class Menu::Impl
{
public:
  Impl(Display& display, const std::string& data_dir, const std::string& assets_dir)
      : m_display(display), m_data_dir(data_dir), m_assets_dir(assets_dir)
  {
    m_prefs.Load(m_data_dir + "/xmb.ini");
    // (The Roms/<console> folders are made by BuildGames: MakeFolders.)
    for (const System& system : Systems())
    {
      if (GamesFolder(system.id).rfind(m_data_dir, 0) == 0)
        MakeDirs(GamesFolder(system.id));  // the app's own folders (not one on a USB stick)
    }
    MakeDirs(m_data_dir + "/covers");
    MakeDirs(m_data_dir + "/cheats");
    MakeDirs(m_data_dir + "/themes");
    LoadNames();
    BuildGames();
    LoadPlayed();
    LoadFavorites();
  }
  ~Impl()
  {
    m_dl_cancel = true;
    if (m_dl_thread.joinable())
      m_dl_thread.join();
    StopCoverThread();
    {
      std::lock_guard lock(m_find_mutex);
      m_find_stop = true;
      m_find_jobs.clear();
    }
    m_find_wake.notify_all();
    if (m_find_thread.joinable())
      m_find_thread.join();
  }

  bool Run(Library::Game* game);
  void GameEnded(const std::string& error)
  {
    m_intro = true;
    m_intro_time = 0.0f;
    m_pending_error = error;
  }
  bool DefaultGame(Library::Game* game)
  {
    if (m_prefs.default_game.empty())
      return false;
    for (const Game& g : m_games)
    {
      if (g.rom.path == m_prefs.default_game)
      {
        *game = g.rom;
        return true;
      }
    }
    Log("default game not found: " + m_prefs.default_game);
    return false;
  }
  ImFont* m_font = nullptr;
  ImFont* m_bold = nullptr;
  void LoadAssets();

private:
  void BuildGames();
  void BuildItems();
  const Theme& CurrentTheme() const;
  void ApplyPreferences();
  ImTextureID LoadPngTexture(const std::string& path, float* aspect = nullptr,
                             uint32_t max_w = 0, uint32_t max_h = 0);
  std::string GamesFolder(SystemId id) const
  {
    const System& system = GetSystem(id);
    const auto it = m_prefs.games_dirs.find(system.key);
    return it == m_prefs.games_dirs.end() || it->second.empty() ? DefaultFolder(system) : it->second;
  }
  std::string DefaultFolder(const System& system) const
  {
    return system.default_folder[0] == '/' ? std::string(system.default_folder) :
                                             m_data_dir + "/" + system.default_folder;
  }
  void RefreshGames();
  // The places games are found in by themselves (Library::FindPlaces): looked at again when a game list is
  // entered (at most every 1.5 s, on a thread: PlacesLooked takes its answer), or `now`; the games are read
  // again when they changed (a drive plugged in).
  void CheckPlaces(bool now);
  void PlacesLooked();
  void PlacesFound(const Library::Places& places);
  // Every console's Roms/<key> made where it is missing: the internal storage and the drives a player keeps games
  // on (Library::GameRomsFolders); one line in the log per folder.
  void MakeFolders(const std::vector<std::string>& drives);
  // Settings -> Consoles Shown: the tabs and the XMB's columns follow it (every frame: the tour's "set" too).
  std::string ShownMode() const;
  std::string ShownDetail() const;
  std::set<std::string> OffConsoles() const;  // the keys turned off for Only the Ones I Choose
  void UpdateShownConsoles();
  void OpenConsolesShown(int selected = -1);
  // Settings -> Covers Saved On: the folder new covers go to (check: make it, and the internal storage when it
  // can't be), and what the Settings list says of it.
  std::string CoversSaveDir(bool check);
  std::string CoversDetail() const;
  // The box at the bottom left of a console's page: its rows (a label, the text) and its drawing (its right edge).
  std::vector<std::pair<std::string, std::string>> ConsoleInfo(SystemId id) const;
  float DrawConsoleInfo(ImDrawList* dl, SystemId id, float alpha);

  void HandleInput(uint32_t pressed);
  void ActivateItem(const Item& item);
  void OpenSettingPanel(int setting);
  void ApplySettingOption(int setting, int option);
  void OpenSettingsList(const std::string& category, const std::string& game_id, int system = -1);
  void BuildSettingRows();
  void ChangeSettingRow(int direction);
  void MoveSettingRow(int direction);
  void OpenControlsMenu();
  void OpenControlsList(const std::string& game_key, SystemId system);
  void BuildControlRows();
  void UpdateCapture(float dt);
  void OpenCheatsList(const Game& game);
  void BuildCheatRows();
  void UpdateCheatFinder();
  bool CheatPanelBack();
  void OpenGameFolders();
  void OpenConsoleOrder(int selected = -1);
  bool ConsoleOrderInput(uint32_t pressed);
  void MoveConsole(int from, int to);
  void OpenSettingsPage(Category page);
  void OpenFolderBrowser(const std::string& path);
  void OpenGameOptions();
  void SetLanguage(const std::string& code);
  // Favorites (L2, or Triangle's options) and the search (R2: a keyboard).
  void LoadFavorites();
  void SaveFavorites() const;
  bool IsFavorite(int game) const;
  void ToggleFavorite(int game);
  void RebuildGameLists(bool keep_selection);
  void SetSearch(const std::string& text);
  void EditSearch(const std::string& key);
  void HandleKeyboard(uint32_t pressed);
  void ShowToast(const std::string& text, int icon);
  // A game's name of the user's own (data_dir/names.ini) and a text asked for: the PS4's
  // keyboard, else the menu's own (asking mode).
  void LoadNames();
  void SaveNames() const;
  void RenameGame(const std::string& key, const std::string& name);
  void AskText(const std::string& title, const std::string& text, std::function<void(const std::string&)> done);
  void UpdateAsk();
  // Find Cover (Triangle's options): libretro's pictures most like a name, looked up on a thread.
  void OpenFindCover(int game, bool back_to_list = false);
  void StartFind();
  void BuildFindRows();
  void UpdateFind(float dt);
  void FindWorker();
  // Games Without Cover (Settings): the games without a picture; Find Cover for each comes back.
  void OpenNoCover(int select);
  void BuildNoCoverRows();
  void ReturnToNoCover();
  std::string NoCoverDetail() const;
  void RefreshNoCoverItem();
  // Deleting games: one (Triangle's options), or the duplicates and the other regions (Clean Up).
  void OpenCleanUp();
  void DeleteGames(const std::vector<std::string>& paths);
  std::string OptionHelp(const std::string& option) const;
  void TourCommand(const std::string& command);
  // Saved data.
  void LoadSaves();
  void OpenSavesPanel(int save);
  std::string SaveTitle(const std::string& id) const;
  ImTextureID StatePicture(const std::string& id, int slot);
  void ForgetStatePictures();

  // Covers: loaded on a thread when a game comes into view, a few dozen kept.
  void IndexCovers();
  ImTextureID Cover(int game, float* aspect);
  void UpdateCovers();
  void StopCoverThread();
  void StartCoverDownload(bool closest = false);
  void UpdateCoverDownload();
  bool MissingCovers() const;
  bool HasCover(const Library::Game& rom) const;

  bool Library() const { return m_prefs.style != "xmb"; }
  int LibraryTab() const;
  void LoadPlayed();
  void SavePlayed() const;
  std::string SortName() const;

  void Draw(float dt);
  void DrawBackground();
  void DrawCategoryIcon(ImDrawList* dl, Category category, ImVec2 center, float size, ImU32 color);
  void DrawItem(ImDrawList* dl, const Item& item, float y, bool selected, float alpha);
  void DrawSidePanel(ImDrawList* dl, float dt);
  void DrawHints(ImDrawList* dl);
  void DrawIntro(ImDrawList* dl, float dt);
  void DrawLibraryBackground();
  void DrawLibrary(ImDrawList* dl, float dt);
  void DrawLibraryGames(ImDrawList* dl, float alpha, int c);
  void DrawKeyboard(ImDrawList* dl, float dt);
  void DrawToast(ImDrawList* dl, float dt);
  void DrawLibrarySaves(ImDrawList* dl, float alpha);
  void DrawLibraryList(ImDrawList* dl, float alpha);
  void DrawLibraryHints(ImDrawList* dl);
  void DrawCover(ImDrawList* dl, int game, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float alpha,
                 int focus);
  float DrawBadge(ImDrawList* dl, const Library::Game& rom, ImVec2 at, float alpha, float max_w = 0.0f);
  void DrawTopBar(ImDrawList* dl);
  void DrawPageTitle(ImDrawList* dl, Category category, const std::string& title, const std::string& sub,
                     float alpha, float sub_alpha);
  void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color,
            const std::string& text, bool shadow = true);
  float TextWidth(ImFont* font, float size, const std::string& text);
  std::string WrapText(ImFont* font, float size, const std::string& text, float width);
  std::string Fit(ImFont* font, float size, std::string text, float width);

  void Play(const char* name)
  {
    auto it = m_sounds.find(name);
    if (it != m_sounds.end())
      m_audio.Play(it->second.get());
  }

  Display& m_display;
  std::string m_data_dir, m_assets_dir;
  Preferences m_prefs;
  std::vector<Game> m_games;
  // Where the games were looked for (BuildGames): the places, each console's number of games, the covers'
  // folder on a USB drive ("" without a drive).
  Library::Places m_places;
  float m_places_time = 0.0f;
  std::future<Library::Places> m_places_look;  // CheckPlaces' look on a thread (a sleeping drive can be slow)
  Library::PlacesReads m_places_reads;         // its answer counts only if the places weren't read again since
  std::array<int, SYSTEM_COUNT> m_console_games{};
  std::string m_usb_covers;
  std::set<std::string> m_folders_logged;  // the Roms folders MakeFolders has said something of
  // The console box's text, made for one console, language and folder at a time (DrawConsoleInfo).
  std::string m_info_key;
  float m_info_label_w = 0.0f;
  std::vector<std::pair<std::string, std::vector<std::string>>> m_info_rows;
  std::vector<std::string> m_wallpapers;
  std::vector<Item> m_items[CATEGORY_COUNT];
  int m_category = static_cast<int>(Category::Games);
  int m_selected[CATEGORY_COUNT] = {};
  float m_category_pos = 1.0f;  // the XMB row's position (Columns(): all the games)
  float m_item_pos[CATEGORY_COUNT] = {};
  bool m_all_covered[CATEGORY_COUNT] = {};  // Without Cover emptied the list: its games all have covers
  SidePanel m_panel;
  bool m_assets_loaded = false;

  Audio m_audio;
  Input m_input;
  std::map<std::string, std::unique_ptr<Sound>> m_sounds;
  ImTextureID m_wallpaper{};
  ImTextureID m_logo{};  // the "EmuSuNi" letters
  float m_logo_aspect = 4.0f;
  ImTextureID m_glow{};  // a soft round light (white, gaussian alpha): the intro's stardust

  // Controls page: a game's own layout (its key) or its console's normal one; a button capture.
  bool m_controls_open = false;
  bool m_order_moving = false;  // Console Order: the selected console is picked up (Up / Down move it)
  std::string m_controls_game;
  SystemId m_controls_system = SystemId::SNES;
  bool m_capture = false;
  bool m_capture_released = false;
  float m_capture_time = 0.0f;
  int m_capture_row = 0;

  // Cheats page, and Find Cheats Online's lists and codes (CheatDownload.h).
  enum class CheatPage
  {
    List,
    Search,
    Pick,
  };
  Cheats::List m_cheats;
  std::string m_cheats_game, m_cheats_path, m_cheats_title, m_cheats_folder, m_cheats_file;
  SystemId m_cheats_system = SystemId::SNES;
  CheatPage m_cheat_page = CheatPage::List;
  CheatDownload::Finder m_finder;
  CheatDownload::Finder::State m_finder_state = CheatDownload::Finder::State::Idle;
  int m_finder_mb = -1;
  int m_search_selected = 0;

  // Saved data.
  std::vector<Saves::GameSaves> m_saves;
  int m_saves_panel = -1;
  std::string m_delete_key;  // "sram" or "slot:N" waiting for "Delete?"
  std::map<std::string, ImTextureID> m_state_pictures;  // "<id>/<slot>"
  std::string m_state_pictures_game;

  // Covers.
  std::map<std::string, std::string> m_cover_files;  // lower-case name without .png -> path
  struct CoverEntry
  {
    ImTextureID texture{};
    float aspect = 0.7f;
    uint64_t used = 0;
    bool requested = false, missing = false;
  };
  std::map<int, CoverEntry> m_covers;
  uint64_t m_cover_clock = 0;
  std::thread m_cover_thread;
  std::mutex m_cover_mutex;
  std::condition_variable m_cover_wake;
  // A cover asked for (its game's index into m_games, its file) and its picture, each of one IndexCovers'
  // generation: the games are read again with it (m_games in another order), so an older one's index is
  // another game's now and it is dropped.
  struct CoverJob
  {
    int game = -1;
    std::string path;
    uint64_t generation = 0;
  };
  struct CoverDone
  {
    int game = -1;
    ImageRGBA image;
    uint64_t generation = 0;
  };
  std::deque<CoverJob> m_cover_jobs;
  std::deque<CoverDone> m_cover_done;
  uint64_t m_cover_generation = 0;  // (under m_cover_mutex)
  bool m_cover_stop = false;
  // Download Covers (a background thread).
  std::thread m_dl_thread;
  std::atomic<bool> m_dl_cancel{false}, m_dl_done{false};
  std::mutex m_dl_mutex;
  std::string m_dl_status;
  CoverDownload::Result m_dl_result;
  std::atomic<bool> m_dl_pause{false};  // while a game runs
  std::atomic<int> m_dl_progress{0}, m_dl_total{0};
  std::vector<std::pair<std::string, std::string>> m_dl_saved;  // id, file (under m_dl_mutex)
  bool m_dl_auto = false;  // the start's automatic download is under way or done
  int m_dl_skipped = 0;    // the games the download left out: they have a cover (in either place)
  bool m_covers_changed = false;  // a cover was saved: Without Cover's lists again (Run)
  bool m_dl_closest = false;      // the download is Games Without Cover's "Closest for All"
  std::string m_nocover_status;   // its result, shown in Games Without Cover
  int m_nocover_progress = -1;    // the download's progress the list last showed

  std::string m_browse_dir;  // Game Folder browser: the folder shown ("" = the places)
  SystemId m_browse_system = SystemId::SNES;  // whose game folder it picks
  float m_tab_pos = 0.0f;
  // A page of Settings fades in where its list was (and the list again when it closes).
  int m_drawn_category = -1;
  float m_page_alpha = 1.0f;
  float m_sort_shown = 0.0f;
  bool m_first_language = false;
  float m_time = 0.0f;
  float m_fade = 1.0f;
  bool m_intro = true;
  float m_intro_time = 0.0f;
  int m_launch = -1;
  float m_launch_time = 0.0f;
#ifdef EMUSUNI_SATURN
  bool m_saturn_test = false;  // Settings' Saturn (speed test) picked: Run returns its stand-in (src/SaturnTest.h)
#endif
  std::string m_pending_error;
  float m_memory_log = 0.0f;

  // Favorites (game keys, data_dir/favorites.ini) and the search.
  std::set<std::string> m_favorites;
  std::string m_search;
  bool m_keyboard = false;
  int m_key_row = 1, m_key_col = 0;
  float m_keyboard_slide = 0.0f;
  std::string m_toast;  // a short notice at the top ("Added to Favorites")
  int m_toast_icon = 0;  // 1 a star, 2 an empty star
  float m_toast_time = 0.0f;

  // The user's names for games (key -> name).
  std::map<std::string, std::string> m_names;
  // A text asked for: the PS4's keyboard is open (m_asking), or the menu's own edits m_ask_text.
  std::function<void(const std::string&)> m_ask_done;
  bool m_asking = false;
  bool m_keyboard_ask = false;
  std::string m_ask_text, m_ask_title;
  // Find Cover: the jobs for the thread (0 list the pictures like m_find_query, 1 preview one,
  // 2 make one the cover) and their results.
  struct FindJob
  {
    int type = 0;
    SystemId system = SystemId::SNES;
    bool titles = false;
    std::string text, name, path;
    Library::Game game;  // a search: the game (its region chooses among the closest title's pictures)
    int serial = 0;      // a search: which one (StartFind's)
  };
  struct FindResult
  {
    int type = 0;
    bool ok = false;
    std::vector<std::string> names;
    std::string name, path, key, error;
    int serial = 0;
  };
  std::thread m_find_thread;
  std::mutex m_find_mutex;
  std::condition_variable m_find_wake;
  std::deque<FindJob> m_find_jobs;
  std::deque<FindResult> m_find_results;
  bool m_find_stop = false;
  std::string m_find_key, m_find_title;  // the game (by key: refreshes renumber the games)
  SystemId m_find_system = SystemId::SNES;
  std::string m_find_query;
  bool m_find_titles = false;
  bool m_find_busy = false;
  std::vector<std::string> m_find_names;
  std::string m_find_status;
  bool m_find_return = false;  // opened from Games Without Cover: Circle / its cover saved go back there
  int m_find_return_row = 0;   // the game's row there
  Library::Game m_find_rom;    // the game looked for
  bool m_find_select_first = false;  // its first search: the closest picture is selected
  int m_preview_serial = 0;    // each preview its own file (two can come in one frame)
  int m_find_serial = 0;       // the latest search: older results are dropped
  // The selected picture's preview (downloaded after the selection rests on it a moment).
  std::string m_preview_want, m_preview_name;
  float m_preview_wait = 0.0f;
  ImTextureID m_preview{};
  float m_preview_aspect = 1.0f;

  // Files waiting for "Delete" (Delete Game, Clean Up Games), and Clean Up's two lists.
  std::vector<std::string> m_pending_delete;
  std::vector<std::string> m_cleanup_dups, m_cleanup_others[SYSTEM_COUNT];
  std::string m_cleanup_system;  // Keep Only USA Games' console ("snes", "nes", "n64")
  // The screenshot tour (Tour.h) is running; the menu style to put back at its end.
  bool m_tour = false;
  std::string m_tour_style;
};

// ---------------------------------------------------------------------------------------------
// Games and assets.

void Menu::Impl::BuildGames()
{
  m_games.clear();
  // Each console's games in its folder (Game Folders, else its default one) and, without a folder chosen, in
  // Roms/<console> of the internal storage and of every USB drive (Library::ScanAll: a folder shared with
  // another console has its .zip files opened to see whose they are, a file found twice is one game).
  const std::vector<std::string> drives_now = Library::UsbDrives();
  MakeFolders(drives_now);  // (first: the folders made are found below, the next look sees the same places)
  m_places = Library::FindPlaces(m_data_dir, drives_now);
  m_places_reads.Read();  // (a look on the thread started before this is older now)
  m_places_time = m_time;
  m_usb_covers = Library::UsbCoversFolder(m_places.drives);
  std::array<std::string, SYSTEM_COUNT> chosen;
  for (const System& system : Systems())
    chosen[static_cast<size_t>(system.id)] = GamesFolder(system.id);
  Library::Found scan = Library::ScanAll(chosen, m_places);
  std::string found, drives;
  for (const std::string& drive : m_places.drives)
    drives += (drives.empty() ? "" : ", ") + drive;
  for (const System& system : Systems())
  {
    const size_t id = static_cast<size_t>(system.id);
    std::string where;
    for (size_t f = 0; f < scan.folders[id].size(); f++)
    {
      if (scan.found_in[id][f] > 0)
        where += (where.empty() ? " in " : " + ") + scan.folders[id][f];
    }
    found += std::string(found.empty() ? "" : ", ") + std::to_string(scan.counts[id]) + " " + system.tab + where;
  }
  m_console_games = scan.counts;
  for (Library::Game& rom : scan.games)
  {
    Game game;
    game.rom = std::move(rom);
    m_games.push_back(std::move(game));
  }
  Log("game places: USB drives " + (drives.empty() ? std::string("none") : drives) + "; covers on USB: " +
      (m_usb_covers.empty() ? std::string("none") : m_usb_covers));
  // The user's own names (Rename).
  for (Game& game : m_games)
  {
    const auto it = m_names.find(game.rom.key);
    if (it != m_names.end())
    {
      game.rom.title = it->second;
      game.rom.custom_title = true;
    }
  }
  // Every console's games together, by title (as each console's own list).
  std::stable_sort(m_games.begin(), m_games.end(), [](const Game& a, const Game& b) {
    const std::string la = Lower(a.rom.title), lb = Lower(b.rom.title);
    return la != lb ? la < lb : a.rom.id < b.rom.id;
  });
  Log("games: " + found);
}

void Menu::Impl::LoadAssets()
{
  if (m_assets_loaded)
    return;
  m_assets_loaded = true;
  ImGuiIO& io = ImGui::GetIO();
  // Every text size the menus use is baked into one big atlas (4096 x 2048, made once): a
  // growing atlas would make a new texture, and GPU memory comes and goes at a cost on the PS4.
  io.Fonts->TexMinWidth = 4096;
  io.Fonts->TexMinHeight = 2048;
  const std::string fonts = m_assets_dir + "/xmb/fonts/";
  // Nunito (Latin, Cyrillic), then the Japanese / Chinese / Korean glyphs merged in (PS4Lang).
  auto add_font = [&](const char* file, bool bold) {
    ImFont* font = io.Fonts->AddFontFromFileTTF((fonts + file).c_str(), 32.0f);
    if (!font)
      return font;
    for (const std::string& extra : PS4Lang::ExtraFonts(m_assets_dir, bold))
    {
      ImFontConfig config;
      config.MergeMode = true;
      io.Fonts->AddFontFromFileTTF(extra.c_str(), 32.0f, &config);
    }
    return font;
  };
  m_font = add_font("Nunito-Regular.ttf", false);
  m_bold = add_font("Nunito-ExtraBold.ttf", true);
  if (!m_font)
    m_font = io.Fonts->AddFontDefault();
  if (!m_bold)
    m_bold = m_font;

  for (const char* name : {"move", "category", "confirm", "back", "launch", "error", "menu", "intro"})
  {
    // A sound in data_dir/themes/sounds replaces the built-in one.
    std::unique_ptr<Sound> sound = LoadWav(m_data_dir + "/themes/sounds/" + name + ".wav");
    if (!sound)
      sound = LoadWav(m_assets_dir + "/xmb/sounds/" + name + ".wav");
    if (sound)
      m_sounds[name] = std::move(sound);
    else
      Log(std::string("sound not loaded: ") + name);
  }
  m_logo = LoadPngTexture(m_assets_dir + "/xmb/logo.png", &m_logo_aspect);
  {
    constexpr int SIZE = 64;
    std::vector<uint8_t> pixels(SIZE * SIZE * 4);
    for (int y = 0; y < SIZE; y++)
    {
      for (int x = 0; x < SIZE; x++)
      {
        const float dx = (x + 0.5f) / SIZE * 2.0f - 1.0f, dy = (y + 0.5f) / SIZE * 2.0f - 1.0f;
        uint8_t* px = &pixels[(y * SIZE + x) * 4];
        px[0] = px[1] = px[2] = 255;
        px[3] = static_cast<uint8_t>(std::exp(-(dx * dx + dy * dy) * 4.0f) * 255.0f);
      }
    }
    m_glow = m_display.CreateTexture(pixels.data(), SIZE, SIZE);
  }
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(m_data_dir + "/themes", error))
  {
    std::string name = entry.path().filename().string();
    const std::string lower = Lower(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0)
      m_wallpapers.push_back(name);
  }
  std::sort(m_wallpapers.begin(), m_wallpapers.end());
  IndexCovers();
  m_cover_thread = std::thread([this] {
    for (;;)
    {
      CoverJob job;
      {
        std::unique_lock lock(m_cover_mutex);
        m_cover_wake.wait(lock, [this] { return m_cover_stop || !m_cover_jobs.empty(); });
        if (m_cover_stop)
          return;
        job = std::move(m_cover_jobs.front());
        m_cover_jobs.pop_front();
      }
      ImageRGBA image;
      LoadPng(job.path, &image, 600, 430);
      std::lock_guard lock(m_cover_mutex);
      if (job.generation == m_cover_generation)  // (else the games were read again meanwhile)
        m_cover_done.push_back({job.game, std::move(image), job.generation});
    }
  });
}

void Menu::Impl::StopCoverThread()
{
  {
    std::lock_guard lock(m_cover_mutex);
    m_cover_stop = true;
  }
  m_cover_wake.notify_all();
  if (m_cover_thread.joinable())
    m_cover_thread.join();
}

// Covers: data_dir/covers/<file name>.png or <title>.png (any case), e.g. from libretro's
// thumbnails ("Named_Boxarts"); another console's in its subfolder (covers/nes/). Indexed by the
// game key's way: "nes/<name>". The USB drives' EmuSuNi/covers too (Covers Saved On), the place new
// covers are saved in last: its picture wins over another place's of the same name.
void Menu::Impl::IndexCovers()
{
  m_cover_files.clear();
  const std::string save = CoversSaveDir(false), internal = m_data_dir + "/covers";
  std::vector<std::string> places;
  for (const std::string& folder : Library::UsbCoversFolders(m_places.drives))
  {
    if (Library::PathKey(folder, m_places.drives) != Library::PathKey(save, m_places.drives))
      places.push_back(folder);
  }
  if (internal != save)
    places.push_back(internal);
  places.push_back(save);
  for (const std::string& place : places)
  {
    for (const System& system : Systems())
    {
      const std::string prefix = system.prefix;
      // (Not a range-for: its ++ throws when a USB drive goes away meanwhile; increment(ec) just stops.)
      std::error_code ec;
      for (std::filesystem::directory_iterator it(place + "/" + prefix, ec), end; !ec && it != end; it.increment(ec))
      {
        const std::string name = it->path().filename().string();
        const std::string lower = Lower(name);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0)
          m_cover_files[prefix + lower.substr(0, lower.size() - 4)] = it->path().string();
      }
    }
  }
  for (auto& [index, cover] : m_covers)
  {
    if (cover.texture != ImTextureID{})
      m_display.DestroyTexture(cover.texture);
  }
  m_covers.clear();
  // The covers asked for or decoded before are for the games' old indexes (RefreshGames reads them again in
  // another order): a new generation, the older one's dropped (UpdateCovers).
  {
    std::lock_guard lock(m_cover_mutex);
    m_cover_jobs.clear();
    m_cover_done.clear();
    m_cover_generation++;
  }
  std::string from;
  for (const std::string& place : places)
    from += (from.empty() ? "" : ", ") + place;
  Log("covers: " + std::to_string(m_cover_files.size()) + " pictures (" + from + "; new ones go to " + save + ")");
  RefreshNoCoverItem();
}

std::string Menu::Impl::CoversSaveDir(bool check)
{
  const std::string internal = m_data_dir + "/covers";
  if (Settings::Get("covers_on") != "usb" || m_usb_covers.empty())
    return internal;
  if (!check)
    return m_usb_covers;
  // The drive may have been taken out since the games were read: then the internal storage.
  MakeDirs(m_usb_covers);
  std::error_code ec;
  if (std::filesystem::is_directory(m_usb_covers, ec))
    return m_usb_covers;
  Log("covers: " + m_usb_covers + " can't be made: the internal storage instead");
  return internal;
}

std::string Menu::Impl::CoversDetail() const
{
  if (Settings::Get("covers_on") != "usb")
    return "Internal Storage";
  return m_usb_covers.empty() ? std::string("USB Drive  -  Not connected: internal storage") : "USB Drive  -  " + m_usb_covers;
}

ImTextureID Menu::Impl::Cover(int game, float* aspect)
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return ImTextureID{};
  CoverEntry& cover = m_covers[game];
  cover.used = ++m_cover_clock;
  if (cover.texture != ImTextureID{})
  {
    if (aspect)
      *aspect = cover.aspect;
    return cover.texture;
  }
  if (!cover.requested && !cover.missing)
  {
    const Library::Game& rom = m_games[game].rom;
    std::string path;
    for (const std::string& name : CoverNames(rom))
    {
      auto it = m_cover_files.find(name);
      if (it != m_cover_files.end())
      {
        path = it->second;
        break;
      }
    }
    if (path.empty())
    {
      cover.missing = true;
    }
    else
    {
      cover.requested = true;
      {
        std::lock_guard lock(m_cover_mutex);
        m_cover_jobs.push_back({game, path, m_cover_generation});
      }
      m_cover_wake.notify_one();
    }
  }
  return ImTextureID{};
}

// The covers the thread decoded become textures (a few per frame); the least recently seen go
// when there are too many.
void Menu::Impl::UpdateCovers()
{
  for (int made = 0; made < 3;)
  {
    CoverDone result;
    {
      std::lock_guard lock(m_cover_mutex);
      if (m_cover_done.empty())
        break;
      result = std::move(m_cover_done.front());
      m_cover_done.pop_front();
      if (result.generation != m_cover_generation)
        continue;  // asked for before the games were read again: its index is another game's now
    }
    made++;
    const auto it = m_covers.find(result.game);
    if (it == m_covers.end() || !it->second.requested)
      continue;  // (not waited for anymore)
    CoverEntry& cover = it->second;
    cover.requested = false;
    if (result.image.empty())
    {
      cover.missing = true;
      continue;
    }
    if (cover.texture != ImTextureID{})
      m_display.DestroyTexture(cover.texture);  // (one texture per cover: the descriptor pool holds 512)
    cover.texture = m_display.CreateTexture(result.image.pixels.data(), result.image.width, result.image.height);
    cover.aspect = static_cast<float>(result.image.width) / result.image.height;
  }
  constexpr size_t MAX_COVERS = 48;
  size_t loaded = 0;
  for (const auto& [index, cover] : m_covers)
    loaded += cover.texture != ImTextureID{};
  while (loaded > MAX_COVERS)
  {
    auto oldest = m_covers.end();
    for (auto it = m_covers.begin(); it != m_covers.end(); ++it)
    {
      if (it->second.texture != ImTextureID{} &&
          (oldest == m_covers.end() || it->second.used < oldest->second.used))
        oldest = it;
    }
    if (oldest == m_covers.end())
      break;
    m_display.DestroyTexture(oldest->second.texture);
    m_covers.erase(oldest);
    loaded--;
  }
}

void Menu::Impl::StartCoverDownload(bool closest)
{
  if (m_dl_thread.joinable())
    return;  // already running
  m_dl_cancel = false;
  m_dl_done = false;
  m_dl_closest = closest;
  if (closest)
    m_nocover_status.clear();
  {
    std::lock_guard lock(m_dl_mutex);
    m_dl_status = "Connecting to libretro...";
  }
  // Only the games Games Without Cover lists: a cover of the user's own named in another case is never
  // written over, and one in the other place (the internal storage or the USB drive: Covers Saved On) is
  // never downloaded again into this one. They count as already there.
  std::vector<Library::Game> games;
  m_dl_skipped = 0;
  for (const Game& game : m_games)
  {
    if (!HasCover(game.rom))
      games.push_back(game.rom);
    else
      m_dl_skipped++;
  }
  m_dl_progress = 0;
  m_dl_total = static_cast<int>(games.size());
  const std::string covers = CoversSaveDir(true);
  m_dl_thread = std::thread([this, games, closest, covers] {
    const CoverDownload::Result result = CoverDownload::Download(
        games, covers, m_assets_dir + "/xmb/cacert.pem",
        [this](int done, int total, const std::string& title) {
          m_dl_progress = title.empty() ? total : done + 1;
          m_dl_total = total;
          std::lock_guard lock(m_dl_mutex);
          m_dl_status = title.empty() ? "Finishing..." :
                                        "Downloading " + std::to_string(done + 1) + " of " +
                                            std::to_string(total) + ": " + title;
        },
        [this](const std::string& id, const std::string& path) {
          std::lock_guard lock(m_dl_mutex);
          m_dl_saved.emplace_back(id, path);
        },
        m_dl_pause, m_dl_cancel, closest);
    std::lock_guard lock(m_dl_mutex);
    m_dl_result = result;
    m_dl_done = true;
  });
}

// A cover picture for the game: covers/<key>.png, <prefix><title>.png or an arcade game's thumbnail name,
// any case (as Cover() looks).
bool Menu::Impl::HasCover(const Library::Game& rom) const
{
  for (const std::string& name : CoverNames(rom))
  {
    if (m_cover_files.count(name) > 0)
      return true;
  }
  return false;
}

// Any game without a cover picture?
bool Menu::Impl::MissingCovers() const
{
  for (const Game& game : m_games)
  {
    if (!HasCover(game.rom))
      return true;
  }
  return false;
}

// The covers written since the last frame (they appear one by one), the download's progress in
// its panel and, at the end, its result.
void Menu::Impl::UpdateCoverDownload()
{
  std::vector<std::pair<std::string, std::string>> saved;
  {
    std::lock_guard lock(m_dl_mutex);
    saved.swap(m_dl_saved);
  }
  for (const auto& [key, path] : saved)
  {
    m_cover_files[Lower(key)] = path;
    for (size_t i = 0; i < m_games.size(); i++)
    {
      if (m_games[i].rom.key != key)
        continue;
      auto it = m_covers.find(static_cast<int>(i));
      if (it != m_covers.end() && it->second.texture == ImTextureID{} && !it->second.requested)
        m_covers.erase(it);  // looked for again next time it is drawn
    }
  }
  if (!saved.empty())
  {
    m_covers_changed = true;  // Without Cover: those games leave the lists (Run)
    RefreshNoCoverItem();
  }
  // Games Without Cover: covered games leave it; the download's progress moves in its two rows.
  const bool listed = m_panel.open && m_panel.setting == SET_NO_COVER;
  if (listed && !saved.empty())
  {
    BuildNoCoverRows();
  }
  else if (listed && m_dl_thread.joinable() && !m_dl_done && m_dl_progress.load() != m_nocover_progress &&
           m_panel.rows.size() > 2 && m_panel.rows[1].key == "__closest__")
  {
    m_nocover_progress = m_dl_progress.load();
    m_panel.rows[1].value = std::to_string(m_nocover_progress) + " / " + std::to_string(m_dl_total.load());
    std::lock_guard lock(m_dl_mutex);
    m_panel.rows[2].label = m_dl_status;
  }
  if (!m_dl_thread.joinable())
    return;
  const bool shown = m_panel.open && m_panel.setting == SET_DOWNLOAD;
  if (!m_dl_done)
  {
    if (shown)
    {
      std::lock_guard lock(m_dl_mutex);
      m_panel.lines = {"", m_dl_status, "", "Circle closes this; the download goes on."};
    }
    return;
  }
  m_dl_thread.join();
  CoverDownload::Result result;
  {
    std::lock_guard lock(m_dl_mutex);
    result = m_dl_result;
  }
  result.already += m_dl_skipped;  // (left out at the start: they had a cover)
  if (shown)
  {
    m_panel.lines = {"", "Downloaded: " + std::to_string(result.downloaded),
                     "Already there: " + std::to_string(result.already),
                     "Not found: " + std::to_string(result.not_found)};
    if (result.failed > 0)
    {
      m_panel.lines.push_back("Failed: " + std::to_string(result.failed));
      m_panel.lines.push_back(result.error);
    }
    m_panel.lines.push_back("");
    if (result.not_found > 0)
      m_panel.lines.push_back("The rest, one by one: Games Without Cover");
    m_panel.lines.push_back("Your own pictures: " + CoversSaveDir(false));
    Play(result.failed > 0 && result.downloaded == 0 ? "error" : "confirm");
  }
  const bool closest = m_dl_closest;
  if (closest)
  {
    m_dl_closest = false;
    m_nocover_status = "Downloaded: " + std::to_string(result.downloaded) + "  -  Not found: " +
                       std::to_string(result.not_found) +
                       (result.failed > 0 ? "  -  Failed: " + std::to_string(result.failed) : std::string());
  }
  // Any download's end: Games Without Cover without its progress.
  if (m_panel.open && m_panel.setting == SET_NO_COVER)
  {
    BuildNoCoverRows();
    if (closest)
      Play(result.failed > 0 && result.downloaded == 0 ? "error" : "confirm");
  }
}

// Square in a game list, or a new game folder: the folders are read again. Each game list keeps
// its selected game.
void Menu::Impl::RefreshGames()
{
  std::string selected_path[CATEGORY_COUNT];
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    const auto& items = m_items[c];
    if (IsGameList(c) && m_selected[c] < static_cast<int>(items.size()) && items[m_selected[c]].game >= 0)
      selected_path[c] = m_games[items[m_selected[c]].game].rom.path;
  }
  BuildGames();
  LoadPlayed();
  IndexCovers();
  BuildItems();
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    if (!IsGameList(c))
      continue;
    m_selected[c] = 0;
    m_item_pos[c] = 0.0f;
    const auto& refreshed = m_items[c];
    for (size_t i = 0; i < refreshed.size(); i++)
    {
      if (refreshed[i].game >= 0 && m_games[refreshed[i].game].rom.path == selected_path[c])
      {
        m_selected[c] = static_cast<int>(i);
        m_item_pos[c] = static_cast<float>(i);
      }
    }
  }
  // Covers for the games of a new folder, in the background.
  if (m_dl_auto && !m_dl_thread.joinable() && MissingCovers())
    StartCoverDownload();
  UpdateShownConsoles();  // (Only Those With Games: a console's first game, or its last one gone)
}

void Menu::Impl::CheckPlaces(bool now)
{
  // A look at the drives' top folders and the Roms folders only (Library::FindPlaces): the games are read
  // again only when they changed.
  if (now)
  {
    m_places_time = m_time;
    m_places_reads.Read();
    PlacesFound(Library::FindPlaces(m_data_dir, Library::UsbDrives()));
    return;
  }
  if (m_places_look.valid() || m_time - m_places_time < 1.5f)
    return;
  m_places_time = m_time;
  try
  {
    m_places_reads.LookStarted();
    m_places_look = std::async(std::launch::async, [data_dir = m_data_dir] {
      return Library::FindPlaces(data_dir, Library::UsbDrives());
    });
  }
  catch (...)
  {
    m_places_reads.Read();
    PlacesFound(Library::FindPlaces(m_data_dir, Library::UsbDrives()));  // (no thread: here)
  }
}

// CheckPlaces' look, when the thread has it (every frame). Not while a game is launching (m_launch is its
// index into m_games) or a game's options are open (they act on its list's selected game): it waits for
// later, the games read again would be in another order.
void Menu::Impl::PlacesLooked()
{
  if (m_launch >= 0 || (m_panel.open && m_panel.title == "Game"))
    return;
  if (!m_places_look.valid() || m_places_look.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  Library::Places places;
  try
  {
    places = m_places_look.get();
  }
  catch (const std::exception& e)
  {
    Log(std::string("game places: the look failed (") + e.what() + "): the games stay as they were");
    return;
  }
  catch (...)
  {
    Log("game places: the look failed: the games stay as they were");
    return;
  }
  // (Taken with get(), never dropped by resetting the future: its release would wait for the thread, on the
  // sleeping drive the look is there to wait for.) The places read again since it started (back from a game,
  // Covers Saved On, Square, a folder chosen, a game deleted or renamed): older than m_places, it is dropped.
  if (!m_places_reads.LookCurrent())
    return;
  PlacesFound(places);
}

void Menu::Impl::PlacesFound(const Library::Places& places)
{
  if (places == m_places)
    return;
  Log(std::string("game places changed: ") + std::to_string(m_places.drives.size()) + " -> " +
      std::to_string(places.drives.size()) + " USB drives: the games are read again");
  const size_t before = m_games.size();
  RefreshGames();
  ShowToast("Game folders changed: games looked for again", 0);
  if (m_games.size() != before)
    Log("games: " + std::to_string(before) + " -> " + std::to_string(m_games.size()));
}

// 00.63: nobody has to make the consoles' folders. Each time the games are read (the start, a drive plugged in,
// Square...) Roms/<key> of every console of the console table is made where it is missing, in the internal storage
// and on the drives a player keeps games on - those with a Roms folder in any letters' case, or /usb0 when none has
// one, never one that looks like the PS4's own storage (Library::GameRomsFolders). Only what is missing is made
// (one look at each Roms folder); a read-only or full drive stops at its first failure and is said once. One line
// in the log per folder: the first time, then only when something is made.
void Menu::Impl::MakeFolders(const std::vector<std::string>& drives)
{
  const std::string found = Library::FindEntry(m_data_dir, "Roms");  // (its own spelling)
  std::vector<std::string> folders = {found.empty() ? m_data_dir + "/Roms" : found};
  for (const std::string& roms : Library::GameRomsFolders(drives))
    folders.push_back(roms);
  for (const std::string& roms : folders)
  {
    const Library::FolderMaking made = Library::MakeConsoleFolders(roms);
    const bool first = m_folders_logged.insert(roms).second;
    if (!first && made.made == 0)
      continue;
    std::string line = "folders: " + roms + ": " + std::to_string(made.made) + " made, " + std::to_string(made.there) +
                       " there";
    if (made.failed > 0)
      line += ", " + std::to_string(made.failed) + " not made (" + made.error + ")";
    Log(line);
  }
}

std::string Menu::Impl::ShownMode() const
{
  const std::string mode = Settings::Get("consoles_shown");
  return mode == "games" || mode == "chosen" ? mode : "all";
}

std::string Menu::Impl::ShownDetail() const
{
  const std::string mode = ShownMode();
  return mode == "games" ? "Only Those With Games" : mode == "chosen" ? "Only the Ones I Choose" : "All Consoles";
}

std::set<std::string> Menu::Impl::OffConsoles() const
{
  std::set<std::string> off;
  std::stringstream in(Settings::Get("consoles_off"));
  std::string key;
  while (std::getline(in, key, ','))
  {
    key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
    if (!key.empty())
      off.insert(Lower(key));
  }
  return off;
}

// The consoles with a tab now (Library::ShownConsoles); when that changes, the tabs and columns are made again.
// A list not shown (no longer, or jumped to by the tour) gives way to All.
void Menu::Impl::UpdateShownConsoles()
{
  const std::vector<SystemId> shown =
      Library::ShownConsoles(ConsoleOrder(), ShownMode(), m_console_games, Settings::Get("consoles_off"));
  const bool changed = shown != s_shown_consoles;
  if (changed)
  {
    s_shown_consoles = shown;
    s_console_order_version++;
  }
  const int system = SystemOfList(m_category);
  const bool hidden = system >= 0 && std::find(shown.begin(), shown.end(), static_cast<SystemId>(system)) == shown.end();
  if (hidden)
  {
    if (m_panel.open && m_panel.title == "Game")
      m_panel.open = false;  // (its options act on the list's selected game)
    m_category = static_cast<int>(Category::Games);
  }
  if (changed || hidden)
  {
    m_category_pos = static_cast<float>(ColumnIndex(m_category));
    m_tab_pos = static_cast<float>(LibraryTab());
  }
}

// Consoles Shown: which consoles get a tab (and an XMB column) - all, those with games, or the ones turned on
// here. Turning one on or off chooses the last.
void Menu::Impl::OpenConsolesShown(int selected)
{
  const bool reopen = m_panel.open && m_panel.setting == SET_CONSOLES_SHOWN;
  const int keep = reopen ? m_panel.selected : -1;
  const float scroll = reopen ? m_panel.scroll : 0.0f;
  const float slide = reopen ? m_panel.slide : 0.0f;  // (made again in place: it doesn't slide in again)
  m_panel = {};
  m_panel.open = true;
  m_panel.slide = slide;
  m_panel.setting = SET_CONSOLES_SHOWN;
  m_panel.title = "Consoles Shown";
  m_panel.rows.push_back({"Show", ShownDetail(), "shown:mode"});
  m_panel.rows.push_back({"Only the Ones I Choose", "", "", true});
  const std::set<std::string> off = OffConsoles();
  for (SystemId id : ConsoleOrder())
  {
    const System& system = GetSystem(id);
    const int games = m_console_games[static_cast<size_t>(id)];
    SettingRow row{std::string(system.name) + "  -  " +
                       (games == 0 ? std::string("No games") : std::to_string(games) + (games == 1 ? " game" : " games")),
                   "", std::string("shown:") + system.key};
    row.tag = off.count(system.key) ? -1 : 1;  // the choice for Only the Ones I Choose
    m_panel.rows.push_back(row);
  }
  m_panel.selected = std::clamp(selected >= 0 ? selected : keep >= 0 ? keep : 0, 0,
                                static_cast<int>(m_panel.rows.size()) - 1);
  m_panel.scroll = scroll;
  MoveSettingRow(0);
}

// Saved Data and Themes: pages of Settings, shown in its list's place (Circle: back to the list,
// on the page's row).
void Menu::Impl::OpenSettingsPage(Category page)
{
  m_panel = {};
  m_category = static_cast<int>(page);
  m_item_pos[m_category] = static_cast<float>(m_selected[m_category]);
}

// Game Folders: each console with its folder; Cross on one opens the folder browser for it.
void Menu::Impl::OpenGameFolders()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_FOLDERS;
  m_panel.title = "Game Folders";
  for (const System& system : Systems())
  {
    m_panel.rows.push_back({system.name, "", "", true});
    m_panel.rows.push_back({GamesFolder(system.id), "", std::string("folder:") + system.key});
  }
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// Console Order: the consoles as their tabs come (after All), then Default Order. Cross picks a
// console up and puts it down; while it is up, Up / Down move it. Left / Right move it at once.
void Menu::Impl::OpenConsoleOrder(int selected)
{
  const bool reopen = m_panel.open && m_panel.setting == SET_CONSOLE_ORDER;
  const int keep = reopen ? m_panel.selected : -1;
  const float scroll = reopen ? m_panel.scroll : 0.0f;
  const float slide = reopen ? m_panel.slide : 0.0f;  // (made again in place: it doesn't slide in again)
  m_panel = {};
  m_panel.open = true;
  m_panel.slide = slide;
  m_panel.setting = SET_CONSOLE_ORDER;
  m_panel.title = "Console Order";
  m_panel.rows.push_back({"Cross: pick up / put down. Up / Down: move it.", "", "", true});
  const auto& order = ConsoleOrder();
  for (size_t i = 0; i < order.size(); i++)
  {
    const System& system = GetSystem(order[i]);
    SettingRow row{std::to_string(i + 1) + ".  " + system.name, "", std::string("order:") + system.key};
    m_panel.rows.push_back(row);
  }
  // The ready-made orders (the one in use says so); the consoles above can still be moved by hand.
  m_panel.rows.push_back({"Ready-Made Orders", "", "", true});
  static const std::pair<const char*, const char*> PRESETS[] = {{"company", "By Company (Default)"},
                                                                {"power", "By Power"},
                                                                {"year", "By Year"},
                                                                {"alpha", "Alphabetical"}};
  for (const auto& [preset, label] : PRESETS)
  {
    SettingRow row{label, PresetOrder(preset) == order ? "In Use" : "", std::string("order:preset:") + preset};
    m_panel.rows.push_back(row);
  }
  m_panel.selected = selected >= 0 ? selected : keep >= 0 ? keep : 1;
  m_panel.scroll = scroll;
  if (m_order_moving && m_panel.selected >= 1 && m_panel.selected <= static_cast<int>(order.size()))
    m_panel.rows[m_panel.selected].value = "Moving";
  else
    m_order_moving = false;
}

// The console in row `from` (1 = the first) goes to row `to`; the tabs, the XMB's columns and
// xmb.ini follow.
void Menu::Impl::MoveConsole(int from, int to)
{
  std::vector<SystemId> order = ConsoleOrder();
  const int count = static_cast<int>(order.size());
  if (from < 1 || from > count || to < 1 || to > count || from == to)
  {
    Play("error");
    return;
  }
  const SystemId moved = order[from - 1];
  order.erase(order.begin() + (from - 1));
  order.insert(order.begin() + (to - 1), moved);
  SetConsoleOrder(order);
  m_prefs.console_order = ConsoleOrderKeys();
  m_prefs.Save(m_data_dir + "/xmb.ini");
  m_category_pos = static_cast<float>(ColumnIndex(m_category));
  m_tab_pos = static_cast<float>(LibraryTab());
  BuildItems();
  Play("move");
  OpenConsoleOrder(to);
}

// Console Order's buttons; false when the menu's usual handling should go on (Circle closes).
bool Menu::Impl::ConsoleOrderInput(uint32_t pressed)
{
  const int count = static_cast<int>(ConsoleOrder().size());
  const int row = m_panel.selected;
  const bool console = row >= 1 && row <= count;
  if (pressed & BTN_CROSS)
  {
    if (m_panel.rows[row].key.rfind("order:preset:", 0) == 0)
    {
      const std::string preset = m_panel.rows[row].key.substr(13);
      m_order_moving = false;
      SetConsoleOrder(PresetOrder(preset));
      m_prefs.console_order = preset == "company" ? std::string() : ConsoleOrderKeys();
      m_prefs.Save(m_data_dir + "/xmb.ini");
      m_category_pos = static_cast<float>(ColumnIndex(m_category));
      m_tab_pos = static_cast<float>(LibraryTab());
      BuildItems();
      Play("category");
      OpenConsoleOrder(row);
    }
    else if (console)
    {
      m_order_moving = !m_order_moving;
      Play(m_order_moving ? "category" : "back");
      OpenConsoleOrder(row);
    }
    return true;
  }
  if ((pressed & BTN_CIRCLE) && m_order_moving)
  {
    m_order_moving = false;
    Play("back");
    OpenConsoleOrder(row);
    return true;
  }
  if (console && (pressed & (BTN_LEFT | BTN_RIGHT)))
  {
    MoveConsole(row, row + ((pressed & BTN_LEFT) ? -1 : 1));
    return true;
  }
  if (m_order_moving && (pressed & (BTN_UP | BTN_DOWN)))
  {
    MoveConsole(row, row + ((pressed & BTN_UP) ? -1 : 1));
    return true;
  }
  return false;
}

// A console's game folder (m_browse_system): a folder browser. "" shows the places games can be
// (the console's storage and USB drives); a folder shows "Use This Folder" (with how many of the
// console's games it holds), ".." and its subfolders.
void Menu::Impl::OpenFolderBrowser(const std::string& path)
{
  const bool reopen = m_panel.open && m_panel.setting == SET_GAMES_DIR;
  const System& system = GetSystem(m_browse_system);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_GAMES_DIR;
  m_panel.title = std::string("Game Folder: ") + system.name;
  m_browse_dir = path;
  std::vector<SettingRow> rows;
  std::error_code ec;
  if (path.empty())
  {
    rows.push_back({"Places", "", "", true});
    rows.push_back({"/", "Console root", "dir:/"});
    rows.push_back({"/mnt", "Drives", "dir:/mnt"});
    std::vector<std::string> places = {"/data"};
    for (const System& each : Systems())
      places.push_back(DefaultFolder(each));
    // The USB drives: /usb0... as the app sees them (/mnt/usb0... outside its sandbox).
    for (int i = 0; i < 8; i++)
      places.push_back("/usb" + std::to_string(i));
    for (int i = 0; i < 8; i++)
      places.push_back("/mnt/usb" + std::to_string(i));
    places.push_back("/mnt/ext0");
    places.push_back("/mnt/ext1");
    for (const std::string& place : places)
    {
      std::error_code place_error;
      if (std::filesystem::is_directory(place, place_error))
        rows.push_back({place, "", "dir:" + place});
    }
  }
  else
  {
    rows.push_back({path, "", "", true});
    std::vector<std::string> folders;
    bool readable = false;
    // (Not a range-for: its ++ throws when the drive goes away meanwhile; increment(ec) just stops.)
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
    {
      readable = true;
      const std::string name = it->path().filename().string();
      std::error_code type_error;
      if (it->is_directory(type_error) && !name.empty() && name[0] != '.' &&
          name != "System Volume Information" && name != "$RECYCLE.BIN")
        folders.push_back(name);
    }
    std::sort(folders.begin(), folders.end());
    const int games = Library::CountGames(path, m_browse_system);
    rows.push_back({"Use This Folder", std::to_string(games) + (games == 1 ? " game" : " games"),
                    "usedir"});
    std::string parent = std::filesystem::path(path).parent_path().string();
    if (parent.empty())
      parent = "/";
    const bool top = path == "/";
    rows.push_back({"..", top ? "Places" : parent, top ? "dir:" : "dir:" + parent});
    if (!readable && ec)
      rows.push_back({"(this folder can't be opened: " + ec.message() + ")", "", "", true});
    for (const std::string& folder : folders)
    {
      const std::string full = path == "/" ? "/" + folder : path + "/" + folder;
      rows.push_back({folder + "/", "", "dir:" + full});
    }
  }
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  MoveSettingRow(0);
  if (!reopen)
    Play("menu");
}

ImTextureID Menu::Impl::LoadPngTexture(const std::string& path, float* aspect, uint32_t max_w,
                                       uint32_t max_h)
{
  ImageRGBA image;
  if (!LoadPng(path, &image, max_w, max_h))
    return ImTextureID{};
  if (aspect)
    *aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
  return m_display.CreateTexture(image.pixels.data(), image.width, image.height);
}

const Theme& Menu::Impl::CurrentTheme() const
{
  for (const Theme& theme : THEMES)
  {
    if (m_prefs.theme == theme.name)
      return theme;
  }
  return THEMES[0];
}

void Menu::Impl::ApplyPreferences()
{
  m_audio.SetEnabled(m_prefs.sounds);
  m_audio.SetVolume(m_prefs.volume);
  if (m_wallpaper != ImTextureID{})
    m_display.DestroyTexture(m_wallpaper);
  m_wallpaper = ImTextureID{};
  if (!m_prefs.wallpaper.empty())
    m_wallpaper = LoadPngTexture(m_data_dir + "/themes/" + m_prefs.wallpaper, nullptr, 1920, 1080);
  if (m_prefs.console_order != ConsoleOrderKeys())
    SetConsoleOrder(SavedConsoleOrder(m_prefs.console_order));
  m_prefs.Save(m_data_dir + "/xmb.ini");
}

void Menu::Impl::BuildItems()
{
  for (auto& items : m_items)
    items.clear();

  auto& settings = m_items[static_cast<int>(Category::Settings)];
  // Saved Data and Themes: pages of Settings (they were tabs).
  {
    const size_t saved = m_saves.size();
    settings.push_back({"Saved Data",
                        saved == 0   ? std::string("No saved data yet") :
                        saved == 1   ? std::string("Saves of 1 game") :
                                       "Saves of " + std::to_string(saved) + " games",
                        -1, -1, SET_SAVED_DATA});
    settings.push_back({"Themes",
                        m_prefs.wallpaper.empty() ? m_prefs.theme :
                                                    std::filesystem::path(m_prefs.wallpaper).stem().string(),
                        -1, -1, SET_THEMES});
  }
  settings.push_back({"Language", PS4Lang::Current().name, -1, -1, SET_LANGUAGE});
  // Each console's buttons (its normal layout); a game's own from the game: Triangle -> Controls, or the in-game
  // menu's Controls.
  settings.push_back({"Controls", "Each console's buttons on the DualShock 4", -1, -1, SET_CONTROLS});
  // The emulation settings (Video, Graphics, Performance, Emulation, System) are set from a game
  // (Triangle -> Game Settings: for that game, its console or all games), not here.
  settings.push_back({"Game Folders", "A folder for each console's games", -1, -1, SET_FOLDERS});
  {
    std::string order;
    for (SystemId id : ConsoleOrder())
      order += std::string(order.empty() ? "" : ", ") + GetSystem(id).tab;
    settings.push_back({"Console Order", order, -1, -1, SET_CONSOLE_ORDER});
  }
  settings.push_back({"Consoles Shown", ShownDetail(), -1, -1, SET_CONSOLES_SHOWN});
  settings.push_back({"Clean Up Games", "Duplicates and other regions", -1, -1, SET_CLEANUP});
  settings.push_back({"Download Covers", "Box art from libretro for your games", -1, -1,
                      SET_DOWNLOAD});
  settings.push_back({"Games Without Cover", NoCoverDetail(), -1, -1, SET_NO_COVER});
  settings.push_back({"Covers Saved On", CoversDetail(), -1, -1, SET_COVERS});
  settings.push_back({"Sound Effects", m_prefs.sounds ? "On" : "Off", -1, -1, SET_SOUNDS});
  settings.push_back({"Volume", std::to_string(m_prefs.volume), -1, -1, SET_VOLUME});
  settings.push_back({"Menu Style", Library() ? "Library" : "XMB", -1, -1, SET_STYLE});
  settings.push_back({"Clock", m_prefs.clock ? "On" : "Off", -1, -1, SET_CLOCK});
  settings.push_back({"Reset All Settings", "Every setting back to the defaults", -1, -1,
                      SET_RESET_ALL});
#ifdef EMUSUNI_SATURN
  // The Saturn's speed test: only with the file /data/EmuSuNi/saturn-test (src/SaturnTest.h).
  if (SaturnTest::Enabled())
    settings.push_back({"Saturn (speed test)", "Measures the Saturn emulator on this PS4 (no picture)", -1, -1,
                        SET_SATURN_TEST});
#endif
  settings.push_back({"About", "", -1, -1, SET_ABOUT});

  auto& games = m_items[static_cast<int>(Category::Games)];
  std::vector<int> order(m_games.size());
  for (size_t i = 0; i < order.size(); i++)
    order[i] = static_cast<int>(i);
  if (m_prefs.sort == "recent")
  {
    std::stable_sort(order.begin(), order.end(),
                     [this](int a, int b) { return m_games[a].played > m_games[b].played; });
  }
  else if (m_prefs.sort == "region")
  {
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
      return m_games[a].rom.region < m_games[b].rom.region;
    });
  }
  else if (m_prefs.sort == "console")
  {
    // Each console's games together (in the console order of the tabs), by title in each.
    std::array<int, SYSTEM_COUNT> rank{};
    for (size_t i = 0; i < ConsoleOrder().size(); i++)
      rank[static_cast<size_t>(ConsoleOrder()[i])] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [this, &rank](int a, int b) {
      return rank[static_cast<size_t>(m_games[a].rom.system)] < rank[static_cast<size_t>(m_games[b].rom.system)];
    });
  }
  // The search: every word typed is in the game's name (or its file's), in any order.
  std::vector<std::string> words;
  {
    const std::string query = SearchText(m_search);
    for (size_t at = 0; at < query.size();)
    {
      size_t end = query.find(' ', at);
      if (end == std::string::npos)
        end = query.size();
      if (end > at)
        words.push_back(query.substr(at, end - at));
      at = end + 1;
    }
  }
  auto& favorites = m_items[static_cast<int>(Category::Favorites)];
  // Without Cover (Options): only the games without a cover picture, in m_games' order (A-Z).
  // covered[c]: list c lost a game (found by the search) for its cover.
  const bool nocover = m_prefs.sort == "nocover";
  bool covered[CATEGORY_COUNT] = {};
  for (const int i : order)
  {
    const Game& game = m_games[i];
    if (!words.empty())
    {
      const std::string name = SearchText(game.rom.title) + " " + SearchText(game.rom.id);
      bool match = true;
      for (const std::string& word : words)
        match = match && name.find(word) != std::string::npos;
      if (!match)
        continue;
    }
    if (nocover && HasCover(game.rom))
    {
      covered[static_cast<int>(Category::Games)] = covered[ListOfSystem(game.rom.system)] = true;
      if (m_favorites.count(game.rom.key))
        covered[static_cast<int>(Category::Favorites)] = true;
      continue;
    }
    std::string detail = game.rom.region;
    if (!game.rom.tags.empty())
      detail += (detail.empty() ? "" : "  -  ") + game.rom.tags;
    if (!m_prefs.default_game.empty() && game.rom.path == m_prefs.default_game)
      detail += "  -  Starts when EmuSuNi opens";
    const Item item{game.rom.title, detail, i, -1, -1};
    games.push_back(item);
    m_items[ListOfSystem(game.rom.system)].push_back(item);
    if (m_favorites.count(game.rom.key))
      favorites.push_back(item);
  }
  // A list Without Cover emptied: its games all have covers (before the other empty texts).
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    m_all_covered[c] = covered[c] && m_items[c].empty();
    if (m_all_covered[c])
      m_items[c].push_back({m_search.empty() ? "Every game has a cover" : "Every game found has a cover",
                            "Options shows every game again"});
  }
  if (games.empty() && !m_search.empty())
    games.push_back({"No games match \"" + m_search + "\"", "Circle clears the search"});
  else if (games.empty())
    games.push_back({"No games found", "Copy your games to their console's folder (Settings: Game Folders)"});
  for (const System& system : Systems())
  {
    auto& list = m_items[ListOfSystem(system.id)];
    if (list.empty() && !m_search.empty())
      list.push_back({"No games match \"" + m_search + "\"", "Circle clears the search"});
    else if (list.empty())
      list.push_back({"No games found", "Copy games to Roms/" + std::string(system.key) + ", then press Square"});
  }
  if (favorites.empty() && !m_search.empty() && !m_favorites.empty())
    favorites.push_back({"No favorites match \"" + m_search + "\"", "Circle clears the search"});
  else if (favorites.empty())
    favorites.push_back({"No favorites yet", "L2 on a game adds it here"});

  auto& saves = m_items[static_cast<int>(Category::SavedData)];
  for (size_t i = 0; i < m_saves.size(); i++)
  {
    const Saves::GameSaves& save = m_saves[i];
    std::string detail = SystemOfKey(save.id).tab;
    if (save.sram_time > 0)
      detail += "  -  Battery save";
    int states = 0;
    for (int slot : save.slots)
      states += slot > 0;
    if (states > 0)
      detail += (detail.empty() ? "" : "  -  ") + std::to_string(states) +
                (states == 1 ? " state" : " states");
    if (std::find(save.slots.begin(), save.slots.end(), 0) != save.slots.end())
      detail += (detail.empty() ? "" : "  -  ") + std::string("Resume state");
    Item item{SaveTitle(save.id), detail};
    item.save = static_cast<int>(i);
    saves.push_back(item);
  }
  if (saves.empty())
    saves.push_back({"No saved data yet", "Saves and save states appear here"});

  auto& themes = m_items[static_cast<int>(Category::Themes)];
  for (size_t i = 0; i < std::size(THEMES); i++)
  {
    const bool active = m_prefs.wallpaper.empty() && m_prefs.theme == THEMES[i].name;
    themes.push_back({THEMES[i].name, active ? "In use" : "Color theme", -1, static_cast<int>(i)});
  }
  for (size_t i = 0; i < m_wallpapers.size(); i++)
  {
    const bool active = m_prefs.wallpaper == m_wallpapers[i];
    themes.push_back({std::filesystem::path(m_wallpapers[i]).stem().string(),
                      active ? "In use" : "Wallpaper", -1, -2 - static_cast<int>(i)});
  }
  for (int c = 0; c < CATEGORY_COUNT; c++)
    m_selected[c] = std::clamp(m_selected[c], 0, std::max(0, static_cast<int>(m_items[c].size()) - 1));
}

void Menu::Impl::SetLanguage(const std::string& code)
{
  Settings::Set("language", code);
  PS4Lang::Load(m_assets_dir, code);
  Log("language: " + code);
  BuildItems();
}

// ---------------------------------------------------------------------------------------------
// Saved data.

void Menu::Impl::LoadSaves()
{
  m_saves = Saves::List();
}

std::string Menu::Impl::SaveTitle(const std::string& id) const
{
  for (const Game& game : m_games)
  {
    if (game.rom.key == id)
      return game.rom.title;
  }
  std::string title, region, tags;
  Library::ParseName(id.substr(std::string(SystemOfKey(id).prefix).size()), &title, &region, &tags);
  return title;
}

ImTextureID Menu::Impl::StatePicture(const std::string& id, int slot)
{
  if (m_state_pictures_game != id)
  {
    ForgetStatePictures();
    m_state_pictures_game = id;
  }
  const std::string key = id + "/" + std::to_string(slot);
  auto it = m_state_pictures.find(key);
  if (it != m_state_pictures.end())
    return it->second;
  const ImTextureID texture = LoadPngTexture(Saves::StatePicturePath(id, slot), nullptr, 256, 224);
  m_state_pictures[key] = texture;
  return texture;
}

void Menu::Impl::ForgetStatePictures()
{
  for (auto& [key, texture] : m_state_pictures)
  {
    if (texture != ImTextureID{})
      m_display.DestroyTexture(texture);
  }
  m_state_pictures.clear();
  m_state_pictures_game.clear();
}

void Menu::Impl::OpenSavesPanel(int index)
{
  if (index < 0 || index >= static_cast<int>(m_saves.size()))
    return;
  const Saves::GameSaves& save = m_saves[index];
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_SAVES;
  m_panel.title = SaveTitle(save.id);
  m_saves_panel = index;
  std::vector<SettingRow> rows;
  if (save.sram_time > 0)
  {
    rows.push_back({"Battery Save", "", "", true});
    rows.push_back({DateText(save.sram_time), std::to_string(save.sram_size / 1024) + " KB",
                    "del:sram"});
  }
  if (!save.slots.empty())
    rows.push_back({"Save States", "", "", true});
  for (int slot : save.slots)
  {
    SettingRow row{slot == 0 ? std::string("Resume State") : "Slot " + std::to_string(slot),
                   DateText(Saves::Modified(Saves::StatePath(save.id, slot))),
                   "del:slot:" + std::to_string(slot)};
    row.icon = StatePicture(save.id, slot);
    rows.push_back(std::move(row));
  }
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// ---------------------------------------------------------------------------------------------
// Panels.

void Menu::Impl::OpenSettingPanel(int setting)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = setting;
  switch (setting)
  {
  case SET_SAVED_DATA:
    OpenSettingsPage(Category::SavedData);
    return;
  case SET_THEMES:
    OpenSettingsPage(Category::Themes);
    return;
  case SET_SOUNDS:
    m_panel.title = "Sound Effects";
    m_panel.options = {"On", "Off"};
    m_panel.selected = m_prefs.sounds ? 0 : 1;
    break;
  case SET_VOLUME:
    m_panel.title = "Volume";
    for (int v = 100; v >= 0; v -= 10)
      m_panel.options.push_back(std::to_string(v));
    m_panel.selected = (100 - m_prefs.volume) / 10;
    break;
  case SET_LANGUAGE:
  {
    m_panel.title = "Language";
    const auto& languages = PS4Lang::All();
    for (size_t i = 0; i < languages.size(); i++)
    {
      m_panel.options.push_back(languages[i].name);
      if (std::string(languages[i].code) == PS4Lang::Current().code)
        m_panel.selected = static_cast<int>(i);
    }
    break;
  }
  case SET_STYLE:
    m_panel.title = "Menu Style";
    m_panel.options = {"Library", "XMB"};
    m_panel.selected = Library() ? 0 : 1;
    m_panel.lines = {"", "Library: your covers in a row,", "tabs switched with L1 / R1.", "",
                     "XMB: the PSP-style columns."};
    break;
  case SET_CLOCK:
    m_panel.title = "Clock";
    m_panel.options = {"On", "Off"};
    m_panel.selected = m_prefs.clock ? 0 : 1;
    break;
  case SET_CONTROLS:
    OpenControlsMenu();
    return;
  case SET_FOLDERS:
    OpenGameFolders();
    return;
  case SET_CONSOLE_ORDER:
    m_order_moving = false;
    OpenConsoleOrder();
    return;
  case SET_COVERS:
  {
    // Where new covers go: the internal storage (as before 00.62) or a USB drive's EmuSuNi/covers; both
    // places' covers are shown. (The drives as they are now.)
    CheckPlaces(true);
    m_panel.title = "Covers Saved On";
    m_panel.options = {"Internal Storage", "USB Drive"};
    m_panel.selected = Settings::Get("covers_on") == "usb" ? 1 : 0;
    m_panel.lines = {"",
                     "Internal: " + m_data_dir + "/covers",
                     m_usb_covers.empty() ? std::string("USB: no drive connected") : "USB: " + m_usb_covers,
                     "",
                     "New covers go there (downloaded or chosen);",
                     "the ones in both places are shown.",
                     "",
                     "Your own: a PNG per game, named like",
                     "its file or its title, in a folder per",
                     "console (covers/nes, covers/psp...;",
                     "the Super Nintendo's right in covers).",
                     "Square in the Library reads them again."};
    break;
  }
  case SET_CONSOLES_SHOWN:
    OpenConsolesShown();
    return;
  case SET_DOWNLOAD:
    m_panel.title = "Download Covers";
    StartCoverDownload();
    UpdateCoverDownload();
    break;
  case SET_CLEANUP:
    OpenCleanUp();
    return;
  case SET_NO_COVER:
    OpenNoCover(0);
    return;
  case SET_RESET_ALL:
    m_panel.title = "Reset All Settings";
    m_panel.options = {"Cancel", "Reset everything"};
    m_panel.lines = {"", "All games, global and per game,", "go back to the default settings."};
    break;
  case SET_ABOUT:
    m_panel.title = "About";
    m_panel.lines = {"EmuSuNi", "", "Created by Arturo Fuentes", std::string("Version ") + App::VERSION};
    break;
#ifdef EMUSUNI_SATURN
  case SET_SATURN_TEST:
    // No panel: Run hands its stand-in to main.cpp, which runs the test (src/SaturnTest.cpp) instead of a game.
    m_panel.open = false;
    m_saturn_test = true;
    Log("saturn test: chosen in Settings");
    return;
#endif
  default:
    if (setting >= SET_GROUP && setting < SET_GROUP + static_cast<int>(Settings::Categories().size()))
    {
      OpenSettingsList(Settings::Categories()[setting - SET_GROUP], {});
      return;
    }
    m_panel.open = false;
  }
}

// A settings list: one category of the global settings, or every setting of one game (with
// "Apply To" first: all games = the game follows the global settings, this game = its own); a
// game's list has its console's settings only.
void Menu::Impl::OpenSettingsList(const std::string& category, const std::string& game_id, int system)
{
  Settings::UseConsoleByDefault(game_id);  // (a game's page: This Console unless chosen otherwise)
  m_panel = {};
  m_panel.open = true;
  m_panel.category = category;
  m_panel.game_id = game_id;
  m_panel.system = system;
  m_panel.title = category.empty() ? "Game Settings" : category;
  BuildSettingRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildSettingRows()
{
  std::vector<SettingRow> rows;
  const std::string& game = m_panel.game_id;
  if (!game.empty())
  {
    const Settings::Scope scope = Settings::ScopeOf(game);
    rows.push_back({"Apply To",
                    scope == Settings::Scope::Game      ? "This Game" :
                    scope == Settings::Scope::Console   ? "This Console" :
                                                          "All Games",
                    "", false, true});
    if (scope == Settings::Scope::Game)
      rows.push_back({"Use for All Games", "", "__toall__"});
  }
  // One fixed quality level, Standard, for Flycast's consoles (Emulator.cpp BuildDcVariables); the 3DS at its own
  // resolution only, 1x (Build3dsVariables: no Quality levels since 00.64).
  if (m_panel.system >= 0 && IsFlycast(static_cast<SystemId>(m_panel.system)))
    rows.push_back({std::string(GetSystem(static_cast<SystemId>(m_panel.system)).name) + ": Standard quality only", "",
                    "", true});
  if (m_panel.system == static_cast<int>(SystemId::THREEDS))
  {
    rows.push_back({"Nintendo 3DS: experimental", "", "", true});
    rows.push_back({"3DS: the console's own resolution only", "", "", true});
  }
  for (const char* category : Settings::Categories())
  {
    if (!m_panel.category.empty() && m_panel.category != category)
      continue;
    bool header = !m_panel.category.empty();  // one category: no header
    for (const Settings::Setting& setting : Settings::All())
    {
      if (std::string(setting.category) != category)
        continue;
      if (!game.empty() && (std::string(setting.key) == "language" || std::string(setting.key) == "menu_hotkey"))
        continue;  // the whole app's, not a game's
      if (m_panel.system >= 0 ? !Settings::AppliesTo(setting, static_cast<SystemId>(m_panel.system)) :
                                !Settings::AppliesToAny(setting))
        continue;
      if (!header)
        rows.push_back({category, "", "", true});
      header = true;
      // In the lists of every console, a setting of one console says which: "SuperFX Clock (SNES)".
      std::string label = setting.label;
      const std::string tag = m_panel.system < 0 ? Settings::SystemTag(setting) : std::string();
      if (!tag.empty())
        label = PS4Lang::T(label) + tag;
      rows.push_back({label, Settings::LabelOf(setting, Settings::Get(setting.key, game)), setting.key, false,
                      false, !setting.live});
    }
  }
  rows.push_back({"", "", "", true});
  rows.push_back({game.empty()                                                 ? "Reset to Defaults" :
                  Settings::ScopeOf(game) == Settings::Scope::Console ? "Reset This Console" :
                                                                        "Reset This Game",
                  "", "__reset__"});
  m_panel.rows = std::move(rows);
}

// Moves the selection by direction (0: onto the nearest selectable row), skipping headers.
void Menu::Impl::MoveSettingRow(int direction)
{
  const int count = static_cast<int>(m_panel.rows.size());
  int row = std::clamp(m_panel.selected + direction, 0, std::max(0, count - 1));
  const int step = direction < 0 ? -1 : 1;
  while (row >= 0 && row < count && m_panel.rows[row].header)
    row += step;
  if (row >= 0 && row < count)
    m_panel.selected = row;
}

void Menu::Impl::ChangeSettingRow(int direction)
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.rows.size()))
    return;
  const SettingRow row = m_panel.rows[m_panel.selected];
  const std::string game = m_panel.game_id;
  if (m_panel.setting == SET_CLEANUP)
  {
    if (direction <= 0 || (row.key != "__dups__" && row.key.rfind("__usaonly__:", 0) != 0))
      return;
    const bool dups = row.key == "__dups__";
    const System* system = dups ? nullptr : SystemByKey(row.key.substr(12));
    m_pending_delete.clear();
    if (dups)
      m_pending_delete = m_cleanup_dups;
    else if (system)
      m_pending_delete = m_cleanup_others[static_cast<int>(system->id)];
    if (m_pending_delete.empty())
    {
      Play("error");
      return;
    }
    const size_t count = m_pending_delete.size();
    m_panel = {};
    m_panel.open = true;
    m_panel.setting = SET_CLEANUP_CONFIRM;
    m_panel.title = dups ? std::string("Delete Duplicates") : std::string("Keep Only USA Games: ") + system->tab;
    m_panel.options = {"Cancel", "Delete"};
    m_panel.lines = {"", std::to_string(count) + (count == 1 ? " file is" : " files are") + " deleted from the drive.", "",
                     dups ? std::string("One version of each game stays (the USA one when there is one).") :
                            std::string("Only ") + system->name + " games. Games without a region in their name stay.",
                     "", "Their saves stay (Saved Data)."};
    Play("menu");
    return;
  }
  if (m_panel.setting == SET_SAVES)
  {
    if (direction <= 0 || row.key.rfind("del:", 0) != 0)
      return;
    // "Delete?" first.
    m_delete_key = row.key.substr(4);
    const int saves = m_saves_panel;
    m_panel = {};
    m_panel.open = true;
    m_panel.setting = SET_DELETE;
    m_panel.title = "Delete";
    m_panel.lines = {"", SaveTitle(m_saves[saves].id), "",
                     m_delete_key == "sram" ? "Delete the battery save?" :
                                              "Delete this save state?",
                     "", "It can't be brought back."};
    m_panel.options = {"Cancel", "Delete"};
    m_saves_panel = saves;
    Play("menu");
    return;
  }
  if (m_panel.setting == SET_FIND_COVER)
  {
    if (direction <= 0 && row.key != "find:kind")
      return;
    if (row.key == "find:query")
    {
      AskText("Look For", m_find_query, [this](const std::string& text) {
        if (!m_panel.open || m_panel.setting != SET_FIND_COVER)
          return;  // the cover was saved meanwhile: back in Games Without Cover
        if (!text.empty())
          m_find_query = text;
        StartFind();
      });
    }
    else if (row.key == "find:kind")
    {
      m_find_titles = !m_find_titles;
      StartFind();
    }
    else if (row.key.rfind("pick:", 0) == 0)
    {
      // The picture as the game's cover (covers/<key>.png): over its cover of that name where it is (the
      // internal storage or a USB drive), else where new covers go (Covers Saved On).
      const auto old = m_cover_files.find(Lower(m_find_key));
      const std::string target =
          old != m_cover_files.end() ? old->second : CoversSaveDir(true) + "/" + m_find_key + ".png";
      {
        std::lock_guard lock(m_find_mutex);
        m_find_jobs.push_back({2, m_find_system, m_find_titles, m_find_key, row.label, target});
      }
      m_find_wake.notify_one();
      ShowToast("Downloading the picture...", 0);
    }
    return;
  }
  if (m_panel.setting == SET_NO_COVER)
  {
    // Always returns: a header falling through would reach BuildSettingRows() below.
    if (direction <= 0)
      return;
    if (row.key == "__closest__")
    {
      if (m_dl_thread.joinable())
      {
        ShowToast("A download is under way", 0);  // its progress shows in this list
        Play("error");
        return;
      }
      StartCoverDownload(true);
      BuildNoCoverRows();
    }
    else if (row.key.rfind("nocover:", 0) == 0)
    {
      const std::string key = row.key.substr(8);
      for (size_t i = 0; i < m_games.size(); i++)
      {
        if (m_games[i].rom.key == key)
        {
          m_find_return_row = m_panel.selected;
          OpenFindCover(static_cast<int>(i), true);
          break;
        }
      }
    }
    return;
  }
  if (m_panel.setting == SET_CONSOLES_SHOWN)
  {
    // (Global values of settings.ini, as the language; the tour's "set consoles_shown games" shows them too.)
    if (row.key == "shown:mode")
    {
      static const char* const MODES[] = {"all", "games", "chosen"};
      int at = 0;
      while (at < 3 && ShownMode() != MODES[at])
        at++;
      Settings::Set("consoles_shown", MODES[(at + (direction < 0 ? 2 : 1)) % 3]);
    }
    else if (const System* system = row.key.rfind("shown:", 0) == 0 ? SystemByKey(row.key.substr(6)) : nullptr)
    {
      // On or off, and Only the Ones I Choose is what is shown then.
      std::set<std::string> off = OffConsoles();
      if (!off.erase(system->key))
        off.insert(system->key);
      std::string keys;
      for (const std::string& key : off)
        keys += (keys.empty() ? "" : ",") + key;
      Settings::Set("consoles_off", keys);
      Settings::Set("consoles_shown", "chosen");
    }
    else
    {
      return;
    }
    UpdateShownConsoles();
    BuildItems();
    OpenConsolesShown();
    return;
  }
  if (m_panel.setting == SET_FOLDERS)
  {
    const System* system = row.key.rfind("folder:", 0) == 0 ? SystemByKey(row.key.substr(7)) : nullptr;
    if (direction > 0 && system)
    {
      m_browse_system = system->id;
      OpenFolderBrowser(GamesFolder(system->id));
    }
    return;
  }
  if (m_panel.setting == SET_CONTROLS)
  {
    const System* system = row.key.rfind("ctlsys:", 0) == 0 ? SystemByKey(row.key.substr(7)) : nullptr;
    if (direction > 0 && system)
      OpenControlsList({}, system->id);
    return;
  }
  if (m_panel.setting == SET_GAMES_DIR)
  {
    if (direction <= 0)
      return;
    if (row.key.rfind("dir:", 0) == 0)
    {
      OpenFolderBrowser(row.key.substr(4));
    }
    else if (row.key == "usedir")
    {
      const System& system = GetSystem(m_browse_system);
      m_prefs.games_dirs[system.key] =
          m_browse_dir == DefaultFolder(system) ? "" : m_browse_dir;
      m_prefs.Save(m_data_dir + "/xmb.ini");
      m_panel.open = false;
      RefreshGames();
      Play("confirm");
    }
    return;
  }
  if (m_controls_open)
  {
    const SystemId system = m_controls_system;
    if (row.key == "__ctlscope__")
    {
      if (Controls::HasGameProfile(m_controls_game))
        Controls::DeleteGameProfile(m_controls_game);
      else
        Controls::CreateGameProfile(m_controls_game, system);
      BuildControlRows();
      return;
    }
    if (row.key == "__ctlreset__")
    {
      if (direction > 0)
        Controls::ResetToDefault(Controls::HasGameProfile(m_controls_game) ? m_controls_game : "", system);
      BuildControlRows();
      return;
    }
    if (row.key.rfind("ctl:", 0) == 0)
    {
      const std::string edit = Controls::HasGameProfile(m_controls_game) ? m_controls_game : "";
      const auto& binding = Controls::Bindings(system)[std::atoi(row.key.c_str() + 4)];
      if (binding.stick_group)
      {
        const auto choices = Controls::StickChoices();
        const std::string current = Controls::Get(binding, edit, system);
        int index = 0;
        for (size_t i = 0; i < choices.size(); i++)
        {
          if (choices[i] == current)
            index = static_cast<int>(i);
        }
        const int count = static_cast<int>(choices.size());
        index = ((index + direction) % count + count) % count;
        Controls::Set(binding, choices[index], edit, system);
        BuildControlRows();
      }
      else if (direction > 0)
      {
        // Buttons: wait for the next press (UpdateCapture).
        m_capture = true;
        m_capture_released = false;
        m_capture_time = 0.0f;
        m_capture_row = m_panel.selected;
      }
      else
      {
        Controls::Set(binding, "None", edit, system);
        BuildControlRows();
      }
    }
    return;
  }
  if (row.key.rfind("cheat:", 0) == 0)
  {
    m_cheats.Toggle(static_cast<size_t>(std::atoi(row.key.c_str() + 6)));
    BuildCheatRows();
    return;
  }
  if (row.key == "cheatfind")
  {
    if (direction <= 0)
      return;
    // libretro's lists for this game (the collection downloaded first when it isn't here).
    m_cheat_page = CheatPage::Search;
    m_panel.selected = 0;
    m_search_selected = 0;
    m_finder_state = CheatDownload::Finder::State::Idle;
    m_finder_mb = -1;
    m_finder.Start(m_data_dir, m_cheats_system, m_cheats_path, m_cheats_title);
    BuildCheatRows();
    return;
  }
  if (row.key.rfind("cheatfile:", 0) == 0)
  {
    if (direction <= 0)
      return;
    std::string error;
    if (m_finder.Open(static_cast<size_t>(std::atoi(row.key.c_str() + 10)), &error))
    {
      m_search_selected = m_panel.selected;
      m_cheat_page = CheatPage::Pick;
      m_panel.selected = 0;
      BuildCheatRows();
    }
    else
    {
      ShowToast(error, 0);
    }
    return;
  }
  if (row.key == "pickadd")
  {
    if (direction <= 0)
      return;
    if (m_finder.PickedCount() == 0)
    {
      ShowToast("Choose the cheats to add first", 0);
      return;
    }
    const int added = m_cheats.Add(m_finder.PickedCodes());
    ShowToast(added == 1 ? std::string("1 cheat added") : std::to_string(added) + " cheats added", 0);
    m_cheat_page = CheatPage::List;
    m_panel.selected = 0;
    BuildCheatRows();
    return;
  }
  if (row.key.rfind("pickall:", 0) == 0)
  {
    m_finder.PickAll(row.key == "pickall:on");
    BuildCheatRows();
    return;
  }
  if (row.key.rfind("pick:", 0) == 0)
  {
    const size_t index = static_cast<size_t>(std::atoi(row.key.c_str() + 5));
    m_finder.Pick(index, !m_finder.Picked(index));
    BuildCheatRows();
    return;
  }
  if (row.key.rfind("cheatall:", 0) == 0)
  {
    m_cheats.SetAll(row.key == "cheatall:on");
    BuildCheatRows();
    return;
  }
  if (row.scope)
  {
    // All Games -> This Console -> This Game (Right), the other way round with Left.
    const Settings::Scope order[] = {Settings::Scope::Global, Settings::Scope::Console, Settings::Scope::Game};
    const Settings::Scope now = Settings::ScopeOf(game);
    const int at = now == Settings::Scope::Global ? 0 : now == Settings::Scope::Console ? 1 : 2;
    Settings::SetScope(game, order[(at + (direction > 0 ? 1 : 2)) % 3]);
  }
  else if (row.key == "__toall__")
  {
    if (direction <= 0)
      return;
    Settings::CopyGameToGlobal(game);
    ShowToast("Applied to all games", 0);
    m_panel.selected = 0;  // "Apply To", now All Games (this row goes away)
  }
  else if (row.key == "__reset__")
  {
    if (direction <= 0)
      return;
    if (!game.empty())
    {
      if (Settings::ScopeOf(game) == Settings::Scope::Console)
        Settings::ResetConsole(game);
      else
        Settings::ResetGame(game);
    }
    else
    {
      for (const Settings::Setting& setting : Settings::All())
      {
        if (m_panel.category == setting.category)
          Settings::ResetGlobal(setting.key);
      }
    }
  }
  else if (!row.key.empty())
  {
    const Settings::Setting* setting = Settings::Find(row.key);
    if (!setting)
      return;
    // Where the game's settings come from (Apply To): the global value, its console's or its own.
    Settings::Set(row.key, Settings::Step(*setting, Settings::Get(row.key, game), direction), game);
    if (row.key == "language")
      SetLanguage(Settings::Get("language"));
  }
  BuildSettingRows();
}

// Settings -> Controls: the consoles; Cross on one opens its controls list.
void Menu::Impl::OpenControlsMenu()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_CONTROLS;
  m_panel.title = "Controls";
  m_panel.rows.push_back({"Consoles", "", "", true});
  for (const System& system : Systems())
    m_panel.rows.push_back({system.name, "", std::string("ctlsys:") + system.key});
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// A console's controls: the normal layout (game_key empty) or one game's.
void Menu::Impl::OpenControlsList(const std::string& game_key, SystemId system)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Controls";
  m_controls_open = true;
  m_controls_game = game_key;
  m_controls_system = system;
  BuildControlRows();
  m_panel.selected = 0;
}

void Menu::Impl::BuildControlRows()
{
  std::vector<SettingRow> rows;
  const std::string edit = Controls::HasGameProfile(m_controls_game) ? m_controls_game : "";
  if (!m_controls_game.empty())
  {
    rows.push_back({"Apply To", edit.empty() ? "All Games" : "This Game", "__ctlscope__", false, true});
  }
  rows.push_back({GetSystem(m_controls_system).name, "DualShock 4", "", true});
  const auto& bindings = Controls::Bindings(m_controls_system);
  for (size_t i = 0; i < bindings.size(); i++)
  {
    const std::string value = Controls::Get(bindings[i], edit, m_controls_system);
    SettingRow row{bindings[i].label, bindings[i].stick_group ? value : Controls::Describe(value),
                   "ctl:" + std::to_string(i)};
    row.glyph = bindings[i].stick_group ? "" : value;
    rows.push_back(std::move(row));
  }
  rows.push_back({"", "", "", true});
  rows.push_back({"Reset to Default", "", "__ctlreset__"});
  m_panel.rows = std::move(rows);
}

// A button capture for the selected controls row: waits until the pad is at rest, then takes the
// next press (5 s without one cancels).
void Menu::Impl::UpdateCapture(float dt)
{
  if (!m_capture)
    return;
  m_capture_time += dt;
  OrbisPadData data = {};
  if (!m_input.Raw(&data))
    return;
  if (!m_capture_released)
  {
    m_capture_released = Controls::IsAtRest(data);
    return;
  }
  if (m_capture_time > 5.0f)
  {
    m_capture = false;
    Play("back");
    return;
  }
  const std::string& key = m_panel.rows[m_capture_row].key;
  const auto& binding = Controls::Bindings(m_controls_system)[std::atoi(key.c_str() + 4)];
  const std::string control = Controls::Capture(data);
  if (control.empty())
    return;
  Controls::Set(binding, control, Controls::HasGameProfile(m_controls_game) ? m_controls_game : "",
                m_controls_system);
  BuildControlRows();
  m_capture = false;
  m_input.Poll(0.0f);  // swallow the press
  Play("confirm");
}

void Menu::Impl::OpenCheatsList(const Game& game)
{
  m_finder.Stop();
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Cheats";
  m_cheats_game = game.rom.key;
  m_cheats_path = game.rom.path;
  m_cheats_title = game.rom.title;
  m_cheats_system = game.rom.system;
  m_cheats_folder = m_data_dir + "/cheats/" + GetSystem(game.rom.system).prefix;
  m_cheats_file = game.rom.id + ".cht";
  m_cheat_page = CheatPage::List;
  m_cheats.Load(m_data_dir, game.rom.key);
  BuildCheatRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildCheatRows()
{
  std::vector<SettingRow> rows;
  if (m_cheat_page == CheatPage::List)
  {
    m_panel.title = "Cheats";
    rows.push_back({"Find Cheats Online", "", "cheatfind"});
    const auto& entries = m_cheats.Entries();
    if (!entries.empty())
    {
      rows.push_back({"Enable All", "", "cheatall:on"});
      rows.push_back({"Disable All", "", "cheatall:off"});
      rows.push_back({"Codes", "", "", true});
    }
    for (size_t i = 0; i < entries.size(); i++)
    {
      SettingRow row{entries[i].name, "", "cheat:" + std::to_string(i)};
      row.tag = entries[i].enabled ? 1 : -1;
      rows.push_back(std::move(row));
    }
    if (entries.empty())
    {
      rows.push_back({"No cheats for this game yet.", "", "", true});
      rows.push_back({"Or put a RetroArch .cht file in " + m_cheats_folder, "", "", true});
      rows.push_back({"named " + m_cheats_file, "", "", true});
    }
  }
  else if (m_cheat_page == CheatPage::Search)
  {
    using State = CheatDownload::Finder::State;
    m_panel.title = "Find Cheats Online";
    const State state = m_finder.GetState();
    if (state == State::Downloading)
      rows.push_back({"Downloading the cheat collection: " + std::to_string(m_finder.DownloadedMb()) + " MB", "", "",
                      true});
    else if (state == State::Searching || state == State::Idle)
      rows.push_back({"Looking for this game's cheats...", "", "", true});
    else if (state == State::Failed)
    {
      rows.push_back({m_finder.Error(), "", "", true});
      rows.push_back({"Check the PS4's internet connection", "", "", true});
    }
    else if (m_finder.Results().empty())
      rows.push_back({"No cheats found for this game", "", "", true});
    else
    {
      rows.push_back({"Cheat lists (libretro)", "", "", true});
      const auto& results = m_finder.Results();
      for (size_t i = 0; i < results.size(); i++)
        rows.push_back({results[i], "", "cheatfile:" + std::to_string(i)});
    }
  }
  else
  {
    m_panel.title = "Choose Cheats";
    rows.push_back({"Add the Chosen Cheats", std::to_string(m_finder.PickedCount()), "pickadd"});
    rows.push_back({"Choose All", "", "pickall:on"});
    rows.push_back({"Choose None", "", "pickall:off"});
    rows.push_back({m_finder.OpenName(), "", "", true});
    const auto& codes = m_finder.Codes();
    for (size_t i = 0; i < codes.size(); i++)
    {
      if (m_finder.IsTitle(i))
      {
        rows.push_back({codes[i].name, "", "", true});
        continue;
      }
      SettingRow row{codes[i].name, "", "pick:" + std::to_string(i)};
      row.tag = m_finder.Picked(i) ? 1 : -1;
      rows.push_back(std::move(row));
    }
  }
  m_panel.rows = std::move(rows);
  if (!m_panel.rows.empty())
  {
    m_panel.selected = std::clamp(m_panel.selected, 0, static_cast<int>(m_panel.rows.size()) - 1);
    if (m_panel.rows[m_panel.selected].header)
      MoveSettingRow(1);
  }
}

// While Find Cheats Online looks, its rows follow it.
void Menu::Impl::UpdateCheatFinder()
{
  if (!m_panel.open || m_cheat_page != CheatPage::Search || m_panel.title != "Find Cheats Online")
    return;
  const auto state = m_finder.GetState();
  const int mb = m_finder.DownloadedMb();
  if (state != m_finder_state || mb != m_finder_mb)
  {
    m_finder_state = state;
    m_finder_mb = mb;
    BuildCheatRows();
  }
}

// Circle on Find Cheats Online's pages: one page back (true), else the panel closes as before.
bool Menu::Impl::CheatPanelBack()
{
  if (m_panel.title == "Choose Cheats" && m_cheat_page == CheatPage::Pick)
  {
    m_cheat_page = CheatPage::Search;
    m_panel.selected = m_search_selected;
    BuildCheatRows();
    return true;
  }
  if (m_panel.title == "Find Cheats Online" && m_cheat_page == CheatPage::Search)
  {
    m_finder.Stop();
    m_cheat_page = CheatPage::List;
    m_panel.selected = 0;
    BuildCheatRows();
    return true;
  }
  return false;
}

void Menu::Impl::OpenGameOptions()
{
  const auto& items = m_items[m_category];
  const int index = items[m_selected[m_category]].game;
  const Game& game = m_games[index];
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Game";
  m_panel.options = {"Start",
                     IsFavorite(index) ? "Remove from Favorites" : "Add to Favorites",
                     "Game Settings",
                     "Cheats",  // (left out for the Dreamcast: below)
                     "Controls",
                     "Rename",
                     "Find Cover",
                     "Information",
                     m_prefs.default_game == game.rom.path ? "Remove Default Game" :
                                                             "Set as Default Game",
                     "Delete Game"};
  // (Flycast takes no cheat codes, for the NAOMI and the Atomiswave either; FinalBurn Neo's own cheats come in a
  // later version: libretro's don't fit it; Azahar's libretro core takes none.)
  if (IsFlycast(game.rom.system) || game.rom.system == SystemId::ARCADE || game.rom.system == SystemId::THREEDS)
    m_panel.options.erase(std::find(m_panel.options.begin(), m_panel.options.end(), "Cheats"));
  Play("menu");
}

void Menu::Impl::ApplySettingOption(int setting, int option)
{
  switch (setting)
  {
  case SET_SOUNDS:
    m_prefs.sounds = option == 0;
    break;
  case SET_VOLUME:
    m_prefs.volume = 100 - option * 10;
    break;
  case SET_CLOCK:
    m_prefs.clock = option == 0;
    break;
  case SET_LANGUAGE:
    if (option >= 0 && option < static_cast<int>(PS4Lang::All().size()))
      SetLanguage(PS4Lang::All()[option].code);
    m_first_language = false;
    break;
  case SET_STYLE:
    m_prefs.style = option == 0 ? "library" : "xmb";
    break;
  case SET_COVERS:
  {
    // (Settings::Set: a global value of settings.ini, as the language: Reset All Settings puts it back.)
    Settings::Set("covers_on", option == 1 ? "usb" : "internal");
    const std::string folder = CoversSaveDir(true);  // made now on the drive
    Log("covers: new ones go to " + folder);
    IndexCovers();  // the place saved in wins over the other one now
    ShowToast(option == 1 && folder == m_data_dir + "/covers" ? std::string("No USB drive: covers stay internal") :
                                                                "Covers: " + folder,
              0);
    break;
  }
  case SET_RESET_ALL:
    if (option == 1)
    {
      // The games' settings; the menu's own options stay, as the language does (Consoles Shown and Covers
      // Saved On are global values of settings.ini too).
      std::vector<std::pair<std::string, std::string>> menu;
      for (const auto& [key, value] : Settings::GlobalValues())
      {
        if (key == "consoles_shown" || key == "consoles_off" || key == "covers_on")
          menu.emplace_back(key, value);
      }
      Settings::ResetAll();
      for (const auto& [key, value] : menu)
        Settings::Set(key, value);
    }
    return;
  default:
    return;
  }
  ApplyPreferences();
  BuildItems();
}

void Menu::Impl::HandleInput(uint32_t pressed)
{
  if (m_launch >= 0 || pressed == 0)
    return;
  if (m_capture)
    return;  // a button capture takes the pad (UpdateCapture)
  if (m_keyboard)
  {
    HandleKeyboard(pressed);
    return;
  }
  if (m_panel.open)
  {
    if (m_panel.setting == SET_FIND_COVER)
      m_find_select_first = false;  // the user moved first: no jump to the closest picture
    if (m_panel.setting == SET_CONSOLE_ORDER && ConsoleOrderInput(pressed))
      return;
    if (pressed & BTN_CIRCLE)
    {
      if (m_first_language && m_panel.setting == SET_LANGUAGE)
        ApplySettingOption(SET_LANGUAGE, m_panel.selected);
      if (m_panel.setting == SET_DELETE)
      {
        OpenSavesPanel(m_saves_panel);
        Play("back");
        return;
      }
      if (m_panel.setting == SET_FIND_COVER && m_find_return)
      {
        ReturnToNoCover();  // Games Without Cover, on the next game
        Play("back");
        return;
      }
      if (CheatPanelBack())
      {
        Play("back");
        return;
      }
      m_controls_open = false;
      m_controls_game.clear();
      m_panel.open = false;
      Play("back");
      return;
    }
    if (!m_panel.rows.empty())
    {
      if (pressed & BTN_UP)
      {
        MoveSettingRow(-1);
        Play("move");
      }
      if (pressed & BTN_DOWN)
      {
        MoveSettingRow(1);
        Play("move");
      }
      if (pressed & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))
      {
        ChangeSettingRow((pressed & BTN_LEFT) ? -1 : 1);
        Play("category");
      }
      return;
    }
    if (m_panel.options.empty())
    {
      if (pressed & BTN_CROSS)
      {
        m_panel.open = false;
        Play("back");
      }
      return;
    }
    const int count = static_cast<int>(m_panel.options.size());
    if ((pressed & BTN_UP) && m_panel.selected > 0)
    {
      m_panel.selected--;
      Play("move");
    }
    if ((pressed & BTN_DOWN) && m_panel.selected < count - 1)
    {
      m_panel.selected++;
      Play("move");
    }
    if (!(pressed & BTN_CROSS))
      return;
    if (m_panel.setting == SET_DELETE)
    {
      if (m_panel.selected == 1 && m_saves_panel >= 0 && m_saves_panel < static_cast<int>(m_saves.size()))
      {
        const std::string id = m_saves[m_saves_panel].id;
        const bool ok = m_delete_key == "sram" ?
                            Saves::DeleteSram(id) :
                            Saves::DeleteState(id, std::atoi(m_delete_key.c_str() + 5));
        Log("saved data: delete " + id + " " + m_delete_key + (ok ? " done" : " failed"));
        ForgetStatePictures();
        LoadSaves();
        BuildItems();
        int index = -1;
        for (size_t i = 0; i < m_saves.size(); i++)
        {
          if (m_saves[i].id == id)
            index = static_cast<int>(i);
        }
        if (index >= 0)
          OpenSavesPanel(index);
        else
          m_panel.open = false;
        Play(ok ? "confirm" : "error");
      }
      else
      {
        OpenSavesPanel(m_saves_panel);
        Play("back");
      }
      return;
    }
    if (m_panel.setting == SET_DELETE_GAME || m_panel.setting == SET_CLEANUP_CONFIRM)
    {
      const bool go = m_panel.options[m_panel.selected] == "Delete";
      m_panel.open = false;
      if (go)
        DeleteGames(m_pending_delete);
      else
        Play("back");
      m_pending_delete.clear();
      return;
    }
    if (m_panel.setting >= 0)
    {
      ApplySettingOption(m_panel.setting, m_panel.selected);
      m_panel.open = false;
      Play("confirm");
      return;
    }
    if (m_panel.title == "Game")
    {
      const Item& item = m_items[m_category][m_selected[m_category]];
      const Game& game = m_games[item.game];
      const std::string& choice = m_panel.options[m_panel.selected];
      if (choice == "Start")
      {
        m_panel.open = false;
        ActivateItem(item);
        return;
      }
      if (choice == "Add to Favorites" || choice == "Remove from Favorites")
      {
        const int index = item.game;
        m_panel.open = false;
        ToggleFavorite(index);  // rebuilds the lists: `item` is gone after this
        return;
      }
      if (choice == "Delete Game")
      {
        m_pending_delete = {game.rom.path};
        const std::string title = game.rom.title;
        const std::string file = std::filesystem::path(game.rom.path).filename().string();
        // A .cue's tracks and each disc of a game on several go with it (DeleteGames).
        const size_t files = Library::GameFiles(game.rom).size();
        m_panel = {};
        m_panel.open = true;
        m_panel.setting = SET_DELETE_GAME;
        m_panel.title = "Delete Game";
        m_panel.options = {"Cancel", "Delete"};
        m_panel.lines = {"", title, "",
                         files > 1 ? "Its " + std::to_string(files) + " files are deleted from the drive:" :
                                     std::string("Its file is deleted from the drive:"),
                         file, "", "Its saves stay (Saved Data)."};
        Play("menu");
        return;
      }
      if (choice == "Rename")
      {
        const std::string key = game.rom.key;
        m_panel.open = false;
        AskText("Rename", game.rom.title, [this, key](const std::string& name) { RenameGame(key, name); });
        return;
      }
      if (choice == "Find Cover")
      {
        OpenFindCover(item.game);
        return;
      }
      if (choice == "Game Settings")
      {
        OpenSettingsList({}, game.rom.key, static_cast<int>(game.rom.system));
      }
      else if (choice == "Cheats")
      {
        OpenCheatsList(game);
      }
      else if (choice == "Controls")
      {
        OpenControlsList(game.rom.key, game.rom.system);
      }
      else if (choice == "Information")
      {
        m_panel.title = "Information";
        m_panel.options.clear();
        // Every file of the game (a .cue's tracks, each disc).
        uint64_t bytes = 0;
        for (const std::string& file : Library::GameFiles(game.rom))
        {
          std::error_code ec;
          const uintmax_t file_size = std::filesystem::file_size(file, ec);
          bytes += ec ? 0 : file_size;
        }
        char size[32];
        if (bytes >= 1024ull * 1024 * 1024)
          snprintf(size, sizeof(size), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else
          snprintf(size, sizeof(size), "%.2f MB", bytes / (1024.0 * 1024.0));
        m_panel.lines = {game.rom.title,
                         "",
                         std::string("Console: ") + GetSystem(game.rom.system).name,
                         "Region: " + (game.rom.region.empty() ? std::string("Unknown") : game.rom.region),
                         "Tags: " + (game.rom.tags.empty() ? std::string("-") : game.rom.tags),
                         "Size: " + std::string(size),
                         PlayedText(game.played),
                         "",
                         "File: " + std::filesystem::path(game.rom.path).filename().string(),
                         "Folder: " + std::filesystem::path(game.rom.path).parent_path().string()};
        if (game.rom.discs.size() > 1)
          m_panel.lines.insert(m_panel.lines.begin() + 6, "Discs: " + std::to_string(game.rom.discs.size()));
        if (game.rom.system == SystemId::ARCADE)
        {
          // The romset (a clone's parent too), its board, maker and year, and the zips it may need that aren't
          // there. The panel cuts a line it can't hold at its end, and the cut line is translated only if a
          // pattern of the language file still matches it: so each line ends with its one value (a long maker
          // is cut, its label kept in Spanish).
          if (const Arcade::Driver* driver = Arcade::Find(game.rom.id))
          {
            std::vector<std::string> lines = {"Romset: " + driver->name};
            if (!driver->parent.empty())
              lines.push_back("Clone of: " + driver->parent);
            lines.push_back("Board: " + driver->board);
            if (!driver->maker.empty())
              lines.push_back("Maker: " + driver->maker);
            if (!driver->year.empty())
              lines.push_back("Year: " + driver->year);
            m_panel.lines.insert(m_panel.lines.begin() + 3, lines.begin(), lines.end());
          }
          // "May": a non-merged set carries its parent's files, so the game starts without the zip; the
          // start says where it goes when the core refuses the game (one short line for each zip).
          for (const std::string& zip : game.rom.missing)
            m_panel.lines.push_back("May need: " + zip + " (not found)");
        }
        if (IsFlycastBoard(game.rom.system))
        {
          // A NAOMI or Atomiswave game, the same way: its set (a clone's parent too), its board, and what it
          // needs that isn't there - its BIOS's zip and a GD-ROM game's disc image ("Needs"), a clone's parent
          // zip ("May need": a non-merged set carries its files). A NAOMI cartridge image (.dat, .bin): its
          // board, and naomi.zip when it isn't there.
          if (Naomi::IsImageName(game.rom.path))
          {
            m_panel.lines.insert(m_panel.lines.begin() + 3, "Board: NAOMI (cartridge image)");
            for (const std::string& file : game.rom.missing)
              m_panel.lines.push_back("Needs: " + file + " (not found)");
          }
          else if (const Naomi::Set* set = Naomi::Find(game.rom.id))
          {
            std::vector<std::string> lines = {"Romset: " + set->name};
            if (!set->parent.empty())
              lines.push_back("Clone of: " + set->parent);
            lines.push_back("Board: " + Naomi::BoardName(*set));
            m_panel.lines.insert(m_panel.lines.begin() + 3, lines.begin(), lines.end());
            for (const std::string& file : game.rom.missing)
            {
              const bool may = !set->parent.empty() && file == set->parent + ".zip";
              m_panel.lines.push_back((may ? "May need: " : "Needs: ") + file + " (not found)");
            }
          }
        }
      }
      else
      {
        // One default game: setting another replaces it, the same game again removes it.
        const bool remove = m_prefs.default_game == game.rom.path;
        m_prefs.default_game = remove ? "" : game.rom.path;
        m_prefs.Save(m_data_dir + "/xmb.ini");
        BuildItems();
        m_panel.title = remove ? "Remove Default Game" : "Set as Default Game";
        m_panel.options.clear();
        if (remove)
          m_panel.lines = {"", "No game starts by itself anymore."};
        else
          m_panel.lines = {"", game.rom.title, "", "starts when EmuSuNi opens.", "",
                           "Hold L1 while EmuSuNi starts", "to open the menu instead."};
      }
      Play("confirm");
      return;
    }
    m_panel.open = false;
    Play("confirm");
    return;
  }

  auto& items = m_items[m_category];
  int& selected = m_selected[m_category];
  const int count = static_cast<int>(items.size());
  const bool games = IsGameList(m_category);
  // XMB: Left / Right switch the columns, Up / Down the items, L1 / R1 jump five.
  // Library: L1 / R1 switch the tabs; the games scroll sideways, the other tabs up and down.
  // (A page of Settings is where Settings is.)
  const auto& columns = Columns();
  const int column = ColumnIndex(m_category);
  int prev_category = column > 0 ? columns[column - 1] : -1;
  int next_category = column + 1 < static_cast<int>(columns.size()) ? columns[column + 1] : -1;
  uint32_t prev_tab = BTN_LEFT, next_tab = BTN_RIGHT, prev_item = BTN_UP, next_item = BTN_DOWN;
  uint32_t prev_page = BTN_L1, next_page = BTN_R1;
  if (Library())
  {
    // The tabs go round: R1 on the last one is the first again.
    const auto& tabs = Tabs();
    const int tab = LibraryTab();
    const int tab_count = static_cast<int>(tabs.size());
    prev_category = tabs[(tab + tab_count - 1) % tab_count];
    next_category = tabs[(tab + 1) % tab_count];
    prev_tab = BTN_L1;
    next_tab = BTN_R1;
    prev_item = games ? BTN_LEFT : BTN_UP;
    next_item = games ? BTN_RIGHT : BTN_DOWN;
    // Up / Down jump ten games in the row.
    prev_page = games ? BTN_UP : 0;
    next_page = games ? BTN_DOWN : 0;
  }
  const int page = Library() ? 10 : 5;
  if ((pressed & prev_tab) && prev_category >= 0)
  {
    m_category = prev_category;
    Play("category");
    if (IsGameList(m_category))
      CheckPlaces(false);  // a drive plugged in since: its games come
  }
  else if ((pressed & next_tab) && next_category >= 0)
  {
    m_category = next_category;
    Play("category");
    if (IsGameList(m_category))
      CheckPlaces(false);
  }
  else if ((pressed & prev_item) && selected > 0)
  {
    selected--;
    Play("move");
  }
  else if ((pressed & next_item) && selected < count - 1)
  {
    selected++;
    Play("move");
  }
  else if (prev_page && (pressed & prev_page) && selected > 0)
  {
    selected = std::max(0, selected - page);
    Play("move");
  }
  else if (next_page && (pressed & next_page) && selected < count - 1)
  {
    selected = std::min(count - 1, selected + page);
    Play("move");
  }
  else if (pressed & BTN_CROSS)
  {
    if (!items.empty())
      ActivateItem(items[selected]);
  }
  else if ((pressed & BTN_SQUARE) && games)
  {
    RefreshGames();
    Play("confirm");
  }
  else if ((pressed & BTN_OPTIONS) && games)
  {
    // The next game order; the selected game stays selected.
    size_t sort = 0;
    while (sort < std::size(SORT_NAMES) && m_prefs.sort != SORT_NAMES[sort][0])
      sort++;
    m_prefs.sort = SORT_NAMES[(sort + 1) % std::size(SORT_NAMES)][0];
    m_prefs.Save(m_data_dir + "/xmb.ini");
    const int game = items.empty() ? -1 : items[selected].game;
    BuildItems();
    const auto& sorted = m_items[m_category];
    bool found = false;
    for (size_t i = 0; i < sorted.size(); i++)
    {
      if (game >= 0 && sorted[i].game == game)
      {
        m_selected[m_category] = static_cast<int>(i);
        found = true;
      }
    }
    // Into Without Cover from a game with its cover: the next game without one (A-Z = m_games' order).
    if (!found && game >= 0 && m_prefs.sort == "nocover")
    {
      int next = 0;
      while (next + 1 < static_cast<int>(sorted.size()) && sorted[next].game >= 0 && sorted[next].game < game)
        next++;
      m_selected[m_category] = next;
    }
    m_item_pos[m_category] = static_cast<float>(m_selected[m_category]);
    m_sort_shown = 2.5f;
    if (!Library())
      ShowToast(SortName(), 0);  // the XMB shows the order nowhere else
    Play("category");
  }
  else if (pressed & BTN_TRIANGLE)
  {
    if (!items.empty() && items[selected].game >= 0)
      OpenGameOptions();
  }
  else if ((pressed & BTN_L2) && games)
  {
    if (!items.empty() && items[selected].game >= 0)
      ToggleFavorite(items[selected].game);
    else
      Play("error");
  }
  else if ((pressed & BTN_R2) && games)
  {
    m_keyboard = true;
    Play("menu");
  }
  else if (pressed & BTN_CIRCLE)
  {
    if (IsSettingsPage(m_category))
      m_category = static_cast<int>(Category::Settings);  // its row is still selected there
    else if (games && !m_search.empty())
    {
      SetSearch("");
      ShowToast("Search cleared", 0);
    }
    Play("back");
  }
}

// ---------------------------------------------------------------------------------------------
// Favorites: data_dir/favorites.ini, one game per line (its file name without the extension, as
// for its settings and saves).

void Menu::Impl::LoadFavorites()
{
  m_favorites.clear();
  std::ifstream file(m_data_dir + "/favorites.ini");
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (!line.empty() && line[0] != '#')
      m_favorites.insert(line);
  }
}

void Menu::Impl::SaveFavorites() const
{
  std::string text = "# EmuSuNi favorites: one game per line (its key: the file name without the extension,\n"
                     "# nes/<file name> for a NES game)\n";
  for (const std::string& id : m_favorites)
    text += id + "\n";
  WriteTextFile(m_data_dir + "/favorites.ini", text);
}

bool Menu::Impl::IsFavorite(int game) const
{
  return game >= 0 && game < static_cast<int>(m_games.size()) &&
         m_favorites.count(m_games[game].rom.key) > 0;
}

void Menu::Impl::ToggleFavorite(int game)
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return;
  const std::string id = m_games[game].rom.key;
  const bool add = m_favorites.count(id) == 0;
  if (add)
    m_favorites.insert(id);
  else
    m_favorites.erase(id);
  SaveFavorites();
  RebuildGameLists(true);
  ShowToast(add ? "Added to Favorites" : "Removed from Favorites", add ? 1 : 2);
  Play(add ? "confirm" : "back");
}

// The game lists again (a favorite or the search changed). keep_selection: each list keeps its
// selected game, or its place when that game left it; else both start at their first game.
void Menu::Impl::RebuildGameLists(bool keep_selection)
{
  std::vector<int> lists;
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    if (IsGameList(c))
      lists.push_back(c);
  }
  std::vector<int> kept(lists.size(), -1), was(lists.size(), 0);
  for (size_t i = 0; i < lists.size(); i++)
  {
    const auto& items = m_items[lists[i]];
    const int at = m_selected[lists[i]];
    was[i] = at;
    if (at >= 0 && at < static_cast<int>(items.size()))
      kept[i] = items[at].game;
  }
  BuildItems();  // keeps each selection inside its list
  for (size_t i = 0; i < lists.size(); i++)
  {
    const int c = lists[i];
    if (keep_selection)
    {
      const auto& items = m_items[c];
      for (size_t j = 0; j < items.size(); j++)
      {
        if (kept[i] >= 0 && items[j].game == kept[i])
        {
          // The same game at another place (a game before it left): the row moves with it.
          m_item_pos[c] += static_cast<float>(static_cast<int>(j) - was[i]);
          m_selected[c] = static_cast<int>(j);
        }
      }
    }
    else
    {
      m_selected[c] = 0;
    }
    if (!keep_selection || std::abs(m_item_pos[c] - static_cast<float>(m_selected[c])) > 8.0f)
      m_item_pos[c] = static_cast<float>(m_selected[c]);
  }
}

// ---------------------------------------------------------------------------------------------
// The search: R2 in the games brings up a keyboard; the lists show the games whose names have
// every word typed, as it is typed.

void Menu::Impl::SetSearch(const std::string& text)
{
  if (text == m_search)
    return;
  m_search = text;
  RebuildGameLists(false);
}

// A key of the keyboard: a character, "Space", "Delete", "Clear All" or "Done".
void Menu::Impl::EditSearch(const std::string& key)
{
  if (m_keyboard_ask)
  {
    // A name: capitals at the start of each word, small letters after.
    if (key == "Done")
    {
      m_keyboard = false;
      m_keyboard_ask = false;
      auto done = std::move(m_ask_done);
      m_ask_done = nullptr;
      if (done)
        done(m_ask_text);
      return;
    }
    if (key == "Clear All")
      m_ask_text.clear();
    else if (key == "Delete")
    {
      while (!m_ask_text.empty() && (static_cast<unsigned char>(m_ask_text.back()) & 0xC0) == 0x80)
        m_ask_text.pop_back();
      if (!m_ask_text.empty())
        m_ask_text.pop_back();
    }
    else if (key == "Space")
      m_ask_text += ' ';
    else if (m_ask_text.size() < 100)
    {
      const bool start = m_ask_text.empty() || m_ask_text.back() == ' ';
      m_ask_text += start ? key : Lower(key);
    }
    Play(key == "Delete" || key == "Clear All" ? "back" : "category");
    return;
  }
  if (key == "Done")
  {
    m_keyboard = false;
    Play("confirm");
  }
  else if (key == "Clear All")
  {
    Play(m_search.empty() ? "error" : "back");
    SetSearch("");
  }
  else if (key == "Delete")
  {
    if (m_search.empty())
    {
      Play("error");
      return;
    }
    SetSearch(m_search.substr(0, m_search.size() - 1));
    Play("back");
  }
  else if (key == "Space")
  {
    if (m_search.empty() || m_search.back() == ' ' || m_search.size() >= SEARCH_MAX)
    {
      Play("error");
      return;
    }
    m_search += ' ';  // the same games: no new lists
    Play("move");
  }
  else
  {
    if (m_search.size() >= SEARCH_MAX)
    {
      Play("error");
      return;
    }
    SetSearch(m_search + key);
    Play("category");
  }
}

void Menu::Impl::HandleKeyboard(uint32_t pressed)
{
  if (pressed & BTN_UP)
  {
    m_key_row = (m_key_row + KEY_ROW_COUNT - 1) % KEY_ROW_COUNT;
    Play("move");
  }
  else if (pressed & BTN_DOWN)
  {
    m_key_row = (m_key_row + 1) % KEY_ROW_COUNT;
    Play("move");
  }
  else if (pressed & (BTN_LEFT | BTN_RIGHT))
  {
    const int step = (pressed & BTN_LEFT) ? -1 : 1;
    if (m_key_row == KEY_ROW_COUNT - 1)
    {
      const int count = static_cast<int>(std::size(WIDE_KEYS));
      m_key_col = WIDE_KEYS[(WideKeyAt(m_key_col) + step + count) % count].first;
    }
    else
    {
      m_key_col = (m_key_col + step + KEY_COLUMNS) % KEY_COLUMNS;
    }
    Play("move");
  }
  else if (pressed & BTN_CROSS)
  {
    if (m_key_row == KEY_ROW_COUNT - 1)
      EditSearch(WIDE_KEYS[WideKeyAt(m_key_col)].label);
    else
      EditSearch(std::string(1, KEY_ROWS[m_key_row][m_key_col]));
  }
  else if (pressed & BTN_L2)
  {
    EditSearch("Clear All");
  }
  else if (pressed & (BTN_L1 | BTN_R1))
  {
    // The games found, one by one, without leaving the keyboard.
    const auto& items = m_items[m_category];
    int& selected = m_selected[m_category];
    const int next = selected + ((pressed & BTN_L1) ? -1 : 1);
    if (!m_keyboard_ask && IsGameList(m_category) && !items.empty() && items[0].game >= 0 && next >= 0 &&
        next < static_cast<int>(items.size()))
    {
      selected = next;
      Play("move");
    }
  }
  else if (pressed & BTN_SQUARE)
  {
    EditSearch("Delete");
  }
  else if (pressed & BTN_TRIANGLE)
  {
    EditSearch("Space");
  }
  else if (pressed & (BTN_R2 | BTN_OPTIONS))
  {
    EditSearch("Done");
  }
  else if (pressed & BTN_CIRCLE)
  {
    m_keyboard = false;
    if (m_keyboard_ask)
    {
      m_keyboard_ask = false;
      m_ask_done = nullptr;
    }
    Play("back");
  }
}

void Menu::Impl::ShowToast(const std::string& text, int icon)
{
  m_toast = text;
  m_toast_icon = icon;
  m_toast_time = 2.2f;
}

// Cross on an item (both styles).
void Menu::Impl::ActivateItem(const Item& item)
{
  if (item.game >= 0)
  {
    m_launch = item.game;
    m_launch_time = 0.0f;
    m_prefs.last_game = m_games[item.game].rom.path;
    m_prefs.Save(m_data_dir + "/xmb.ini");
    m_games[item.game].played = static_cast<int64_t>(std::time(nullptr));
    SavePlayed();
    Play("launch");
  }
  else if (item.setting >= 0)
  {
    OpenSettingPanel(item.setting);
    Play("menu");
  }
  else if (item.theme != -1)
  {
    if (item.theme >= 0)
    {
      m_prefs.theme = THEMES[item.theme].name;
      m_prefs.wallpaper.clear();
    }
    else
    {
      m_prefs.wallpaper = m_wallpapers[-2 - item.theme];
    }
    ApplyPreferences();
    BuildItems();
    Play("confirm");
  }
  else if (item.save >= 0)
  {
    OpenSavesPanel(item.save);
    Play("menu");
  }
  else
  {
    Play("error");
  }
}

// ---------------------------------------------------------------------------------------------
// The user's names for games, and texts asked for.

// data_dir/names.ini: "<game key><tab><name>" per line.
void Menu::Impl::LoadNames()
{
  m_names.clear();
  std::ifstream file(m_data_dir + "/names.ini");
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    const size_t tab = line.find('\t');
    if (line.empty() || line[0] == '#' || tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
      continue;
    m_names[line.substr(0, tab)] = line.substr(tab + 1);
  }
}

void Menu::Impl::SaveNames() const
{
  std::string text = "# EmuSuNi: your names for games (Triangle -> Rename): <game key><tab><name>\n";
  for (const auto& [key, name] : m_names)
    text += key + "\t" + name + "\n";
  WriteTextFile(m_data_dir + "/names.ini", text);
}

void Menu::Impl::RenameGame(const std::string& key, const std::string& typed)
{
  std::string name = typed;
  while (!name.empty() && name.back() == ' ')
    name.pop_back();
  while (!name.empty() && name.front() == ' ')
    name.erase(name.begin());
  // Empty, or the file's own name: the name from the file again.
  std::string own, region, tags;
  const System& system = SystemOfKey(key);
  Library::ParseName(key.substr(std::string(system.prefix).size()), &own, &region, &tags);
  if (name.empty() || name == own)
    m_names.erase(key);
  else
    m_names[key] = name;
  SaveNames();
  Log("renamed " + key + " -> " + (name.empty() ? own : name));
  RefreshGames();
  ShowToast(name.empty() || name == own ? "Its own name again" : "Renamed", 0);
  Play("confirm");
}

void Menu::Impl::AskText(const std::string& title, const std::string& text,
                         std::function<void(const std::string&)> done)
{
  m_ask_done = std::move(done);
  if (SystemKeyboard::Open(PS4Lang::T(title), text))
  {
    m_asking = true;
    Play("menu");
    return;
  }
  // The menu's own keyboard (capitals at the start of each word, as in a name).
  m_keyboard_ask = true;
  m_ask_title = title;
  m_ask_text = text;
  m_keyboard = true;
  Play("menu");
}

// The PS4's keyboard, each frame while it is open.
void Menu::Impl::UpdateAsk()
{
  if (!m_asking)
    return;
  std::string text;
  const SystemKeyboard::Result result = SystemKeyboard::Poll(&text);
  if (result == SystemKeyboard::Result::Running)
    return;
  m_asking = false;
  auto done = std::move(m_ask_done);
  m_ask_done = nullptr;
  m_input.Poll(0.0f);  // the press that closed it
  if (result == SystemKeyboard::Result::Done && done)
    done(text);
  else
    Play("back");
}

// ---------------------------------------------------------------------------------------------
// Find Cover.

void Menu::Impl::OpenFindCover(int index, bool back_to_list)
{
  if (index < 0 || index >= static_cast<int>(m_games.size()))
    return;
  const Library::Game& rom = m_games[index].rom;
  m_find_key = rom.key;
  m_find_title = rom.title;
  m_find_system = rom.system;
  m_find_query = rom.title;
  m_find_titles = false;
  m_find_return = back_to_list;
  m_find_rom = rom;
  m_find_select_first = true;
  if (!m_find_thread.joinable())
  {
    m_find_stop = false;
    m_find_thread = std::thread([this] { FindWorker(); });
  }
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_FIND_COVER;
  m_panel.title = "Find Cover";
  StartFind();
  Play("menu");
}

// The pictures like m_find_query, looked up again.
void Menu::Impl::StartFind()
{
  m_find_names.clear();
  m_find_busy = true;
  m_find_status = "Looking through libretro's pictures...";
  {
    std::lock_guard lock(m_find_mutex);
    FindJob job{0, m_find_system, m_find_titles, m_find_query, "", ""};
    job.game = m_find_rom;
    job.serial = ++m_find_serial;
    m_find_jobs.push_back(std::move(job));
  }
  m_find_wake.notify_one();
  BuildFindRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildFindRows()
{
  std::vector<SettingRow> rows;
  rows.push_back({m_find_title, GetSystem(m_find_system).tab, "", true});
  rows.push_back({"Look For", m_find_query, "find:query"});
  rows.push_back({"Pictures", m_find_titles ? "Title Screens" : "Box Art", "find:kind"});
  if (!m_find_status.empty())
    rows.push_back({m_find_status, "", "", true});
  else
    rows.push_back({"Pictures Found", std::to_string(m_find_names.size()), "", true});
  for (size_t i = 0; i < m_find_names.size(); i++)
    rows.push_back({m_find_names[i], "", "pick:" + std::to_string(i)});
  m_panel.rows = std::move(rows);
}

// The thread's results; the selected picture's preview once the selection rests on it.
void Menu::Impl::UpdateFind(float dt)
{
  std::deque<FindResult> results;
  {
    std::lock_guard lock(m_find_mutex);
    results.swap(m_find_results);
  }
  bool shown = m_panel.open && m_panel.setting == SET_FIND_COVER;
  for (FindResult& result : results)
  {
    if (result.type == 0)
    {
      if (result.serial != m_find_serial)
        continue;  // a search before the latest (another game's, or another name)
      m_find_busy = false;
      m_find_names = std::move(result.names);
      m_find_status = !m_find_names.empty() ? "" :
                      result.ok             ? "No pictures with that name: Look For another" :
                                              "libretro's list can't be read: " + result.error;
      if (shown)
      {
        const int selected = m_panel.selected;
        BuildFindRows();
        m_panel.selected = std::min(selected, static_cast<int>(m_panel.rows.size()) - 1);
        MoveSettingRow(0);
        // A game's first search: on its first picture (the closest one), whose preview comes;
        // Cross takes it.
        if (m_find_select_first)
        {
          for (size_t i = 0; i < m_panel.rows.size(); i++)
          {
            if (m_panel.rows[i].key == "pick:0")
              m_panel.selected = static_cast<int>(i);
          }
          m_find_select_first = false;
        }
      }
    }
    else if (result.type == 1)
    {
      if (result.ok && result.name == m_preview_want)
      {
        if (m_preview != ImTextureID{})
          m_display.DestroyTexture(m_preview);
        m_preview = LoadPngTexture(result.path, &m_preview_aspect, 320, 320);
      }
      std::remove(result.path.c_str());
    }
    else if (result.ok)
    {
      // The new cover: shown at once (the old picture's texture goes).
      m_cover_files[Lower(result.key)] = result.path;
      for (size_t i = 0; i < m_games.size(); i++)
      {
        if (m_games[i].rom.key != result.key)
          continue;
        auto it = m_covers.find(static_cast<int>(i));
        if (it != m_covers.end() && !it->second.requested)
        {
          if (it->second.texture != ImTextureID{})
            m_display.DestroyTexture(it->second.texture);
          m_covers.erase(it);
        }
      }
      m_covers_changed = true;  // Without Cover: the game leaves the lists (Run)
      ShowToast("Cover saved", 0);
      Play("confirm");
      RefreshNoCoverItem();
      if (shown && m_find_return && result.key == m_find_key)
      {
        ReturnToNoCover();  // the list again, on the next game
        shown = false;      // not Find Cover anymore for the rest of this frame
      }
      else if (m_panel.open && m_panel.setting == SET_NO_COVER)
      {
        BuildNoCoverRows();  // picked, then Circle before the picture came
      }
    }
    else
    {
      ShowToast("The picture couldn't be downloaded", 0);
      Play("error");
    }
  }
  // The preview: after a moment on a picture (not one download per press of Down).
  std::string want;
  if (shown && m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.rows.size()))
  {
    const std::string& key = m_panel.rows[m_panel.selected].key;
    if (key.rfind("pick:", 0) == 0)
      want = m_panel.rows[m_panel.selected].label;
  }
  if (want != m_preview_want)
  {
    m_preview_want = want;
    m_preview_wait = 0.0f;
    if (m_preview != ImTextureID{})
      m_display.DestroyTexture(m_preview);
    m_preview = ImTextureID{};
  }
  else if (!want.empty() && want != m_preview_name)
  {
    m_preview_wait += dt;
    if (m_preview_wait > 0.35f)
    {
      m_preview_name = want;
      {
        std::lock_guard lock(m_find_mutex);
        m_find_jobs.push_back({1, m_find_system, m_find_titles, "", want,
                               m_data_dir + "/covers/.preview" + std::to_string(++m_preview_serial) + ".png"});
      }
      m_find_wake.notify_one();
    }
  }
  if (!shown)
    m_preview_name.clear();
}

// The Find Cover thread: libretro's lists (read once per console and kind), the searches and the
// downloads.
void Menu::Impl::FindWorker()
{
  std::map<std::pair<int, bool>, std::vector<std::string>> lists;
  std::map<std::pair<int, bool>, std::shared_ptr<const CoverDownload::Prepared>> prepared;  // for Closest()
  const std::string covers = m_data_dir + "/covers", ca = m_assets_dir + "/xmb/cacert.pem";
  for (;;)
  {
    FindJob job;
    {
      std::unique_lock lock(m_find_mutex);
      m_find_wake.wait(lock, [this] { return m_find_stop || !m_find_jobs.empty(); });
      if (m_find_stop)
        return;
      job = m_find_jobs.front();
      m_find_jobs.pop_front();
    }
    if (job.type == 3)
    {
      // A game starts: the lists go (prepared first, it points into them); read again next time.
      prepared.clear();
      lists.clear();
      continue;
    }
    FindResult result;
    result.type = job.type;
    result.serial = job.serial;
    if (job.type == 0)
    {
      const std::pair<int, bool> list{static_cast<int>(job.system), job.titles};
      auto& names = lists[list];
      if (names.empty())
        names = CoverDownload::PictureNames(job.system, job.titles, covers, ca, &result.error);
      result.ok = !names.empty();
      result.names = CoverDownload::Search(names, job.text, 40);
      // The closest picture first: Download()'s own match, else the closest title (Search's first
      // can be another edition, or a name sharing a word only).
      if (!names.empty())
      {
        auto& ready = prepared[list];
        if (!ready)
          ready = CoverDownload::Prepare(names);
        bool sure = false;
        if (const std::string* best = CoverDownload::Closest(*ready, job.text, job.game, &sure))
        {
          result.names.erase(std::remove(result.names.begin(), result.names.end(), *best), result.names.end());
          result.names.insert(result.names.begin(), *best);
          if (result.names.size() > 40)
            result.names.pop_back();
        }
      }
    }
    else
    {
      result.name = job.name;
      result.path = job.path;
      result.key = job.text;  // the game (a cover)
      if (job.type == 2)
        MakeDirs(job.path.substr(0, job.path.rfind('/')));
      result.ok = CoverDownload::DownloadPicture(job.system, job.titles, job.name, job.path,
                                                 job.type == 1 ? 320 : 600, job.type == 1 ? 320 : 430, ca,
                                                 &result.error);
    }
    std::lock_guard lock(m_find_mutex);
    m_find_results.push_back(std::move(result));
  }
}

// ---------------------------------------------------------------------------------------------
// Games Without Cover.

// Settings' line for it: how many games have no cover picture.
std::string Menu::Impl::NoCoverDetail() const
{
  int missing = 0;
  for (const Game& game : m_games)
    missing += HasCover(game.rom) ? 0 : 1;
  return missing == 0 ? std::string("Every game has a cover") :
                        std::to_string(missing) + (missing == 1 ? " game" : " games");
}

// Settings' count again when covers come (BuildItems isn't called for them).
void Menu::Impl::RefreshNoCoverItem()
{
  for (Item& item : m_items[static_cast<int>(Category::Settings)])
  {
    if (item.setting == SET_NO_COVER)
      item.detail = NoCoverDetail();
  }
}

void Menu::Impl::OpenNoCover(int select)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_NO_COVER;
  m_panel.title = "Games Without Cover";
  m_panel.selected = select;
  BuildNoCoverRows();
}

// [0] how many, [1] Download the Closest for All (its progress), [2] the download's status or
// result ("" = a gap), then the games (All Games' order: A-Z). The selection follows its row's key,
// else keeps its index (the next game, when its game got its cover).
void Menu::Impl::BuildNoCoverRows()
{
  const int at = m_panel.selected;
  const std::string was =
      at >= 0 && at < static_cast<int>(m_panel.rows.size()) ? m_panel.rows[at].key : std::string();
  std::vector<SettingRow> games;
  for (const Game& game : m_games)
  {
    if (!HasCover(game.rom))
      games.push_back({game.rom.title, GetSystem(game.rom.system).tab, "nocover:" + game.rom.key});
  }
  const bool running = m_dl_thread.joinable() && !m_dl_done;
  std::string status = m_nocover_status;
  if (running)
  {
    std::lock_guard lock(m_dl_mutex);
    status = m_dl_status;
  }
  m_nocover_progress = running ? m_dl_progress.load() : -1;
  std::vector<SettingRow> rows;
  if (games.empty())
    rows.push_back({"Every game has a cover", "", "", true});
  else
    rows.push_back({"Without a Cover", std::to_string(games.size()), "", true});
  rows.push_back({"Download the Closest for All",
                  running ? std::to_string(m_dl_progress.load()) + " / " + std::to_string(m_dl_total.load()) :
                            std::string(),
                  "__closest__"});
  rows.push_back({status, "", "", true});
  rows.insert(rows.end(), games.begin(), games.end());
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(at, 0, static_cast<int>(m_panel.rows.size()) - 1);
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!was.empty() && m_panel.rows[i].key == was)
      m_panel.selected = static_cast<int>(i);
  }
  MoveSettingRow(0);
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(-1);  // it was past the last game
}

// Back from Find Cover: on the game after the one looked for (once covered it left the list, and
// its old row is the next game's).
void Menu::Impl::ReturnToNoCover()
{
  m_find_return = false;
  OpenNoCover(m_find_return_row);
  // The rows follow m_games' order: the first one after the game looked for (it may have left the
  // list, and so may games before it), else the row it had.
  std::unordered_map<std::string, int> index;
  for (size_t i = 0; i < m_games.size(); i++)
    index[m_games[i].rom.key] = static_cast<int>(i);
  const auto looked = index.find(m_find_key);
  if (looked == index.end())
    return;
  for (size_t r = 0; r < m_panel.rows.size(); r++)
  {
    const std::string& key = m_panel.rows[r].key;
    if (key.rfind("nocover:", 0) != 0)
      continue;
    const auto it = index.find(key.substr(8));
    if (it != index.end() && it->second > looked->second)
    {
      m_panel.selected = static_cast<int>(r);
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Deleting games.

// Clean Up Games: the duplicates (one game in several files: regions, revisions, dumps, two ways
// of naming) and the games of other regions than the USA, with what each action would delete.
void Menu::Impl::OpenCleanUp()
{
  // Files of one console with the same title (spaces and signs aside: "Rock N' Roll Racing", "Rock
  // n' Roll Racing"). Not the Arcade's, the NAOMI's or the Atomiswave's: a romset is no copy of another (a
  // clone is another version of the game, and a split set needs its parent's zip to start), and the region
  // in a driver's name says nothing about which of them to keep.
  std::map<std::string, std::vector<int>> groups;
  for (size_t i = 0; i < m_games.size(); i++)
  {
    if (IsRomsetSystem(m_games[i].rom.system))
      continue;
    std::string key = SearchText(m_games[i].rom.title);
    key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
    if (!key.empty())
      groups[std::string(GetSystem(m_games[i].rom.system).key) + "|" + key].push_back(static_cast<int>(i));
  }
  // The one that stays: the USA version (else World, Europe, Japan), a good dump ([!]), the
  // latest revision; no bad dumps, hacks, betas or special editions.
  auto score = [this](int index) {
    const Library::Game& rom = m_games[index].rom;
    const std::string name = rom.id;
    int value = 0;
    if (rom.region.find("USA") != std::string::npos)
      value += 1000;
    else if (rom.region == "World")
      value += 900;
    else if (rom.region.find("Europe") != std::string::npos)
      value += 500;
    else if (rom.region.find("Japan") != std::string::npos)
      value += 300;
    else if (!rom.region.empty())
      value += 200;
    if (name.find("[!]") != std::string::npos)
      value += 50;
    for (const char* bad : {"[b", "[h", "[o", "[t", "[f", "[p", "[a", "(Beta", "(Proto", "(Demo", "(Sample",
                            "(Hack", "(Competition", "(Pirate", "(Unl"})
    {
      if (name.find(bad) != std::string::npos)
        value -= 100;
    }
    // The latest revision: (V1.1), (Rev 1), (Rev B).
    for (const char* revision : {"(V1.", "(Rev "})
    {
      const size_t at = name.find(revision);
      if (at != std::string::npos && at + std::char_traits<char>::length(revision) < name.size())
      {
        const char c = name[at + std::char_traits<char>::length(revision)];
        value += std::isdigit(static_cast<unsigned char>(c)) ? (c - '0') * 5 : std::isalpha(static_cast<unsigned char>(c)) ? (std::toupper(c) - 'A' + 1) * 5 : 0;
      }
    }
    return value;
  };
  std::vector<SettingRow> listed;
  m_cleanup_dups.clear();
  for (auto& others : m_cleanup_others)
    others.clear();
  for (const auto& [key, members] : groups)
  {
    if (members.size() < 2)
      continue;
    int keep = members[0];
    for (const int i : members)
    {
      if (score(i) > score(keep))
        keep = i;
    }
    listed.push_back({m_games[keep].rom.title, GetSystem(m_games[keep].rom.system).tab, "", true});
    for (const int i : members)
    {
      const bool stays = i == keep;
      SettingRow row{std::filesystem::path(m_games[i].rom.path).filename().string(), stays ? "Stays" : "Deleted",
                     "dupfile:" + m_games[i].rom.path};
      row.note = !stays;
      listed.push_back(row);
      if (!stays)
        m_cleanup_dups.push_back(m_games[i].rom.path);
    }
  }
  // Other regions: a region in the name without the USA in it (no region: nothing known, it stays),
  // each console apart.
  std::vector<SettingRow> others[SYSTEM_COUNT];
  for (const Game& game : m_games)
  {
    const std::string& region = game.rom.region;
    if (region.empty() || region.find("USA") != std::string::npos || region == "World" ||
        IsRomsetSystem(game.rom.system))
      continue;
    const int s = static_cast<int>(game.rom.system);
    others[s].push_back({std::filesystem::path(game.rom.path).filename().string(), region, "otherfile:" + game.rom.path});
    m_cleanup_others[s].push_back(game.rom.path);
  }
  auto files = [](size_t count) { return std::to_string(count) + (count == 1 ? " file" : " files"); };
  std::vector<SettingRow> rows;
  rows.push_back({"Delete Duplicates", m_cleanup_dups.empty() ? "None" : files(m_cleanup_dups.size()), "__dups__"});
  bool any_others = false;
  for (const System& system : Systems())
  {
    const auto& paths = m_cleanup_others[static_cast<int>(system.id)];
    if (paths.empty())
      continue;
    any_others = true;
    rows.push_back({std::string("Keep Only USA Games: ") + system.tab, files(paths.size()),
                    std::string("__usaonly__:") + system.key});
  }
  if (!any_others)
    rows.push_back({"Keep Only USA Games", "None", "__usaonly__:"});
  if (!listed.empty())
  {
    rows.push_back({"", "", "", true});
    rows.push_back({"Duplicates", "", "", true});
    rows.insert(rows.end(), listed.begin(), listed.end());
  }
  for (const System& system : Systems())
  {
    const auto& list = others[static_cast<int>(system.id)];
    if (list.empty())
      continue;
    rows.push_back({"", "", "", true});
    rows.push_back({std::string("Other regions: ") + system.tab, "", "", true});
    rows.insert(rows.end(), list.begin(), list.end());
  }
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_CLEANUP;
  m_panel.title = "Clean Up Games";
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// Deletes games (by their path) with every file of each - a .cue's tracks, each disc of a game on
// several (Library::GameFiles); their favorite and default-game marks go with them, the list is read
// again.
void Menu::Impl::DeleteGames(const std::vector<std::string>& paths)
{
  int deleted = 0;
  std::string error;
  for (const std::string& path : paths)
  {
    std::vector<std::string> files = {path};
    for (const Game& game : m_games)
    {
      if (game.rom.path == path)
      {
        files = Library::GameFiles(game.rom);
        break;
      }
    }
    bool all = true;
    for (const std::string& file : files)
    {
      std::error_code ec;
      if (std::filesystem::remove(file, ec))
      {
        Log("deleted " + file);
        continue;
      }
      all = false;
      if (error.empty())
        error = ec ? ec.message() : "the file isn't there";
      Log("couldn't delete " + file + ": " + (ec ? ec.message() : "not found"));
    }
    if (all)
    {
      deleted++;
      for (const Game& game : m_games)
      {
        if (game.rom.path == path)
          m_favorites.erase(game.rom.key);
      }
      if (m_prefs.default_game == path)
        m_prefs.default_game.clear();
    }
  }
  m_prefs.Save(m_data_dir + "/xmb.ini");
  SaveFavorites();
  RefreshGames();
  const int failed = static_cast<int>(paths.size()) - deleted;
  if (failed == 0)
  {
    ShowToast(deleted == 1 ? "Game deleted" : std::to_string(deleted) + " games deleted", 0);
    Play("confirm");
    return;
  }
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Delete";
  m_panel.lines = {"", std::to_string(deleted) + " deleted, " + std::to_string(failed) + " not:", error, "",
                   "Is the drive read-only?"};
  Play("error");
}

// What an option of a side panel does (shown at the panel's bottom).
std::string Menu::Impl::OptionHelp(const std::string& option) const
{
  static const std::map<std::string, const char*> HELP = {
      {"Start", "Plays the game."},
      {"Add to Favorites", "Adds the game to the Favorites tab (L2 on a game does the same)."},
      {"Remove from Favorites", "Takes the game out of the Favorites tab."},
      {"Game Settings", "This game's settings: for all games, or only for this one (Apply To)."},
      {"Cheats", "Turns this game's cheat codes on and off."},
      {"Controls", "Which DualShock 4 button does what, for all games or only this one."},
      {"Information", "The game's file, size, region and when you last played it."},
      {"Rename", "A name of your own for the game (it is found by it too, covers included). Empty: its own name again."},
      {"Find Cover", "Choose the game's cover from libretro's pictures: the ones most like its name, or another name."},
      {"Set as Default Game", "This game starts by itself when EmuSuNi opens (hold L1 to get the menu)."},
      {"Remove Default Game", "EmuSuNi opens on the menu again."},
      {"Delete Game", "Deletes the game's files from the drive (a .cue's tracks and every disc too; its saves stay)."},
      {"Cancel", "Nothing is changed."},
      {"Delete", "Deletes for good: it can't be brought back."},
      {"Reset everything", "Every setting, global and per game, back to the default."},
      {"Internal Storage", "New covers are saved in the PS4's own storage, as before."},
      {"USB Drive", "New covers are saved on the USB drive (EmuSuNi/covers): the PS4's storage stays free. "
                    "Without the drive they go to the internal storage."},
  };
  const auto it = HELP.find(option);
  return it == HELP.end() ? std::string() : it->second;
}

// A command of the screenshot tour (Tour.h).
void Menu::Impl::TourCommand(const std::string& command)
{
  const size_t space = command.find(' ');
  const std::string word = command.substr(0, space);
  const std::string rest = space == std::string::npos ? std::string() : command.substr(space + 1);
  const int games = static_cast<int>(Category::Games);
  if (word == "select" || word == "play")
  {
    // The game whose title is the text (else the first that has it).
    const std::string wanted = Lower(rest);
    int at = -1;
    for (size_t i = 0; i < m_items[games].size(); i++)
    {
      const int game = m_items[games][i].game;
      if (game < 0)
        continue;
      const std::string title = Lower(m_games[game].rom.title);
      if (title == wanted || (at < 0 && title.find(wanted) != std::string::npos))
        at = static_cast<int>(i);
      if (title == wanted)
        break;
    }
    if (at < 0)
    {
      Log("tour: no game " + rest);
      return;
    }
    m_panel.open = false;
    m_category = games;
    m_selected[games] = at;
    m_item_pos[games] = static_cast<float>(at);
    m_category_pos = static_cast<float>(ColumnIndex(games));
    m_tab_pos = static_cast<float>(LibraryTab());
    if (word == "play")
    {
      // Without "last played": the tour leaves no trace.
      m_launch = m_items[games][at].game;
      m_launch_time = 0.0f;
      Play("launch");
    }
  }
  else if (word == "search")
  {
    SetSearch(rest);
    m_keyboard = !rest.empty();
  }
  else if (word == "settings")
  {
    const size_t gap = rest.find(' ');
    OpenSettingsList(rest.substr(0, gap), {});
    if (gap != std::string::npos)
    {
      for (size_t i = 0; i < m_panel.rows.size(); i++)
      {
        if (m_panel.rows[i].key == rest.substr(gap + 1))
          m_panel.selected = static_cast<int>(i);
      }
    }
  }
  else if (word == "options")
  {
    OpenGameOptions();
  }
  else if (word == "tab")
  {
    // A page by its console's key ("arcade"), or all / favorites / settings (a console not shown: All), and on
    // it the first game whose title has the words after it ("tab snes donkey kong").
    const size_t gap = rest.find(' ');
    const std::string name = rest.substr(0, gap), wanted = gap == std::string::npos ? "" : Lower(rest.substr(gap + 1));
    int page = -1;
    if (name == "all")
      page = games;
    else if (name == "favorites")
      page = static_cast<int>(Category::Favorites);
    else if (name == "settings")
      page = static_cast<int>(Category::Settings);
    else if (const System* system = SystemByKey(name))
      page = ListOfSystem(system->id);
    if (page < 0)
    {
      Log("tour: no tab " + name);
      return;
    }
    m_panel.open = false;
    m_category = page;
    UpdateShownConsoles();
    for (size_t i = 0; !wanted.empty() && i < m_items[m_category].size(); i++)
    {
      const int game = m_items[m_category][i].game;
      if (game >= 0 && Lower(m_games[game].rom.title).find(wanted) != std::string::npos)
      {
        m_selected[m_category] = static_cast<int>(i);
        m_item_pos[m_category] = static_cast<float>(i);
        break;
      }
    }
    m_category_pos = static_cast<float>(ColumnIndex(m_category));
    m_tab_pos = static_cast<float>(LibraryTab());
  }
  else if (word == "open")
  {
    // A page of Settings by a word: consoles (Consoles Shown), covers (Covers Saved On), folders (Game Folders).
    BuildItems();  // (its rows with the tour's "set" values)
    const int setting = rest == "consoles" ? SET_CONSOLES_SHOWN : rest == "covers" ? SET_COVERS :
                        rest == "folders"  ? SET_FOLDERS :
                        rest == "controls" ? SET_CONTROLS :
                                             -1;
    if (setting < 0)
      Log("tour: nothing to open called " + rest);
    else
      OpenSettingPanel(setting);
  }
  else if (word == "close")
  {
    m_panel.open = false;
  }
  else if (word == "row")
  {
    // The open list's row by its key or its words ("row quality", "row fast_forward", "row Fast Forward (hold)").
    for (size_t i = 0; i < m_panel.rows.size(); i++)
    {
      if (!m_panel.rows[i].header && (m_panel.rows[i].key == rest || m_panel.rows[i].label == rest))
      {
        m_panel.selected = static_cast<int>(i);
        break;
      }
    }
  }
  else if (word == "style")
  {
    m_prefs.style = rest == "xmb" ? "xmb" : "library";
    ApplyPreferences();
    BuildItems();
    m_category_pos = static_cast<float>(ColumnIndex(m_category));
    m_tab_pos = static_cast<float>(LibraryTab());
  }
}

int Menu::Impl::LibraryTab() const
{
  const auto& tabs = Tabs();
  const auto it = std::find(tabs.begin(), tabs.end(), ColumnOf(m_category));
  return it == tabs.end() ? 0 : static_cast<int>(it - tabs.begin());
}

std::string Menu::Impl::SortName() const
{
  for (const auto& [key, name] : SORT_NAMES)
  {
    if (m_prefs.sort == key)
      return name;
  }
  return SORT_NAMES[0][1];
}

// data_dir/played.ini: "<Unix time> <game path>" per line.
void Menu::Impl::LoadPlayed()
{
  std::map<std::string, int64_t> played;
  std::ifstream file(m_data_dir + "/played.ini");
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    const size_t space = line.find(' ');
    if (space != std::string::npos && space > 0)
      played[line.substr(space + 1)] = std::atoll(line.c_str());
  }
  for (Game& game : m_games)
  {
    const auto it = played.find(game.rom.path);
    game.played = it == played.end() ? 0 : it->second;
  }
}

void Menu::Impl::SavePlayed() const
{
  std::map<std::string, int64_t> played;
  {
    std::ifstream file(m_data_dir + "/played.ini");
    std::string line;
    while (std::getline(file, line))
    {
      const size_t space = line.find(' ');
      if (space != std::string::npos && space > 0)
        played[line.substr(space + 1)] = std::atoll(line.c_str());
    }
  }
  for (const Game& game : m_games)
  {
    if (game.played > 0)
      played[game.rom.path] = game.played;
  }
  std::string text;
  for (const auto& [path, when] : played)
    text += std::to_string(when) + " " + path + "\n";
  WriteTextFile(m_data_dir + "/played.ini", text);
}

// ---------------------------------------------------------------------------------------------
// Drawing.

void Menu::Impl::Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color,
                      const std::string& text, bool shadow)
{
  // Every text is drawn in the chosen language (PS4Lang); TextWidth measures the same text.
  const char* shown = PS4Lang::T(text).c_str();
  if (shadow)
  {
    const ImU32 shade = IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) * 90 / 255);
    dl->AddText(font, size, ImVec2(pos.x + 2, pos.y + 2), shade, shown);
  }
  dl->AddText(font, size, pos, color, shown);
}

// Line breaks for a width: at the last space that fits, or between any two characters when a
// line has no space (Japanese and Chinese are written without them).
std::string Menu::Impl::WrapText(ImFont* font, float size, const std::string& text, float width)
{
  std::string out;
  std::string line;
  size_t last_space = std::string::npos;
  auto width_of = [&](const std::string& t) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x;
  };
  for (size_t i = 0; i < text.size();)
  {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : 4;
    const std::string ch = text.substr(i, length);
    i += length;
    if (ch == "\n")
    {
      out += line + "\n";
      line.clear();
      last_space = std::string::npos;
      continue;
    }
    if (width_of(line + ch) > width && !line.empty())
    {
      if (ch == " ")
      {
        out += line + "\n";
        line.clear();
        last_space = std::string::npos;
        continue;
      }
      if (last_space != std::string::npos)
      {
        out += line.substr(0, last_space) + "\n";
        line = line.substr(last_space + 1);
      }
      else
      {
        out += line + "\n";
        line.clear();
      }
      last_space = std::string::npos;
    }
    if (ch == " ")
      last_space = line.size();
    line += ch;
  }
  return out + line;
}

float Menu::Impl::TextWidth(ImFont* font, float size, const std::string& text)
{
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, PS4Lang::T(text).c_str()).x;
}

// The text, shortened with "..." to fit a width.
std::string Menu::Impl::Fit(ImFont* font, float size, std::string text, float width)
{
  if (TextWidth(font, size, text) <= width)
    return text;
  while (text.size() > 4 && TextWidth(font, size, text + "...") > width)
  {
    text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
      text.pop_back();  // a whole UTF-8 character
  }
  return text + "...";
}

void Menu::Impl::DrawBackground()
{
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const Theme& theme = CurrentTheme();
  if (m_wallpaper != ImTextureID{})
  {
    dl->AddImage(ImTextureRef(m_wallpaper), ImVec2(0, 0), ImVec2(W, H));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.5f), IM_COL32(0, 0, 0, 90),
                                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(0, H * 0.5f), ImVec2(W, H), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 110),
                                IM_COL32(0, 0, 0, 110));
    return;
  }
  const ImU32 top = Rgba(theme.top), bottom = Rgba(theme.bottom);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), top, top, bottom, bottom);

  // Waves: translucent ribbons drifting across the middle of the screen.
  struct Wave
  {
    float base, amplitude, wavelength, speed, phase, thickness, alpha;
  };
  constexpr Wave waves[] = {
      {640, 60, 1300, 0.35f, 0.0f, 110, 0.10f}, {660, 45, 900, -0.25f, 1.7f, 70, 0.12f},
      {620, 80, 1700, 0.18f, 3.1f, 160, 0.07f}, {680, 30, 700, 0.45f, 4.4f, 40, 0.14f},
      {650, 55, 1100, -0.32f, 5.2f, 90, 0.09f},
  };
  constexpr float step = 24.0f;
  for (const Wave& wave : waves)
  {
    const float k = 2.0f * 3.14159265f / wave.wavelength;
    ImVec2 prev_top, prev_bottom;
    std::vector<ImVec2> edge;
    for (float x = -step; x <= W + step; x += step)
    {
      const float t = m_time * wave.speed;
      const float y = wave.base + wave.amplitude * std::sin(x * k + t + wave.phase) +
                      wave.amplitude * 0.35f * std::sin(x * k * 2.3f - t * 1.7f + wave.phase);
      const float thick =
          wave.thickness * (0.65f + 0.35f * std::sin(x * k * 0.6f + m_time * 0.4f + wave.phase));
      const ImVec2 p_top(x, y), p_bottom(x, y + thick);
      if (x > -step)
      {
        QuadMultiColor(dl, prev_top, p_top, p_bottom, prev_bottom, Rgba(theme.wave, wave.alpha),
                       Rgba(theme.wave, wave.alpha), Rgba(theme.wave, 0.0f), Rgba(theme.wave, 0.0f));
      }
      edge.push_back(p_top);
      prev_top = p_top;
      prev_bottom = p_bottom;
    }
    dl->AddPolyline(edge.data(), static_cast<int>(edge.size()), Rgba(theme.wave, wave.alpha * 2.2f),
                    ImDrawFlags_None, 2.0f);
  }
}

void Menu::Impl::DrawCategoryIcon(ImDrawList* dl, Category category, ImVec2 c, float s, ImU32 color)
{
  const float r = s * 0.5f;
  switch (category)
  {
  case Category::Settings:
  {
    // Gear: a ring with eight teeth.
    for (int i = 0; i < 8; i++)
    {
      const float a = i * 3.14159265f / 4.0f;
      const ImVec2 d(std::cos(a), std::sin(a));
      const ImVec2 n(-d.y, d.x);
      const float w = r * 0.16f;
      dl->AddQuadFilled(ImVec2(c.x + d.x * r * 0.62f + n.x * w, c.y + d.y * r * 0.62f + n.y * w),
                        ImVec2(c.x + d.x * r * 0.95f + n.x * w, c.y + d.y * r * 0.95f + n.y * w),
                        ImVec2(c.x + d.x * r * 0.95f - n.x * w, c.y + d.y * r * 0.95f - n.y * w),
                        ImVec2(c.x + d.x * r * 0.62f - n.x * w, c.y + d.y * r * 0.62f - n.y * w),
                        color);
    }
    dl->AddCircle(c, r * 0.55f, color, 40, r * 0.22f);
    break;
  }
  case Category::Games:
  {
    // Every console's games: three game boxes side by side, the middle one in front.
    const ImU32 shade = IM_COL32(0, 0, 0, 100);
    for (int i : {-1, 1, 0})
    {
      const float bw = r * 0.62f, bh = r * (i == 0 ? 1.55f : 1.3f);
      const ImVec2 bc(c.x + i * r * 0.55f, c.y + (i == 0 ? 0.0f : r * 0.1f));
      const ImVec2 b0(bc.x - bw * 0.5f, bc.y - bh * 0.5f), b1(bc.x + bw * 0.5f, bc.y + bh * 0.5f);
      if (i == 0)
        dl->AddRectFilled(ImVec2(b0.x - r * 0.07f, b0.y), ImVec2(b1.x + r * 0.07f, b1.y), shade, r * 0.1f);
      dl->AddRectFilled(b0, b1, color, r * 0.08f);
      dl->AddRectFilled(b0, ImVec2(b1.x, b0.y + bh * 0.2f), shade, r * 0.08f, ImDrawFlags_RoundCornersTop);
    }
    break;
  }
  case Category::SnesGames:
  {
    // A Super Nintendo pad: the body, the D-pad and four buttons.
    const float w = r * 1.9f, h = r * 0.95f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f),
                      color, h * 0.5f);
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 dp(c.x - w * 0.27f, c.y);
    const float arm = h * 0.3f, thick = h * 0.11f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    const ImVec2 bp(c.x + w * 0.27f, c.y);
    const float d = h * 0.22f, br = h * 0.11f;
    dl->AddCircleFilled(ImVec2(bp.x + d, bp.y), br, hole, 16);
    dl->AddCircleFilled(ImVec2(bp.x - d, bp.y), br, hole, 16);
    dl->AddCircleFilled(ImVec2(bp.x, bp.y + d), br, hole, 16);
    dl->AddCircleFilled(ImVec2(bp.x, bp.y - d), br, hole, 16);
    break;
  }
  case Category::NesGames:
  {
    // A NES pad: a flat box, the D-pad, SELECT and START, B and A.
    const float w = r * 1.9f, h = r * 0.82f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), color,
                      h * 0.12f);
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 dp(c.x - w * 0.3f, c.y);
    const float arm = h * 0.32f, thick = h * 0.11f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    for (int i = 0; i < 2; i++)
    {
      const float sx = c.x - w * 0.07f + i * w * 0.14f;
      dl->AddRectFilled(ImVec2(sx - w * 0.05f, c.y + h * 0.04f), ImVec2(sx + w * 0.05f, c.y + h * 0.15f), hole,
                        h * 0.05f);
    }
    dl->AddCircleFilled(ImVec2(c.x + w * 0.22f, c.y + h * 0.06f), h * 0.15f, hole, 16);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.38f, c.y + h * 0.06f), h * 0.15f, hole, 16);
    break;
  }
  case Category::N64Games:
  {
    // An N64 pad: three handles under a wide body, the stick in the middle one.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.9f, h = r * 0.62f, top = c.y - r * 0.5f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, top), ImVec2(c.x + w * 0.5f, top + h), color, h * 0.45f);
    for (int i = -1; i <= 1; i++)
    {
      const float hx = c.x + i * w * 0.36f, hw = r * 0.32f;
      dl->AddRectFilled(ImVec2(hx - hw * 0.5f, top + h * 0.6f), ImVec2(hx + hw * 0.5f, c.y + r * 0.75f), color,
                        hw * 0.45f);
    }
    dl->AddCircleFilled(ImVec2(c.x, top + h * 0.85f), r * 0.12f, hole, 16);
    const ImVec2 dp(c.x - w * 0.33f, top + h * 0.45f);
    const float arm = h * 0.3f, thick = h * 0.1f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.27f, top + h * 0.55f), h * 0.13f, hole, 12);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.37f, top + h * 0.35f), h * 0.13f, hole, 12);
    break;
  }
  case Category::GbGames:
  case Category::GbcGames:
  {
    // A Game Boy standing up: the screen, the D-pad, B and A, SELECT and START. The first one's
    // bottom right corner is the round one; the Color is a little smaller, its corners all alike.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const bool first = category == Category::GbGames;
    const float w = r * (first ? 1.2f : 1.1f), h = r * (first ? 1.9f : 1.75f);
    const ImVec2 b0(c.x - w * 0.5f, c.y - h * 0.5f), b1(c.x + w * 0.5f, c.y + h * 0.5f);
    const float corner = first ? w * 0.34f : w * 0.14f, small = w * 0.1f;
    // Three pieces that don't overlap (the colour is see-through when the icon isn't selected).
    dl->AddRectFilled(b0, ImVec2(b1.x, b1.y - corner), color, small, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(b0.x, b1.y - corner), ImVec2(b1.x - corner, b1.y), color, first ? small : corner,
                      ImDrawFlags_RoundCornersBottomLeft);
    const ImVec2 arc(b1.x - corner, b1.y - corner);
    dl->PathLineTo(arc);
    dl->PathArcTo(arc, corner, 0.0f, IM_PI * 0.5f, 12);
    dl->PathFillConvex(color);
    dl->AddRectFilled(ImVec2(b0.x + w * 0.13f, b0.y + h * 0.08f), ImVec2(b1.x - w * 0.13f, b0.y + h * 0.47f), hole,
                      w * 0.05f);
    const ImVec2 dp(b0.x + w * 0.28f, b0.y + h * 0.66f);
    const float arm = w * 0.15f, thick = w * 0.05f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(b1.x - w * 0.2f, b0.y + h * 0.62f), w * 0.085f, hole, 16);
    dl->AddCircleFilled(ImVec2(b1.x - w * 0.4f, b0.y + h * 0.69f), w * 0.085f, hole, 16);
    for (int i = 0; i < 2; i++)
    {
      const float sx = c.x - w * 0.16f + i * w * 0.2f;
      dl->AddRectFilled(ImVec2(sx, b0.y + h * 0.82f), ImVec2(sx + w * 0.12f, b0.y + h * 0.85f), hole, h * 0.015f);
    }
    break;
  }
  case Category::GbaGames:
  {
    // A Game Boy Advance: a wide, round body, the screen in the middle, the D-pad and A and B.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.95f, h = r * 1.05f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), color,
                      h * 0.42f);
    dl->AddRectFilled(ImVec2(c.x - w * 0.24f, c.y - h * 0.34f), ImVec2(c.x + w * 0.24f, c.y + h * 0.26f), hole,
                      h * 0.06f);
    const ImVec2 dp(c.x - w * 0.36f, c.y - h * 0.04f);
    const float arm = h * 0.17f, thick = h * 0.06f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.4f, c.y - h * 0.12f), h * 0.09f, hole, 16);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.31f, c.y + h * 0.04f), h * 0.09f, hole, 16);
    break;
  }
  case Category::Ps1Games:
  {
    // A PlayStation pad: the body with its two grips, the D-pad, the four buttons and two sticks.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.9f, h = r * 0.7f, top = c.y - r * 0.45f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, top), ImVec2(c.x + w * 0.5f, top + h), color, h * 0.45f);
    for (int side : {-1, 1})
    {
      const float gx = c.x + side * w * 0.33f, gw = r * 0.46f;
      dl->AddRectFilled(ImVec2(gx - gw * 0.5f, top + h * 0.45f), ImVec2(gx + gw * 0.5f, c.y + r * 0.7f), color,
                        gw * 0.48f);
    }
    const ImVec2 dp(c.x - w * 0.32f, top + h * 0.42f);
    const float arm = h * 0.27f, thick = h * 0.09f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    const ImVec2 bp(c.x + w * 0.32f, top + h * 0.42f);
    const float d = h * 0.22f, br = h * 0.1f;
    for (const ImVec2 at : {ImVec2(bp.x, bp.y - d), ImVec2(bp.x + d, bp.y), ImVec2(bp.x, bp.y + d), ImVec2(bp.x - d, bp.y)})
      dl->AddCircleFilled(at, br, hole, 12);
    dl->AddCircleFilled(ImVec2(c.x - w * 0.13f, top + h * 0.85f), h * 0.15f, hole, 16);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.13f, top + h * 0.85f), h * 0.15f, hole, 16);
    break;
  }
  case Category::NdsGames:
  {
    // A DS, open: the two halves on their hinge, a screen in each, the D-pad and the buttons below.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.35f, h = r * 0.88f, gap = r * 0.08f;
    const ImVec2 t0(c.x - w * 0.5f, c.y - h - gap * 0.5f), t1(c.x + w * 0.5f, c.y - gap * 0.5f);
    const ImVec2 b0(c.x - w * 0.5f, c.y + gap * 0.5f), b1(c.x + w * 0.5f, c.y + h + gap * 0.5f);
    dl->AddRectFilled(t0, t1, color, h * 0.14f);
    dl->AddRectFilled(b0, b1, color, h * 0.14f);
    dl->AddRectFilled(ImVec2(c.x - w * 0.3f, t0.y + h * 0.16f), ImVec2(c.x + w * 0.3f, t1.y - h * 0.16f), hole,
                      h * 0.05f);
    dl->AddRectFilled(ImVec2(c.x - w * 0.24f, b0.y + h * 0.16f), ImVec2(c.x + w * 0.24f, b1.y - h * 0.16f), hole,
                      h * 0.05f);
    const ImVec2 dp(c.x - w * 0.39f, b0.y + h * 0.42f);
    const float arm = h * 0.11f, thick = h * 0.04f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.39f, b0.y + h * 0.42f), h * 0.06f, hole, 12);
    break;
  }
  case Category::PspGames:
  {
    // A PSP: the long rounded body, its wide screen in the middle, the D-pad and the stick on the
    // left, the four buttons on the right.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 2.0f, h = r * 0.9f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), color,
                      h * 0.48f);
    dl->AddRectFilled(ImVec2(c.x - w * 0.25f, c.y - h * 0.32f), ImVec2(c.x + w * 0.25f, c.y + h * 0.32f), hole,
                      h * 0.05f);
    const ImVec2 dp(c.x - w * 0.37f, c.y - h * 0.1f);
    const float arm = h * 0.13f, thick = h * 0.045f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(c.x - w * 0.37f, c.y + h * 0.24f), h * 0.07f, hole, 12);
    const ImVec2 bp(c.x + w * 0.37f, c.y - h * 0.02f);
    const float d = h * 0.13f, br = h * 0.055f;
    for (const ImVec2 at : {ImVec2(bp.x, bp.y - d), ImVec2(bp.x + d, bp.y), ImVec2(bp.x, bp.y + d), ImVec2(bp.x - d, bp.y)})
      dl->AddCircleFilled(at, br, hole, 12);
    break;
  }
  case Category::DcGames:
  {
    // A Dreamcast: the square console seen from above, its lid with the swirl, the front's stripe
    // with the power button and the four controller ports.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.75f, h = r * 1.5f;
    const ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f), p1(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(p0, p1, color, h * 0.12f);
    dl->AddCircleFilled(ImVec2(c.x, c.y - h * 0.08f), h * 0.3f, hole, 32);  // the lid
    dl->AddCircle(ImVec2(c.x, c.y - h * 0.08f), h * 0.17f, color, 32, h * 0.06f);  // the swirl
    dl->AddRectFilled(ImVec2(p0.x + w * 0.08f, p1.y - h * 0.2f), ImVec2(p1.x - w * 0.08f, p1.y - h * 0.08f), hole,
                      h * 0.03f);  // the front's stripe
    for (int k = 0; k < 4; k++)
      dl->AddRectFilled(ImVec2(p0.x + w * (0.14f + k * 0.12f), p1.y - h * 0.17f),
                        ImVec2(p0.x + w * (0.22f + k * 0.12f), p1.y - h * 0.11f), color);
    dl->AddCircleFilled(ImVec2(p1.x - w * 0.18f, p1.y - h * 0.14f), h * 0.035f, color, 12);
    break;
  }
  case Category::Sg1000Games:
  {
    // An SG-1000 seen from above: the console with its cartridge slot, the front's line and the
    // power switch, and the joystick wired to it beside it (its base, the stick and the ball).
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 p0(c.x - r * 0.95f, c.y - r * 0.42f), p1(c.x + r * 0.25f, c.y + r * 0.42f);
    dl->AddRectFilled(p0, p1, color, r * 0.07f);
    dl->AddRectFilled(ImVec2(p0.x + r * 0.2f, p0.y + r * 0.12f), ImVec2(p1.x - r * 0.2f, p0.y + r * 0.26f), hole,
                      r * 0.03f);
    dl->AddRectFilled(ImVec2(p0.x + r * 0.1f, c.y + r * 0.1f), ImVec2(p1.x - r * 0.1f, c.y + r * 0.16f), hole);
    dl->AddRectFilled(ImVec2(p1.x - r * 0.32f, c.y + r * 0.23f), ImVec2(p1.x - r * 0.14f, c.y + r * 0.32f), hole);
    const float jx = c.x + r * 0.68f;
    dl->AddRectFilled(ImVec2(jx - r * 0.27f, c.y + r * 0.08f), ImVec2(jx + r * 0.27f, c.y + r * 0.42f), color,
                      r * 0.06f);
    dl->AddRectFilled(ImVec2(jx - r * 0.07f, c.y - r * 0.27f), ImVec2(jx + r * 0.07f, c.y + r * 0.08f), color);
    dl->AddCircleFilled(ImVec2(jx, c.y - r * 0.41f), r * 0.14f, color, 16);
    break;
  }
  case Category::SmsGames:
  {
    // A Master System seen from above: the wide box, the cartridge slot at the back, the ribs in front
    // and the PAUSE and RESET buttons.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 p0(c.x - r * 0.95f, c.y - r * 0.5f), p1(c.x + r * 0.95f, c.y + r * 0.5f);
    dl->AddRectFilled(p0, p1, color, r * 0.08f);
    dl->AddRectFilled(ImVec2(c.x - r * 0.45f, p0.y + r * 0.14f), ImVec2(c.x + r * 0.45f, p0.y + r * 0.28f), hole,
                      r * 0.03f);
    for (int k = 0; k < 4; k++)
    {
      const float y = c.y + r * (0.02f + k * 0.11f);
      dl->AddRectFilled(ImVec2(p0.x + r * 0.14f, y), ImVec2(c.x + r * 0.22f, y + r * 0.05f), hole);
    }
    for (int k = 0; k < 2; k++)
    {
      const float x = c.x + r * (0.46f + k * 0.24f);
      dl->AddRectFilled(ImVec2(x, c.y + r * 0.1f), ImVec2(x + r * 0.15f, c.y + r * 0.25f), hole, r * 0.02f);
    }
    break;
  }
  case Category::GgGames:
  {
    // A Game Gear: the wide, round body, the big screen in its frame, the D-pad, buttons 1 and 2 and
    // START.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 2.0f, h = r * 1.16f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), color,
                      h * 0.31f);
    dl->AddRectFilled(ImVec2(c.x - r * 0.46f, c.y - r * 0.44f), ImVec2(c.x + r * 0.46f, c.y + r * 0.4f), hole,
                      r * 0.06f);  // the frame
    dl->AddRectFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.32f), ImVec2(c.x + r * 0.32f, c.y + r * 0.22f), hole,
                      r * 0.03f);  // the screen in it
    const ImVec2 dp(c.x - r * 0.74f, c.y + r * 0.02f);
    const float arm = r * 0.16f, thick = r * 0.055f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.62f, c.y + r * 0.1f), r * 0.085f, hole, 16);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.82f, c.y - r * 0.02f), r * 0.085f, hole, 16);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.78f, c.y - r * 0.3f), r * 0.045f, hole, 12);
    break;
  }
  case Category::MdGames:
  {
    // A Mega Drive seen from the front: the low, wide body, the round top raised on it with the
    // cartridge slot, the front's stripe, the power and reset buttons on the right. (The round top from
    // above was a no-entry sign.)
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 top(c.x - r * 0.14f, c.y - r * 0.1f);
    const ImVec2 p0(c.x - r * 0.97f, top.y), p1(c.x + r * 0.97f, c.y + r * 0.42f);
    dl->AddRectFilled(p0, p1, color, r * 0.12f);
    // (The top's upper half only, on the body: no overlap, the colour can be see-through.)
    dl->PathEllipticalArcTo(top, ImVec2(r * 0.64f, r * 0.38f), 0.0f, IM_PI, IM_PI * 2.0f, 32);
    dl->PathFillConvex(color);
    dl->AddEllipseFilled(ImVec2(top.x, top.y - r * 0.12f), ImVec2(r * 0.4f, r * 0.11f), hole, 0.0f, 24);
    dl->AddRectFilled(ImVec2(p0.x + r * 0.12f, p1.y - r * 0.17f), ImVec2(p1.x - r * 0.12f, p1.y - r * 0.1f), hole);
    for (int k = 0; k < 2; k++)
    {
      const float x = c.x + r * (0.56f + k * 0.2f);
      dl->AddRectFilled(ImVec2(x, top.y + r * 0.08f), ImVec2(x + r * 0.13f, top.y + r * 0.18f), hole, r * 0.02f);
    }
    break;
  }
  case Category::ArcadeGames:
  {
    // An arcade cabinet seen from the front: the tall body with its marquee on top, the slanted
    // monitor in its bezel, the control panel with the stick and two buttons, the coin door below.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.3f, h = r * 2.0f;
    const ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f), p1(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(p0, p1, color, h * 0.05f);
    dl->AddRectFilled(ImVec2(p0.x + w * 0.1f, p0.y + h * 0.05f), ImVec2(p1.x - w * 0.1f, p0.y + h * 0.15f), hole,
                      h * 0.02f);  // the marquee
    dl->AddRectFilled(ImVec2(p0.x + w * 0.14f, p0.y + h * 0.22f), ImVec2(p1.x - w * 0.14f, p0.y + h * 0.5f), hole,
                      h * 0.03f);  // the monitor
    dl->AddRectFilled(ImVec2(p0.x + w * 0.08f, p0.y + h * 0.56f), ImVec2(p1.x - w * 0.08f, p0.y + h * 0.66f), hole,
                      h * 0.02f);  // the control panel
    const ImVec2 stick(p0.x + w * 0.3f, p0.y + h * 0.61f);
    dl->AddRectFilled(ImVec2(stick.x - w * 0.025f, stick.y - h * 0.1f), ImVec2(stick.x + w * 0.025f, stick.y), color);
    dl->AddCircleFilled(ImVec2(stick.x, stick.y - h * 0.11f), w * 0.07f, color, 12);  // the stick's ball
    for (int k = 0; k < 2; k++)
      dl->AddCircleFilled(ImVec2(p0.x + w * (0.58f + k * 0.2f), stick.y), w * 0.06f, color, 12);  // the buttons
    dl->AddRectFilled(ImVec2(c.x - w * 0.14f, p1.y - h * 0.22f), ImVec2(c.x + w * 0.14f, p1.y - h * 0.1f), hole,
                      h * 0.015f);  // the coin door
    break;
  }
  case Category::ThreedsGames:
  {
    // A 3DS, open: two halves on their hinge, the wide top screen over the narrower touch screen, the
    // Circle Pad and the D-pad at the touch screen's left, the four buttons at its right.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.75f, h = r * 0.9f, gap = r * 0.07f;
    const ImVec2 t0(c.x - w * 0.5f, c.y - h - gap * 0.5f), t1(c.x + w * 0.5f, c.y - gap * 0.5f);
    const ImVec2 b0(c.x - w * 0.5f, c.y + gap * 0.5f), b1(c.x + w * 0.5f, c.y + h + gap * 0.5f);
    dl->AddRectFilled(t0, t1, color, h * 0.2f);
    dl->AddRectFilled(b0, b1, color, h * 0.2f);
    dl->AddRectFilled(ImVec2(c.x - w * 0.36f, t0.y + h * 0.14f), ImVec2(c.x + w * 0.36f, t1.y - h * 0.14f), hole,
                      h * 0.04f);  // the top screen (400x240)
    dl->AddRectFilled(ImVec2(c.x - w * 0.288f, b0.y + h * 0.14f), ImVec2(c.x + w * 0.288f, b1.y - h * 0.14f), hole,
                      h * 0.04f);  // the touch screen (320x240)
    dl->AddCircleFilled(ImVec2(b0.x + w * 0.1f, b0.y + h * 0.36f), h * 0.13f, hole, 16);  // the Circle Pad
    const ImVec2 dp(b0.x + w * 0.1f, b0.y + h * 0.75f);
    const float arm = h * 0.09f, thick = h * 0.03f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    const ImVec2 bp(b1.x - w * 0.1f, b0.y + h * 0.4f);
    const float d = h * 0.11f, br = h * 0.045f;
    for (const ImVec2 at : {ImVec2(bp.x, bp.y - d), ImVec2(bp.x + d, bp.y), ImVec2(bp.x, bp.y + d), ImVec2(bp.x - d, bp.y)})
      dl->AddCircleFilled(at, br, hole, 12);
    break;
  }
  case Category::NaomiGames:
  {
    // A NAOMI's cabinet (Sega's wide "candy" cabinet) seen from the front: the body with its round top, the big
    // monitor, the control panel across it with each player's stick and three buttons, the coin door below.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const float w = r * 1.9f, h = r * 1.7f;
    const ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f), p1(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRectFilled(p0, p1, color, w * 0.22f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p0.x + w * 0.14f, p0.y + h * 0.1f), ImVec2(p1.x - w * 0.14f, p0.y + h * 0.52f), hole,
                      w * 0.06f);  // the monitor
    dl->AddRectFilled(ImVec2(p0.x + w * 0.05f, p0.y + h * 0.6f), ImVec2(p1.x - w * 0.05f, p0.y + h * 0.73f), hole,
                      h * 0.02f);  // the control panel
    for (int player = 0; player < 2; player++)
    {
      const float x = c.x + (player == 0 ? -0.38f : 0.08f) * w, y = p0.y + h * 0.665f;
      dl->AddCircleFilled(ImVec2(x, y), r * 0.09f, color, 12);  // the stick's ball
      for (int k = 0; k < 3; k++)
        dl->AddCircleFilled(ImVec2(x + w * (0.11f + k * 0.07f), y), r * 0.06f, color, 10);  // the buttons
    }
    dl->AddRectFilled(ImVec2(c.x - w * 0.12f, p1.y - h * 0.19f), ImVec2(c.x + w * 0.12f, p1.y - h * 0.07f), hole,
                      h * 0.015f);  // the coin door
    break;
  }
  case Category::AtomiswaveGames:
  {
    // Atomiswave's wave: a disc with a wave across it (the board's name), as a sign.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    dl->AddCircleFilled(c, r * 0.92f, color, 40);
    ImVec2 wave[25];
    for (int k = 0; k < 25; k++)
    {
      const float t = static_cast<float>(k) / 24.0f;
      wave[k] = ImVec2(c.x - r * 0.68f + t * r * 1.36f, c.y - std::sin(t * IM_PI * 2.0f) * r * 0.3f);
    }
    dl->AddPolyline(wave, 25, hole, ImDrawFlags_None, r * 0.17f);
    break;
  }
  case Category::Favorites:
    DrawStar(dl, c, r * 0.95f, color);
    break;
  case Category::SavedData:
    DrawCartridgeIcon(dl, c, s * 0.9f, color, IM_COL32(0, 0, 0, 110));
    break;
  case Category::Themes:
  {
    // A palette with paint dots.
    dl->AddCircleFilled(c, r * 0.9f, color, 48);
    const ImU32 hole = IM_COL32(0, 0, 0, 120);
    const float dots[][2] = {{-0.45f, -0.3f}, {0.0f, -0.55f}, {0.45f, -0.3f}, {0.5f, 0.2f}};
    for (const auto& d : dots)
      dl->AddCircleFilled(ImVec2(c.x + d[0] * r, c.y + d[1] * r), r * 0.14f, hole, 20);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.25f, c.y + r * 0.38f), r * 0.2f, hole, 24);
    break;
  }
  }
}

void Menu::Impl::DrawItem(ImDrawList* dl, const Item& item, float y, bool selected, float alpha)
{
  const Theme& theme = CurrentTheme();
  const float x = 470.0f;
  const float icon_h = selected ? 96.0f : 72.0f;
  const float icon_w = icon_h * 3.0f;
  const ImVec2 icon_min(x - icon_w * 0.5f, y - icon_h * 0.5f);
  const ImVec2 icon_max(x + icon_w * 0.5f, y + icon_h * 0.5f);
  const ImU32 text = Rgba(theme.text, alpha);

  if (item.game >= 0)
  {
    float aspect = 0.7f;
    const ImTextureID cover = Cover(item.game, &aspect);
    if (cover != ImTextureID{})
    {
      const float ch = selected ? 150.0f : 112.0f;
      const float cw = ch * aspect;
      const ImVec2 cmin(x - cw * 0.5f, y - ch * 0.5f), cmax(x + cw * 0.5f, y + ch * 0.5f);
      dl->AddRectFilled(ImVec2(cmin.x + 4, cmin.y + 6), ImVec2(cmax.x + 4, cmax.y + 6),
                        IM_COL32(0, 0, 0, static_cast<int>(alpha * 90)), 6.0f);
      dl->AddImageRounded(ImTextureRef(cover), cmin, cmax, ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)), 6.0f);
      if (selected)
        dl->AddRect(cmin, cmax, Rgba(theme.text, alpha * 0.9f), 6.0f, 0, 3.0f);
    }
    else
    {
      // No cover: a cartridge-grey tile with the emblem.
      dl->AddRectFilledMultiColor(icon_min, icon_max, Rgba(0x4A3D86, alpha * 0.95f),
                                  Rgba(0x4A3D86, alpha * 0.95f), Rgba(0x241B4D, alpha * 0.95f),
                                  Rgba(0x241B4D, alpha * 0.95f));
      DrawEmblem(dl, ImVec2(x, y), icon_h * 0.8f, alpha);
      if (selected)
        dl->AddRect(icon_min, icon_max, Rgba(theme.text, alpha * 0.9f), 8.0f, 0, 3.0f);
    }
  }
  else if (item.theme != -1)
  {
    if (item.theme >= 0)
    {
      const Theme& t = THEMES[item.theme];
      dl->AddRectFilledMultiColor(icon_min, icon_max, Rgba(t.top, alpha), Rgba(t.top, alpha),
                                  Rgba(t.bottom, alpha), Rgba(t.bottom, alpha));
      dl->AddLine(ImVec2(icon_min.x + 10, y + 6), ImVec2(icon_max.x - 10, y - 8), Rgba(t.wave, alpha),
                  4.0f);
    }
    else
    {
      dl->AddRectFilled(icon_min, icon_max, Rgba(0x404650, alpha), 8.0f);
      Text(dl, m_bold, 26, ImVec2(x - 40, y - 15), Rgba(0xFFFFFF, alpha), "PNG", false);
    }
    dl->AddRect(icon_min, icon_max, Rgba(theme.text, alpha * (selected ? 0.9f : 0.4f)), 6.0f, 0,
                selected ? 3.0f : 1.5f);
  }
  else if (item.save >= 0)
  {
    DrawCartridgeIcon(dl, ImVec2(x, y), icon_h * 0.9f, Rgba(0x8A90A0, alpha), Rgba(0xFFFFFF, alpha * 0.85f));
  }
  else
  {
    dl->AddCircle(ImVec2(x, y), icon_h * 0.32f, text, 32, 4.0f);
    dl->AddCircleFilled(ImVec2(x, y), icon_h * 0.12f, text, 20);
  }

  const float tx = x + icon_w * 0.5f + 36.0f;
  const float title_size = selected ? 46.0f : 36.0f;
  Text(dl, m_bold, title_size, ImVec2(tx, y - title_size * 0.62f), text,
       Fit(m_bold, title_size, item.label, W - tx - 80));
  float sx = tx;
  const float sub_y = y + title_size * 0.42f;
  if (item.game >= 0)
  {
    // The console's badge: SNES (purple), SUPER FAMICOM (red) for Japanese games; NES, FAMICOM.
    const Game& game = m_games[item.game];
    sx += DrawBadge(dl, game.rom, ImVec2(sx, sub_y + 2), alpha) + 16;
    if (m_favorites.count(game.rom.key))
    {
      DrawStar(dl, ImVec2(sx + 12, sub_y + 17), 16.0f, Rgba(0xFFD34D, alpha), Rgba(0x7A5A00, alpha));
      sx += 44;
    }
  }
  if (!item.detail.empty())
    Text(dl, m_font, 26.0f, ImVec2(sx, sub_y + 1), Rgba(theme.text, alpha * 0.75f), item.detail);
}

void Menu::Impl::DrawSidePanel(ImDrawList* dl, float dt)
{
  Approach(m_panel.slide, m_panel.open ? 1.0f : 0.0f, 16.0f, dt);
  if (m_panel.slide < 0.01f)
    return;
  const Theme& theme = CurrentTheme();
  const float width = m_panel.rows.empty() ? 640.0f : 880.0f;
  const float x0 = W - width * m_panel.slide;
  const bool light = theme.text != 0xFFFFFF;
  dl->AddRectFilled(ImVec2(x0, 0), ImVec2(W, H),
                    light ? Rgba(0xFFFFFF, 0.95f * m_panel.slide) : Rgba(0x000000, 0.90f * m_panel.slide));
  dl->AddLine(ImVec2(x0, 0), ImVec2(x0, H), Rgba(theme.wave, 0.8f * m_panel.slide), 2.0f);
  const ImU32 text = Rgba(theme.text, m_panel.slide);
  Text(dl, m_bold, 40, ImVec2(x0 + 50, 140), text, Fit(m_bold, 40, m_panel.title, width - 100));
  if (m_panel.setting == SET_FIND_COVER && m_preview != ImTextureID{})
  {
    // The selected picture, top right (190 tall at most, its own shape).
    const float box_w = 300.0f, box_h = 190.0f;
    float w = box_w, h = box_w / std::max(0.2f, m_preview_aspect);
    if (h > box_h)
    {
      h = box_h;
      w = box_h * m_preview_aspect;
    }
    const ImVec2 p0(W - 40 - w, 24 + (box_h - h) * 0.5f);
    dl->AddRectFilled(ImVec2(p0.x - 4, p0.y - 4), ImVec2(p0.x + w + 4, p0.y + h + 4), Rgba(0x000000, 0.5f * m_panel.slide), 6.0f);
    dl->AddImage(ImTextureRef(m_preview), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, static_cast<int>(255 * m_panel.slide)));
  }
  float y = 230;
  if (!m_panel.options.empty())
  {
    for (size_t i = 0; i < m_panel.options.size(); i++, y += 62)
    {
      const bool sel = static_cast<int>(i) == m_panel.selected;
      if (sel)
      {
        dl->AddRectFilled(ImVec2(x0 + 30, y - 6), ImVec2(W - 30, y + 52),
                          Rgba(theme.wave, 0.30f * m_panel.slide), 10.0f);
      }
      Text(dl, sel ? m_bold : m_font, 34, ImVec2(x0 + 60, y), text, m_panel.options[i]);
    }
    // What the selected option does, at the bottom.
    if (m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.options.size()))
    {
      const std::string help = OptionHelp(m_panel.options[m_panel.selected]);
      if (!help.empty())
      {
        const std::string wrapped = WrapText(m_font, 25, PS4Lang::T(help), width - 100);
        dl->AddText(m_font, 25, ImVec2(x0 + 50, H - 176.0f), Rgba(theme.text, 0.8f * m_panel.slide),
                    wrapped.c_str());
      }
    }
  }
  // A long panel (About) closer, clear of the button hints at the bottom.
  const float line_step = m_panel.lines.size() > 18 ? 38.0f : 42.0f;
  for (const std::string& line : m_panel.lines)
  {
    Text(dl, m_font, 28, ImVec2(x0 + 50, y), text, Fit(m_font, 28, line, width - 90));
    y += line_step;
  }
  if (!m_panel.rows.empty())
  {
    // Settings list, scrolling with the selection; the setting's help text below it.
    const float row_h = 58.0f, top = 220.0f, bottom = H - 200.0f;
    const float visible = (bottom - top) / row_h;
    const float target =
        std::clamp(static_cast<float>(m_panel.selected) - visible * 0.5f + 1.0f, 0.0f,
                   std::max(0.0f, static_cast<float>(m_panel.rows.size()) - visible));
    Approach(m_panel.scroll, target, 16.0f, dt);
    dl->PushClipRect(ImVec2(x0, top - 8), ImVec2(W, bottom + 8), true);
    for (size_t i = 0; i < m_panel.rows.size(); i++)
    {
      const SettingRow& row = m_panel.rows[i];
      const float ry = top + (static_cast<float>(i) - m_panel.scroll) * row_h;
      if (ry < top - row_h || ry > bottom)
        continue;
      if (row.header)
      {
        if (row.label.empty())
          continue;
        Text(dl, m_bold, 26, ImVec2(x0 + 50, ry + 14), Rgba(theme.wave, m_panel.slide),
             Fit(m_bold, 26, row.label, width - 300), false);
        if (!row.value.empty())
        {
          const float vw = TextWidth(m_bold, 26, row.value);
          Text(dl, m_bold, 26, ImVec2(W - 70 - vw, ry + 14), Rgba(theme.wave, m_panel.slide),
               row.value, false);
        }
        dl->AddLine(ImVec2(x0 + 50, ry + 50), ImVec2(W - 50, ry + 50),
                    Rgba(theme.wave, 0.35f * m_panel.slide), 1.5f);
        continue;
      }
      const bool sel = static_cast<int>(i) == m_panel.selected;
      if (sel)
      {
        dl->AddRectFilled(ImVec2(x0 + 30, ry - 4), ImVec2(W - 30, ry + 50),
                          Rgba(theme.wave, 0.30f * m_panel.slide), 10.0f);
      }
      float label_x = x0 + 60;
      if (row.icon != ImTextureID{})
      {
        dl->AddImage(ImTextureRef(row.icon), ImVec2(label_x, ry + 1), ImVec2(label_x + 52, ry + 46),
                     ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * m_panel.slide)));
        label_x += 66;
      }
      constexpr float GLYPH_H = 38.0f;
      const float gw = row.glyph.empty() || (sel && m_capture) ? 0.0f : PS4Glyphs::Width(row.glyph, GLYPH_H);
      float value_w = 0.0f;
      if (row.tag != 0)
        value_w = TextWidth(m_bold, 24, row.tag > 0 ? "ON" : "OFF") + 30;
      else if (gw > 0.0f)
        value_w = gw;
      else
        value_w = TextWidth(m_font, 28, row.value);
      Text(dl, sel ? m_bold : m_font, sel ? 32.0f : 30.0f, ImVec2(label_x, ry + 4), text,
           Fit(sel ? m_bold : m_font, sel ? 32.0f : 30.0f, row.label, W - 90 - value_w - label_x - 20));
      if (row.tag != 0)
      {
        const char* label = row.tag > 0 ? "ON" : "OFF";
        const float right = W - 70, w = value_w;
        const ImU32 fill = row.tag > 0 ? Rgba(0x28AA5A, 0.9f * m_panel.slide) :
                                         Rgba(0xD23746, 0.9f * m_panel.slide);
        dl->AddRectFilled(ImVec2(right - w, ry + 6), ImVec2(right, ry + 42), fill, 18.0f);
        Text(dl, m_bold, 24, ImVec2(right - w + 15, ry + 11), Rgba(0xFFFFFF, m_panel.slide), label);
      }
      else if (gw > 0.0f)
      {
        PS4Glyphs::Draw(dl, m_bold, row.glyph, W - 70 - gw, ry + 23, GLYPH_H, m_panel.slide);
      }
      else
      {
        const ImU32 colour = row.note ? Rgba(0xFFC870, m_panel.slide) : text;
        Text(dl, m_font, 28, ImVec2(W - 70 - value_w, ry + 7), colour, row.value);
      }
    }
    dl->PopClipRect();
    if (m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.rows.size()))
    {
      const SettingRow& row = m_panel.rows[m_panel.selected];
      std::string help;
      if (m_capture)
        help = "Press the DualShock 4 button for " + row.label + "...  (5 s: cancel)";
      else if (row.key.rfind("ctl:", 0) == 0)
      {
        // The Arcade's buttons are FinalBurn Neo's generic ones (Buttons 1 to 10): which of them a Neo Geo
        // or a six-button fighting game uses is said here, on the buttons' rows.
        const auto& bindings = Controls::Bindings(m_controls_system);
        const size_t index = static_cast<size_t>(std::atoi(row.key.c_str() + 4));
        const bool arcade_button =
            m_controls_system == SystemId::ARCADE && index < bindings.size() && !bindings[index].stick_group;
        help = arcade_button ?
                   "Cross: press a new button. Left: none. Neo Geo games: A, B, C, D are Buttons 3, 1, 4, 2 (Square, "
                   "Cross, Triangle, Circle). Six-button fighters: punches 3, 4, 6; kicks 1, 2, 5." :
                   "Cross: press a new button for it. Left: none. D-Pad: Left/Right to choose.";
        // A NAOMI's or Atomiswave's buttons (Flycast's: the cabinet's own numbers), TEST and SERVICE, Gun Reload.
        if (IsFlycastBoard(m_controls_system) && index < bindings.size())
        {
          const std::string key = bindings[index].key;
          if (key == "l3")
            help = "Cross: press a new button. Left: none. TEST opens the board's operator menu (coins, difficulty, "
                   "the game's settings); the Arcade Button of the L3+R3 menu presses it too.";
          else if (key == "r3")
            help = "Cross: press a new button. Left: none. SERVICE gives a credit; in the operator menu it moves to "
                   "the next line (TEST chooses it).";
          else if (key == "reload")
            help = "Cross: press a new button. Left: none. A light gun game's reload (the gun shot off the screen); "
                   "the left stick aims.";
          else if (bindings[index].stick_group)
            help = "D-Pad: Left/Right to choose. A game with a wheel, a flight stick or a gun has that on the left "
                   "stick, not the D-pad (unless Left Stick is chosen).";
          else if (m_controls_system == SystemId::NAOMI)
            help = "Cross: press a new button. Left: none. Fighting games: punches 1, 2, 3, kicks 4, 5, 6 (as on a "
                   "cabinet: punches on Square, Triangle, R1, kicks on Cross, Circle, R2).";
          else
            help = "Cross: press a new button. Left: none. Fighting games: punches 1 and 2, kicks 4 and 5, 3 their "
                   "special move.";
        }
        // Fast Forward: every console has the row (None at first where all the buttons are the console's).
        if (index < bindings.size() && std::string(bindings[index].key) == "fast_forward")
          help = "Cross: press a new button. Left: none. Held, the game runs faster (how fast: Fast Forward in the "
                 "game's settings). A button the console uses too does both.";
      }
      else if (row.key == "__ctlreset__")
        help = "Back to the default layout.";
      else if (row.key == "__ctlscope__")
        help = "All Games: the normal layout. This Game: its own.";
      else if (row.key.rfind("dir:", 0) == 0)
        help = "Cross: open this folder.";
      else if (row.key == "find:query")
        help = "Cross: type another name to look for (the game's name in English often finds more).";
      else if (row.key == "find:kind")
        help = "Box art, or the game's title screen (for games without a box picture).";
      else if (row.key.rfind("pick:", 0) == 0)
        help = "Cross: this picture becomes the game's cover (the selected one shows at the top).";
      else if (row.key == "__closest__")
        help = "Downloads for each game here its libretro picture, or else the one most like its name when "
               "it is sure of it. The doubtful ones stay here: Find Cover shows them first, with a preview.";
      else if (row.key.rfind("nocover:", 0) == 0)
        help = "Cross: choose this game's cover (Find Cover). Circle there brings you back here.";
      else if (row.key.rfind("folder:", 0) == 0)
        help = "Cross: choose a folder of this console's games (its subfolders count too). Roms/<console> on a "
               "USB drive and in /data/EmuSuNi are looked in anyway.";
      else if (row.key == "shown:mode")
        help = "Which consoles get a tab (L1 / R1) and an XMB column: all of them, only those with games, or "
               "only the ones turned on below. When none would be left, all of them are shown.";
      else if (row.key.rfind("shown:", 0) == 0)
        help = "Cross: this console's tab on or off (that chooses Only the Ones I Choose). Its games stay in All "
               "and in Favorites.";
      else if (row.key == "order:preset:company")
        help = "Nintendo's consoles, then Sony's, then Sega's, each from the most basic to the most powerful, "
               "then the arcade boards: the Arcade, the NAOMI and the Atomiswave.";
      else if (row.key == "order:preset:power")
        help = "From the most basic console to the most powerful, whoever made it (the Arcade's boards, from the "
               "1970s to the 2000s, after the Dreamcast, then the NAOMI and the Atomiswave; the Nintendo 3DS last).";
      else if (row.key == "order:preset:year")
        help = "In the order they came out in Japan: NES and SG-1000 1983, Master System 1985, Mega Drive 1988, "
               "Game Boy 1989, Game Gear, the Arcade (by the Neo Geo) and Super Nintendo 1990, PlayStation 1994, "
               "Nintendo 64 1996, Game Boy Color, Dreamcast and NAOMI 1998, Game Boy Advance 2001, Atomiswave 2003, "
               "Nintendo DS and PSP 2004, Nintendo 3DS 2011.";
      else if (row.key == "order:preset:alpha")
        help = "By name, from A to Z.";
      else if (row.key.rfind("order:", 0) == 0)
        help = "Cross picks this console up; Up / Down move it; Cross again puts it down. Left / Right move it at once.";
      else if (row.key.rfind("ctlsys:", 0) == 0)
        help = "Cross: this console's buttons on the DualShock 4.";
      else if (row.key == "usedir")
        help = "Cross: look for games in this folder and its subfolders.";
      else if (row.key.rfind("del:", 0) == 0)
        help = "Cross: delete it.";
      else if (row.key.rfind("cheat:", 0) == 0 || row.key.rfind("cheatall:", 0) == 0)
        help = "Applies the next time the game starts, or now from the in-game menu (L3+R3).";
      else if (row.key == "cheatfind")
        help = "Looks for this game's cheats in libretro's collection (RetroArch's): you choose the "
               "ones to add. The collection (about 35 MB) is downloaded the first time.";
      else if (row.key.rfind("cheatfile:", 0) == 0)
        help = "Cross: see this list's cheats and choose the ones to add.";
      else if (row.key == "pickadd")
        help = "Adds the chosen cheats to this game's list, off: turn on the ones you want there.";
      else if (row.key.rfind("pick:", 0) == 0)
        help = "Cross chooses this cheat to add, or leaves it out.";
      else if (row.scope)
        help = "All Games: what you change here applies to every game. This Console: to every game of "
               "this console. This Game: only to this game.";
      else if (row.key == "__toall__")
        help = "Copies this game's settings to every game; this game follows them again.";
      else if (row.key == "__dups__")
        help = "Deletes the extra copies of each game and keeps one: the USA version (else the best "
               "other one), the best dump, the latest revision.";
      else if (row.key.rfind("__usaonly__:", 0) == 0)
        help = "Deletes this console's games of other regions (Europe, Japan...). Games without a "
               "region in their name stay.";
      else if (row.key.rfind("dupfile:", 0) == 0)
        help = row.note ? "Delete Duplicates deletes this file." : "This version stays.";
      else if (row.key.rfind("otherfile:", 0) == 0)
        help = "Keep Only USA Games deletes this file.";
      // Translated piece by piece: a setting's help, then what its current choice does.
      std::string shown = PS4Lang::T(help);
      if (const Settings::Setting* setting = help.empty() ? Settings::Find(row.key) : nullptr)
      {
        shown = PS4Lang::T(setting->help);
        // Fast Forward on a game's page: the button that does it in that game (its controls or its console's).
        if (row.key == "fast_forward" && m_panel.system >= 0)
          shown = PS4Lang::T(Controls::FastForwardHelp(m_panel.game_id, static_cast<SystemId>(m_panel.system)));
        if (!setting->live)
          shown += std::string(" ") + PS4Lang::T("(Next game start.)");
        const Settings::Choice* choice = Settings::FindChoice(*setting, Settings::Get(row.key, m_panel.game_id));
        // Quality on a game's page: what the level does on that console.
        const std::string level = choice && row.key == "quality" && m_panel.system >= 0 ?
                                      Settings::QualityHelp(choice->value, static_cast<SystemId>(m_panel.system)) :
                                      std::string();
        if (!level.empty())
          shown += std::string("\n") + PS4Lang::T(choice->label) + ": " + PS4Lang::T(level);
        else if (choice && choice->help)
          shown += std::string("\n") + PS4Lang::T(choice->label) + ": " + PS4Lang::T(choice->help);
      }
      const std::string wrapped = WrapText(m_font, 25, shown, width - 100);
      dl->AddText(m_font, 25, ImVec2(x0 + 50, bottom + 24), Rgba(theme.text, 0.8f * m_panel.slide),
                  wrapped.c_str());
    }
  }
}

void Menu::Impl::DrawHints(ImDrawList* dl)
{
  const Theme& theme = CurrentTheme();
  const ImU32 text = Rgba(theme.text, 0.85f);
  float x = W - 120;
  const float y = H - 80;
  // XMB: a shade under the hints, the list runs behind them.
  if (!Library())
  {
    const uint32_t shade = theme.text != 0xFFFFFF ? 0xFFFFFF : 0x000000;
    dl->AddRectFilledMultiColor(ImVec2(0, H - 150), ImVec2(W, H), Rgba(shade, 0.0f), Rgba(shade, 0.0f),
                                Rgba(shade, 0.8f), Rgba(shade, 0.8f));
  }
  auto hint = [&](const char* label, const char* glyph) {
    const float tw = TextWidth(m_font, 28, label);
    x -= tw;
    Text(dl, m_font, 28, ImVec2(x, y - 17), text, label);
    const float gw = PS4Glyphs::Width(glyph, 36);
    x -= gw + 12;
    PS4Glyphs::Draw(dl, m_bold, glyph, x, y, 36, 0.9f);
    x -= 40;
  };
  if (m_panel.open)
  {
    hint("Back", "Circle");
    if (!m_panel.options.empty())
      hint("Select", "Cross");
    if (!m_panel.rows.empty())
      hint(m_panel.setting == SET_NO_COVER ? "Select" : "Change", "Cross");
  }
  else if (m_keyboard)
  {
    hint("Done", "R2");
    hint("Clear All", "L2");
    hint("Space", "Triangle");
    hint("Delete", "Square");
    hint("Close", "Circle");
    hint("Browse Games", "R1");
    x += 32.0f - PS4Glyphs::Width("L1", 36);
    PS4Glyphs::Draw(dl, m_bold, "L1", x, y, 36, 0.9f);
    x -= 40.0f;
    hint("Type", "Cross");
  }
  else
  {
    const auto& items = m_items[m_category];
    const bool game = !items.empty() && items[m_selected[m_category]].game >= 0;
    if (IsGameList(m_category))
    {
      if (!m_search.empty())
        hint("Clear Search", "Circle");
      hint("Search", "R2");
      if (game)
        hint("Favorite", "L2");
      hint("Refresh", "Square");
      hint("Sort", "Options");
    }
    if (IsSettingsPage(m_category))
      hint("Back", "Circle");
    if (game)
      hint("Options", "Triangle");
    hint(game ? "Play" : "Select", "Cross");
  }
}

void Menu::Impl::DrawTopBar(ImDrawList* dl)
{
  const Theme& theme = CurrentTheme();
  DrawEmblem(dl, ImVec2(104, 79), 64, 0.95f);
  float x = 152.0f;
  for (const char ch : std::string("EMUSUNI"))
  {
    const std::string letter(1, ch);
    Text(dl, m_bold, 30, ImVec2(x, 62), Rgba(theme.text, 0.95f), letter);
    x += TextWidth(m_bold, 30, letter) + 7.0f;
  }
  // The version under the name.
  Text(dl, m_font, 22, ImVec2(154, 100), Rgba(theme.text, 0.6f), std::string("v") + App::VERSION, false);
}

// A Library page's title: the page's icon (its console's, Favorites' star, Settings' gear...), its
// name beside it (a console's full name: the tabs show short ones) and a line under the name.
void Menu::Impl::DrawPageTitle(ImDrawList* dl, Category category, const std::string& title,
                               const std::string& sub, float alpha, float sub_alpha)
{
  const Theme& theme = CurrentTheme();
  constexpr float X = 90.0f, Y = 150.0f, ICON = 76.0f;
  DrawCategoryIcon(dl, category, ImVec2(X + ICON * 0.5f, Y + 46.0f), ICON, Rgba(theme.text, alpha));
  const float x = X + ICON + 26.0f;
  Text(dl, m_bold, 50, ImVec2(x, Y), Rgba(theme.text, alpha), title);
  Text(dl, m_bold, 26, ImVec2(x + 2, Y + 64), Rgba(theme.wave, sub_alpha), sub, false);
}

// The rows of a console's box (DrawConsoleInfo): what it plays, the BIOS it needs, its files, and its folders -
// Roms/<key> (found there by themselves) and, when it is another one, its folder of Game Folders.
std::vector<std::pair<std::string, std::string>> Menu::Impl::ConsoleInfo(SystemId id) const
{
  const System& system = GetSystem(id);
  const ConsoleNote* note = nullptr;
  for (const ConsoleNote& each : CONSOLE_NOTES)
  {
    if (each.id == id)
      note = &each;
  }
  std::string files;
  if (note && note->files)
  {
    files = note->files;
  }
  else if (std::find(system.extensions.begin(), system.extensions.end(), ".zip") != system.extensions.end())
  {
    files = "One .zip or .7z per game, not unpacked";  // (a romset console without a note of its own)
  }
  else
  {
    for (const std::string& extension : system.extensions)
      files += (files.empty() ? "" : " ") + extension;
    files += system.by_path ? ", not in a .zip" : ", or in a .zip";
  }
  std::vector<std::pair<std::string, std::string>> rows = {{"Plays", note ? note->plays : system.name}};
  if (note && note->bios)
    rows.push_back({"BIOS", note->bios});
  rows.push_back({"Files", files});
  // The folders written out in full, one per place, as the app makes them and looks in them (MakeFolders,
  // Library::FindPlaces) in their own spelling: on a USB drive as a PC shows it ("USB: Roms/psp", with the drive
  // when there are several), in the PS4's storage as FTP shows it ("PS4: /data/EmuSuNi/Roms/psp"); then the
  // console's own folder of Game Folders when it is another one. Without a drive, where it will be made.
  std::vector<std::pair<std::string, std::string>> on_drives;  // (path on the drive, the drive's name)
  std::vector<std::string> lines;
  const auto found = m_places.folders.find(system.key);
  for (const std::string& folder : found == m_places.folders.end() ? std::vector<std::string>{} : found->second)
  {
    if (folder.rfind(m_data_dir + "/", 0) == 0)
      continue;  // (below)
    for (const std::string& drive : m_places.drives)
    {
      if (folder.rfind(drive + "/", 0) == 0)
        on_drives.push_back({folder.substr(drive.size() + 1), std::filesystem::path(drive).filename().string()});
    }
  }
  for (const auto& [path, drive] : on_drives)
    lines.push_back("USB: " + path + (on_drives.size() > 1 ? " (" + drive + ")" : std::string()));
  if (on_drives.empty())
    lines.push_back("USB: Roms/" + std::string(system.key));
  const size_t usb_lines = lines.size();
  for (const std::string& folder : found == m_places.folders.end() ? std::vector<std::string>{} : found->second)
  {
    if (folder.rfind(m_data_dir + "/", 0) == 0)
      lines.push_back("PS4: " + folder);
  }
  if (lines.size() == usb_lines)
    lines.push_back("PS4: " + m_data_dir + "/Roms/" + system.key);
  const std::string own = GamesFolder(id);
  if (!Library::IsRomsFolder(own, system.key, m_data_dir))
    lines.push_back("Also: " + own);
  for (size_t i = 0; i < lines.size(); i++)
    rows.push_back({i == 0 ? "Folders" : "", lines[i]});
  return rows;
}

// A console's box at the bottom left of its page (Library style): under the covers' row (their feet at 706-720),
// above the hints' line (972), as wide as leaves the selected game's title room in the middle (DrawLibraryGames
// moves a long one right of it). The help texts' 25 px and the headers' bold 26 px, sizes the font atlas has
// already (each size drawn is one more set of glyphs in it); its text made once for each console, language and
// folder. Its right edge (0: not drawn).
float Menu::Impl::DrawConsoleInfo(ImDrawList* dl, SystemId id, float alpha)
{
  if (alpha <= 0.01f)
    return 0.0f;
  constexpr float X = 64.0f, BOX_W = 620.0f, BOTTOM = 962.0f, TOP_MOST = 724.0f, SIZE = 25.0f, LABEL = 26.0f,
                  LINE = 30.0f, PAD = 14.0f;
  // (Its rows change with the language, the drives and Game Folders: wrapped again when they do.)
  const auto rows = ConsoleInfo(id);
  std::string key = PS4Lang::Current().code;
  for (const auto& [label, text] : rows)
    key += '\n' + label + '\t' + text;
  if (key != m_info_key)
  {
    m_info_key = key;
    m_info_rows.clear();
    float label_w = 0.0f;
    for (const auto& [label, text] : rows)
      label_w = std::max(label_w, TextWidth(m_bold, LABEL, label));
    m_info_label_w = label_w + 16.0f;
    const float text_w = BOX_W - PAD * 2 - m_info_label_w;
    std::vector<std::pair<std::string, std::vector<std::string>>> wrapped_rows;  // (its label in English, lines)
    for (const auto& [label, text] : rows)
    {
      std::vector<std::string> lines;
      std::stringstream wrapped(WrapText(m_font, SIZE, PS4Lang::T(text), text_w));
      for (std::string line; std::getline(wrapped, line);)
        lines.push_back(line);
      wrapped_rows.push_back({label, lines});
    }
    // More lines than the box holds (several drives, a folder of the player's, a console with long notes): what it
    // plays gives way. The BIOS (the Arcade's, the NAOMI's, the Atomiswave's: where it goes), the files and the
    // folders always stay - the notes are short enough for two drives and a folder of the player's, and more than
    // that makes the box taller (below).
    const int fit = static_cast<int>((BOTTOM - TOP_MOST - PAD * 2 + (LINE - SIZE)) / LINE);
    auto count = [&wrapped_rows] {
      int n = 0;
      for (const auto& row : wrapped_rows)
        n += std::max(1, static_cast<int>(row.second.size()));
      return n;
    };
    if (count() > fit)
      wrapped_rows.erase(std::remove_if(wrapped_rows.begin(), wrapped_rows.end(),
                                        [](const auto& row) { return row.first == "Plays"; }),
                         wrapped_rows.end());
    for (auto& [label, lines] : wrapped_rows)
      m_info_rows.push_back({PS4Lang::T(label), std::move(lines)});
  }
  // Every line of the rows left: more than fit under the covers only with three drives or more (or a long folder of
  // Game Folders) - then the box is taller, over the covers' feet, rather than without a folder.
  int lines = 0;
  for (const auto& row : m_info_rows)
    lines += std::max(1, static_cast<int>(row.second.size()));
  const float top = BOTTOM - PAD * 2 - static_cast<float>(lines) * LINE + (LINE - SIZE);
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  dl->AddRectFilled(ImVec2(X, top), ImVec2(X + BOX_W, BOTTOM),
                    light ? Rgba(0xFFFFFF, 0.6f * alpha) : Rgba(0x000000, 0.4f * alpha), 14.0f);
  dl->AddRect(ImVec2(X, top), ImVec2(X + BOX_W, BOTTOM), Rgba(theme.wave, 0.45f * alpha), 14.0f, 0, 1.5f);
  float y = top + PAD;
  int drawn = 0;
  for (const auto& [label, row_lines] : m_info_rows)
  {
    if (drawn >= lines)
      break;
    if (!label.empty())
      dl->AddText(m_bold, LABEL, ImVec2(X + PAD, y - 1.0f), Rgba(theme.wave, alpha), label.c_str());
    const size_t count = std::max<size_t>(1, row_lines.size());
    for (size_t i = 0; i < count && drawn < lines; i++, drawn++, y += LINE)
    {
      if (i < row_lines.size())
        dl->AddText(m_font, SIZE, ImVec2(X + PAD + m_info_label_w, y), Rgba(theme.text, 0.92f * alpha),
                    row_lines[i].c_str());
    }
  }
  return X + BOX_W;
}

void Menu::Impl::Draw(float dt)
{
  if (Library())
    DrawLibraryBackground();
  else
    DrawBackground();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Theme& theme = CurrentTheme();

  Approach(m_category_pos, static_cast<float>(ColumnIndex(m_category)), 12.0f, dt);
  for (int c = 0; c < CATEGORY_COUNT; c++)
    Approach(m_item_pos[c], static_cast<float>(m_selected[c]), 14.0f, dt);
  if (m_category != m_drawn_category)
  {
    if (m_drawn_category >= 0 && ColumnOf(m_category) == ColumnOf(m_drawn_category))
      m_page_alpha = 0.0f;  // into a page of Settings, or back to its list
    m_drawn_category = m_category;
  }
  Approach(m_page_alpha, 1.0f, 10.0f, dt);

  if (Library())
  {
    DrawLibrary(dl, dt);
  }
  else
  {
    // Categories: the selected one sits at x = 470, the others spaced to the left and right.
    const float row_y = 250.0f;
    const float spacing = 240.0f;
    const auto& columns = Columns();
    for (int i = 0; i < static_cast<int>(columns.size()); i++)
    {
      const int c = columns[i];
      const float offset = static_cast<float>(i) - m_category_pos;
      const float x = 470.0f + offset * spacing;
      const float focus = std::max(0.0f, 1.0f - std::abs(offset));
      const float size = 92.0f + 34.0f * focus;
      const float alpha = 0.55f + 0.45f * focus;
      DrawCategoryIcon(dl, static_cast<Category>(c), ImVec2(x, row_y), size, Rgba(theme.text, alpha));
      if (focus > 0.05f)
      {
        // Under Settings' icon, the name of its page when one is open.
        const char* name = CATEGORY_NAMES[c == ColumnOf(m_category) ? m_category : c];
        const float tw = TextWidth(m_font, 32, name);
        Text(dl, m_font, 32, ImVec2(x - tw * 0.5f, row_y + 76), Rgba(theme.text, focus), name);
      }
    }
    const int c = m_category;
    const float switch_alpha =
        std::max(0.0f, 1.0f - std::abs(m_category_pos - ColumnIndex(c)) * 2.0f) * m_page_alpha;
    const auto& items = m_items[c];
    for (int i = 0; i < static_cast<int>(items.size()); i++)
    {
      const float rel = static_cast<float>(i) - m_item_pos[c];
      float y;
      float alpha;
      if (rel >= -0.5f)
      {
        y = 470.0f + rel * 120.0f + (rel > 0.5f ? 40.0f : rel * 80.0f);
        alpha = rel < 0.5f ? 1.0f : std::max(0.0f, 0.85f - (rel - 1.0f) * 0.12f);
      }
      else
      {
        y = 120.0f + (rel + 1.0f) * 100.0f;
        alpha = std::max(0.0f, 0.55f + (rel + 1.0f) * 0.2f);
      }
      if (y < -80.0f || y > H + 80.0f || alpha <= 0.01f)
        continue;
      DrawItem(dl, items[i], y, i == m_selected[c], alpha * switch_alpha);
    }
    if (m_prefs.clock)
    {
      const std::time_t now = std::time(nullptr);
      std::tm local = {};
      localtime_r(&now, &local);
      char clock[32];
      std::strftime(clock, sizeof(clock), "%H:%M", &local);
      const float tw = TextWidth(m_bold, 40, clock);
      Text(dl, m_bold, 40, ImVec2(W - 110 - tw, 70), Rgba(theme.text, 0.95f), clock);
    }
    DrawEmblem(dl, ImVec2(110, 90), 70, 0.9f);
    // The version under the emblem.
    const std::string version = std::string("v") + App::VERSION;
    Text(dl, m_font, 22, ImVec2(110 - TextWidth(m_font, 22, version) * 0.5f, 132), Rgba(theme.text, 0.6f),
         version, false);
  }

  if (m_keyboard_ask)
  {
    DrawSidePanel(dl, dt);
    DrawKeyboard(dl, dt);
  }
  else
  {
    DrawKeyboard(dl, dt);
    DrawSidePanel(dl, dt);
  }
  DrawToast(dl, dt);
  if (Library() && m_panel.slide < 0.5f)
    DrawLibraryHints(dl);
  else
    DrawHints(dl);

  // Fades: in at start, out when a game launches.
  if (m_launch >= 0)
  {
    m_launch_time += dt;
    m_fade = std::min(1.0f, m_launch_time / 0.7f);
  }
  else
  {
    Approach(m_fade, 0.0f, 6.0f, dt);
  }
  if (m_fade > 0.005f)
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, static_cast<int>(m_fade * 255)));
  DrawIntro(dl, dt);
}

// Start-up animation (app start and back from a game): the four buttons of the Super Famicom pad
// drop in and bounce into their diamond, a ring of light bursts out, the EmuSuNi letters rise
// under them, then it all dissolves into the menu. Any button skips it.
void Menu::Impl::DrawIntro(ImDrawList* dl, float dt)
{
  if (!m_intro)
    return;
  if (m_intro_time == 0.0f)
    Play("intro");
  m_intro_time += dt;
  const float t = m_intro_time;
  constexpr float LAND = 0.9f, OUT_START = 2.1f, END = 2.7f;
  if (t >= END)
  {
    m_intro = false;
    return;
  }
  auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
  auto smooth = [](float v) { return v * v * (3.0f - 2.0f * v); };
  auto alpha = [](float a) { return static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f); };
  const float out = smooth(clamp01((t - OUT_START) / (END - OUT_START)));
  const float vis = 1.0f - out;

  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(14, 8, 34, alpha(vis)),
                              IM_COL32(14, 8, 34, alpha(vis)), IM_COL32(0, 0, 0, alpha(vis)),
                              IM_COL32(0, 0, 0, alpha(vis)));
  // The buttons' diamond (340 tall), a gap and the letters, centred on the screen together.
  const float letters_h = m_logo != ImTextureID{} ? 760.0f / m_logo_aspect : 150.0f;
  const float group_top = (H - (340.0f + 60.0f + letters_h)) * 0.5f;
  const ImVec2 centre(W * 0.5f, group_top + 170.0f);
  const float letters_y = group_top + 340.0f + 60.0f;
  auto hash = [](int i, float k) { return std::abs(std::fmod(std::sin(i * k) * 43758.5453f, 1.0f)); };
  auto light = [&](float x, float y, float radius, uint32_t rgb, float a) {
    if (a <= 0.004f || m_glow == ImTextureID{})
      return;
    dl->AddImage(ImTextureRef(m_glow), ImVec2(x - radius, y - radius), ImVec2(x + radius, y + radius),
                 ImVec2(0, 0), ImVec2(1, 1), Rgba(rgb, a));
  };
  // Pixels of light drifting up, in the four colours.
  for (int i = 0; i < 140; i++)
  {
    const float x = hash(i, 12.9898f) * W;
    const float speed = 40.0f + 90.0f * hash(i, 4.1f);
    const float y = H - std::fmod(hash(i, 78.233f) * H + t * speed, H + 40.0f);
    const float size = 3.0f + 5.0f * hash(i, 3.3f);
    const float twinkle = 0.4f + 0.6f * std::sin(t * (3.0f + 3.0f * hash(i, 7.7f)) + i);
    const ImU32 colour = Rgba(BUTTON_COLORS[i % 4], 0.5f * twinkle * vis * clamp01(t / 0.4f));
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), colour);
  }
  // The four buttons fall from above, one after the other, and bounce into place.
  const float size = 340.0f * (1.0f + 0.15f * out);
  const float r = size * 0.2f, d = size * 0.3f;
  const ImVec2 at[4] = {ImVec2(centre.x + d, centre.y), ImVec2(centre.x, centre.y + d),
                        ImVec2(centre.x - d, centre.y), ImVec2(centre.x, centre.y - d)};
  const int order[4] = {3, 0, 1, 2};  // top, right, bottom, left
  for (int k = 0; k < 4; k++)
  {
    const int i = order[k];
    const float start = 0.05f + k * 0.12f;
    const float q = clamp01((t - start) / (LAND - 0.3f));
    // A fall with two small bounces.
    float drop;
    if (q < 0.6f)
      drop = 1.0f - (q / 0.6f) * (q / 0.6f);
    else if (q < 0.85f)
      drop = 0.12f * std::sin((q - 0.6f) / 0.25f * 3.14159f);
    else
      drop = 0.04f * std::sin((q - 0.85f) / 0.15f * 3.14159f);
    const ImVec2 p(at[i].x, at[i].y - drop * H * 0.7f);
    if (q <= 0.0f)
      continue;
    light(p.x, p.y, r * 2.6f, BUTTON_COLORS[i], 0.25f * vis);
    dl->AddCircleFilled(ImVec2(p.x + r * 0.1f, p.y + r * 0.14f), r, IM_COL32(0, 0, 0, alpha(0.5f * vis)), 40);
    dl->AddCircleFilled(p, r, Rgba(BUTTON_COLORS[i], vis), 40);
    dl->AddCircleFilled(ImVec2(p.x - r * 0.3f, p.y - r * 0.32f), r * 0.3f,
                        IM_COL32(255, 255, 255, alpha(0.35f * vis)), 24);
  }
  // A ring of light when the last one lands.
  const float ring_t = t - (0.05f + 3 * 0.12f + (LAND - 0.3f) * 0.6f);
  if (ring_t > 0.0f)
  {
    const float q = clamp01(ring_t / 0.8f);
    dl->AddCircle(centre, size * (0.6f + 1.4f * q), IM_COL32(255, 255, 255, alpha((1.0f - q) * 0.6f * vis)),
                  96, 6.0f * (1.0f - q) + 1.0f);
    for (int i = 0; i < 48; i++)
    {
      const float ang = i * 6.2832f / 48 + hash(i, 2.1f);
      const float rr = size * (0.5f + 1.3f * q) * (0.8f + 0.4f * hash(i, 5.5f));
      light(centre.x + std::cos(ang) * rr, centre.y + std::sin(ang) * rr, 8.0f, BUTTON_COLORS[i % 4],
            (1.0f - q) * vis);
    }
  }
  // The letters rise into place under the buttons.
  const float q = smooth(clamp01((t - 1.05f) / 0.45f));
  if (m_logo != ImTextureID{})
  {
    const float tw = 760.0f, th = tw / m_logo_aspect;
    const ImVec2 p(W * 0.5f - tw / 2, letters_y + 30.0f * (1.0f - q));
    dl->AddImage(ImTextureRef(m_logo), p, ImVec2(p.x + tw, p.y + th), ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, alpha(q * vis)));
  }
  else
  {
    const float tw = TextWidth(m_bold, 120, "EmuSuNi");
    dl->AddText(m_bold, 120, ImVec2(W * 0.5f - tw * 0.5f, letters_y + 30.0f * (1.0f - q)),
                IM_COL32(255, 255, 255, alpha(q * vis)), "EmuSuNi");
  }
  // The version, small, at the bottom right.
  const std::string version = std::string("v") + App::VERSION;
  const float vw = TextWidth(m_font, 28, version.c_str());
  dl->AddText(m_font, 28, ImVec2(W - vw - 48.0f, H - 64.0f), IM_COL32(255, 255, 255, alpha(0.55f * q * vis)),
              version.c_str());
}

// ---------------------------------------------------------------------------------------------
// The Library style.

void Menu::Impl::DrawLibraryBackground()
{
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const Theme& theme = CurrentTheme();
  if (m_wallpaper != ImTextureID{})
  {
    dl->AddImage(ImTextureRef(m_wallpaper), ImVec2(0, 0), ImVec2(W, H));
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 110));
    return;
  }
  const ImU32 top = Rgba(theme.top), bottom = Rgba(theme.bottom);
  const float horizon = 600.0f;
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, horizon), bottom, bottom, top, top);
  dl->AddRectFilledMultiColor(ImVec2(0, horizon), ImVec2(W, H), top, top, bottom, bottom);
  for (int r = 0; r < 3; r++)
  {
    const float base = 210.0f + r * 95.0f, amplitude = 50.0f + r * 18.0f;
    const float k = 2.0f * 3.14159265f / (1500.0f + r * 400.0f);
    const float speed = (r % 2 == 0 ? 0.12f : -0.09f), phase = r * 2.1f;
    constexpr float STEP = 32.0f;
    ImVec2 prev_top, prev_bottom;
    for (float x = -STEP; x <= W + STEP; x += STEP)
    {
      const float y = base + amplitude * std::sin(x * k + m_time * speed + phase);
      const float thick = 70.0f + 40.0f * std::sin(x * k * 0.7f + m_time * 0.2f + phase);
      const ImVec2 p_top(x, y), p_bottom(x, y + thick);
      if (x > -STEP)
      {
        const float mid = 1.0f - std::min(1.0f, std::abs(x - W * 0.5f) / (W * 0.62f));
        const float a = 0.055f * mid;
        QuadMultiColor(dl, prev_top, p_top, p_bottom, prev_bottom, Rgba(theme.wave, 0.0f),
                       Rgba(theme.wave, 0.0f), Rgba(theme.wave, a), Rgba(theme.wave, a));
      }
      prev_top = p_top;
      prev_bottom = p_bottom;
    }
  }
  dl->AddRectFilledMultiColor(ImVec2(0, horizon), ImVec2(W, H), Rgba(theme.wave, 0.10f),
                              Rgba(theme.wave, 0.10f), Rgba(theme.wave, 0.0f), Rgba(theme.wave, 0.0f));
  for (int i = 0; i < 8; i++)
  {
    dl->AddEllipseFilled(ImVec2(W * 0.5f, 728.0f), ImVec2(160.0f + i * 70.0f, 26.0f + i * 9.0f),
                         Rgba(theme.wave, 0.028f), 0.0f, 64);
  }
  for (int i = 0; i < 6; i++)
  {
    dl->AddEllipseFilled(ImVec2(W * 0.5f, horizon), ImVec2(300.0f + i * 140.0f, 110.0f + i * 40.0f),
                         Rgba(theme.wave, 0.022f), 0.0f, 64);
  }
  {
    const ImU32 clear = Rgba(theme.wave, 0.0f), bright = Rgba(theme.wave, 0.55f);
    QuadMultiColor(dl, ImVec2(0, horizon - 1), ImVec2(W * 0.5f, horizon - 1), ImVec2(W * 0.5f, horizon + 1),
                   ImVec2(0, horizon + 1), clear, bright, bright, clear);
    QuadMultiColor(dl, ImVec2(W * 0.5f, horizon - 1), ImVec2(W, horizon - 1), ImVec2(W, horizon + 1),
                   ImVec2(W * 0.5f, horizon + 1), bright, clear, clear, bright);
    const ImU32 haze = Rgba(theme.wave, 0.12f);
    QuadMultiColor(dl, ImVec2(W * 0.15f, horizon - 18), ImVec2(W * 0.5f, horizon - 18),
                   ImVec2(W * 0.5f, horizon), ImVec2(W * 0.15f, horizon), clear, clear, haze, clear);
    QuadMultiColor(dl, ImVec2(W * 0.5f, horizon - 18), ImVec2(W * 0.85f, horizon - 18),
                   ImVec2(W * 0.85f, horizon), ImVec2(W * 0.5f, horizon), clear, clear, clear, haze);
  }
  // Square specks of light rising slowly (pixels, for a pixel-art console).
  for (int i = 0; i < 46; i++)
  {
    const float h1 = std::fmod(std::sin(i * 12.9898f) * 43758.5453f, 1.0f);
    const float h2 = std::fmod(std::sin(i * 78.233f) * 12345.6789f, 1.0f);
    const float fx = std::abs(h1), fy = std::abs(h2);
    const float speed = 8.0f + fx * 14.0f;
    const float y = H - std::fmod(fy * H + m_time * speed, H + 40.0f);
    const float x = fx * W + std::sin(m_time * 0.3f + i) * 18.0f;
    const float twinkle = 0.5f + 0.5f * std::sin(m_time * (0.8f + fy) + i * 1.7f);
    const float size = 3.0f + fy * 4.0f;
    const float a = (0.10f + 0.25f * twinkle) * std::clamp(y / H * 1.4f, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(x - size * 1.5f, y - size * 1.5f), ImVec2(x + size * 1.5f, y + size * 1.5f),
                      Rgba(theme.wave, a * 0.18f));
    dl->AddRectFilled(ImVec2(x - size * 0.5f, y - size * 0.5f), ImVec2(x + size * 0.5f, y + size * 0.5f),
                      Rgba(0xFFFFFF, a));
  }
}

// The console's badge with its colour, at `at` (top left), 30 tall: SNES / SUPER FAMICOM, NES /
// FAMICOM (Japanese games). Wider than max_w (when given): the short one. Returns its width.
float Menu::Impl::DrawBadge(ImDrawList* dl, const Library::Game& rom, ImVec2 at, float alpha, float max_w)
{
  const System& system = GetSystem(rom.system);
  const bool japan = rom.region.find("Japan") == 0;
  constexpr float SIZE = 22.0f;  // the size the XMB rows used for their tag (one more atlas size costs)
  const char* label = japan ? system.badge_japan : system.badge;
  float tw = TextWidth(m_bold, SIZE, label);
  if (max_w > 0.0f && tw + 24.0f > max_w)
  {
    label = system.badge;
    tw = TextWidth(m_bold, SIZE, label);
  }
  const ImVec2 end(at.x + tw + 24.0f, at.y + 30.0f);
  dl->AddRectFilled(ImVec2(at.x + 2, at.y + 3), ImVec2(end.x + 2, end.y + 3), IM_COL32(0, 0, 0, static_cast<int>(alpha * 90)),
                    15.0f);
  dl->AddRectFilled(at, end, Rgba(japan ? system.color_japan : system.color, alpha), 15.0f);
  Text(dl, m_bold, SIZE, ImVec2(at.x + 12, at.y + 2), Rgba(0xFFFFFF, alpha), label, false);
  return tw + 24.0f;
}

// A cover (or, without one, a box in the console's colours with the emblem and the title) on the
// quad a-b-c-d (top left, top right, bottom right, bottom left), with its reflection on the floor
// and the console's badge. focus: 2 = the selected game, 1 = the others.
void Menu::Impl::DrawCover(ImDrawList* dl, int index, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d,
                           float alpha, int focus)
{
  const Theme& theme = CurrentTheme();
  const Game& game = m_games[index];
  const int a255 = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
  const ImU32 white = IM_COL32(255, 255, 255, a255);
  const float h = d.y - a.y;
  dl->AddQuadFilled(ImVec2(a.x + 8, a.y + 12), ImVec2(b.x + 8, b.y + 12), ImVec2(c.x + 8, c.y + 12),
                    ImVec2(d.x + 8, d.y + 12), IM_COL32(0, 0, 0, a255 * 45 / 100));
  if (focus == 2)
  {
    for (int i = 6; i >= 1; i--)
    {
      const float g = i * 4.0f;
      dl->AddQuad(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y - g), ImVec2(c.x + g, c.y + g),
                  ImVec2(d.x - g, d.y + g), Rgba(theme.wave, alpha * 0.07f * (7 - i)), 5.0f);
    }
  }
  const ImTextureID cover = Cover(index, nullptr);
  const bool japan = game.rom.region.find("Japan") == 0;
  const System& system = GetSystem(game.rom.system);
  if (cover != ImTextureID{})
  {
    dl->AddImageQuad(ImTextureRef(cover), a, b, c, d, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                     ImVec2(0, 1), white);
    const float r = 0.30f;
    const ImVec2 ra(d.x, d.y + 6), rb(c.x, c.y + 6);
    const ImVec2 rc(c.x, c.y + 6 + (c.y - b.y) * r), rd(d.x, d.y + 6 + (d.y - a.y) * r);
    ImageQuadColors(dl, cover, ra, rb, rc, rd, ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 1 - r),
                    ImVec2(0, 1 - r), IM_COL32(255, 255, 255, a255 * 28 / 100),
                    IM_COL32(255, 255, 255, a255 * 28 / 100), IM_COL32(255, 255, 255, 0),
                    IM_COL32(255, 255, 255, 0));
  }
  else
  {
    // A box in the console's colours: grey with its stripe (purple Super Nintendo, red Super
    // Famicom, slate NES, dark red Famicom).
    QuadMultiColor(dl, a, b, c, d, Rgba(0xD9D8E2, alpha), Rgba(0xD9D8E2, alpha), Rgba(0xA9A8B8, alpha),
                   Rgba(0xA9A8B8, alpha));
    const float stripe = h * 0.16f;
    const uint32_t tone = Shade(japan ? system.color_japan : system.color, 0.85f);
    QuadMultiColor(dl, a, b, ImVec2(b.x, b.y + stripe), ImVec2(a.x, a.y + stripe), Rgba(tone, alpha),
                   Rgba(tone, alpha), Rgba(Shade(tone, 0.72f), alpha), Rgba(Shade(tone, 0.72f), alpha));
    const float cx = (a.x + b.x) * 0.5f, cy = a.y + h * 0.40f, s = (b.x - a.x) * 0.55f;
    DrawEmblem(dl, ImVec2(cx, cy), s, alpha);
    const float size = std::max(18.0f, h * 0.07f);
    dl->AddText(m_bold, size, ImVec2(a.x + 14, a.y + h * 0.66f), IM_COL32(40, 36, 60, a255),
                game.rom.title.c_str(), nullptr, b.x - a.x - 28);
    const std::string label = Upper(japan ? system.name_japan : system.name);
    dl->AddText(m_bold, std::max(12.0f, h * 0.04f), ImVec2(a.x + 14, a.y + stripe * 0.3f),
                IM_COL32(255, 255, 255, a255), label.c_str());
  }
  if (focus == 2)
  {
    dl->AddQuad(a, b, c, d, Rgba(theme.wave, alpha), 5.0f);
    dl->AddQuad(ImVec2(a.x + 4, a.y + 4), ImVec2(b.x - 4, b.y + 4), ImVec2(c.x - 4, c.y - 4),
                ImVec2(d.x + 4, d.y - 4), Rgba(0xFFFFFF, alpha * 0.55f), 1.5f);
  }
  else
  {
    dl->AddQuad(a, b, c, d, Rgba(0x8C6CFF, alpha * 0.6f), 3.0f);
  }
  const bool favorite = m_favorites.count(game.rom.key) > 0;
  // The console's badge on the cover's top left corner (the box without a cover says it already).
  if (cover != ImTextureID{})
    DrawBadge(dl, game.rom, ImVec2(a.x + 12, a.y + 12), alpha, (b.x - a.x) - 24.0f - (favorite ? h * 0.2f : 0.0f));
  if (favorite)
  {
    const float sr = std::max(12.0f, h * 0.07f);
    const ImVec2 sc(b.x - sr * 1.35f, b.y + sr * 1.35f);
    dl->AddCircleFilled(sc, sr * 1.25f, IM_COL32(0, 0, 0, a255 * 55 / 100), 24);
    DrawStar(dl, sc, sr, Rgba(0xFFD34D, alpha), Rgba(0x7A5A00, alpha));
  }
}

void Menu::Impl::DrawLibrary(ImDrawList* dl, float dt)
{
  const Theme& theme = CurrentTheme();
  const int tab = LibraryTab();
  const auto& tabs = Tabs();
  const int TAB_COUNT = static_cast<int>(tabs.size());
  // m_tab_pos is a place on an endless row of the tabs (tab i is at i + any multiple of the count):
  // it moves to the nearest place of the selected tab, so R1 on the last one goes on to the first.
  {
    const float count = static_cast<float>(TAB_COUNT);
    const float target = static_cast<float>(tab) + std::round((m_tab_pos - tab) / count) * count;
    Approach(m_tab_pos, target, 14.0f, dt);
    if (std::abs(m_tab_pos - target) < 0.001f)
      m_tab_pos = static_cast<float>(tab);
  }
  if (m_sort_shown > 0.0f)
    m_sort_shown -= dt;
  DrawTopBar(dl);
  constexpr float TAB_FONT = 28.0f, BAR_Y = 52.0f, BAR_H = 58.0f, PAD = 26.0f, FADE = 150.0f;
  // The tabs between the logo and the clock (with the L1 / R1 signs beside them) as an endless
  // carousel: the selected one in the middle of the bar, the others on both sides round and round,
  // fading out towards the bar's ends.
  const float area_l = 470.0f, area_r = W - (m_prefs.clock ? 310.0f : 165.0f);
  const float center = (area_l + area_r) * 0.5f, half = (area_r - area_l) * 0.5f;
  std::vector<float> tab_w(TAB_COUNT);
  for (int i = 0; i < TAB_COUNT; i++)
    tab_w[i] = TextWidth(m_bold, TAB_FONT, TabName(tabs[i])) + PAD * 2;
  auto wrap = [TAB_COUNT](int i) { return ((i % TAB_COUNT) + TAB_COUNT) % TAB_COUNT; };
  const int base = static_cast<int>(std::floor(m_tab_pos));
  const float frac = m_tab_pos - static_cast<float>(base);
  const float w0 = tab_w[wrap(base)], w1 = tab_w[wrap(base + 1)];
  const float shift = frac * (w0 + w1) * 0.5f;  // how far the row has moved past tab `base`
  dl->AddRectFilled(ImVec2(area_l - 8, BAR_Y - 6), ImVec2(area_r + 8, BAR_Y + BAR_H + 6), Rgba(0x000000, 0.28f),
                    16.0f);
  dl->AddRect(ImVec2(area_l - 8, BAR_Y - 6), ImVec2(area_r + 8, BAR_Y + BAR_H + 6), Rgba(theme.text, 0.14f), 16.0f,
              0, 1.5f);
  {
    const float hw = w0 + (w1 - w0) * frac;
    const ImVec2 h0(center - hw * 0.5f + 2, BAR_Y), h1(center + hw * 0.5f - 2, BAR_Y + BAR_H);
    dl->AddRectFilledMultiColor(h0, h1, Rgba(theme.wave, 0.55f), Rgba(theme.wave, 0.55f),
                                Rgba(theme.wave, 0.30f), Rgba(theme.wave, 0.30f));
    dl->AddRect(h0, h1, Rgba(theme.wave, 0.95f), 12.0f, 0, 2.0f);
  }
  dl->PushClipRect(ImVec2(area_l, BAR_Y - 6), ImVec2(area_r, BAR_Y + BAR_H + 6), true);
  auto draw_tab = [&](int v, float mid) {
    const int i = wrap(v);
    const bool sel = i == tab;
    const float fade = std::clamp((half - std::abs(mid - center)) / FADE, 0.0f, 1.0f);
    Text(dl, sel ? m_bold : m_font, TAB_FONT, ImVec2(mid - tab_w[i] * 0.5f + PAD, BAR_Y + 12),
         Rgba(theme.text, (sel ? 1.0f : 0.72f) * fade), TabName(tabs[i]), sel);
  };
  // To the right from tab `base`, then to the left, until past the bar's ends.
  {
    float mid = center - shift;
    for (int v = base; mid - tab_w[wrap(v)] * 0.5f < area_r; v++)
    {
      draw_tab(v, mid);
      mid += (tab_w[wrap(v)] + tab_w[wrap(v + 1)]) * 0.5f;
    }
    mid = center - shift;
    for (int v = base - 1;; v--)
    {
      mid -= (tab_w[wrap(v + 1)] + tab_w[wrap(v)]) * 0.5f;
      if (mid + tab_w[wrap(v)] * 0.5f <= area_l)
        break;
      draw_tab(v, mid);
    }
  }
  dl->PopClipRect();
  PS4Glyphs::Draw(dl, m_bold, "L1", area_l - 8 - 30 - PS4Glyphs::Width("L1", 40), BAR_Y + BAR_H * 0.5f, 40, 0.9f);
  PS4Glyphs::Draw(dl, m_bold, "R1", area_r + 8 + 30, BAR_Y + BAR_H * 0.5f, 40, 0.9f);
  if (m_prefs.clock)
  {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_r(&now, &local);
    char clock[32];
    std::strftime(clock, sizeof(clock), "%H:%M", &local);
    const float tw = TextWidth(m_bold, 36, clock);
    Text(dl, m_bold, 36, ImVec2(W - 90 - tw, 62), Rgba(theme.text, 0.95f), clock);
  }
  // The page fades in as its tab reaches the middle (the shortest way round).
  float away = m_tab_pos - static_cast<float>(tab);
  away -= std::round(away / static_cast<float>(TAB_COUNT)) * static_cast<float>(TAB_COUNT);
  const float alpha = std::clamp(1.0f - std::abs(away) * 1.6f, 0.0f, 1.0f) * m_page_alpha;
  switch (static_cast<Category>(m_category))
  {
  case Category::Games:
  case Category::SnesGames:
  case Category::NesGames:
  case Category::N64Games:
  case Category::GbGames:
  case Category::GbcGames:
  case Category::GbaGames:
  case Category::Ps1Games:
  case Category::NdsGames:
  case Category::PspGames:
  case Category::DcGames:
  case Category::Sg1000Games:
  case Category::SmsGames:
  case Category::GgGames:
  case Category::MdGames:
  case Category::ArcadeGames:
  case Category::ThreedsGames:
  case Category::NaomiGames:
  case Category::AtomiswaveGames:
  case Category::Favorites:
    DrawLibraryGames(dl, alpha, m_category);
    break;
  case Category::SavedData:
    DrawLibrarySaves(dl, alpha);
    break;
  default:
    DrawLibraryList(dl, alpha);
    break;
  }
}

void Menu::Impl::DrawLibraryGames(ImDrawList* dl, float alpha, int c)
{
  const Theme& theme = CurrentTheme();
  const bool favorites = c == static_cast<int>(Category::Favorites);
  const int list_system = SystemOfList(c);  // a console's tab, else -1
  const auto& items = m_items[c];
  const int count = static_cast<int>(items.size());
  const bool any = count > 0 && items[0].game >= 0;
  // While the search keyboard is up (k: 0 -> 1) the covers move up and shrink above it, and the
  // title under them makes way.
  const float k = m_keyboard_slide;
  const std::string page_title = favorites        ? "Your favorites" :
                                 list_system >= 0 ? GetSystem(static_cast<SystemId>(list_system)).name :
                                                    "Your games";
  std::string sub;
  if (any)
    sub = std::to_string(count) + (count == 1 ? " game" : " games") + "  -  " + SortName();
  else if (m_all_covered[c])
    sub = "0 games  -  " + SortName();
  else if (!m_search.empty())
    sub = "0 games";
  else
    sub = favorites ? "No favorites yet" : "No games yet";
  if (!m_search.empty())
    sub += "  -  Search: " + m_search;
  if (m_dl_thread.joinable() && !m_dl_done)
  {
    sub += "  -  Downloading covers " + std::to_string(m_dl_progress.load()) + " / " +
           std::to_string(m_dl_total.load());
  }
  DrawPageTitle(dl, static_cast<Category>(c), page_title, sub, alpha,
                alpha * (m_sort_shown > 0.0f ? 1.0f : 0.85f));
  if (!any)
  {
    std::string line1;
    std::vector<std::string> lines;  // smaller, under it
    if (m_all_covered[c])
    {
      line1 = m_search.empty() ? "Every game has a cover" : "Every game found has a cover";
      lines = {"Options shows every game again"};
    }
    else if (!m_search.empty())
    {
      line1 = "No games match \"" + m_search + "\"";
      lines = {"Circle clears the search. R2 changes it."};
    }
    else if (favorites)
    {
      line1 = "No favorites yet";
      lines = {"Press L2 on a game in the Library (or Triangle: Add to Favorites)."};
    }
    else if (list_system >= 0)
    {
      // Where the games go (found there by themselves); what goes there - the systems, the BIOS, the files -
      // is in the console's box at the bottom left (DrawConsoleInfo). (No " = " in a text: the language
      // file's separator.)
      const std::string key = GetSystem(static_cast<SystemId>(list_system)).key;
      line1 = "Copy your games to Roms/" + key + " on the USB drive or to " + m_data_dir + "/Roms/" + key;
      lines = {"then press Square to look again (or pick a folder in Settings)."};
      if (list_system == static_cast<int>(SystemId::ARCADE))
        lines.push_back("Only FinalBurn Neo v1.0.0.03 sets work (MAME's often don't).");
      if (list_system == static_cast<int>(SystemId::THREEDS))
        lines.push_back("Light 2D games play well on the PS4; heavy 3D ones run slower than on a 3DS.");
      if (list_system == static_cast<int>(SystemId::NAOMI))
        lines.push_back("MAME sets (mvsc2.zip) or unzipped .dat / .bin images, and naomi.zip, the BIOS, beside them.");
      if (list_system == static_cast<int>(SystemId::ATOMISWAVE))
        lines.push_back("MAME sets, one .zip per game (kofxi.zip), and awbios.zip, the BIOS, beside them.");
    }
    else
    {
      line1 = "Copy your games to their console's folder";
      // Roms/<console> anywhere, the consoles' folder names, then the folders of Game Folders that aren't one
      // of those (and the first ones of the PS4's storage) in rows as wide as the screen allows.
      std::string keys;
      for (SystemId id : ConsoleOrder())
        keys += (keys.empty() ? "" : "   ") + std::string(GetSystem(id).key);
      lines = {"USB: Roms/<console>      PS4: " + m_data_dir + "/Roms/<console>", keys};
      std::string row;
      for (const System& system : Systems())
      {
        if (Library::IsRomsFolder(GamesFolder(system.id), system.key, m_data_dir))
          continue;
        const std::string folder = std::string(system.tab) + ": " + GamesFolder(system.id);
        if (!row.empty() && TextWidth(m_font, 28, "Also: " + row + "      " + folder) > W - 120.0f)
        {
          lines.push_back("Also: " + row);
          row.clear();
        }
        row += (row.empty() ? "" : "      ") + folder;
      }
      if (!row.empty())
        lines.push_back("Also: " + row);
      lines.push_back("then press Square to look again (or pick the folders in Settings).");
    }
    const float y = 470.0f - 160.0f * k;
    if (favorites && m_search.empty() && !m_all_covered[c])
      DrawStar(dl, ImVec2(W * 0.5f, y - 80.0f), 46.0f, Rgba(0xFFD34D, alpha * 0.9f), Rgba(0x7A5A00, alpha));
    Text(dl, m_bold, 36, ImVec2(W * 0.5f - TextWidth(m_bold, 36, line1) * 0.5f, y), Rgba(theme.text, alpha),
         line1);
    for (size_t i = 0; i < lines.size(); i++)
    {
      Text(dl, m_font, 28,
           ImVec2(W * 0.5f - TextWidth(m_font, 28, lines[i]) * 0.5f, y + 60.0f + 48.0f * static_cast<float>(i)),
           Rgba(theme.text, alpha * 0.75f), lines[i]);
    }
    if (list_system >= 0)
      DrawConsoleInfo(dl, static_cast<SystemId>(list_system), alpha * (1.0f - k));
    return;
  }
  {
    char counter[32];
    snprintf(counter, sizeof(counter), "%d / %d", m_selected[c] + 1, count);
    const float cw = TextWidth(m_bold, 28, counter);
    Text(dl, m_bold, 28, ImVec2(W - 90 - cw, 212), Rgba(theme.text, alpha * 0.7f * (1.0f - m_panel.slide)),
         counter, false);
  }
  // The row: neighbours smaller and turned slightly towards the middle, farther ones behind.
  const float pos = m_item_pos[c];
  const int selected = m_selected[c];
  const float BASE_H = 420.0f - 190.0f * k, FLOOR_Y = 720.0f - 230.0f * k;
  const float spread = BASE_H / 420.0f;
  std::vector<std::pair<float, int>> order;
  for (int i = 0; i < count; i++)
  {
    const float d = static_cast<float>(i) - pos;
    if (std::abs(d) < 6.5f)
      order.push_back({std::abs(d), i});
  }
  std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  float selected_top = FLOOR_Y - BASE_H;
  for (const auto& [ad, i] : order)
  {
    const int game = items[i].game;
    const float d = static_cast<float>(i) - pos;
    const float near = std::min(ad, 1.0f), far = std::max(0.0f, ad - 1.0f);
    const float scale = 1.0f - 0.24f * near - 0.05f * far;
    const float h = BASE_H * scale;
    float aspect = 0.72f;
    if (Cover(game, &aspect) != ImTextureID{})
      aspect = std::clamp(aspect, 0.55f, 1.45f);
    else
      aspect = 0.72f;
    const float w = h * aspect;
    const float side = d < 0.0f ? -1.0f : 1.0f;
    const float cx = W * 0.5f + side * (near * 330.0f + far * 230.0f) * spread;
    const float bottom = FLOOR_Y - 14.0f * near * spread;
    const float tilt = h * 0.045f * near;
    ImVec2 a(cx - w * 0.5f, bottom - h), b(cx + w * 0.5f, bottom - h);
    ImVec2 cc(cx + w * 0.5f, bottom), dd(cx - w * 0.5f, bottom);
    if (side > 0)
    {
      b.y += tilt;
      cc.y -= tilt;
    }
    else
    {
      a.y += tilt;
      dd.y -= tilt;
    }
    const float fade = std::clamp((6.0f - ad) / 1.5f, 0.0f, 1.0f);
    const float dim = 1.0f - 0.22f * near;
    DrawCover(dl, game, a, b, cc, dd, alpha * fade * dim, i == selected && ad < 0.5f ? 2 : 1);
    if (i == selected)
      selected_top = a.y;
  }
  const float info_alpha = alpha * (1.0f - k);
  // The console's box at the bottom left, over the covers' reflections (it fades out under the keyboard).
  const float box_right =
      list_system >= 0 ? DrawConsoleInfo(dl, static_cast<SystemId>(list_system), info_alpha) : 0.0f;
  const float pulse = 0.65f + 0.35f * std::sin(m_time * 3.0f);
  const float gw = PS4Glyphs::Width("Triangle", 40);
  if (info_alpha > 0.01f)
    PS4Glyphs::Draw(dl, m_bold, "Triangle", W * 0.5f - gw * 0.5f, selected_top - 44, 40, info_alpha * pulse);
  // The title stays in view above the keyboard: the game found. (Two sizes only: every font size
  // drawn is one more set of glyphs in the font atlas.) Centred, unless that would put it over the
  // console's box: then just right of it (and the line under it too).
  const float left = box_right > 0.0f && k < 0.5f ? box_right + 24.0f : 80.0f, right = W - 80.0f;
  const Game& game = m_games[items[selected].game];
  const float title_size =
      k > 0.5f ? 34.0f : TextWidth(m_bold, 46, game.rom.title) > right - left - 40.0f ? 36.0f : 46.0f;
  const std::string title = Fit(m_bold, title_size, game.rom.title, right - left);
  const float tw = TextWidth(m_bold, title_size, title);
  Text(dl, m_bold, title_size, ImVec2(std::max(left, W * 0.5f - tw * 0.5f), 800.0f - 290.0f * k),
       Rgba(theme.text, alpha), title);
  if (info_alpha < 0.01f)
    return;
  const System& system = GetSystem(game.rom.system);
  std::string info = game.rom.region.find("Japan") == 0 ? system.name_japan : system.name;
  if (!game.rom.region.empty())
    info += "  -  " + game.rom.region;
  info += "  -  " + PlayedText(game.played);
  if (!m_prefs.default_game.empty() && game.rom.path == m_prefs.default_game)
    info += "  -  Starts when EmuSuNi opens";
  const float iw = TextWidth(m_font, 26, info);
  Text(dl, m_font, 26, ImVec2(std::max(left, W * 0.5f - iw * 0.5f), 860), Rgba(theme.text, info_alpha * 0.72f), info);
  const float bw = 210.0f, bh = 62.0f, by = 908.0f;
  const ImVec2 b0(W * 0.5f - bw * 0.5f, by), b1(W * 0.5f + bw * 0.5f, by + bh);
  dl->AddRectFilled(b0, b1, Rgba(theme.wave, info_alpha * 0.18f), 14.0f);
  dl->AddRect(b0, b1, Rgba(theme.wave, info_alpha * 0.95f), 14.0f, 0, 2.5f);
  const float cw = PS4Glyphs::Width("Cross", 38);
  const float pw = TextWidth(m_bold, 32, "Play");
  const float px = W * 0.5f - (cw + 14 + pw) * 0.5f;
  PS4Glyphs::Draw(dl, m_bold, "Cross", px, by + bh * 0.5f, 38, info_alpha);
  Text(dl, m_bold, 32, ImVec2(px + cw + 14, by + 12), Rgba(theme.text, info_alpha), "Play");
}

// The search keyboard: a field with what is typed and how many games match, and the keys. It
// slides up from the bottom (both menu styles).
void Menu::Impl::DrawKeyboard(ImDrawList* dl, float dt)
{
  Approach(m_keyboard_slide, m_keyboard ? 1.0f : 0.0f, 14.0f, dt);
  if (m_keyboard_slide < 0.01f)
    return;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const float a = std::min(1.0f, m_keyboard_slide * 1.5f);
  constexpr float KEY_W = 96.0f, KEY_H = 50.0f, GAP = 8.0f, PAD = 20.0f, FIELD_H = 60.0f;
  const float panel_w = KEY_COLUMNS * KEY_W + (KEY_COLUMNS - 1) * GAP + PAD * 2;
  const float panel_h = PAD + FIELD_H + 14.0f + KEY_ROW_COUNT * KEY_H + (KEY_ROW_COUNT - 1) * GAP + PAD;
  const float bottom = H - 112.0f;  // above the hints
  const float x0 = W * 0.5f - panel_w * 0.5f;
  const float y0 = H + (bottom - panel_h - H) * m_keyboard_slide;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + panel_w, y0 + panel_h),
                    light ? Rgba(0xFFFFFF, 0.94f * a) : Rgba(0x0B0A14, 0.90f * a), 22.0f);
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + panel_w, y0 + panel_h), Rgba(theme.wave, 0.9f * a), 22.0f, 0, 2.5f);

  // The field: a magnifying glass, the text (as typed, not translated) and a blinking cursor.
  const ImVec2 f0(x0 + PAD, y0 + PAD), f1(x0 + panel_w - PAD, y0 + PAD + FIELD_H);
  dl->AddRectFilled(f0, f1, light ? Rgba(0x000000, 0.08f * a) : Rgba(0x000000, 0.45f * a), 12.0f);
  dl->AddRect(f0, f1, Rgba(theme.wave, 0.6f * a), 12.0f, 0, 1.5f);
  const ImVec2 mc(f0.x + 34, f0.y + FIELD_H * 0.5f - 4);
  dl->AddCircle(mc, 12.0f, Rgba(theme.text, 0.85f * a), 20, 3.0f);
  dl->AddLine(ImVec2(mc.x + 9, mc.y + 9), ImVec2(mc.x + 19, mc.y + 19), Rgba(theme.text, 0.85f * a), 4.0f);
  const float tx = f0.x + 66;
  float cursor_x = tx;
  const std::string& typed = m_keyboard_ask ? m_ask_text : m_search;
  if (typed.empty())
  {
    Text(dl, m_font, 30, ImVec2(tx, f0.y + 13), Rgba(theme.text, 0.45f * a),
         m_keyboard_ask ? m_ask_title : std::string("Type a game's name"), false);
  }
  else
  {
    dl->AddText(m_bold, 36, ImVec2(tx, f0.y + 10), Rgba(theme.text, a), typed.c_str());
    cursor_x += m_bold->CalcTextSizeA(36, FLT_MAX, 0.0f, typed.c_str()).x;
  }
  if (std::fmod(m_time, 1.0f) < 0.6f)
    dl->AddRectFilled(ImVec2(cursor_x + 3, f0.y + 13), ImVec2(cursor_x + 6, f1.y - 13), Rgba(theme.wave, a));
  if (m_keyboard_ask)
  {
    const float fw = TextWidth(m_bold, 28, m_ask_title);
    Text(dl, m_bold, 28, ImVec2(f1.x - 24 - fw, f0.y + 15), Rgba(theme.wave, a), m_ask_title, false);
  }
  else if (!m_search.empty())
  {
    const auto& items = m_items[IsGameList(m_category) ? m_category : static_cast<int>(Category::Games)];
    const int found = !items.empty() && items[0].game >= 0 ? static_cast<int>(items.size()) : 0;
    const std::string text = std::to_string(found) + (found == 1 ? " game" : " games");
    const float fw = TextWidth(m_bold, 28, text);
    Text(dl, m_bold, 28, ImVec2(f1.x - 24 - fw, f0.y + 15), Rgba(theme.wave, a), text, false);
  }

  // The keys; the wide ones show the button that does the same.
  auto key = [&](float kx, float ky, float kw, const std::string& label, const char* glyph, bool selected) {
    const ImVec2 k0(kx, ky), k1(kx + kw, ky + KEY_H);
    dl->AddRectFilled(k0, k1, selected ? Rgba(theme.wave, 0.9f * a) : Rgba(theme.text, 0.10f * a), 10.0f);
    if (selected)
      dl->AddRect(k0, k1, Rgba(0xFFFFFF, 0.9f * a), 10.0f, 0, 2.5f);
    const float lw = TextWidth(m_bold, 28, label);
    const float gw = glyph ? PS4Glyphs::Width(glyph, 30) + 10.0f : 0.0f;
    const float lx = kx + kw * 0.5f - (lw + gw) * 0.5f;
    Text(dl, m_bold, 28, ImVec2(lx, ky + 10), selected ? Rgba(0xFFFFFF, a) : Rgba(theme.text, 0.9f * a), label,
         false);
    if (glyph)
      PS4Glyphs::Draw(dl, m_bold, glyph, lx + lw + 10.0f, ky + KEY_H * 0.5f, 30, a);
  };
  float ky = f1.y + 14.0f;
  for (int row = 0; row < KEY_ROW_COUNT; row++, ky += KEY_H + GAP)
  {
    if (row < KEY_ROW_COUNT - 1)
    {
      for (int col = 0; col < KEY_COLUMNS; col++)
      {
        key(x0 + PAD + col * (KEY_W + GAP), ky, KEY_W, std::string(1, KEY_ROWS[row][col]), nullptr,
            row == m_key_row && col == m_key_col);
      }
      continue;
    }
    for (const WideKey& wide : WIDE_KEYS)
    {
      const bool selected = row == m_key_row && WideKeyAt(m_key_col) == static_cast<int>(&wide - WIDE_KEYS);
      key(x0 + PAD + wide.first * (KEY_W + GAP), ky, wide.span * KEY_W + (wide.span - 1) * GAP, wide.label,
          wide.glyph, selected);
    }
  }
}

// A short notice under the tabs ("Added to Favorites"), fading out.
void Menu::Impl::DrawToast(ImDrawList* dl, float dt)
{
  if (m_toast_time <= 0.0f)
    return;
  m_toast_time -= dt;
  const float a = std::clamp(std::min(m_toast_time / 0.3f, (2.2f - m_toast_time) / 0.12f), 0.0f, 1.0f);
  const Theme& theme = CurrentTheme();
  const float tw = TextWidth(m_bold, 30, m_toast);
  const float icon_w = m_toast_icon ? 40.0f : 0.0f;
  const float w = tw + icon_w + 64.0f, h = 56.0f, y = 124.0f;
  const ImVec2 p0(W * 0.5f - w * 0.5f, y), p1(W * 0.5f + w * 0.5f, y + h);
  dl->AddRectFilled(p0, p1, Rgba(0x000000, 0.78f * a), h * 0.5f);
  dl->AddRect(p0, p1, Rgba(theme.wave, 0.9f * a), h * 0.5f, 0, 2.0f);
  if (m_toast_icon == 1)
    DrawStar(dl, ImVec2(p0.x + 46, y + h * 0.5f), 16.0f, Rgba(0xFFD34D, a), Rgba(0x7A5A00, a));
  else if (m_toast_icon == 2)
    DrawStar(dl, ImVec2(p0.x + 46, y + h * 0.5f), 16.0f, Rgba(0x000000, 0.0f), Rgba(0xFFD34D, a));
  Text(dl, m_bold, 30, ImVec2(p0.x + 32 + icon_w, y + 11), Rgba(0xFFFFFF, a), m_toast, false);
}

// Saved Data: the games with saves on the left, the selected game's save states (pictures) on the
// right.
void Menu::Impl::DrawLibrarySaves(ImDrawList* dl, float alpha)
{
  const Theme& theme = CurrentTheme();
  const int c = static_cast<int>(Category::SavedData);
  const auto& items = m_items[c];
  const int selected = m_selected[c];
  DrawPageTitle(dl, Category::SavedData, "Saved Data", "Battery saves and save states, the latest first", alpha,
                alpha * 0.85f);
  constexpr float LIST_X = 90.0f, LIST_W = 700.0f, ROW_H = 118.0f, TOP = 280.0f, BOTTOM = 960.0f;
  const float visible = (BOTTOM - TOP) / ROW_H;
  const float scroll = std::clamp(m_item_pos[c] - visible * 0.5f + 0.5f, 0.0f,
                                  std::max(0.0f, static_cast<float>(items.size()) - visible));
  dl->PushClipRect(ImVec2(0, TOP - 20), ImVec2(W, BOTTOM + 10), true);
  for (int i = 0; i < static_cast<int>(items.size()); i++)
  {
    const Item& item = items[i];
    const float y = TOP + (static_cast<float>(i) - scroll) * ROW_H;
    if (y < TOP - ROW_H || y > BOTTOM)
      continue;
    const bool sel = i == selected;
    const ImVec2 r0(LIST_X, y), r1(LIST_X + LIST_W, y + ROW_H - 18);
    dl->AddRectFilled(r0, r1, sel ? Rgba(theme.wave, alpha * 0.20f) : Rgba(0x000000, alpha * 0.25f), 14.0f);
    dl->AddRect(r0, r1, sel ? Rgba(theme.wave, alpha) : Rgba(theme.text, alpha * 0.12f), 14.0f, 0,
                sel ? 2.5f : 1.5f);
    DrawCartridgeIcon(dl, ImVec2(LIST_X + 58, y + (ROW_H - 18) * 0.5f), 66,
                      sel ? Rgba(theme.wave, alpha) : Rgba(0x8A90A0, alpha), Rgba(0xFFFFFF, alpha * 0.85f));
    Text(dl, m_bold, 32, ImVec2(LIST_X + 110, y + 14), Rgba(theme.text, alpha),
         Fit(m_bold, 32, item.label, LIST_W - 140));
    Text(dl, m_font, 25, ImVec2(LIST_X + 110, y + 58), Rgba(theme.text, alpha * 0.72f), item.detail);
  }
  dl->PopClipRect();

  constexpr float PANE_X = 850.0f, PANE_R = W - 90.0f;
  dl->AddRectFilled(ImVec2(PANE_X, TOP - 20), ImVec2(PANE_R, BOTTOM - 8), Rgba(0x000000, alpha * 0.28f), 18.0f);
  dl->AddRect(ImVec2(PANE_X, TOP - 20), ImVec2(PANE_R, BOTTOM - 8), Rgba(theme.text, alpha * 0.10f), 18.0f, 0, 1.5f);
  if (items.empty() || items[selected].save < 0)
  {
    Text(dl, m_font, 30, ImVec2(PANE_X + 40, TOP + 20), Rgba(theme.text, alpha * 0.8f),
         "Games save here as they do on the console;");
    Text(dl, m_font, 30, ImVec2(PANE_X + 40, TOP + 66), Rgba(theme.text, alpha * 0.8f),
         "L3 + R3 in a game saves its state in a slot.");
    return;
  }
  const Saves::GameSaves& save = m_saves[items[selected].save];
  std::string head = "Save states";
  if (save.sram_time > 0)
    head = "Battery save: " + DateText(save.sram_time);
  Text(dl, m_bold, 30, ImVec2(PANE_X + 40, TOP + 4), Rgba(theme.text, alpha), head);
  // Up to eight state pictures in a grid (4 x 2).
  const float cell_w = (PANE_R - PANE_X - 80 - 3 * 24) / 4.0f, cell_h = cell_w * 7.0f / 8.0f;
  int shown = 0;
  for (int slot : save.slots)
  {
    if (shown >= 8)
      break;
    const int col = shown % 4, row = shown / 4;
    const ImVec2 p0(PANE_X + 40 + col * (cell_w + 24), TOP + 70 + row * (cell_h + 70));
    const ImVec2 p1(p0.x + cell_w, p0.y + cell_h);
    const ImTextureID picture = StatePicture(save.id, slot);
    if (picture != ImTextureID{})
      dl->AddImage(ImTextureRef(picture), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    else
      dl->AddRectFilled(p0, p1, Rgba(0x8A90A0, alpha * 0.4f), 8.0f);
    dl->AddRect(p0, p1, Rgba(theme.text, alpha * 0.25f), 6.0f, 0, 1.5f);
    const std::string name = slot == 0 ? "Resume" : "Slot " + std::to_string(slot);
    Text(dl, m_bold, 24, ImVec2(p0.x, p1.y + 8), Rgba(theme.text, alpha), name);
    shown++;
  }
  if (save.slots.empty())
    Text(dl, m_font, 28, ImVec2(PANE_X + 40, TOP + 70), Rgba(theme.text, alpha * 0.7f),
         "No save states. Press Cross for the battery save.");
}

// Settings and Themes: a list of rows, scrolling with the selection.
void Menu::Impl::DrawLibraryList(ImDrawList* dl, float alpha)
{
  const Theme& theme = CurrentTheme();
  const int c = m_category;
  const auto& items = m_items[c];
  const bool themes = c == static_cast<int>(Category::Themes);
  DrawPageTitle(dl, themes ? Category::Themes : Category::Settings, themes ? "Themes" : "Settings",
                themes ? "Colours and wallpapers (PNG in " + m_data_dir + "/themes)" :
                         "Saved data, themes, emulation, controls, folders and more",
                alpha, alpha * 0.85f);
  constexpr float X = 90.0f, ROW_H = 84.0f, TOP = 280.0f, BOTTOM = 960.0f;
  const float right = W - 90.0f;
  const float visible = (BOTTOM - TOP) / ROW_H;
  const float scroll = std::clamp(m_item_pos[c] - visible * 0.5f + 0.5f, 0.0f,
                                  std::max(0.0f, static_cast<float>(items.size()) - visible));
  dl->PushClipRect(ImVec2(0, TOP - 20), ImVec2(W, BOTTOM + 10), true);
  for (int i = 0; i < static_cast<int>(items.size()); i++)
  {
    const Item& item = items[i];
    const float y = TOP + (static_cast<float>(i) - scroll) * ROW_H;
    if (y < TOP - ROW_H || y > BOTTOM)
      continue;
    const bool sel = i == m_selected[c];
    const ImVec2 r0(X, y), r1(right, y + ROW_H - 12);
    if (sel)
    {
      dl->AddRectFilled(r0, r1, Rgba(theme.wave, alpha * 0.20f), 14.0f);
      dl->AddRect(r0, r1, Rgba(theme.wave, alpha), 14.0f, 0, 2.5f);
    }
    else
    {
      dl->AddRectFilled(r0, r1, Rgba(0x000000, alpha * 0.22f), 14.0f);
    }
    float tx = X + 34;
    if (item.theme != -1)
    {
      const ImVec2 s0(X + 24, y + 12), s1(X + 144, y + ROW_H - 24);
      if (item.theme >= 0)
      {
        const Theme& t = THEMES[item.theme];
        dl->AddRectFilledMultiColor(s0, s1, Rgba(t.top, alpha), Rgba(t.top, alpha),
                                    Rgba(t.bottom, alpha), Rgba(t.bottom, alpha));
        dl->AddLine(ImVec2(s0.x + 10, s1.y - 14), ImVec2(s1.x - 10, s0.y + 14), Rgba(t.wave, alpha), 4.0f);
      }
      else
      {
        dl->AddRectFilled(s0, s1, Rgba(0x404650, alpha), 6.0f);
        Text(dl, m_bold, 22, ImVec2(s0.x + 34, s0.y + 12), Rgba(0xFFFFFF, alpha), "PNG", false);
      }
      dl->AddRect(s0, s1, Rgba(theme.text, alpha * 0.4f), 6.0f, 0, 1.5f);
      tx = X + 170;
    }
    const float dw = TextWidth(m_font, 26, item.detail);
    Text(dl, sel ? m_bold : m_font, 32, ImVec2(tx, y + 18), Rgba(theme.text, alpha), item.label);
    Text(dl, m_font, 26, ImVec2(right - 34 - std::min(dw, 900.0f), y + 22), Rgba(theme.text, alpha * 0.7f),
         Fit(m_font, 26, item.detail, 900.0f));
  }
  dl->PopClipRect();
}

void Menu::Impl::DrawLibraryHints(ImDrawList* dl)
{
  const Theme& theme = CurrentTheme();
  const float y = H - 62.0f;
  const ImU32 text = Rgba(theme.text, 0.85f);
  dl->AddLine(ImVec2(70, y - 46), ImVec2(W - 70, y - 46), Rgba(theme.text, 0.10f), 1.5f);
  float x = 90.0f;
  auto left = [&](const char* glyph, const std::string& label) {
    x += PS4Glyphs::Draw(dl, m_bold, glyph, x, y, 36, 0.9f) + 12;
    Text(dl, m_font, 28, ImVec2(x, y - 17), text, label);
    x += TextWidth(m_font, 28, label) + 44;
  };
  float rx = W - 90.0f;
  auto right = [&](const char* glyph, const std::string& label) {
    const float lw = TextWidth(m_font, 28, label);
    rx -= lw;
    Text(dl, m_font, 28, ImVec2(rx, y - 17), text, label);
    const float gw = PS4Glyphs::Width(glyph, 36);
    rx -= gw + 12;
    PS4Glyphs::Draw(dl, m_bold, glyph, rx, y, 36, 0.9f);
    rx -= 44;
  };
  if (m_keyboard)
  {
    left("D-Pad", "Move");
    left("Cross", "Type");
    x += PS4Glyphs::Draw(dl, m_bold, "L1", x, y, 36, 0.9f) + 8;
    left("R1", "Browse Games");
    right("R2", "Done");
    right("L2", "Clear All");
    right("Triangle", "Space");
    right("Square", "Delete");
    right("Circle", "Close");
    return;
  }
  const bool games = IsGameList(m_category);
  const auto& items = m_items[m_category];
  const bool has_game = games && !items.empty() && items[0].game >= 0;
  left("D-Pad", "Browse");
  left("Cross", games ? "Play" : "Select");
  if (IsSettingsPage(m_category))
    left("Circle", "Back");
  if (games && !m_search.empty())
    left("Circle", "Clear Search");
  if (games)
  {
    right("Options", "Sort");
    right("Square", "Refresh");
    right("R2", "Search");
    if (has_game)
    {
      right("L2", "Favorite");
      right("Triangle", "Options");
    }
  }
}

// ---------------------------------------------------------------------------------------------

bool Menu::Impl::Run(Library::Game* result)
{
  int user = 0;
  sceUserServiceGetInitialUser(&user);
  ImGuiIO& io = ImGui::GetIO();
  LoadAssets();
  m_audio.Start();
  m_input.Open(user);
  LoadSaves();
  ApplyPreferences();
  BuildItems();
  m_launch = -1;
  m_launch_time = 0.0f;
#ifdef EMUSUNI_SATURN
  m_saturn_test = false;
#endif
  m_fade = 1.0f;
  m_panel = {};
  m_controls_open = false;
  m_keyboard = false;
  m_keyboard_slide = 0.0f;
  m_toast_time = 0.0f;
  // A drive plugged in or taken out while a game ran: the games read again (the first time, as just read).
  CheckPlaces(true);
  UpdateShownConsoles();
  if (Tour::Active() && !m_tour)
  {
    m_tour = true;
    m_tour_style = m_prefs.style;
  }
  // Start on the game played last (in every list that has it).
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    if (!IsGameList(c))
      continue;
    const auto& games = m_items[c];
    for (size_t i = 0; i < games.size(); i++)
    {
      if (games[i].game >= 0 && m_games[games[i].game].rom.path == m_prefs.last_game)
      {
        m_selected[c] = static_cast<int>(i);
        m_item_pos[c] = static_cast<float>(i);
      }
    }
  }
  m_tab_pos = static_cast<float>(LibraryTab());
  // Box art: the missing covers are downloaded in the background when the app opens (as Dolphin
  // for PS4 gets GameTDB's), paused while a game runs.
  m_dl_pause = false;
  if (!m_dl_auto)
  {
    m_dl_auto = true;
    if (MissingCovers())
    {
      Log("covers: downloading the missing ones in the background");
      StartCoverDownload();
    }
  }
  // The first start (no language chosen yet): the language list first, on the PS4's own language.
  {
    bool chosen = false;
    for (const auto& [key, value] : Settings::GlobalValues())
      chosen = chosen || key == "language";
    if (!chosen)
    {
      int32_t system = 1;
      sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &system);
      const char* code = system == 0                  ? "ja" :
                         system == 2 || system == 22  ? "fr" :
                         system == 3 || system == 20  ? "es" :
                         system == 4                  ? "de" :
                         system == 5                  ? "it" :
                         system == 6                  ? "nl" :
                         system == 7 || system == 17  ? "pt" :
                         system == 8                  ? "ru" :
                         system == 9                  ? "ko" :
                         system == 10 || system == 11 ? "zh" :
                         system == 16                 ? "pl" :
                         system == 19                 ? "tr" :
                                                        "en";
      PS4Lang::Load(m_assets_dir, code);
      BuildItems();
      OpenSettingPanel(SET_LANGUAGE);
      m_first_language = true;
      Log(std::string("first start: language list (suggested ") + code + ")");
    }
  }
  if (!m_pending_error.empty())
  {
    m_panel = {};
    m_panel.open = true;
    m_panel.title = "The game couldn't start";
    m_panel.lines = {""};
    for (size_t start = 0; start <= m_pending_error.size();)
    {
      const size_t end = std::min(m_pending_error.find('\n', start), m_pending_error.size());
      m_panel.lines.push_back(m_pending_error.substr(start, end - start));
      start = end + 1;
    }
    m_pending_error.clear();
  }

  auto last = Clock::now();
  bool picked = false;
  for (;;)
  {
    const auto now = Clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
    last = now;
    m_time += dt;
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

    UpdateCapture(dt);
    // The screenshot tour plays itself: its commands and its button presses.
    const Tour::Step tour = Tour::Next(dt);
    if (!tour.command.empty())
      TourCommand(tour.command);
    if (m_tour && !Tour::Active())
    {
      // The tour is over: the menu as it was.
      m_tour = false;
      m_keyboard = false;
      SetSearch("");
      if (m_prefs.style != m_tour_style)
      {
        m_prefs.style = m_tour_style;
        ApplyPreferences();
      }
      BuildItems();  // (the Settings rows without the tour's "set" values)
    }
    // A drive plugged in (CheckPlaces' look on its thread), Consoles Shown (the Settings page, a console's first
    // or last game, the tour's "set"): the games and the tabs follow.
    PlacesLooked();
    UpdateShownConsoles();
    // While the tour plays, the controller is left out (a press would change what it shows).
    const bool touring = Tour::Active();
    if (m_intro)
    {
      if (m_input.Poll(dt) != 0 && !touring)
        m_intro_time = std::max(m_intro_time, 2.1f);
    }
    else if (m_asking)
    {
      UpdateAsk();  // the PS4's keyboard has the controller
    }
    else if (!m_capture)
    {
      const uint32_t pad = m_input.Poll(dt);
      HandleInput((touring ? 0u : pad) | MenuButtons(tour.buttons));
    }
    UpdateCovers();
    UpdateCoverDownload();
    UpdateCheatFinder();
    UpdateFind(dt);
    // Without Cover: the games whose covers were just saved leave the lists, each list keeping its
    // place (the next game without a cover). Not while a game's options are open: Cross there acts
    // on the selected item (HandleInput, "Game").
    if (m_covers_changed && !(m_panel.open && m_panel.title == "Game"))
    {
      m_covers_changed = false;
      if (m_prefs.sort == "nocover")
        RebuildGameLists(true);
    }
    m_memory_log -= dt;
    if (m_memory_log <= 0.0f)
    {
      m_memory_log = 15.0f;
      LogMemory("menu");
    }

    m_display.BeginFrame();
    ImGui::NewFrame();
    Draw(dt);
    ImGui::Render();
    m_display.EndFrame();
    if (!tour.shot.empty())
      Tour::SaveShot(m_display, tour.shot);

    if (m_launch >= 0 && m_launch_time >= 0.75f)
    {
      *result = m_games[m_launch].rom;
      picked = true;
      break;
    }
#ifdef EMUSUNI_SATURN
    if (m_saturn_test)
    {
      *result = SaturnTest::Entry();  // (main.cpp runs the test instead of a game)
      picked = true;
      break;
    }
#endif
  }
  m_input.Close();
  m_audio.Stop();
  ForgetStatePictures();
  m_dl_pause = true;
  if (m_find_thread.joinable())
  {
    {
      std::lock_guard lock(m_find_mutex);
      m_find_jobs.push_back({3, SystemId::SNES, false, "", "", ""});  // its lists go while the game runs
    }
    m_find_wake.notify_one();
  }
  return picked;
}

// ---------------------------------------------------------------------------------------------

Menu::Menu(Display& display, const std::string& data_dir, const std::string& assets_dir)
    : m_impl(std::make_unique<Impl>(display, data_dir, assets_dir))
{
}

Menu::~Menu() = default;

bool Menu::Run(Library::Game* game)
{
  return m_impl->Run(game);
}

void Menu::GameEnded(const std::string& error)
{
  m_impl->GameEnded(error);
}

bool Menu::DefaultGame(Library::Game* game)
{
  return m_impl->DefaultGame(game);
}

ImFont* Menu::RegularFont() const
{
  m_impl->LoadAssets();
  return m_impl->m_font;
}

ImFont* Menu::BoldFont() const
{
  m_impl->LoadAssets();
  return m_impl->m_bold;
}
