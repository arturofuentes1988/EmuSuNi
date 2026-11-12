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
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#include <sys/stat.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include "Achievements.h"
#include "AchievementsUi.h"
#include "Arcade.h"
#include "AssetPack.h"
#include "BezelProject.h"
#include "CheatDownload.h"
#include "Cheats.h"
#include "Controls.h"
#include "CoverDownload.h"
#include "CoverFiles.h"
#include "Display.h"
#include "GameAudio.h"
#include "GameDelete.h"
#include "Image.h"
#include "Log.h"
#include "LogReport.h"
#include "MenuNav.h"
#include "Multiplayer.h"
#include "Naomi.h"
#include "Net.h"
#include "PS4Glyphs.h"
#include "PS4Lang.h"
#include "Portable.h"
#include "Ratings.h"
#include "Saves.h"
#include "Settings.h"
#include "StartGuard.h"
#include "Systems.h"
#include "TextKeyboard.h"
#include "Tour.h"
#include "GbLink.h"
#include "Netplay.h"
#include "NetplayLibrary.h"
#include "NetplayUi.h"
#include "MyStats.h"
#include "UsageStats.h"
#include "Update.h"
#include "Voice.h"
#ifdef EMUSUNI_SATURN
#include "SaturnTest.h"
#endif
#ifdef EMUSUNI_3DS
#include "ThreeDsCia.h"  // ((01.09) the 3DS's updates and DLC from CIAs, a game's mods on or off)
#include "GameExtras.h"  // ((01.22) the Arcade's patched romsets; the in-game menu's asks after the game)
#endif

namespace
{
#ifdef EMUSUNI_3DS
// (01.09) A text in the player's language as a string, to be joined to others (PS4Lang::T keeps a literal a pointer).
std::string Tr(const std::string& english)
{
  return PS4Lang::T(english);
}
#endif
using Clock = std::chrono::steady_clock;
constexpr float W = static_cast<float>(Display::WIDTH);
constexpr float H = static_cast<float>(Display::HEIGHT);
// (01.05) A panel's help text ends above the button hints (DrawHints: their glyphs from H - 98), at 25 px a line.
constexpr float HELP_END = H - 106.0f, HELP_LINE = 25.0f;

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

// A game's rating (src/Ratings.h): five stars in a row centred on c, each r in radius, gold up to its half stars
// (0 to 10: a half star is its left half), the others in `empty`; each over a shadow (a cover's bright reflection
// under them). How wide the row is: RatingWidth.
constexpr float RatingWidth(float r)
{
  return r * 2.5f * 4.0f + r * 2.0f;
}

// The star's points for a radius of 1 (DrawStar's shape).
const std::array<ImVec2, 10>& StarPoints()
{
  static const std::array<ImVec2, 10> UNIT = [] {
    std::array<ImVec2, 10> points;
    for (int i = 0; i < 10; i++)
    {
      const float angle = -IM_PI * 0.5f + static_cast<float>(i) * IM_PI / 5.0f;
      const float radius = i % 2 == 0 ? 1.0f : 0.45f;
      points[i] = ImVec2(std::cos(angle) * radius, std::sin(angle) * radius);
    }
    return points;
  }();
  return UNIT;
}

// The star as a picture (white, its edges soft): STAR_SIZE square, its points STAR_RADIUS from the middle.
constexpr int STAR_SIZE = 64;
constexpr float STAR_RADIUS = 31.0f;
std::vector<uint8_t> StarPixels()
{
  const auto& unit = StarPoints();
  std::vector<uint8_t> pixels(STAR_SIZE * STAR_SIZE * 4, 255);
  auto inside = [&](float x, float y) {
    bool in = false;  // (even-odd: the polygon's edges a ray to the right crosses)
    for (int i = 0, j = 9; i < 10; j = i++)
    {
      const ImVec2 a(STAR_SIZE * 0.5f + unit[i].x * STAR_RADIUS, STAR_SIZE * 0.5f + unit[i].y * STAR_RADIUS);
      const ImVec2 b(STAR_SIZE * 0.5f + unit[j].x * STAR_RADIUS, STAR_SIZE * 0.5f + unit[j].y * STAR_RADIUS);
      if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
        in = !in;
    }
    return in;
  };
  for (int y = 0; y < STAR_SIZE; y++)
  {
    for (int x = 0; x < STAR_SIZE; x++)
    {
      int covered = 0;  // of 4 x 4 samples in the pixel
      for (int sy = 0; sy < 4; sy++)
      {
        for (int sx = 0; sx < 4; sx++)
          covered += inside(static_cast<float>(x) + (sx + 0.5f) / 4.0f, static_cast<float>(y) + (sy + 0.5f) / 4.0f);
      }
      pixels[(y * STAR_SIZE + x) * 4 + 3] = static_cast<uint8_t>(covered * 255 / 16);
    }
  }
  return pixels;
}

// RetroAchievements' mark on a game (00.77): a small "RA" box whose bottom right corner is `corner`, h tall - gold
// when the game is completed (every achievement unlocked; 00.78: Casual and Hardcore alike), a gold line inside for
// the share unlocked; nothing for a game it has no achievements for (or not checked yet). Its width: 1.75 h.
void DrawAchievementMark(ImDrawList* dl, ImFont* bold, ImVec2 corner, float h, const Achievements::LibraryInfo& info,
                         float alpha)
{
  if (info.state != Achievements::LibraryInfo::State::Known || alpha <= 0.01f)
    return;
  const float w = h * 1.75f;
  const ImVec2 p0(corner.x - w, corner.y - h);
  const bool completed = info.progress && info.achievements > 0 && info.unlocked >= info.achievements;
  const int a = static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f));
  dl->AddRectFilled(p0, corner, completed ? IM_COL32(232, 178, 40, a) : IM_COL32(14, 14, 22, a * 82 / 100), h * 0.28f);
  dl->AddRect(p0, corner, completed ? IM_COL32(255, 214, 90, a) : IM_COL32(255, 255, 255, a * 60 / 100),
              h * 0.28f, 0, std::max(1.0f, h * 0.07f));
  const float size = h * 0.62f;
  const ImVec2 t = bold->CalcTextSizeA(size, FLT_MAX, 0, "RA");
  dl->AddText(bold, size, ImVec2(p0.x + (w - t.x) * 0.5f, p0.y + (h - t.y) * 0.5f - h * 0.05f),
              completed ? IM_COL32(48, 32, 0, a) : IM_COL32(255, 255, 255, a), "RA");
  if (info.progress && info.unlocked > 0 && !completed)
  {
    const float share = static_cast<float>(info.unlocked) / static_cast<float>(info.achievements);
    const float inset = h * 0.2f, y = corner.y - h * 0.17f;
    dl->AddLine(ImVec2(p0.x + inset, y), ImVec2(p0.x + inset + (w - 2.0f * inset) * share, y),
                IM_COL32(255, 200, 60, a), std::max(1.5f, h * 0.1f));
  }
}

// `star`: the star's picture (Menu's m_star) - a quad per star, its shadow and its gold half (4 vertices each, no
// triangulation: 13 rows, 65 stars, every frame); without it the star's polygon (ImGui's concave fill).
void DrawRating(ImDrawList* dl, ImTextureID star, ImVec2 c, float r, int half_stars, ImU32 empty, float alpha)
{
  const ImU32 gold = Rgba(0xFFD34D, alpha);
  const ImU32 shade = IM_COL32(0, 0, 0, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 115));
  const float step = r * 2.5f, q = r * (STAR_SIZE * 0.5f) / STAR_RADIUS;
  const ImVec2 offset(r * 0.12f, r * 0.16f);
  const auto& unit = StarPoints();
  for (int i = 0; i < 5; i++)
  {
    const float x = c.x + (static_cast<float>(i) - 2.0f) * step;
    const int fill = std::clamp(half_stars - 2 * i, 0, 2);
    if (star != ImTextureID{})
    {
      dl->AddImage(ImTextureRef(star), ImVec2(x - q + offset.x, c.y - q + offset.y),
                   ImVec2(x + q + offset.x, c.y + q + offset.y), ImVec2(0, 0), ImVec2(1, 1), shade);
      dl->AddImage(ImTextureRef(star), ImVec2(x - q, c.y - q), ImVec2(x + q, c.y + q), ImVec2(0, 0), ImVec2(1, 1),
                   fill == 2 ? gold : empty);
      if (fill == 1)
        dl->AddImage(ImTextureRef(star), ImVec2(x - q, c.y - q), ImVec2(x, c.y + q), ImVec2(0, 0), ImVec2(0.5f, 1),
                     gold);
      continue;
    }
    ImVec2 points[10];
    for (int k = 0; k < 10; k++)
      points[k] = ImVec2(x + unit[k].x * r + offset.x, c.y + unit[k].y * r + offset.y);
    dl->AddConcavePolyFilled(points, 10, shade);
    for (int k = 0; k < 10; k++)
      points[k] = ImVec2(x + unit[k].x * r, c.y + unit[k].y * r);
    dl->AddConcavePolyFilled(points, 10, fill == 2 ? gold : empty);
    if (fill == 1)
    {
      // The left half: from the inner point at the bottom round to the top point (the same way round).
      const ImVec2 half[6] = {points[5], points[6], points[7], points[8], points[9], points[0]};
      dl->AddConcavePolyFilled(half, 6, gold);
    }
  }
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
// console's prefix, no extension): the game's key and its title, and an arcade game's full name and its
// parent's (libretro's thumbnails are named so, ':' and '/' made '_'). (00.81: src/CoverFiles.h's, which
// Download Covers looks with too.)
std::vector<std::string> CoverNames(const Library::Game& rom)
{
  return CoverFiles::Names(rom);
}

// ---------------------------------------------------------------------------------------------
// Sounds: 16-bit PCM WAV files mixed into one 48 kHz stereo AudioOut port.

struct Sound
{
  std::vector<int16_t> frames;  // interleaved stereo, 48 kHz
};

std::unique_ptr<Sound> LoadWav(const std::string& path)
{
  std::string data;
  AssetPack::Read(path, &data);  // (00.90: the app's own sounds are in xmb/assets.zip in the package)
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
      ::Voice::Mix(buffer.data(), FRAMES);  // (00.23: the voice chat's voices, Test Microphone's playback)
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
  BTN_TOUCHPAD = 1 << 13,  // (00.80: the games' RetroAchievements filter)
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
      {ORBIS_PAD_BUTTON_OPTIONS, BTN_OPTIONS}, {ORBIS_PAD_BUTTON_TOUCH_PAD, BTN_TOUCHPAD},
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
        if (b & ORBIS_PAD_BUTTON_TOUCH_PAD)
          held |= BTN_TOUCHPAD;
      }
    }
    uint32_t pressed = held & ~m_held;
    // Held, the directions repeat, and L1 / R1 too (00.76: the Library's console tabs one after another, src/MenuNav.h).
    // Repeated() says which of this pass's presses are a repeat: a list goes round only on a fresh press.
    m_repeated = m_directions.Update(held, m_held, dt) | m_shoulders.Update(held, m_held, dt);
    pressed |= m_repeated;
    m_held = held;
    return pressed;
  }
  uint32_t Repeated() const { return m_repeated; }

private:
  int m_pad = -1;
  uint32_t m_held = 0;
  uint32_t m_repeated = 0;
  MenuNav::HeldRepeat m_directions{BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT, MenuNav::DIRECTION_DELAY,
                                   MenuNav::DIRECTION_PERIOD};
  MenuNav::HeldRepeat m_shoulders{BTN_L1 | BTN_R1, MenuNav::SHOULDER_DELAY, MenuNav::SHOULDER_PERIOD};
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
  bool ratings = true;  // Settings -> Star Ratings: the stars under the covers (00.73)
  // Settings -> Game Folders: each console's ("snes" -> folder); none = data_dir/<its folder>.
  std::map<std::string, std::string> games_dirs;
  std::string last_game;
  std::string style = "library";  // "library" (covers) or "xmb"
  std::string sort = "title";     // "title", "recent" (last played first), "region", "console",
                                  // "nocover" (Without Cover: this session only)
  std::string default_game;       // starts by itself when the app opens
  // (00.80) The games' RetroAchievements filter (Touchpad): "off", "known" (the games with achievements), "progress"
  // (those with achievements begun, not all unlocked).
  std::string ra_filter = "off";
  // (01.18) Order and Filters (Options in a list of games): Hide Adult Games - an arcade set whose category is for
  // adults, a game LaunchBox marks so. Off by default; kept (the genre and players filters last the session).
  bool hide_adult = false;
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
      else if (key == "ratings")
        ratings = value != "off";
      // (00.87: the folders and games on a USB drive's portable data are kept relative to the drive - src/Portable.h.)
      else if (key == "games_dir")
        games_dirs["snes"] = Portable::FromStored(value);  // the Super Nintendo's (as before EmuSuNi played other consoles)
      else if (key.rfind("games_dir.", 0) == 0)
        games_dirs[key.substr(10)] = Portable::FromStored(value);
      else if (key == "last_game")
        last_game = Portable::FromStored(value);
      else if (key == "style")
        style = value == "xmb" ? "xmb" : "library";
      else if (key == "sort")
        sort = value == "nocover" ? "title" : value;  // Without Cover lasts one session
      else if (key == "default_game")
        default_game = Portable::FromStored(value);
      else if (key == "ra_filter")
        ra_filter = value == "known" || value == "progress" ? value : "off";
      else if (key == "hide_adult")
        hide_adult = value == "on";
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
            "\nratings=" + (ratings ? "on" : "off") +
            "\ngames_dir=" + (snes == games_dirs.end() ? std::string() : Portable::ToStored(snes->second)) +
            "\nlast_game=" + Portable::ToStored(last_game) + "\nstyle=" + style + "\nsort=" + sort +
            "\ndefault_game=" + Portable::ToStored(default_game) + "\nra_filter=" + ra_filter +
            "\nhide_adult=" + (hide_adult ? "on" : "off") + "\nconsole_order=" + console_order +
            "\nconsole_order_version=" + std::to_string(console_order_version) + "\n";
    for (const auto& [key, dir] : games_dirs)
    {
      if (key != "snes")
        text += "games_dir." + key + "=" + Portable::ToStored(dir) + "\n";
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
  SaturnGames,   // builds with the Sega Saturn (EMUSUNI_SATURN)
  PceGames,      // builds with the PC Engine (EMUSUNI_PCE): its HuCards
  A2600Games,    // (00.90) builds with Atari's consoles (EMUSUNI_A2600, EMUSUNI_A7800, EMUSUNI_JAGUAR)
  A7800Games,
  JaguarGames,
#ifdef EMUSUNI_OPENBOR
  OpenborGames,  // builds with OpenBOR (EMUSUNI_OPENBOR, with-openbor): its .pak games
#endif
  Favorites,
  SavedData,
  Themes,
  Online,  // (00.23) online play's tab: rooms, codes, public rooms, nickname, the online profile
};
#ifdef EMUSUNI_OPENBOR
constexpr int CATEGORY_COUNT = 30;
static_assert(static_cast<int>(Category::OpenborGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::OPENBOR));
#else
constexpr int CATEGORY_COUNT = 29;
#endif
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
static_assert(static_cast<int>(Category::SaturnGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::SATURN));
static_assert(static_cast<int>(Category::PceGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::PCE));
static_assert(static_cast<int>(Category::JaguarGames) ==
              static_cast<int>(Category::Games) + 1 + static_cast<int>(SystemId::JAGUAR));
static_assert(static_cast<int>(Category::Favorites) == static_cast<int>(Category::Games) + 1 + SYSTEM_COUNT);
constexpr const char* CATEGORY_NAMES[CATEGORY_COUNT] = {
    "Settings",  "All Games", "Super Nintendo", "NES",        "Nintendo 64", "Game Boy",
    "Game Boy Color", "Game Boy Advance", "PlayStation", "Nintendo DS", "PSP", "Dreamcast",
    "SG-1000", "Master System", "Game Gear", "Genesis", "Arcade", "Nintendo 3DS", "NAOMI", "Atomiswave",
    "Saturn", "PC Engine", "Atari 2600", "Atari 7800", "Atari Jaguar",
#ifdef EMUSUNI_OPENBOR
    "OpenBOR",
#endif
    "Favorites", "Saved Data", "Themes", "Online"};
constexpr const char* SORT_NAMES[][2] = {
    {"title", "A-Z"}, {"recent", "Recently Played"}, {"region", "Region"}, {"console", "Console"},
    {"nocover", "Without Cover"}};  // a view: only the games without a cover picture, A-Z

// The console of a console's game list (a console of this build), else -1.
int SystemOfList(int category)
{
  const int system = category - static_cast<int>(Category::Games) - 1;
  return system >= 0 && system < SYSTEM_COUNT && HasSystem(static_cast<SystemId>(system)) ? system : -1;
}

// (00.88) The game list of a console that plays online (src/Netplay.h): its globe in the XMB and the Library's tabs.
bool PlaysOnline(int category)
{
  const int system = SystemOfList(category);
  return system >= 0 && Netplay::RulesFor(GetSystem(static_cast<SystemId>(system)).key).online;
}

// The consoles' order in the Library's tabs, the XMB's columns and the Console sort (Settings ->
// Console Order, kept in xmb.ini as console_order): every console of this build once, the ones the
// saved order doesn't name after the others in their usual order.
std::vector<SystemId> s_console_order;
int s_console_order_version = 1;  // changes with the order (Tabs() and Columns() are made again)
// The consoles with a tab and an XMB column (Settings -> Consoles Shown, Library::ShownConsoles): some or all of
// the console order's, in it; empty until the menu sets it (then all). Changing it changes the version too.
std::vector<SystemId> s_shown_consoles;
// (00.85, a player: "Add MENU SCALE FACTOR! I can see only 10 games on my TV") Settings -> Menu Size (settings.ini's
// menu_size: 100, 90, 80 or 70): how big the XMB's rows and the Library's covers are drawn, smaller fitting more games
// on the screen. Read again at each frame's start (Draw): a change shows at once. Smaller means the same drawing
// shrunk (ScaleDrawn), not other text sizes: every size drawn is one more set of glyphs in the font atlas.
float s_menu_scale = 1.0f;

float MenuScaleOf(const std::string& value)
{
  const int percent = std::atoi(value.c_str());
  return percent >= 50 && percent <= 100 ? static_cast<float>(percent) / 100.0f : 1.0f;
}

// What was drawn into dl since its vertex `first`, made `scale` times as big around `pivot` (Menu Size).
void ScaleDrawn(ImDrawList* dl, int first, ImVec2 pivot, float scale)
{
  for (int i = first; i < dl->VtxBuffer.Size; i++)
  {
    ImVec2& p = dl->VtxBuffer[i].pos;
    p.x = pivot.x + (p.x - pivot.x) * scale;
    p.y = pivot.y + (p.y - pivot.y) * scale;
  }
}

// The standard order (Default Order): Nintendo's consoles, then Sony's, then Sega's, each from the
// most basic to the most powerful, then NEC's (the PC Engine, 00.82), Atari's (the 2600, the 7800, the Jaguar: 00.90),
// then the arcade boards: the Arcade, the NAOMI (Sega's, the Dreamcast's hardware in a cabinet) and the Atomiswave
// (Sammy's, the same again).
constexpr SystemId STANDARD_ORDER[] = {SystemId::GB,  SystemId::NES, SystemId::GBC, SystemId::SNES,
                                       SystemId::GBA, SystemId::N64, SystemId::NDS, SystemId::THREEDS,
                                       SystemId::PS1, SystemId::PSP, SystemId::SG1000, SystemId::SMS,
                                       SystemId::GG,  SystemId::MD,  SystemId::SATURN, SystemId::DC,
                                       SystemId::PCE, SystemId::A2600, SystemId::A7800, SystemId::JAGUAR,
                                       SystemId::ARCADE, SystemId::NAOMI, SystemId::ATOMISWAVE};

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
    order = {SystemId::A2600, SystemId::GB,  SystemId::SG1000, SystemId::NES,    SystemId::A7800, SystemId::SMS,
             SystemId::GG,    SystemId::PCE, SystemId::GBC,    SystemId::MD,     SystemId::SNES,  SystemId::GBA,
             SystemId::JAGUAR, SystemId::PS1, SystemId::SATURN, SystemId::N64,   SystemId::NDS,   SystemId::DC,
             SystemId::ARCADE, SystemId::NAOMI, SystemId::ATOMISWAVE, SystemId::PSP, SystemId::THREEDS};
  else if (preset == "year")
    order = {SystemId::A2600,                                                                   // 1977
             SystemId::NES, SystemId::SG1000, SystemId::SMS,  SystemId::A7800, SystemId::PCE,  // 1983 1983 1985 1986 1987
             SystemId::MD,                                                                      // 1988
             SystemId::GB,  SystemId::GG,     SystemId::ARCADE, SystemId::SNES,                 // 1989 1990 1990 1990
             SystemId::JAGUAR, SystemId::SATURN, SystemId::PS1,                                 // 1993 1994 1994
             SystemId::N64, SystemId::GBC,    SystemId::DC,   SystemId::NAOMI, SystemId::GBA,  // 1996 1998 1998 1998 2001
             SystemId::ATOMISWAVE, SystemId::NDS, SystemId::PSP, SystemId::THREEDS};            // 2003 2004 2004 2011
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

// The XMB style's columns, left to right: Settings, Online (00.23), all the games, a column per console shown (in
// the console order), the favorites. Saved Data and Themes are pages of Settings in both styles (Cross
// on them in its list, Circle back).
const std::vector<int>& Columns()
{
  static std::vector<int> columns;
  static int version = 0;
  if (version != s_console_order_version)
  {
    version = s_console_order_version;
    columns = {static_cast<int>(Category::Settings), static_cast<int>(Category::Online),
               static_cast<int>(Category::Games)};
    for (SystemId id : ShownOrder())
      columns.push_back(static_cast<int>(Category::Games) + 1 + static_cast<int>(id));
    columns.push_back(static_cast<int>(Category::Favorites));
  }
  return columns;
}

// The Library style's tabs (L1 / R1, round and round), left to right: All, a tab per console shown
// (in the console order), Favorites, Online (00.23), Settings.
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
    tabs.push_back(static_cast<int>(Category::Online));  // (00.23)
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

// The search keyboard (R2) types at most this many characters; its keys and every other text's are src/TextKeyboard.h's
// (00.80: one keyboard for every text the app asks for).
constexpr size_t SEARCH_MAX = 40;

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
    // (00.81: a .bin without its .cue too - src/Library.cpp.)
    {SystemId::PS1, "PlayStation games, on one disc or several", nullptr,
     ".cue (with its .bin tracks) or a .bin alone, .chd, .pbp, .iso, .m3u; not in a .zip"},
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
    // (The app finds the Saturn's BIOS in system/saturn, system or the game's folder: src/SaturnTest.h.)
    {SystemId::SATURN, "Sega Saturn games (experimental: slower than the console)",
     "The USA one, 512 KB (any name with \"bios\" in it), in /data/EmuSuNi/system/saturn",
     ".cue (with its .bin tracks), .chd, .iso, .ccd, .mds; one disc each, not in a .zip"},
    // (00.82. No SuperGrafx.)
    {SystemId::PCE, "PC Engine and TurboGrafx-16 HuCards", nullptr, ".pce, or a .zip of one (no SuperGrafx .sgx)"},
    // (00.90. The 7800's and the Jaguar's BIOS are optional: their games start without it.)
    {SystemId::A2600, "Atari 2600 (VCS) cartridges", nullptr, ".a26, or a .bin of 64 KB or less, or a .zip of one"},
    {SystemId::A7800, "Atari 7800 cartridges", nullptr, ".a78 (with its header), or a .zip of one"},
    // (Experimental as the Saturn: the 3D games are slower than the console. Its number pad: the touchpad, Emulator.cpp.)
    {SystemId::JAGUAR, "Atari Jaguar cartridges (experimental); its number pad on the touchpad", nullptr,
     ".j64, .jag (homebrew: .abs, .cof), or a .zip of one"},
#ifdef EMUSUNI_OPENBOR
    // (with-openbor. The engine version, 3.0 or 4.0, is picked by what the .pak holds: scripts/build-openbor-core.sh.)
    {SystemId::OPENBOR, "OpenBOR games: free fan-made beat 'em ups", nullptr,
     ".pak, not in a .zip; the engine version is picked by itself"},
#endif
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
  SET_MENU_SIZE,   // Menu Size (00.85): the games drawn smaller, more of them in view (settings.ini's menu_size)
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
  SET_UPDATES,          // Check for Updates: this version, the newest one, Update (00.73, src/Update.h)
  SET_UPDATE_NOTICE,    // the start's notice of a newer version: Update or Later
  SET_RATINGS,          // Star Ratings: the stars under the covers on or off (00.73)
  SET_ACHIEVEMENTS,     // RetroAchievements (00.73, src/Achievements.h): the account (log in, log out), its settings
  SET_RA_HISTORY,       // RetroAchievements -> My Achievements (00.75): the unlocks and the games played
  SET_RA_GAME,          // My Achievements -> a game (00.76): all its achievements, locked or not
  SET_RA_VERSIONS,      // a game's Triangle -> Achievements on a file RetroAchievements doesn't know (00.79)
  SET_RA_ABOUT,         // RetroAchievements -> What is RetroAchievements? (00.79)
  SET_BEZELS,           // Game Bezels (00.80, src/BezelProject.h): The Bezel Project's bezels of the player's games
  SET_BEZELS_DELETE,    // "Delete?" for the downloaded bezels
  SET_VOICE,            // Voice Chat (00.23, src/Voice.h): its settings and Test Microphone
  SET_ONLINE,           // Online Play (00.23, src/Netplay.h): Settings' way to the Online tab
  SET_ONLINE_HOST,      // a game's Triangle -> Play Online: a private or a public room
  SET_ONLINE_ROOM,      // the room: its code, its players, Start (made again every frame)
  SET_STATS,            // Anonymous Statistics (00.23, src/UsageStats.h): on or off
  SET_OL_CREATE,        // the Online tab (00.23; Square in its table, 00.85): Create Room - with a game, or Choose Together
  SET_OL_SETTINGS,      //   (00.85) Options in its table: Online Settings - the rows below, Nickname, Hide My Address
  SET_OL_PROFILE,       //   Online Profile: on or off (src/UsageStats.h)
  SET_OL_PUBLIC,        //   Public Profile: on or off
  SET_OL_CODE,          //   Profile Code: the code, where it opens
  SET_OL_DELETE,        //   Delete My Profile: asked first
  SET_MY_STATS,         // My Stats (00.23, src/MyStats.h): this PS4's own numbers
  SET_OL_REPLACE,       // (00.86) one room at a time: close ours to make or join another? Yes / No
  SET_PORTABLE,         // (00.87, src/Portable.h) Keep My Data on the USB Drive: turn on (a drive), use a drive's, off
  SET_PORTABLE_COPY,    //   its copy as it goes, then its result (Restart Now)
  SET_PORTABLE_ASK,     //   a drive with portable data this PS4 never answered for: use it? Yes / No
  SET_PORTABLE_NOTICE,  //   a drive this PS4 uses, connected while the app runs: Restart Now / Later
  SET_PORTABLE_WARNING, //   the drive in use taken out (saving to this PS4), a write to it failed, it didn't answer
  SET_STATS_NOTICE,     // (00.89) the anonymous statistics' notice, once: OK / Turn Them Off (OpenStatsNotice)
  SET_HELP,             // (00.90) How to Help: a star, a report, a word to others - no money (OpenHelpPanel)
  SET_SEND_LOG,         // (00.91) Send Log (src/LogReport.h): what goes, Send; then sending, its code, or why not
  SET_SEND_LOG_ASK,     //   at the start after EmuSuNi closed by itself: send its log? Yes / No
  SET_OL_LINKGAME,      // (00.95) a Game Boy link cable room's guest: its own game for Game Boy 2
  SET_OL_CHECK,         // (01.05) Online Settings -> Check My Games: each console's standard versions (OpenCheckGames)
  SET_OL_CHECK_LIST,    //   a console's games: standard or not
  SET_3DS_CIA,          // (01.09, src/ThreeDsCia.h) 3DS Updates and DLC: the CIAs found ((01.22) none: from the game)
  SET_3DS_CIA_CONFIRM,  //   what it is, the free space: Install / Cancel
  SET_3DS_CIA_INSTALL,  //   its install as it goes (Cancel), then its result: Delete the CIA / Keep It
  SET_3DS_CIA_DELETE,   //   "Delete?" for that CIA: it can't be brought back
  SET_3DS_EXTRAS,       //   a 3DS game's Triangle -> Updates, DLC and Mods: what it has, Remove, Mods On / Off
  SET_3DS_REMOVE,       //   "Remove?" for its update or its DLC (copy its save first)
  SET_LIST_VIEW,        // (01.18) Options in a list of games: Order and Filters - the order, genre, players, adults
  SET_ARCADE_PATCHED,   // (01.22, src/GameExtras.h) an Arcade game's Triangle -> Patched Romset: in use, Remove, found
  SET_ARCADE_CONFIRM,   //   one found chosen: Install / Cancel (the room it takes)
  SET_ARCADE_COPY,      //   its copy as it goes (Cancel), then its result
  SET_ARCADE_REMOVE,    //   "Remove?" for the one in use
#ifdef EMUSUNI_SATURN
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
  float icon_aspect = 0.0f;  // the icon's width / height (00.89: kept in its 52 x 45 box); 0 = fills it
  std::string picture;  // RetroAchievements' picture (its address: drawn once downloaded, 00.75)
  std::string help;     // the row's own help text (made with it: RetroAchievements' history, 00.75)
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
  bool snap = false;  // the list went round (00.76): its scroll jumps there instead of sliding through every row
  // (01.05) The help text's lines under the list, as last drawn: the list ends higher for a long one, so that it ends
  // above the button hints (v00.23's tour: Dreamcast Online's ran through them).
  int help_lines = 0;
  // Information: a game's rating in stars (half stars 0 to 10; -1 none), drawn at the end of its line of `lines`.
  int stars = -1;
  std::string stars_line;
  // (00.87) Each option's own help at the bottom (an empty one, or none: OptionHelp's for its text).
  std::vector<std::string> helps;
};

struct Game
{
  Library::Game rom;
  int64_t played = 0;
  Ratings::Rating rating;  // its stars (src/Ratings.h; half_stars -1: none, or not matched yet)
  Achievements::LibraryInfo ra;  // RetroAchievements' mark (00.77: src/Achievements.h's library marks)
  int order = 0;  // its place in the read's list (00.85: games of one title stay in that order after a rename)
  // (01.05) Its mark under the stars (StandardMark): 0 not known yet (its file not read), 1 the standard version, 2
  // another, 3 nothing to say (a console the list hasn't, discs). Known once, then kept.
  mutable int8_t standard = 0;
};

// (00.85) A game for the matchers of names - the stars' (Ratings::Matcher), the covers' download - which read none of
// its file's path, tags or missing files: a copy of a big library's games for their threads without them.
Library::Game NamesOf(const Library::Game& rom)
{
  Library::Game names;
  names.id = rom.id;
  names.title = rom.title;
  names.region = rom.region;
  names.size = rom.size;
  names.system = rom.system;
  names.key = rom.key;
  names.custom_title = rom.custom_title;
  names.discs = rom.discs;
  names.cover_names = rom.cover_names;
  return names;
}

// (01.05, the owner: "que le dé prioridad solo a las consolas que se juegan en online con EmuSuNi") The consoles Check
// My Games and the marks under the stars are for: those of the online rooms (Netplay::RulesFor) and of the link cable
// rooms (the Game Boy, the Game Boy Color, the Game Boy Advance) - the others have neither.
bool PlaysOnline(SystemId id)
{
  return Netplay::RulesFor(GetSystem(id).key).online || id == SystemId::GB || id == SystemId::GBC ||
         id == SystemId::GBA;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

class Menu::Impl
{
public:
  Impl(Display& display, const std::string& data_dir, const std::string& assets_dir)
      : m_display(display), m_data_dir(data_dir), m_assets_dir(assets_dir)
  {
    m_prefs.Load(UserDir() + "/xmb.ini");
    // (The Roms/<console> folders are made by the read of the games: MakeFolders, on its thread.)
    for (const System& system : Systems())
    {
      if (GamesFolder(system.id).rfind(m_data_dir, 0) == 0)
        MakeDirs(GamesFolder(system.id));  // the app's own folders (not one on a USB stick)
    }
    MakeDirs(UserDir() + "/covers");
    MakeDirs(UserDir() + "/cheats");
    MakeDirs(UserDir() + "/themes");
    g_start_step = "the menu: the games' names";
    LoadNames();
    m_rating_copy = Ratings::ReadCopy(m_data_dir);  // (LaunchBox's ratings downloaded before: SetGames matches)
    // (00.85) The last read's games at once, from the internal storage (no USB drive is asked anything before the
    // menu's first frame: a sleeping or broken one would hold it back); the read of the drives on its thread.
    g_start_step = "the menu: the games' list kept from the last read";
    LoadCachedGames();
    g_start_step = "the menu: the games played and the favorites";
    LoadPlayed();
    LoadFavorites();
    AskRead(true, true);
  }
  ~Impl()
  {
    {
      std::lock_guard lock(m_read_mutex);
      m_read_quit = true;
    }
    m_read_progress.stop = true;
    m_read_progress.pause = false;
    m_read_wake.notify_all();
    if (m_read_thread.joinable())
      m_read_thread.join();
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
    if (m_portable_thread.joinable())
    {
      m_portable_progress->stop = true;  // (00.87: the copy of the player's data stops at its next file)
      m_portable_thread.join();
    }
  }

  bool Run(Library::Game* game);
  void GameEnded(const std::string& error)
  {
    m_intro = true;
    m_intro_time = 0.0f;
    m_pending_error = error;
    // (00.23) An online match's end (the other left, the connection was lost): said under Online Play's title.
    // (00.86: a match's only - after a game played alone, the end of our room that stayed open, if it ended meanwhile,
    // is UpdateOnline's to say.)
    m_pending_title = m_launch_online && !Netplay::TakeError().empty() && !error.empty() ? "Online Play" : "";
    // ((01.23) Back to the room from its match - Back to the Room, a guest who left -: the room shows, the host's
    // too with no one else in it.)
    m_online_back = m_launch_online && error.empty() && Netplay::GetView().phase == Netplay::Phase::Room;
    m_launch_online = false;
  }
  bool DefaultGame(Library::Game* game)
  {
    if (m_prefs.default_game.empty())
      return false;
    WaitForGames();  // (00.85: the cache's games, else the first read's)
    for (const Game& g : m_games)
    {
      if (g.rom.path == m_prefs.default_game)
      {
        // (01.06) The cache's games are the last read's: a file gone since (v00.23's E46NUS: deleted while the app was
        // closed) opens the menu, not a start that says it can't be opened.
        if (!FileExists(g.rom.path))
        {
          Log("default game's file isn't there any more: " + m_prefs.default_game);
          return false;
        }
        TellInstallFolders(g.rom.system);  // ((01.22) its in-game menu's Updates, DLC and Mods)
        *game = g.rom;
        HoldRead();  // (the game starts before the menu)
        return true;
      }
    }
    Log("default game not found: " + m_prefs.default_game);
    return false;
  }
  bool FindGame(const std::string& path, Library::Game* game)
  {
    WaitForGames();
    for (const Game& g : m_games)
    {
      if (g.rom.path == path)
      {
        TellInstallFolders(g.rom.system);  // ((01.22) its in-game menu's Updates, DLC and Mods)
        *game = g.rom;
        HoldRead();
        return true;
      }
    }
    return false;
  }
  void SafeStart() { m_safe_start = true; }  // (01.11: main.cpp)
  ImFont* m_font = nullptr;
  ImFont* m_bold = nullptr;
  void LoadAssets();

private:
  // ---- Reading the games (00.85: big libraries) ----
  // Until 00.84 the games were read before the menu's first frame (BuildGames): a library of 10 000 games on a USB
  // drive took longer than the start's watchdog waits for a picture (main.cpp: 20 s), and the app closed before it
  // opened - a folder shared by every console, the drive's top folder, took most of an hour. Now a thread of the
  // menu's own reads them (ReadMain: the drives, the Roms folders made where missing, the places, the covers, then
  // every console's games) while the menu draws; the start shows at once the games the last read found (the library's
  // cache, data_dir/cache/library.tsv, the same version and the same folders asked for), its read replacing them
  // only when it found something else. The very first start (and the first after an update) shows "Reading your
  // games..." with how many it has found until they come. One read at a time: a read asked for while one runs stops
  // that one at its next file and starts over (Square twice, a folder chosen meanwhile); a game running waits it.
  struct ReadAsk
  {
    bool games = false;   // every console's games (with the places and the covers)
    bool covers = false;  // the covers' pictures (a look at the places alone when neither: the games if they changed)
    std::array<std::string, SYSTEM_COUNT> chosen;  // each console's folder (Game Folders, else its default one)
    std::string covers_on;                         // Settings -> Covers Saved On
    Library::Places known;                         // the places the menu had (a look: the games read when they changed)
  };
  struct ReadDone
  {
    bool scanned = false;         // the games were read (found, fingerprint)
    bool places_changed = false;  // a look found other places (a drive plugged in or taken out)
    Library::Places places;
    std::string usb_covers;
    Library::Found found;
    uint64_t fingerprint = 0;
    CoverFiles::Index cover_files;  // (covers: the pictures, the places they were listed from, where new ones go)
    std::string covers_line;
  };
  std::array<std::string, SYSTEM_COUNT> ChosenFolders() const;
  std::array<std::string, SYSTEM_COUNT> StoredChosen(const std::array<std::string, SYSTEM_COUNT>& chosen) const;
  void LoadCachedGames();
  void AskRead(bool games, bool covers);
  void ReadMain();
  bool ReadNow(const ReadAsk& ask, ReadDone* done);  // (the thread's; false: stopped, a newer read asked for)
  void ListCovers(const std::vector<std::string>& drives, const std::string& usb_covers, const std::string& covers_on,
                  ReadDone* done);
  void TakeRead();         // what the thread found, into the menu (every frame; not while a game is launched or its
                           // options are open: the games are in another order then)
  void CoversRead(ReadDone& done);
  void GamesRead(ReadDone& done);
  int ReadingGames() const;  // the games found so far while a read of them goes on, else -1
  void UpdateReadingItems();
  void StartCoversOnce();  // the start's download of the missing covers, once the menu has the read's games and covers
  void HoldRead();         // a game starts: a read of the games stops, to start again with the menu (m_read_resume)
  bool WaitForGames();       // main.cpp's DefaultGame, FindGame before the menu's first frame
  void DrawReading();
  // The games of a read in m_games: in its order, the user's own names on them, by title (ApplyNames), the stars and
  // RetroAchievements' marks for them; GamesChanged after any change of m_games' order: what points into it by index
  // goes by the games' files again.
  void SetGames(std::vector<Library::Game>&& roms);
  void ApplyNames();
  std::array<std::string, CATEGORY_COUNT> SelectedPaths();
  void GamesChanged(const std::array<std::string, CATEGORY_COUNT>& selected, const std::string& online);
  void ForgetCovers();
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
  void MoveSettingRow(int direction, bool wrap = false);
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
  // The keyboard (00.80, src/TextKeyboard.h): the search's, or a text asked for (AskText).
  void HandleKeyboard(uint32_t pressed);
  void KeyboardDid(TextKeyboard::Result result, int direction, uint32_t button);
  void ShowToast(const std::string& text, int icon);
  // A game's name of the user's own (data_dir/names.ini) and a text asked for: the app's keyboard (00.80; until 00.79
  // the PS4's), with a title, the line under the field, at most `max_length` characters.
  void LoadNames();
  void SaveNames() const;
  void RenameGame(const std::string& key, const std::string& name);
  void AskText(const std::string& title, const std::string& text, std::function<void(const std::string&)> done,
               uint32_t max_length = 100, TextKeyboard::Mode mode = TextKeyboard::Mode::Text,
               const std::string& hint = "");
  // Voice Chat (00.23, src/Voice.h): Settings -> Voice Chat - its rows made by Voice (again while Test Microphone runs).
  void OpenVoicePanel();
  void BuildVoiceRows();
  // Online play (00.23, src/Netplay.h): Settings -> Online Play, a game's Triangle -> Play Online (a private or a
  // public room), the room's panel (made again every frame from the room as it is: UpdateOnline); the game a guest
  // plays, found in its games by the room's file (CRC32 and size), else by its title.
  void OpenOnlinePanel();  // (00.23: the Online tab now)
  void BuildOnlineItems();
  bool OnlineAction(const std::string& action);
  void OpenMyStats();
  std::string MyStatsDetail() const;
  std::string m_online_signature;      // what Online Settings' rows show (rebuilt when it changes)
  bool m_online_tab_entered = false;
  std::string OnlineNowLine() const;  // (00.23) "12 players online now" ("": not known)
  void OpenOnlineHost(int game, bool link = false);
  // (00.95) The Game Boy link cable online (src/GbLink.h): the guest's game for Game Boy 2 found among ours (m_games'
  // index, -1: not here), said to the host with our save, chosen in its panel; the match set before Emulator::Run.
  int FindLinkGame(const NetProto::LibraryGame& game);
  void SayLinkGame();
  void OpenLinkGamePicker();
  void SetGbLinkMatch();
  void OpenOnlineRoom();
  void BuildOnlinePanel();
  void UpdateOnline();
  bool OnlinePanelInput(uint32_t pressed);
  NetProto::LobbyGame FindOnlineGame(const NetProto::GameInfo& game, int* index);
  bool OnlineGameInfo(int game, NetProto::GameInfo* info);
  bool OnlineFileId(int game, uint32_t* crc, uint64_t* size);
  // (01.05, the owner) A game's version as its file's name says it ("USA, Rev 1"; "" none), and a player's country as
  // a small tag (its width; 0 none) - the rooms' cards and the Online tab's table.
  std::string OnlineVersionText(int game) const;
  bool MayBeContent(const Library::Game& rom, uint64_t size) const;
  // (01.05, the owner: "una marca debajo de las estrellas") A game's file the standard version or not (1, 2; 0 nothing
  // to show), and its mark: a green circle with a tick, an amber one with a bar - its word beside it (label) for the
  // game selected. Its width.
  int StandardMark(const Game& game) const;
  float DrawStandardMark(ImDrawList* dl, ImVec2 left_center, float r, int mark, float alpha, bool label);
  float StandardMarkWidth(float r, int mark, bool label) const;
  float DrawCountryTag(ImDrawList* dl, float x, float cy, const std::string& country, float size, float a);
  int OnlinePlayersMost(int game) const;
  std::string OnlinePlayersHelp() const;
  int m_online_game = -1;  // the game played online (m_games' index): the host's, or the one a guest found
  // (00.95) The room's players the host chose (Play Online's Players: 2 to m_online_players_most).
  int m_online_players = 2, m_online_players_most = 2;
  NetProto::LobbyGame m_online_found = NetProto::LobbyGame::Looking;
  // (00.95) Triangle -> Link Cable Online's room being made (m_online_link); the game for Game Boy 2 (m_games' index:
  // a guest's own - the room's game at first, when it has it -; the host's file of the guest's), a guest's said.
  bool m_online_link = false;
  int m_online_link_game = -1;
  bool m_online_link_said = false;
  std::vector<std::string> m_online_actions;  // what each option of an online panel does ("start", "kick:1"...)
  std::map<std::string, std::pair<uint32_t, uint64_t>> m_online_crcs;  // the files' CRC32 and size, by path
  // (01.06) Their whole files' (Netplay::FileCrc: v00.23's), read only for a room of such an app or our own room.
  std::map<std::string, std::pair<uint32_t, uint64_t>> m_online_file_crcs;
  // The listed rooms' games among ours (and their m_games' index), by game (console, CRC32, size, title).
  std::map<std::string, std::pair<NetProto::LobbyGame, int>> m_online_marks;
  // The Online tab (00.85): a table of the rooms there are - public and private, waiting or playing, with their game
  // -, asked for while the tab shows and again every few seconds (Netplay::Browse: the server's ListAll; (01.07) every
  // console's, the filters gone - (01.08) L2 / R2 the tab's pages); Cross joins the selected room (a private one with its
  // code, asked for), Square makes one, Triangle joins with a code, Options opens Online Settings (Nickname, Hide My
  // Address, the online
  // profile, My Stats). The player's own room, while there is one, on top (the last list under it, dimmed). The same
  // table in both styles: the Library's page, the XMB's column.
  struct OnlineRow
  {
    NetProto::RoomEntry room;
    std::string key;  // the selection's: "id:<n>" (an older server's list: "code:<code>"), "mine"
    int ping_ms = -1;
    NetProto::LobbyGame found = NetProto::LobbyGame::Missing;  // the room's game among ours
    int game = -1;      // ... its m_games' index (its cover)
    bool mine = false;  // the player's own room
    bool together = false;
  };
  void BuildOnlineTable();
  void DrawOnlineTable(ImDrawList* dl, float alpha, float x0, float x1, float top, float bottom, bool summary);
  bool OnlineTabInput(uint32_t pressed);
  // (`watch`: (01.15) as a spectator, though a player's place is left - the Touchpad.)
  void JoinOnlineRow(const OnlineRow& row, bool watch = false);
  // (01.15, the owner) The selected room takes a spectator now - its host lets them, a spectator's place is left, the
  // game is ours: the Touchpad watches it, a player's place left or not (Cross joins it as before). An older host's room
  // (before 01.05) never does: the server says it isn't watchable.
  bool OnlineRowCanWatch() const
  {
    if (m_online_index < 0 || m_online_index >= static_cast<int>(m_online_rows.size()))
      return false;
    const OnlineRow& row = m_online_rows[static_cast<size_t>(m_online_index)];
    return !row.mine && !row.together && row.room.watchable && row.room.watchers < NetProto::MAX_WATCHERS &&
           row.found != NetProto::LobbyGame::Missing && Netplay::CanJoinVersion(row.room);
  }
  // (01.05) The selected room's match may be watched (Cross watches it: the hint says so).
  bool OnlineRowWatch() const
  {
    if (m_online_index < 0 || m_online_index >= static_cast<int>(m_online_rows.size()))
      return false;
    const OnlineRow& row = m_online_rows[static_cast<size_t>(m_online_index)];
    const NetProto::RoomEntry& room = row.room;
    const bool late = room.playing && room.late && room.players < room.max_players;  // ((01.05) joined instead)
    return !row.mine && !late && room.watchable && room.watchers < NetProto::MAX_WATCHERS &&
           (room.playing || room.players >= room.max_players) && row.found != NetProto::LobbyGame::Missing;
  }
  void OpenOnlineSettings(const std::string& select = "");
  // (01.05, the owner: "Revisar mis juegos") Check My Games: console "" each console's count, else that console's
  // games; the rows come again as NetplayLibrary reads the files (m_check_signature).
  void OpenCheckGames(const std::string& console, const std::string& select = "");
  void BuildCheckRows();
  std::string CheckSignature() const;
  std::string m_check_console, m_check_signature;
  double m_check_at = 0.0;
  // (01.05, the owner: "haz una tabla con los más jugados") The Online tab's table shows the games most played online
  // instead of the rooms (m_online_top: Touchpad); its rows (BuildTopTable: the server's list - Netplay::TopGames - and
  // whether each is in our games: found 1 the same version, 2 another of its title, 0 none), the selected one.
  struct TopRow
  {
    Netplay::TopGame top;
    std::string title, version, standard, tab;
    const System* system = nullptr;
    bool link = false;  // a link cable room's (its game: a Game Boy's, a GBA's)
    int game = -1;      // ours (m_games' index)
    int found = 0;
  };
  void BuildTopTable();
  void DrawTopTable(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom);
  // (01.07, the owner) The Online tab's pages - Rooms, Chat (the world chat), Most Played (the table above),
  // Players (the ranking) -, L1 / R1 between them (the Library's: past the first or the last, the menu's tabs).
  enum OnlinePage
  {
    PAGE_ROOMS = 0,
    PAGE_CHAT = 1,
    PAGE_TOP = 2,
    PAGE_PLAYERS = 3,
    ONLINE_PAGES = 4,
  };
  int m_online_page = PAGE_ROOMS;
  bool OnlinePageInput(uint32_t pressed);
  void DrawOnlinePages(ImDrawList* dl, float alpha, float x0, float x1, float y);
  // The world chat's page (Netplay::GetWorldChat): its lines, the one selected (-1: none - the newest at the bottom),
  // a line's actions (mute its player, report it), the last answer said (a toast).
  void DrawWorldChat(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom);
  bool WorldChatInput(uint32_t pressed);
  void OpenWorldPhrases();
  void OpenWorldLine(int index);
  void WriteWorldLine();
  void WorldSaid(Netplay::WorldSayResult result);
  int m_world_index = -1;  // (the selected line's number - its serial -, -1: none)
  uint32_t m_world_said_seen = 0;
  // The players' ranking's page (Netplay::TopPlayers): the last 7 days or since ever (Triangle), the row selected.
  void DrawPlayersTable(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom);
  int m_players_days = NetProto::TOP_DAYS;
  int m_players_index = 0;
  std::vector<TopRow> m_top_rows;
  int m_top_index = 0;
  uint32_t m_top_serial = 0, m_top_logged = 0;
  float m_top_built = -10.0f;
  void BuildOnlineSettingsRows();
  std::string OnlineSettingsSignature() const;
  std::vector<std::string> OnlineConsoles() const;  // "" (every one), then those online
  std::string OnlineSummary() const;  // "12 players online now  ·  3 rooms" (translated)
  std::vector<OnlineRow> m_online_rows;          // as the table shows them (ours on top)
  std::vector<Netplay::RoomView> m_online_last;  // the last list that came whole
  int m_online_last_total = 0;
  bool m_online_listed = false;  // a list came (since the last failure)
  std::string m_online_row_key;  // the selected room's (kept while the list is asked for again)
  int m_online_index = 0;        // its row (m_selected's: BuildItems puts that one back to 0)
  // ((01.07, the owner: the console filter's row out - every console's rooms, the pages' tabs in its place.)
  float m_online_entered = 0.0f, m_online_refresh_at = 0.0f, m_online_retry_at = 0.0f;
  // Choose Together rooms (00.23): the library's files hashed on NetplayLibrary's thread (started with Online Play),
  // this console's list sent to the room once complete, each game of it by its file (console, CRC32, size), the
  // common list's rows as shown (every common game, or only those known to take 2 players or more).
  void StartOnlineLibrary();
  void BuildTogetherRows(const Netplay::View& v);
  bool m_online_library_sent = false;
  std::map<NetProto::LibraryGame, int> m_online_library;
  bool m_online_show_all = false;
  // (00.92, the owner) A Choose Together room with its players takes the whole screen (BuildTogetherRows,
  // DrawTogetherScreen, TogetherInput): the console tabs (L1 / R1: every console's games, then each console's, with
  // how many), the tab's games as a grid of covers, the players with the latency bar above them. The tab and the
  // selected game are kept by their console and their place in m_games while the room's lists change.
  struct TogetherCard
  {
    int game = -1, players = 0;
    bool suggested = false;
  };
  struct TogetherScreen
  {
    static constexpr int COLS = 5;  // cards a row ((01.05) 5: the room's chat beside them; 7 before)
    bool on = false;
    std::vector<std::string> tabs;  // "": every console's; then the consoles' keys
    std::vector<int> counts;
    int tab = 0;
    std::vector<TogetherCard> cards;  // the tab's
    int index = 0;
    float scroll = 0.0f;
    std::string status;  // the lists not in yet, or none in common ("": the cards)
    int known = 0, unknown = 0;  // games both have for 2 players or more; whose number of players isn't known
    int need = 2;                // (00.95) the room's players: the games for that many or more
  } m_together;
  std::string m_together_tab;
  int m_together_game = -1;
  // (A new room starts on the Online tab's console filter's tab; the log's last line of the tabs.)
  std::string m_together_room, m_together_said;
  void DrawTogetherScreen(ImDrawList* dl, float dt);
  bool TogetherInput(uint32_t pressed);
  void TogetherPick(int game);
  // (01.05, the owner) The room on the whole screen (DrawRoomScreen): the game and its cover, its players with their
  // ping and voice, the options (the host's Start, a player's actions...), the room's chat (DrawChatBox: a Choose
  // Together room's beside its games too). Over it, a picker: a quick phrase, or a player's actions (mute, block,
  // remove). L2: a quick phrase; R2: a line typed on the app's keyboard (src/Netplay.h: the chat).
  struct RoomPicker
  {
    bool open = false;
    std::string title;
    std::vector<std::string> items, actions;  // items translated when drawn (raw: as they are - a nickname)
    std::vector<bool> raw;
    int selected = 0;
  } m_room_picker;
  std::vector<std::string> m_room_notes;  // what the room says (BuildOnlinePanel): the game found, waiting, starting
  // (01.15, the owner: "todas las opciones a la vista sin tener que bajar por la lista") Every option on the room screen
  // at once: a player's actions on its card (column -1), under the cards the actions as buttons (column 0) and the
  // room's settings - Entry, Late Joining, Spectators: a value and a switch - beside them (column 1; Reject too, beside
  // Accept). A room without settings (a guest's): its actions in both columns. Up / Down: a column (through the cards),
  // Left / Right: the other column. (Until 01.14 a list, scrolled past four options.)
  struct RoomPlace
  {
    int column = 0, row = 0;
  };
  std::vector<RoomPlace> RoomPlaces() const;
  static bool RoomSetting(const std::string& action);
  bool RoomScreenShown() const;
  void MoveRoomSelection(uint32_t pressed);
  void DrawRoomScreen(ImDrawList* dl, float dt);
  void DrawChatBox(ImDrawList* dl, const Netplay::View& v, float x0, float y0, float x1, float y1, float a);
  void DrawRoomPicker(ImDrawList* dl, float a);
  bool RoomPickerInput(uint32_t pressed);
  bool PickerAction(const std::string& action);
  void OpenQuickPhrases();
  void OpenPlayerActions(int member);
  void WriteChatLine();
  void ChatSaid(Netplay::ChatResult result);
  // (00.86) The host's room stays open while the host goes anywhere (Circle on its panel goes back, Close Room closes
  // it): the Online tab's table lists the rooms on its connection, ours on top (Cross: back to it). A player asking to
  // join is a notice over whatever shows (Accept, Reject; Circle: later, in the room) - unless the room's panel shows
  // (Accept and Reject there). One room at a time: making or joining another while ours is open asks first to close
  // it (SET_OL_REPLACE; then m_online_after_close).
  bool HostingRoom() const;
  bool AskCloseRoom(bool join, std::function<void()> then);
  void OnlineAskInput(uint32_t pressed);
  void DrawOnlineAsk(ImDrawList* dl, float dt);
  std::function<void()> m_online_after_close;
  int m_online_ask = -1;              // the member asking, its notice shown (-1: none shown)
  uint32_t m_online_ask_serial = 0;   // the last request noticed (Netplay::View::asking_serial)
  int m_online_ask_choice = 0;        // Accept 0, Reject 1
  float m_online_ask_slide = 0.0f;
  std::string m_online_prefer;        // the room panel's option to select once it is there ("accept", "start")
  bool m_online_was_hosting = false;  // our room was open last frame (its end in the background is said)
  bool m_launch_online = false;       // the game launched is an online match (its end's error is the room's)
  bool m_online_back = false;         // (01.23) that match ended back in the room: the room shows (Run)
  // RetroAchievements (00.73, src/Achievements.h): Settings -> RetroAchievements - log in (the user name, then the
  // password, on the keyboard), log out; the panel follows the login as it goes (every frame).
  void OpenAchievementsPanel();
  void BuildAchievementsRows();
  void UpdateAchievements(float dt);
  std::string AchievementsDetail() const;
  Achievements::Login m_ra_login = Achievements::Login::Out;  // the login's state the panel and the row show
  std::string m_ra_user;          // the user name typed, until the password's keyboard opens
  int m_ra_ask_password = 0;      // frames until the password's keyboard opens (one closes before the next opens)
  float m_ra_refresh = 0.0f;
  // (00.75) The session's line as shown (the panels follow it when it changes), and My Achievements.
  std::string m_ra_status;
  void OpenHistoryPanel();
  void BuildHistoryRows();
  std::string m_ra_history_shown;  // what the history panel was made from (made again when it changes)
  // (00.76) My Achievements -> a game: every achievement of it.
  void OpenGameAchievements(uint32_t game_id);
  void BuildGameAchievementRows();
  std::string m_ra_game_shown;  // what the game's panel was made from
  uint32_t m_ra_game = 0;       // the game shown (Circle goes back to its row in My Achievements)
  bool m_ra_from_game = false;  // (00.79) opened from a game's Triangle -> Achievements: Circle goes back there
  // (00.79) Triangle -> Achievements on a file RetroAchievements doesn't know: the files it knows of that game.
  void OpenVersionsPanel(const Game& game);
  void BuildVersionsRows();
  std::string m_ra_versions_shown;
  // (00.79) Settings -> RetroAchievements -> What is RetroAchievements?: how to start, what it is.
  void OpenAboutPanel();
  // (00.77) RetroAchievements' marks on the games: the games handed to the module after each scan, their states taken
  // back once a second at most when it says something changed.
  void StartAchievementMarks();
  void RefreshAchievementMarks();
  bool m_ra_marks = true;         // the setting (Settings -> RetroAchievements -> Marks on the Games)
  int m_ra_lib_version = -1;
  float m_ra_lib_time = 0.0f;
  float m_ra_rebuilt = -10.0f;   // (00.85) when the filter's lists were last made again for the marks
  bool m_ra_counts_due = false;  // only their counts changed since: made again 3 s after it
  std::string AchievementLine(const Game& game) const;  // Information's line
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
  void UpdateNoCoverItem();
  mutable int m_no_cover_count = -1;  // (00.85) the games without a cover (-1: to count again)
  bool m_no_cover_due = false;
  float m_no_cover_time = -10.0f;
  // Updates (00.73, src/Update.h): Settings -> Check for Updates and the start's notice of a newer version (one
  // panel each), what they show (rebuilt when the updater's state changes) and their options; every frame.
  void OpenUpdatePanel(bool notice);
  void BuildUpdatePanel();
  void UpdatePanelChoice();
  void UpdateUpdater();
  std::string UpdatesDetail(const Update::Status& status) const;
  // Portable data (00.87, src/Portable.h): Settings -> Keep My Data on the USB Drive (its page, the copy's page, the
  // question for a drive never answered for, the notice for a drive used here connected meanwhile, the warnings), the
  // drives' look for portable data (each time the menu's drives change) and the restart that switches the data.
  std::string PortableDetail() const;
  void OpenPortablePanel();
  void OpenPortableQuestion(bool notice);
  void OpenPortableWarning(const std::string& warning);
  void PortablePanelChoice();
  // (00.89) The anonymous statistics' notice, once (STATS_NOTICE): wanted, shown, the statistics started after it.
  bool StatsNoticeWanted() const;
  void OpenStatsNotice();
  void StartStats();
  void OpenHelpPanel();  // (00.90) Settings -> How to Help
  // (00.91) Send Log (src/LogReport.h): asked, sent, its code; the question after a crash.
  void OpenSendLogPanel();
  void BuildSendLogPanel();
  void SendLogChoice();
  void OpenCrashQuestion();
  void StartSendingLog();
  void StartPortableCopy(Portable::Copying copying, const std::string& drive, Portable::Policy policy);
  void BuildPortableCopyPanel();
  bool PortableCopyBusy() const;
  void UpdatePortable();
  void PortableLook(const std::vector<std::string>& drives);
  void RestartApp(const std::string& why);
  // Deleting games: one (Triangle's options), or the duplicates and the other regions (Clean Up).
  void OpenCleanUp();
  // (01.19, the owner: "Borrar" deletes everything of the game) `saves`: its saves and states too (Delete Game's
  // choice, off by default; never Clean Up's).
  void DeleteGames(const std::vector<std::string>& paths, bool saves = false);
  // What else is a deleted game's: its cover (the pictures no remaining game shows), name, own settings, controls,
  // cheats, rating, saves and states (`saves`) - only when no remaining game has its key (a copy of it elsewhere keeps
  // them). (DeleteGames: its downloaded bezel, the recents' and RetroAchievements' lines of its files, for them all at
  // once.) What the remaining games keep is looked at once (Clean Up deletes thousands). Returns what went (the log).
  struct KeptByOthers
  {
    std::unordered_set<std::string> keys, names, pictures;  // the remaining games' keys, "<console>/<id>", pictures
  };
  std::string ForgetGame(const Game& game, bool saves, const KeptByOthers& kept);
  void SetDeleteGameLines();
  std::string OptionHelp(const std::string& option) const;
  void TourCommand(const std::string& command);
  // Saved data.
  void LoadSaves();
  void OpenSavesPanel(int save);
  std::string SaveTitle(const std::string& id) const;
  ImTextureID StatePicture(const std::string& id, int slot, float* aspect = nullptr);
  void ForgetStatePictures();

  // Covers: loaded on a thread when a game comes into view, a few dozen kept.
  void IndexCovers();
  void CoverSaved(const std::string& name, const std::string& path);
  ImTextureID Cover(int game, float* aspect);
  void UpdateCovers();
  void StopCoverThread();
  void StartCoverDownload(bool closest = false);
  void UpdateCoverDownload();
  bool MissingCovers() const;
  bool HasCover(const Library::Game& rom) const;
  // Star ratings (src/Ratings.h): the games not matched yet go to the matcher's thread (StartRatings, after the
  // games are read; again: every game, after a new download); what it found comes into the games (UpdateRatings,
  // every frame) and stays, by game key. LaunchBox's ratings are downloaded when the menu opens without them (or
  // with ones 60 days old), once a session, or from Settings -> Star Ratings (StartRatingsDownload).
  void StartRatings(bool again = false);
  void UpdateRatings();
  bool StartRatingsDownload();
  // A game's rating as Information says it (its stars are drawn on the first line), and Star Ratings' lines.
  std::vector<std::string> RatingLines(const Game& game) const;
  std::vector<std::string> RatingsPanelLines();
  std::string RatingsDownloadText() const;  // "Downloading ratings 45 / 109 MB", else ""
  // Game Bezels (00.80, src/BezelProject.h): The Bezel Project's bezels of the player's games, downloaded on its
  // thread (paused while a game runs; a download cut goes on at the next start), the page's rows as it goes
  // (UpdateBezelDownload, every frame), Delete Downloaded Bezels.
  std::vector<std::string> BezelConsoles() const;  // the consoles with games that The Bezel Project has bezels for
  bool StartBezelDownload(const std::vector<std::string>& consoles, bool resumed);
  void OpenBezelsPanel();
  void BuildBezelRows();
  void UpdateBezelDownload();
  // (the bezels there are counted the first time; (01.11) count false: not now - the text without the count)
  std::string BezelsDetail(bool count = true);
  // (01.09, the owner, src/ThreeDsCia.h) A 3DS game's Triangle -> Updates, DLC and Mods: its update and DLC (removed,
  // its save copied first), its mods on or off, and ((01.22) until then: Settings -> 3DS Updates and DLC) the CIAs of
  // the 3DS's game folders' "cia" folders that are its own, one installed at a time on its thread (Update3dsInstall,
  // every frame), then its CIA deleted if the player wants. Not in a build without the 3DS.
#ifdef EMUSUNI_3DS
  void List3dsCias();  // m_cias: every CIA of the "cia" folders (made where there is none), each one's information
  void Open3dsCiaConfirm(size_t index);
  void Update3dsInstall();
  // select_key: that row selected; reread false: the CIAs as listed (back from one of them).
  void Open3dsExtras(int game, const std::string& select_key = {}, bool reread = true);
  void Ask3dsRemove(bool dlc);  // "Remove?" for its update or its DLC
  void ThreeDsPanelChoice();  // Cross on one of their options
  bool ThreeDsPanelBack();    // Circle on one of them (true: taken)
  std::string ThreeDsSaveDir() const;  // the 3DS's save folder (the core's: <UserDir>/saves/3ds)
  // The title of the player's 3DS game with that title ID ("" none: its file's header read once, m_3ds_titles).
  std::string ThreeDsGameName(uint64_t game_title_id);
  // A CIA said in a line: its game (the player's game's title, its own name, or its file's), what it is, its version.
  std::string CiaLine(const ThreeDsCia::File& file);
#endif
  // (01.22, src/GameExtras.h) An Arcade game's Triangle -> Patched Romset: the one FinalBurn Neo uses (Remove), and
  // those of its set in the Arcade's "patched" folders (Roms/arcade/patched), one copied in at a time on its thread
  // (UpdatePatchedCopy, every frame).
  void OpenArcadePatched(int game, const std::string& select_key = {});
  void OpenPatchedConfirm(size_t index);
  void AskPatchedRemove();
  void UpdatePatchedCopy();
  void ArcadePanelChoice();  // Cross on one of their options
  bool ArcadePanelBack();    // Circle on one of them (true: taken)
  std::string PatchedDir() const;  // <the Arcade's system folder>/fbneo/patched (Emulator.cpp's)
  // The game's index in m_games by its file (-1 gone: the games read again meanwhile).
  int GameIndexOf(const std::string& path) const;
  // A game's extras panels: Start the Game, its row (after the game closed for an install or removal).
  void StartGameFromPanel(int game);
  // As a game starts: the folders the in-game menu looks into for what it can install (GameExtras::InstallFolders).
  void TellInstallFolders(SystemId system);
  // Back from a game closed by its in-game menu to install or remove something: that game's panel, on that.
  void OpenAfterGame(const GameExtras::AfterGame& after);

  bool Library() const { return m_prefs.style != "xmb"; }
  int LibraryTab() const;
  void LoadPlayed();
  void SavePlayed() const;
  std::string SortName() const;

  void Draw(float dt);
  void DrawBackground();
  void DrawCategoryIcon(ImDrawList* dl, Category category, ImVec2 center, float size, ImU32 color);
  void DrawOnlineMark(ImDrawList* dl, ImVec2 center, float size, float alpha);
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
  // stars_x: where its rating's stars are centred (the part of it in view), -1: its middle.
  void DrawCover(ImDrawList* dl, int game, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float alpha,
                 int focus, float stars_x = -1.0f);
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
  // (00.87) The player's data folder (src/Portable.h): a USB drive's portable data, else m_data_dir - xmb.ini, the
  // covers, cheats, themes, bezels, favorites, names, played games and the game list's cache. m_data_dir keeps what is
  // this PS4's own (its Roms folders, the caches, the logins, the statistics).
  std::string UserDir() const { return Portable::UserDir(); }
  Preferences m_prefs;
  std::vector<Game> m_games;
  // (00.85) The read's thread and what goes between it and the menu (under m_read_mutex): the read asked for and not
  // begun, the one at work (games or not), what it found (the covers first, then the games); its progress (atomics).
  std::thread m_read_thread;
  mutable std::mutex m_read_mutex;
  std::condition_variable m_read_wake;
  bool m_read_quit = false;
  std::optional<ReadAsk> m_read_ask;
  bool m_read_busy = false, m_read_busy_games = false;
  std::optional<ReadDone> m_read_covers, m_read_done;
  Library::ScanProgress m_read_progress;
  // The thread's own: the files' contents it learnt (and the cache's), what the cache on disk has.
  Library::Contents m_read_contents;
  uint64_t m_cache_fingerprint = 0;
  size_t m_cache_facts = 0;
  Library::Places m_cache_places;
  std::array<std::string, SYSTEM_COUNT> m_cache_chosen;
  // The menu's: m_games is a read's (or the cache's) - false at the first start until its read ends -, a read of this
  // session is in (the start's covers download waits for it, and for the covers' list), the read m_games was made from
  // (Library::Fingerprint: the same found again leaves m_games as it is), a renamed game's own title (by key), the
  // count the "Reading your games..." items show.
  bool m_games_known = false, m_games_read = false, m_covers_listed = false;
  uint64_t m_games_fingerprint = 0;
  std::map<std::string, std::string> m_own_titles;
  int m_reading_shown = -1;
  bool m_running = false;  // Run() is drawing the menu (not before its first time, nor while a game runs)
  bool m_read_resume = false;  // a read of the games was stopped for a game: again when the menu comes back
  std::vector<std::pair<NetProto::LibraryGame, std::string>> m_online_library_files;  // (SelectedPaths' note)
  // Where the games were looked for (the last read): the places, each console's number of games, the covers'
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
  // (00.80) The RetroAchievements filter (Touchpad): each game list's games before it (the search's), those of the
  // consoles it has, those not checked yet, and whether it emptied the list.
  struct RaCount
  {
    int total = 0, files = 0, unchecked = 0;
  };
  RaCount m_ra_count[CATEGORY_COUNT];
  bool m_ra_emptied[CATEGORY_COUNT] = {};
  bool m_ra_rebuild = false;  // the games' states changed under a panel: the lists made again once it closes
  bool RaFilterOn() const { return m_ra_marks && m_prefs.ra_filter != "off"; }
  bool RaFilterPasses(const Game& game) const;
  std::string RaFilterHiddenLine(int c) const;  // (00.89) "Only games with achievements - 355 hidden - Touchpad: show all"
  std::string RaFilterLine(int c) const;  // "With achievements: 438 of 793  -  checking 1200 of 3585..."
  // (01.18, the owner: the Library filtered by genre and players) Options in a list of games: Order and Filters
  // (SET_LIST_VIEW) - the order (as Options went through them until 01.17), a genre, the players, Hide Adult Games.
  // A game's genres and players are its rating's (src/Ratings.h: LaunchBox's, an arcade set's category); one without
  // them is left out while a genre or players are chosen. The genre and players last the session.
  int m_filter_genre = -1;    // Ratings::GenreName's index; -1 every genre
  int m_filter_players = 0;   // 0 any, 1 one player only, 2 two or more, 5 cooperative ((01.23) no 3 or 4 any more)
  int m_view_hidden[CATEGORY_COUNT] = {};   // each list's games the genre or players left out
  bool m_view_emptied[CATEGORY_COUNT] = {};  // ... all of them
  int m_view_category = -1;                  // the list Order and Filters was opened in (its genres' counts)
  std::vector<int> m_view_genres;            // ... the genres of its games (Genre's choices after Every Genre)
  int m_view_players_known = 0;              // ... its games whose players are known (none: said when chosen)
  bool ViewFilterOn() const { return m_filter_genre >= 0 || m_filter_players > 0; }
  bool ViewPasses(const Game& game) const;
  std::string ViewFilterLine(int c) const;  // "Genre: Fighting  -  2 Players or More  -  355 hidden  -  Options: change"
  static std::string PlayersName(int players);
  void OpenViewPanel();
  void BuildViewRows();
  void ChangeViewRow(int direction);
  void ViewChanged(int list = -1);  // (-1: the list Order and Filters was opened in)
  SidePanel m_panel;
  bool m_assets_loaded = false;

  Audio m_audio;
  Input m_input;
  // (00.76) This pass's presses that are a held button's repeat (Input::Repeated): a list goes round only on a fresh
  // press. The last change of a console tab or an XMB column (m_time): its covers and the drives' look wait for
  // MenuNav::TAB_SETTLE; a look asked for meanwhile waits too.
  uint32_t m_repeated = 0;
  float m_tab_changed = -1.0f;
  bool m_places_waiting = false;
  bool Fresh(uint32_t button) const { return (m_repeated & button) == 0; }
  bool TabsSettled() const { return m_time - m_tab_changed >= MenuNav::TAB_SETTLE; }
  std::map<std::string, std::unique_ptr<Sound>> m_sounds;
  ImTextureID m_wallpaper{};
  ImTextureID m_logo{};  // the "EmuSuNi" letters
  ImTextureID m_help_qr{};  // (00.90) How to Help's QR code (loaded the first time it opens)
  // (01.04, the owner) The group on Telegram's (assets/xmb/telegram-qr.png, scripts/make-help-qr.py): How to Help, beside
  // the website's, and a log sent, under its code.
  ImTextureID m_telegram_qr{};
  float m_logo_aspect = 4.0f;
  ImTextureID m_glow{};  // a soft round light (white, gaussian alpha): the intro's stardust
  ImTextureID m_star{};  // a star (white, soft edges): the ratings' stars, tinted (DrawRating)

  // Controls page: a game's own layout (its key) or its console's normal one; a button capture.
  bool m_controls_open = false;
  bool m_order_moving = false;  // Console Order: the selected console is picked up (Up / Down move it)
  std::string m_controls_game;
  SystemId m_controls_system = SystemId::SNES;
  bool m_capture = false;
  bool m_capture_released = false;
  float m_capture_time = 0.0f;
  int m_capture_row = 0;
  // (00.89) A macro's row: its button first (step 0), then its presses held together (step 1: Controls::IsMacro).
  int m_capture_step = 0;
  std::string m_capture_trigger;
  uint32_t m_capture_chord = 0;

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
  int m_update_serial = -1;     // the updater's state the update panel and the Settings row show
  bool m_update_started = false;
  bool m_stats_notice = false;   // (00.89) the statistics' notice to show when the menu is free
  bool m_stats_waiting = false;  //   UsageStats::Start waits for it (answered or closed)
  bool m_report_ask = false;     // (00.91) EmuSuNi closed by itself last time: the question when the menu is free
  int m_report_serial = -1;      //   the send's state the Send Log panel shows
  bool m_report_confirm = false;  //   it shows what goes and Send (not the send's state)
  // Portable data (00.87, src/Portable.h).
  std::vector<std::string> m_portable_actions;  // what each option of a portable panel does ("on:/usb0", "off"...)
  std::string m_portable_drive, m_portable_id;  // the drive (its root, its data's id) a question or notice is for
  std::string m_portable_pending;               // "ask" or "notice" waiting for the menu to be free
  std::string m_portable_warning;               // a warning waiting for the menu to be free
  std::vector<std::string> m_portable_drives;   // the drives of the last look
  bool m_portable_looked = false;
  std::set<std::string> m_portable_said;        // the drives' data asked or told about this session
  Portable::CopyPlan m_portable_plan;           // the copy under way or done
  std::unique_ptr<Portable::CopyProgress> m_portable_progress;
  std::thread m_portable_thread;
  bool m_portable_ok = false;                   // the copy done, every file there (and the drive marked)
  bool m_portable_ended = false;                // the copy's end taken (its thread joined)
  float m_portable_built = 0.0f;                // when its page was made last (m_time)
  int m_search_selected = 0;

  // Saved data.
  std::vector<Saves::GameSaves> m_saves;
  int m_saves_panel = -1;
  std::string m_delete_key;  // "sram" or "slot:N" waiting for "Delete?"
  std::map<std::string, ImTextureID> m_state_pictures;  // "<id>/<slot>"
  std::map<std::string, float> m_state_aspects;         // (00.89) their width / height
  std::string m_state_pictures_game;

  // Covers.
  std::map<std::string, std::string> m_cover_files;  // lower-case name without its extension -> path (CoverFiles)
  std::vector<std::pair<std::string, std::string>> m_covers_saved;  // (CoverSaved: saved while a list was made)
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
    std::string path, why, detail;  // a picture that couldn't be read: why (LoadPicture's words)
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
  // Why the last downloads left each game without a cover (game key -> CoverDownload's plain reason), said under
  // Games Without Cover's list for the selected game (00.68).
  std::unordered_map<std::string, std::string> m_cover_why;
  // Star ratings: each game's once matched (game key -> its rating; half_stars -1: none), the thread matching the
  // others, and the games it was given (by key: they aren't given again while it works).
  std::unordered_map<std::string, Ratings::Rating> m_ratings;
  Ratings::Matcher m_rating_matcher;
  std::set<std::string> m_rating_asked;
  bool m_rating_rematch = false;  // a new copy came while the matcher worked: every game again after it
  // LaunchBox's ratings downloaded (data_dir/cache/ratings/launchbox.tsv), and whether this session tried already.
  Ratings::Downloader m_rating_download;
  bool m_rating_download_tried = false;
  Ratings::Copy m_rating_copy;  // what is there (read at the start and after each download)
  // Game Bezels (00.80): the download, the progress the page last showed, the bezels there (per console, read again
  // after each download or deletion), whether this session resumed a download cut before.
  BezelProject::Downloader m_bezel_download;
  unsigned m_bezel_serial = 0;
  std::map<std::string, BezelProject::Downloaded> m_bezels_there;
  bool m_bezels_counted = false;
  bool m_bezel_resume_tried = false;
  // (01.11) The start after one the start-up watchdog closed (main.cpp: SafeStart): no big inventory this session - the
  // bezels counted only when their page opens, no star ratings matched, no covers', bezels' or ratings' download.
  bool m_safe_start = false;
  bool m_ratings_waiting = false;  // (01.11) the games to match wait for the menu's first frames (StartRatings)

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
#endif
  std::string m_pending_error;
  std::string m_pending_title;  // (its panel's title: "" the game couldn't start)
  float m_memory_log = 0.0f;

  // Favorites (game keys, data_dir/favorites.ini) and the search.
  std::set<std::string> m_favorites;
  std::string m_search;
  bool m_keyboard = false;  // the keyboard is up (m_board: the search's, or a text asked for)
  TextKeyboard::Board m_board;
  float m_keyboard_slide = 0.0f;
  std::string m_toast;  // a short notice at the top ("Added to Favorites")
  int m_toast_icon = 0;  // 1 a star, 2 an empty star
  float m_toast_time = 0.0f;

  // The user's names for games (key -> name).
  std::map<std::string, std::string> m_names;
  // A text asked for (the keyboard's Text, Plain or Password mode): what OK does with it, the keyboard's title and the
  // line under its field.
  std::function<void(const std::string&)> m_ask_done;
  std::string m_ask_title, m_ask_hint;
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
  bool m_delete_saves = false;  // (01.19) Delete Game: its saves and states too (off each time it opens)
  std::string m_delete_title, m_delete_file;  // ... the game's title and file, for its lines
  size_t m_delete_files = 0;                  // ... its files' count
  std::vector<std::string> m_cleanup_dups, m_cleanup_others[SYSTEM_COUNT];
  std::string m_cleanup_system;  // Keep Only USA Games' console ("snes", "nes", "n64")
#ifdef EMUSUNI_3DS
  // (01.09) 3DS Updates and DLC: the CIAs listed, the one chosen; a game's Updates, DLC and Mods: the game, its title
  // ID, what its options do, the title to remove; each 3DS game file's title ID once read (by its path).
  std::vector<ThreeDsCia::File> m_cias;
  size_t m_cia_chosen = 0;
  int m_3ds_game = -1;
  std::string m_3ds_path;  // ((01.22) its file: its index again if the games were read meanwhile)
  uint64_t m_3ds_title = 0;
  std::vector<std::string> m_3ds_actions;
  uint64_t m_3ds_remove = 0;
  std::unordered_map<std::string, uint64_t> m_3ds_titles;
#endif
  // (01.22) An Arcade game's Patched Romset: the game, those found, the one chosen; a game's panel opened after the
  // game closed for it (Start the Game at its top).
  int m_patched_game = -1;
  std::string m_patched_path;
  std::vector<GameExtras::PatchedFile> m_patched_found;
  size_t m_patched_chosen = 0;
  bool m_extras_from_game = false;
  // The screenshot tour (Tour.h) is running; the menu style to put back at its end.
  bool m_tour = false;
  std::string m_tour_style;
};

// ---------------------------------------------------------------------------------------------
// Games and assets.

// ---- Reading the games (00.85: big libraries) ----

std::array<std::string, SYSTEM_COUNT> Menu::Impl::ChosenFolders() const
{
  std::array<std::string, SYSTEM_COUNT> chosen;
  for (const System& system : Systems())
    chosen[static_cast<size_t>(system.id)] = GamesFolder(system.id);
  return chosen;
}

// (00.87) The folders asked for as the library's cache keeps them: one chosen (Game Folders) on the USB drive with the
// player's portable data relative to the drive (src/Portable.h: the same drive on usb1 is the same folder), a console's
// default folder as it is ("/usb0/Roms/n64": whichever drive is usb0). As they are without portable data.
std::array<std::string, SYSTEM_COUNT> Menu::Impl::StoredChosen(const std::array<std::string, SYSTEM_COUNT>& chosen) const
{
  std::array<std::string, SYSTEM_COUNT> stored = chosen;
  for (const System& system : Systems())
  {
    std::string& folder = stored[static_cast<size_t>(system.id)];
    if (folder != DefaultFolder(system))
      folder = Portable::ToStored(folder);
  }
  return stored;
}

// The last read's games (the library's cache), before the read's thread starts: shown at once when this version read
// them in the same folders. The files' contents it learnt go to the thread either way (a file it looked inside isn't
// opened again while it is the same).
void Menu::Impl::LoadCachedGames()
{
  const auto start = Clock::now();
  Library::Cache cache;
  // (00.87) A USB drive's portable data (src/Portable.h): its own cache, its paths relative to the drive.
  const std::string drive = Portable::Active() ? Portable::DriveRoot() : std::string();
  if (!Library::LoadCache(UserDir() + "/cache/library.tsv", &cache, drive))
  {
    Log("games: none read before (no cache): read now, the menu meanwhile");
    return;
  }
  if (!drive.empty() && !cache.writer.empty() && cache.writer != Portable::LibraryWriter())
  {
    // Written by another PS4: its games on its own storage and on its other drives aren't this one's - only the drive's
    // are shown at once (the read on its thread finds this PS4's own).
    const size_t before = cache.found.games.size();
    std::erase_if(cache.found.games, [&drive](const Library::Game& game) {
      return game.path.compare(0, drive.size() + 1, drive + "/") != 0;
    });
    cache.found.counts.fill(0);
    for (const Library::Game& game : cache.found.games)
      cache.found.counts[static_cast<size_t>(game.system)]++;
    Log("games: the cache on the USB drive was written by another PS4: its " + std::to_string(cache.found.games.size()) +
        " games on the drive shown, " + std::to_string(before - cache.found.games.size()) + " left out");
  }
  m_read_contents = std::move(cache.contents);
  if (cache.version != App::VERSION || cache.chosen != StoredChosen(ChosenFolders()))
  {
    Log("games: the last read was " +
        (cache.version != App::VERSION ? "version " + cache.version + "'s" : std::string("of other folders")) +
        ": read now, the menu meanwhile");
    return;
  }
  m_cache_fingerprint = Library::Fingerprint(cache.found);
  m_cache_facts = m_read_contents.size();
  m_cache_places = cache.places;
  m_cache_chosen = cache.chosen;
  m_places = cache.places;
  m_console_games = cache.found.counts;
  const size_t count = cache.found.games.size();
  SetGames(std::move(cache.found.games));
  m_games_fingerprint = m_cache_fingerprint;
  m_games_known = true;
  char line[160];
  std::snprintf(line, sizeof(line), "games: %zu as the last read found them (%.0f ms); read again on a thread", count,
                std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  Log(line);
}

// A read for the thread: the games (and with them the places and the covers), the covers alone (Covers Saved On), or
// a look at the places (neither: the games when they changed - a drive plugged in or taken out while a game ran). A
// read of the games asked for while one runs stops that one at its next file (it starts over with the folders as they
// are now); a look while a read is under way or waiting is left out (that one answers it).
void Menu::Impl::AskRead(bool games, bool covers)
{
  ReadAsk ask;
  ask.games = games;
  ask.covers = covers || games;
  ask.chosen = ChosenFolders();
  ask.covers_on = Settings::Get("covers_on");
  ask.known = m_places;
  {
    std::lock_guard lock(m_read_mutex);
    if (!ask.games && !ask.covers && (m_read_busy || m_read_ask || m_read_done))
      return;
    if (m_read_ask)
    {
      ask.games = ask.games || m_read_ask->games;
      ask.covers = ask.covers || m_read_ask->covers;
    }
    if (ask.games && m_read_busy)
      m_read_progress.stop = true;
    m_read_ask = std::move(ask);
  }
  if (!m_read_thread.joinable())
  {
    try
    {
      m_read_thread = std::thread([this] { ReadMain(); });
    }
    catch (...)
    {
      // (No thread: the read here, as before 00.85.)
      Log("games: no thread to read them on: read now");
      ReadAsk now;
      {
        std::lock_guard lock(m_read_mutex);
        now = std::move(*m_read_ask);
        m_read_ask.reset();
      }
      ReadDone done;
      if (ReadNow(now, &done) && (done.scanned || done.places_changed))
      {
        std::lock_guard lock(m_read_mutex);
        m_read_done = std::move(done);
      }
      return;
    }
  }
  m_read_wake.notify_all();
}

// The read's thread: one read after another, the newest asked for.
void Menu::Impl::ReadMain()
{
  for (;;)
  {
    ReadAsk ask;
    {
      std::unique_lock lock(m_read_mutex);
      m_read_wake.wait(lock, [this] { return m_read_quit || m_read_ask.has_value(); });
      if (m_read_quit)
        return;
      ask = std::move(*m_read_ask);
      m_read_ask.reset();
      m_read_busy = true;
      m_read_busy_games = ask.games;
      m_read_progress.stop = false;
      m_read_progress.games = 0;
    }
    ReadDone done;
    const bool complete = ReadNow(ask, &done);
    std::lock_guard lock(m_read_mutex);
    m_read_busy = m_read_busy_games = false;
    if (complete && (done.scanned || done.places_changed))
      m_read_done = std::move(done);
  }
}

// One read (on the thread): the USB drives, the places, the covers' pictures (handed to the menu at once), then every
// console's games in its folder (Game Folders, else its default one) and, without a folder chosen, in Roms/<console>
// of the internal storage and of every USB drive (Library::ScanAll: a folder shared with another console has its .zip
// files opened to see whose they are, a file found twice is one game); what it found kept for the next start.
bool Menu::Impl::ReadNow(const ReadAsk& ask, ReadDone* done)
{
  const auto start = Clock::now();
  const std::vector<std::string> drives = Library::UsbDrives();
  bool games = ask.games;
  const bool covers = ask.covers;
  if (!games)
  {
    done->places = Library::FindPlaces(m_data_dir, drives);
    if (done->places != ask.known)
    {
      Log("game places changed: " + std::to_string(ask.known.drives.size()) + " -> " +
          std::to_string(done->places.drives.size()) + " USB drives: the games are read again");
      done->places_changed = games = true;
      std::lock_guard lock(m_read_mutex);
      m_read_busy_games = true;
    }
    else if (!covers)
    {
      return true;  // (nothing changed)
    }
  }
  if (games)
  {
    MakeFolders(drives);  // (first: the folders made are found below, the next look sees the same places)
    done->places = Library::FindPlaces(m_data_dir, drives);
  }
  done->usb_covers = Library::UsbCoversFolder(done->places.drives);
  if (covers || games)
  {
    ReadDone listed;
    ListCovers(done->places.drives, done->usb_covers, ask.covers_on, &listed);
    std::lock_guard lock(m_read_mutex);
    m_read_covers = std::move(listed);  // (the menu has them at once: the games may take a while)
  }
  if (!games)
    return true;
  Library::Found found = Library::ScanAll(ask.chosen, done->places, &m_read_progress, &m_read_contents);
  if (found.stopped)
  {
    Log("games: the read stopped (another one was asked for)");
    return false;
  }
  done->fingerprint = Library::Fingerprint(found);
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  std::string counts, drives_line;
  for (const std::string& drive : done->places.drives)
    drives_line += (drives_line.empty() ? "" : ", ") + drive;
  for (const System& system : Systems())
  {
    const size_t id = static_cast<size_t>(system.id);
    std::string where;
    for (size_t f = 0; f < found.folders[id].size() && f < found.found_in[id].size(); f++)
    {
      if (found.found_in[id][f] > 0)
        where += (where.empty() ? " in " : " + ") + found.folders[id][f];
    }
    counts += std::string(counts.empty() ? "" : ", ") + std::to_string(found.counts[id]) + " " + system.tab + where;
  }
  Log("game places: USB drives " + (drives_line.empty() ? std::string("none") : drives_line) + "; covers on USB: " +
      (done->usb_covers.empty() ? std::string("none") : done->usb_covers));
  char took[96];
  std::snprintf(took, sizeof(took), " (read in %.1f s; %zu files' contents known)", seconds, m_read_contents.size());
  Log("games: " + counts + took);
  if (found.missing > 0)
    Log("games: " + std::to_string(found.missing) + " file(s) their folder lists aren't there (left out; deleted while "
        "another program had them open?): " + found.missing_first);
  // The next start's games: written when they (or the places, the folders, the files' contents) aren't the cache's.
  if (done->fingerprint != m_cache_fingerprint || m_read_contents.size() != m_cache_facts ||
      done->places != m_cache_places || StoredChosen(ask.chosen) != m_cache_chosen)
  {
    const auto save_start = Clock::now();
    // (00.87) A USB drive's portable data (src/Portable.h): its paths relative to the drive, with a new writer's word
    // (this PS4's, kept here once the file is written: the next start knows it wrote it).
    const std::string user = UserDir();
    const std::string drive = Portable::Active() ? Portable::DriveRoot() : std::string();
    const std::string writer = drive.empty() ? std::string() : Portable::NewWord();
    MakeDirs(user + "/cache");
    if (Library::SaveCache(user + "/cache/library.tsv", App::VERSION, StoredChosen(ask.chosen), done->places, found,
                           m_read_contents, drive, writer))
    {
      if (!writer.empty())
        Portable::SetLibraryWriter(writer);
      m_cache_fingerprint = done->fingerprint;
      m_cache_facts = m_read_contents.size();
      m_cache_places = done->places;
      m_cache_chosen = StoredChosen(ask.chosen);
      char line[96];
      std::snprintf(line, sizeof(line), "games: kept for the next start (%.0f ms)",
                    std::chrono::duration<double, std::milli>(Clock::now() - save_start).count());
      Log(line);
    }
    else
    {
      Log("games: the cache couldn't be written: the next start reads them first");
    }
  }
  done->found = std::move(found);
  done->scanned = true;
  return true;
}

// Covers: data_dir/covers/<console>/<file name> or <title> (.png, .jpg, .jpeg, any case), e.g. from libretro's
// thumbnails ("Named_Boxarts"): covers/snes/, covers/nes/... (src/CoverFiles.h; the Super Nintendo's right in
// covers/ too, as before 00.81, moved into covers/snes/ once). Indexed by the game key's way: "nes/<name>". The
// USB drives' EmuSuNi/covers too (Covers Saved On), the place new covers are saved in last: its picture wins over
// another place's of the same name. (00.85: listed on the read's thread; the menu takes them in CoversRead.)
void Menu::Impl::ListCovers(const std::vector<std::string>& drives, const std::string& usb_covers,
                            const std::string& covers_on, ReadDone* done)
{
  const std::string internal = UserDir() + "/covers";
  const std::string save = covers_on != "usb" || usb_covers.empty() ? internal : usb_covers;  // (CoversSaveDir's)
  std::vector<std::string> places;
  for (const std::string& folder : Library::UsbCoversFolders(drives))
  {
    if (Library::PathKey(folder, drives) != Library::PathKey(save, drives))
      places.push_back(folder);
  }
  if (internal != save)
    places.push_back(internal);
  places.push_back(save);
  for (const std::string& place : places)
  {
    CoverFiles::MoveOldSnesCovers(place);
    CoverFiles::List(place, &done->cover_files);
  }
  std::string from;
  for (const std::string& place : places)
    from += (from.empty() ? "" : ", ") + place;
  done->usb_covers = usb_covers;
  done->covers_line =
      "covers: " + std::to_string(done->cover_files.size()) + " pictures (" + from + "; new ones go to " + save + ")";
}

void Menu::Impl::TakeRead()
{
  std::optional<ReadDone> covers, games;
  {
    std::lock_guard lock(m_read_mutex);
    covers.swap(m_read_covers);
    if (m_read_done && !(m_launch >= 0 || (m_panel.open && m_panel.title == "Game")))
      games.swap(m_read_done);
  }
  if (covers)
    CoversRead(*covers);
  if (games)
    GamesRead(*games);
}

void Menu::Impl::CoversRead(ReadDone& done)
{
  m_cover_files = std::move(done.cover_files);
  // (The covers saved while the list was made - downloaded, Find Cover's - stay.)
  for (const auto& [name, path] : m_covers_saved)
    m_cover_files[name] = path;
  m_covers_saved.clear();
  m_usb_covers = done.usb_covers;
  ForgetCovers();
  m_covers_listed = true;
  Log(done.covers_line);
  RefreshNoCoverItem();
  m_covers_changed = true;  // (Without Cover's lists again: Run)
  if (m_panel.open && m_panel.setting == SET_NO_COVER)
    BuildNoCoverRows();
}

// A read's games into the menu: unless they are those it shows already (the same read again: nothing moves), every
// list made again, each keeping its selected game; then the covers of a new folder's games downloaded.
void Menu::Impl::GamesRead(ReadDone& done)
{
  std::array<std::string, CATEGORY_COUNT> selected = SelectedPaths();
  // (The first games of a start without them before - none read yet -: each list on the game played last, as Run puts
  // it when it has them.)
  if (!m_games_known)
    selected.fill(m_prefs.last_game);
  const std::string online = m_online_game >= 0 && m_online_game < static_cast<int>(m_games.size()) ?
                                 m_games[static_cast<size_t>(m_online_game)].rom.path :
                                 std::string();
  m_places = std::move(done.places);
  m_places_reads.Read();  // (a look on the thread started before this is older now)
  m_places_time = m_time;
  m_usb_covers = done.usb_covers;
  m_console_games = done.found.counts;
  m_games_read = true;
  if (!m_games_known || done.fingerprint != m_games_fingerprint)
  {
    const size_t before = m_games.size();
    SetGames(std::move(done.found.games));
    m_games_fingerprint = done.fingerprint;
    m_games_known = true;
    LoadPlayed();
    GamesChanged(selected, online);
    if (m_games.size() != before)
      Log("games: " + std::to_string(before) + " -> " + std::to_string(m_games.size()));
    // Covers for the games of a new folder, in the background.
    if (m_dl_auto && !m_dl_thread.joinable() && MissingCovers())
      StartCoverDownload();
  }
  UpdateShownConsoles();  // (Only Those With Games: a console's first game, or its last one gone)
  if (done.places_changed)
    ShowToast("Game folders changed: games looked for again", 0);
  PortableLook(m_places.drives);  // (00.87: a drive with portable data - the start's, or one plugged in)
}

// Box art: the missing covers are downloaded in the background when the app opens (as Dolphin for PS4 gets GameTDB's),
// paused while a game runs - (00.85) once the start's read has the games and their covers' list: before, every game
// would look without one.
void Menu::Impl::StartCoversOnce()
{
  if (m_dl_auto || !m_running || !m_games_read || !m_covers_listed)
    return;
  m_dl_auto = true;
  if (m_safe_start)
  {
    Log("covers: no download of the missing ones in a safe start");  // (01.11)
    return;
  }
  if (MissingCovers())
  {
    Log("covers: downloading the missing ones in the background");
    StartCoverDownload();
  }
}

// A game starts (Run's end, or main.cpp's default game): a read of the games under way or waiting stops - what it held
// given back (the games it found, the folders it read: several MB of a big library while the game needs its memory),
// the USB drive the game's alone - and starts again when the menu comes back (Run: the files' contents it learnt kept,
// so the files it looked inside aren't opened again). A covers' list or a look at the places is short: it goes on.
void Menu::Impl::HoldRead()
{
  std::lock_guard lock(m_read_mutex);
  if ((m_read_busy && m_read_busy_games) || (m_read_ask && m_read_ask->games))
  {
    m_read_resume = true;
    m_read_ask.reset();
    m_read_progress.stop = true;
  }
  m_read_progress.pause = true;
}

int Menu::Impl::ReadingGames() const
{
  std::lock_guard lock(m_read_mutex);
  const bool reading = (m_read_busy && m_read_busy_games) || (m_read_ask && m_read_ask->games) ||
                       (m_read_done && m_read_done->scanned);
  return reading ? m_read_progress.games.load() : -1;
}

// "Reading your games..." in the lists the read leaves empty for now (BuildItems), its count as it goes; the lists
// made again when a read starts or ends.
void Menu::Impl::UpdateReadingItems()
{
  const int reading = ReadingGames();
  if (reading == m_reading_shown)
    return;
  const bool turned = (reading < 0) != (m_reading_shown < 0);
  m_reading_shown = reading;
  if (turned)
  {
    RebuildGameLists(true);
    return;
  }
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    auto& items = m_items[c];
    if (IsGameList(c) && items.size() == 1 && items[0].game < 0 && items[0].label == "Reading your games...")
      items[0].detail = reading > 0 ? std::to_string(reading) + " found so far" : std::string();
  }
}

// The games before the menu's first frame (main.cpp: DefaultGame, FindGame): the cache's, else the first read's - a
// frame drawn now and then meanwhile (the start's watchdog closes the app without a picture in 20 s), a minute at
// most (a drive that doesn't answer: the menu opens, the game asked for isn't started).
bool Menu::Impl::WaitForGames()
{
  if (m_games_known)
    return true;
  const auto start = Clock::now();
  ImGuiIO& io = ImGui::GetIO();
  while (Clock::now() - start < std::chrono::seconds(60))
  {
    TakeRead();
    if (m_games_known)
      return true;
    io.DeltaTime = 1.0f / 60.0f;
    m_display.BeginFrame();
    ImGui::NewFrame();
    DrawReading();
    ImGui::Render();
    m_display.EndFrame();
    usleep(10 * 1000);
  }
  Log("games: still being read after a minute: the menu opens meanwhile");
  return false;
}

// WaitForGames' frame: the letters, "Reading your games..." and how many so far.
void Menu::Impl::DrawReading()
{
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
  if (m_logo != ImTextureID{})
  {
    const float w = 520.0f, h = w / std::max(m_logo_aspect, 0.1f);
    dl->AddImage(m_logo, ImVec2(W * 0.5f - w * 0.5f, 420.0f - h), ImVec2(W * 0.5f + w * 0.5f, 420.0f));
  }
  if (!m_bold || !m_font)
    return;
  const std::string line = "Reading your games...";
  Text(dl, m_bold, 40, ImVec2(W * 0.5f - TextWidth(m_bold, 40, line) * 0.5f, 520), IM_COL32(255, 255, 255, 255), line);
  const int found = ReadingGames();
  if (found > 0)
  {
    const std::string count = std::to_string(found) + " found so far";
    Text(dl, m_font, 30, ImVec2(W * 0.5f - TextWidth(m_font, 30, count) * 0.5f, 590), IM_COL32(255, 255, 255, 190),
         count);
  }
}

void Menu::Impl::SetGames(std::vector<Library::Game>&& roms)
{
  std::vector<Game>().swap(m_games);  // (the old ones' room given back first: 30 000 games are several MB)
  m_games.reserve(roms.size());
  for (size_t i = 0; i < roms.size(); i++)
  {
    Game game;
    game.rom = std::move(roms[i]);
    game.order = static_cast<int>(i);
    m_games.push_back(std::move(game));
  }
  std::vector<Library::Game>().swap(roms);
  ApplyNames();
  StartRatings();  // (the stars: the games matched before at once, the others on the matcher's thread)
  StartAchievementMarks();  // (RetroAchievements' marks, 00.77: hashed on the module's thread)
  // ((01.05) the files' ids - the marks under the stars, online play's: read once on NetplayLibrary's thread, its copy
  // kept; nothing read while a game runs)
  StartOnlineLibrary();
}

// The user's own names (Rename) on the games - a name taken away gives the game its own title back -, then every
// console's games together by title, as each console's own list (one title's games in the read's order: the order
// the stable sort of 00.84 gave them, the titles in lower case made once each).
void Menu::Impl::ApplyNames()
{
  for (Game& game : m_games)
  {
    const auto it = m_names.find(game.rom.key);
    if (it != m_names.end())
    {
      if (!game.rom.custom_title)
        m_own_titles[game.rom.key] = game.rom.title;
      game.rom.title = it->second;
      game.rom.custom_title = true;
    }
    else if (game.rom.custom_title)
    {
      const auto own = m_own_titles.find(game.rom.key);
      if (own != m_own_titles.end())
        game.rom.title = own->second;
      game.rom.custom_title = false;
    }
  }
  std::vector<std::string> lower(m_games.size());
  std::vector<size_t> order(m_games.size());
  for (size_t i = 0; i < m_games.size(); i++)
  {
    lower[i] = Lower(m_games[i].rom.title);
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (lower[a] != lower[b])
      return lower[a] < lower[b];
    if (m_games[a].rom.id != m_games[b].rom.id)
      return m_games[a].rom.id < m_games[b].rom.id;
    return m_games[a].order < m_games[b].order;
  });
  std::vector<Game> sorted;
  sorted.reserve(m_games.size());
  for (const size_t i : order)
    sorted.push_back(std::move(m_games[i]));
  m_games = std::move(sorted);
}

// Each game list's selected game, by its file (the games about to change); a Choose Together room's games
// (m_online_library: their places in m_games) by their files too, for GamesChanged.
std::array<std::string, CATEGORY_COUNT> Menu::Impl::SelectedPaths()
{
  std::array<std::string, CATEGORY_COUNT> paths;
  for (int c = 0; c < CATEGORY_COUNT; c++)
  {
    const auto& items = m_items[c];
    if (IsGameList(c) && m_selected[c] >= 0 && m_selected[c] < static_cast<int>(items.size()) &&
        items[m_selected[c]].game >= 0 && items[m_selected[c]].game < static_cast<int>(m_games.size()))
      paths[c] = m_games[static_cast<size_t>(items[m_selected[c]].game)].rom.path;
  }
  m_online_library_files.clear();
  for (const auto& [game, index] : m_online_library)
  {
    if (index >= 0 && index < static_cast<int>(m_games.size()))
      m_online_library_files.emplace_back(game, m_games[static_cast<size_t>(index)].rom.path);
  }
  return paths;
}

// m_games changed (a read's games, a rename's order, games deleted): the covers asked for by index go (a new
// generation), the lists are made again, each keeping its selected game (by its file; else its first), the online
// room's game found again by its file.
void Menu::Impl::GamesChanged(const std::array<std::string, CATEGORY_COUNT>& selected, const std::string& online)
{
  ForgetCovers();
  m_no_cover_count = -1;
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
      if (refreshed[i].game >= 0 && !selected[c].empty() && m_games[refreshed[i].game].rom.path == selected[c])
      {
        m_selected[c] = static_cast<int>(i);
        m_item_pos[c] = static_cast<float>(i);
      }
    }
  }
  if (m_online_game >= 0)
  {
    m_online_game = -1;
    for (size_t i = 0; i < m_games.size() && !online.empty(); i++)
    {
      if (m_games[i].rom.path == online)
      {
        m_online_game = static_cast<int>(i);
        break;
      }
    }
  }
  if (!m_online_library.empty())
  {
    // (A Choose Together room's games at their new places; one gone leaves the room's list.)
    std::unordered_map<std::string, int> at;
    for (size_t i = 0; i < m_games.size(); i++)
      at.emplace(m_games[i].rom.path, static_cast<int>(i));
    m_online_library.clear();
    for (const auto& [game, path] : m_online_library_files)
    {
      const auto found = at.find(path);
      if (found != at.end())
        m_online_library.emplace(game, found->second);
    }
  }
  m_online_library_files.clear();
}

// The cover textures and the pictures asked for are of the games' old indexes: gone, a new generation (UpdateCovers
// drops the older one's).
void Menu::Impl::ForgetCovers()
{
  for (auto& [index, cover] : m_covers)
  {
    if (cover.texture != ImTextureID{})
      m_display.DestroyTexture(cover.texture);
  }
  m_covers.clear();
  std::lock_guard lock(m_cover_mutex);
  m_cover_jobs.clear();
  m_cover_done.clear();
  m_cover_generation++;
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
    std::unique_ptr<Sound> sound = LoadWav(UserDir() + "/themes/sounds/" + name + ".wav");
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
  {
    const std::vector<uint8_t> star = StarPixels();
    m_star = m_display.CreateTexture(star.data(), STAR_SIZE, STAR_SIZE);
  }
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(UserDir() + "/themes", error))
  {
    std::string name = entry.path().filename().string();
    const std::string lower = Lower(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0)
      m_wallpapers.push_back(name);
  }
  std::sort(m_wallpapers.begin(), m_wallpapers.end());
  // (The covers' pictures: listed by the read the constructor asked for - 00.85: not here, before the first frame.)
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
      // (00.81: a PNG or a JPEG, by what it is; a big one made smaller as it is read.)
      ImageRGBA image;
      std::string why, detail;
      if (LoadPicture(job.path, &image, 600, 430, &why, &detail))
        why.clear();
      std::lock_guard lock(m_cover_mutex);
      if (job.generation == m_cover_generation)  // (else the games were read again meanwhile)
        m_cover_done.push_back({job.game, std::move(image), job.generation, job.path, why, detail});
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

// The covers' pictures listed again (Covers Saved On changed: the place saved in wins now) - on the read's thread
// (00.85: ListCovers; a USB drive's covers folder of 30 000 pictures is a long look), the menu taking them as they
// come (CoversRead).
void Menu::Impl::IndexCovers()
{
  AskRead(false, true);
}

// A cover saved (downloaded, Find Cover's) into the index; kept aside too while a list of them is being made, so the
// list taken after it still has it (CoversRead).
void Menu::Impl::CoverSaved(const std::string& name, const std::string& path)
{
  m_cover_files[name] = path;
  m_no_cover_count = -1;
  std::lock_guard lock(m_read_mutex);
  if (m_read_busy || m_read_ask || m_read_covers)
    m_covers_saved.emplace_back(name, path);
}

std::string Menu::Impl::CoversSaveDir(bool check)
{
  const std::string internal = UserDir() + "/covers";
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
  // (00.90) With the data on a USB drive (Portable), "internal" is the drive's EmuSuNi-Portable/covers: said so.
  if (Settings::Get("covers_on") != "usb")
    return Portable::Active() ? "With My Data (USB Drive)" : "Internal Storage";
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
  // (00.76: none asked for while the console tabs move - L1 / R1 held -: the tab they stop on loads its own.)
  if (!cover.requested && !cover.missing && TabsSettled())
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
      // (00.81) A picture that can't be read - not a PNG or a JPEG (a WebP named .png), damaged, too big - is said in
      // the log and under Games Without Cover, and isn't the game's cover anymore: the game has none (its other
      // names' pictures, if any, are looked for), and Download Covers leaves the file as it is (the player's own).
      if (!result.why.empty() && result.game < static_cast<int>(m_games.size()))
      {
        Log("covers: " + result.path + " isn't used: " + result.why +
            (result.detail.empty() ? std::string() : " (" + result.detail + ")"));
        for (auto file = m_cover_files.begin(); file != m_cover_files.end();)
          file = file->second == result.path ? m_cover_files.erase(file) : std::next(file);
        const Library::Game& rom = m_games[result.game].rom;
        m_cover_why[rom.key] = "Your picture isn't used: " + result.path + "\n" + result.why;
        if (HasCover(rom))
          m_covers.erase(it);  // another of its names' pictures: looked for the next time it is drawn
        m_covers_changed = true;
        RefreshNoCoverItem();
        if (m_panel.open && m_panel.setting == SET_NO_COVER)
          BuildNoCoverRows();
      }
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

// The games matched before get their ratings now; the others (the first read, a new folder's, a game renamed) go to
// the matcher's thread - when it isn't at work: then they wait for its end (UpdateRatings starts them). again (a new
// copy of LaunchBox's ratings): every game, as soon as the matcher is free.
void Menu::Impl::StartRatings(bool again)
{
  if (again)
  {
    m_ratings.clear();
    m_rating_rematch = m_rating_matcher.Busy();  // (its run's answers are the old copy's: dropped)
    if (m_rating_rematch)
      return;
    m_rating_asked.clear();
  }
  // (00.85: the games for the matcher only when it is free - each a copy of its names alone: the matcher reads no
  // file's path - and handed over, not copied again: 30 000 games were two copies of every field.)
  const bool busy = m_rating_matcher.Busy();
  std::vector<Library::Game> wanted;
  for (Game& game : m_games)
  {
    const auto it = m_ratings.find(game.rom.key);
    if (it != m_ratings.end())
      game.rating = it->second;
    else if (!again)
      game.rating = Ratings::Rating{};  // (again: the stars stay until the new copy's come)
    if (!busy && it == m_ratings.end() && !m_rating_asked.count(game.rom.key))
      wanted.push_back(NamesOf(game.rom));
  }
  if (wanted.empty() || busy)
    return;
  // (01.11) Not before the menu's first frames (its thread took the disk and the CPU from them: Gabriel's 14 146 games,
  // 8.5 s), and not at all in a safe start (SafeStart): UpdateRatings starts it then.
  if (m_safe_start || g_display_frames.load(std::memory_order_relaxed) < 3)
  {
    m_ratings_waiting = true;
    return;
  }
  std::vector<std::string> keys;
  keys.reserve(wanted.size());
  for (const Library::Game& rom : wanted)
    keys.push_back(rom.key);
  if (m_rating_matcher.Start(std::move(wanted), m_assets_dir + "/xmb/ratings", Ratings::CopyPath(m_data_dir)))
  {
    for (std::string& key : keys)
      m_rating_asked.insert(std::move(key));
  }
}

void Menu::Impl::UpdateRatings()
{
  if (m_ratings_waiting && !m_safe_start && g_display_frames.load(std::memory_order_relaxed) >= 3)
  {
    m_ratings_waiting = false;
    StartRatings();  // ((01.11) the games that waited for the menu's first frames)
  }
  // A download ended: a new copy means every game's rating again (the panel of Star Ratings says how it went).
  bool downloaded = false;
  if (m_rating_download.Finished(&downloaded))
  {
    m_rating_copy = Ratings::ReadCopy(m_data_dir);
    if (downloaded)
      StartRatings(true);
  }
  if (m_panel.open && m_panel.setting == SET_RATINGS)
    m_panel.lines = RatingsPanelLines();
  std::unordered_map<std::string, Ratings::Rating> found;
  if (!m_rating_matcher.Take(&found))
    return;
  if (m_rating_rematch)
  {
    m_rating_rematch = false;
    m_rating_asked.clear();
    StartRatings(true);
    return;
  }
  // (A game renamed while it was matched isn't asked anymore: its old name's rating is dropped, it is matched again.)
  for (const auto& [key, rating] : found)
  {
    if (m_rating_asked.count(key))
      m_ratings[key] = rating;
  }
  m_rating_asked.clear();
  StartRatings();
  // ((01.18) The genres and players came (a new copy of the star ratings' data, a game matched now): with a genre, the
  // players or Hide Adult Games on, the lists again - until then a filter chosen before they came kept its empty lists
  // (01.18's first test: the players chosen with the copy before 01.18, the new one 29 minutes later). The panel's
  // counts too.)
  if (ViewFilterOn() || m_prefs.hide_adult)
    ViewChanged(m_category);
  if (m_panel.open && m_panel.setting == SET_LIST_VIEW)
    BuildViewRows();
}

bool Menu::Impl::StartRatingsDownload()
{
  m_rating_download_tried = true;
  return m_rating_download.Start(m_data_dir);
}

std::string Menu::Impl::RatingsDownloadText() const
{
  using State = Ratings::Downloader::State;
  if (m_rating_download.Now() == State::Reading)
    return "Reading the ratings";
  if (m_rating_download.Now() != State::Downloading)
    return "";
  const int64_t total = m_rating_download.Total();
  return "Downloading ratings " + std::to_string(m_rating_download.Bytes() >> 20) +
         (total > 0 ? " / " + std::to_string(total >> 20) : std::string()) + " MB";
}

// ---- Game Bezels (00.80, src/BezelProject.h) ----

// The consoles with games that The Bezel Project has bezels for, in the tabs' order.
std::vector<std::string> Menu::Impl::BezelConsoles() const
{
  std::vector<std::string> keys;
  for (SystemId id : ConsoleOrder())
  {
    const System& system = GetSystem(id);
    if (HasSystem(id) && m_console_games[static_cast<size_t>(id)] > 0 && BezelProject::Has(system.key))
      keys.push_back(system.key);
  }
  return keys;
}

// The download of those consoles' bezels for the games read now (resumed: the start of one a cut stopped).
bool Menu::Impl::StartBezelDownload(const std::vector<std::string>& consoles, bool resumed)
{
  std::vector<Library::Game> games;
  for (const Game& game : m_games)
  {
    if (std::find(consoles.begin(), consoles.end(), GetSystem(game.rom.system).key) != consoles.end())
      games.push_back(game.rom);
  }
  m_bezels_counted = false;
  return !games.empty() && m_bezel_download.Start(UserDir(), games, consoles, resumed);
}

// The Settings row's value: the download as it goes, else the bezels there (counted the first time, and again after a
// download or a deletion). (01.11) Counted from their records (BezelProject::Count), never before the menu's first
// frame: BuildItems asks without the count (count false), and Run's frames count them once the menu shows - in a safe
// start (SafeStart), only when their page opens.
std::string Menu::Impl::BezelsDetail(bool count)
{
  if (m_bezel_download.Busy())
  {
    const BezelProject::Downloader::Progress progress = m_bezel_download.Now();
    int64_t done = 0, total = 0;
    for (const auto& console : progress.consoles)
    {
      done += console.done;
      total += console.bytes;
    }
    if (progress.paused)
      return "Waiting while a game runs";
    return "Downloading bezels: " + std::to_string(done >> 20) + " / " + std::to_string(total >> 20) + " MB";
  }
  if (!m_bezels_counted && !count)
    return "The Bezel Project's, for your games";
  if (!m_bezels_counted)
  {
    const auto start = Clock::now();
    m_bezels_counted = true;
    m_bezels_there.clear();
    int files = 0;
    for (const System& system : Systems())
    {
      if (BezelProject::Has(system.key) && FileExists(BezelProject::RecordPath(UserDir(), system.key)))
      {
        m_bezels_there[system.key] = BezelProject::Count(UserDir(), system.key);
        files += m_bezels_there[system.key].files;
      }
    }
    LogF("bezels: %d downloaded, as their records say (%d consoles, %lld ms)", files,
         static_cast<int>(m_bezels_there.size()),
         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()));
  }
  int files = 0;
  for (const auto& [key, there] : m_bezels_there)
    files += there.files;
  return files > 0 ? std::to_string(files) + " bezels downloaded" : std::string("The Bezel Project's, for your games");
}

void Menu::Impl::OpenBezelsPanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_BEZELS;
  m_panel.title = "Game Bezels";
  BuildBezelRows();
}

// The page: what the bezels are, how the download goes, Download for All Consoles (Stop the Download while it runs),
// each console with games (Cross: its bezels only) and what it has, Delete Downloaded Bezels.
void Menu::Impl::BuildBezelRows()
{
  using Step = BezelProject::Downloader::Step;
  BezelsDetail();  // (what is there, counted once)
  const BezelProject::Downloader::Progress progress = m_bezel_download.Now();
  m_bezel_serial = progress.serial;
  const bool running = m_bezel_download.Busy();
  std::vector<SettingRow> rows;
  // A note over header lines of the panel's width (the text translated here).
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  note("Bezels made for each game by The Bezel Project (github.com/thebezelproject), downloaded from GitHub for the "
       "games you have, found by their names as their covers are. A game shows its own with Video -> Bezel: Per Game "
       "(The Bezel Project).");
  int64_t done = 0, total = 0;
  bool listing = false;
  for (const auto& console : progress.consoles)
  {
    done += console.done;
    total += console.bytes;
    listing = listing || console.step == Step::Listing;
  }
  if (running && progress.paused)
    note("Waiting while a game runs: it goes on afterwards.");
  else if (running && listing)
    note("Asking GitHub for The Bezel Project's lists...");
  else if (running)
    note("Downloading " + std::to_string(done >> 20) + " of " + std::to_string(total >> 20) + " MB: " +
         (progress.game.empty() ? std::string("...") : progress.game));
  else if (!progress.error.empty())
    note(progress.error);
  SettingRow all{running ? "Stop the Download" : "Download for All Consoles", "", "bz:all"};
  all.help = running ? "Stops it: what came stays, and Download for All Consoles goes on from there." :
                       "The bezels of every console's games here, those not downloaded yet: in the background, waiting "
                       "while a game runs; a download cut goes on at the next start.";
  rows.push_back(all);
  const std::vector<std::string> consoles = BezelConsoles();
  rows.push_back(
      {consoles.empty() ? "No games here that The Bezel Project has bezels for" : "Your Consoles", "", "", true});
  for (const std::string& key : consoles)
  {
    const System* system = SystemByKey(key);
    const auto there = m_bezels_there.find(key);
    const int files = there == m_bezels_there.end() ? 0 : there->second.files;
    std::string value = files == 1 ? std::string("1 bezel") :
                        files > 1  ? std::to_string(files) + " bezels" :
                                     std::string("Not downloaded");
    std::string error;
    for (const auto& console : progress.consoles)
    {
      if (console.key != key)
        continue;
      if (console.step == Step::Waiting && running)
        value = "Waiting";
      else if (console.step == Step::Listing)
        value = "Looking...";
      else if (console.step == Step::Downloading)
        value = std::to_string(console.downloaded + console.failed) + " / " +
                std::to_string(console.matched - console.already);
      else if (console.step == Step::Done && console.matched == 0)
        value = "None for these games";
      else if (console.step == Step::Failed)
      {
        value = "Failed";
        error = console.error;
      }
    }
    SettingRow row{system ? system->name : key, value, "bz:console:" + key};
    row.help = "This console's bezels only, those not downloaded yet.";
    rows.push_back(row);
    if (!error.empty())
      note(error);
  }
  int files = 0;
  int64_t bytes = 0;
  for (const auto& [key, there] : m_bezels_there)
  {
    files += there.files;
    bytes += there.bytes;
  }
  SettingRow erase{"Delete Downloaded Bezels", files > 0 ? std::to_string((bytes + (1 << 19)) >> 20) + " MB" : "",
                   "bz:delete"};
  erase.help = "Deletes the bezels downloaded from The Bezel Project. Your own bezels in the same folders stay.";
  rows.push_back(erase);
  const int selected = m_panel.selected;
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(selected, 0, std::max(0, static_cast<int>(m_panel.rows.size()) - 1));
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

// Every frame: the download's progress on the page and in the Settings row, its end (a toast).
void Menu::Impl::UpdateBezelDownload()
{
  const bool ended = m_bezel_download.Finished();
  if (!ended && m_bezel_download.Serial() == m_bezel_serial)
    return;
  if (ended)
  {
    m_bezels_counted = false;
    const BezelProject::Downloader::Progress progress = m_bezel_download.Now();
    int downloaded = 0;
    int64_t bytes = 0;
    for (const auto& console : progress.consoles)
    {
      downloaded += console.downloaded;
      bytes += console.done;
    }
    if (!progress.stopped)
    {
      ShowToast(progress.error.empty() ? "Game bezels: " + std::to_string(downloaded) + " new (" +
                                             std::to_string((bytes + (1 << 19)) >> 20) + " MB)" :
                                         progress.error,
                0);
    }
  }
  m_bezel_serial = m_bezel_download.Serial();
  for (Item& item : m_items[static_cast<int>(Category::Settings)])
  {
    if (item.setting == SET_BEZELS)
      item.detail = BezelsDetail();
  }
  if (m_panel.open && m_panel.setting == SET_BEZELS)
    BuildBezelRows();
}

std::vector<std::string> Menu::Impl::RatingLines(const Game& game) const
{
  const Ratings::Rating& rating = game.rating;
  if (rating.half_stars < 0)
  {
    if (!m_rating_copy.there && m_rating_download.Busy())
      return {"Rating: being downloaded now"};
    if (!m_rating_copy.there)
      return {"Rating: not downloaded yet", "(Settings: Star Ratings)"};
    if (m_ratings.count(game.rom.key) == 0)
      return {"Rating: still being looked up"};
    return {std::string("Rating: none in the ") + Ratings::SOURCE};
  }
  char stars[16];
  snprintf(stars, sizeof(stars), "%d.%d", rating.half_stars / 2, rating.half_stars % 2 == 0 ? 0 : 5);
  std::string line = std::string("Rating: ") + stars + " of 5";
  if (rating.votes == 1)
    line += " from 1 vote";
  else if (rating.votes > 1)
    line += " from " + std::to_string(rating.votes) + " votes";
  return {line, std::string("By the players of the ") + Ratings::SOURCE};
}

std::vector<std::string> Menu::Impl::RatingsPanelLines()
{
  using State = Ratings::Downloader::State;
  std::vector<std::string> lines = {"", "Stars under each cover: the game's", "rating by the players of the",
                                    std::string(Ratings::SOURCE) + ",", "in half stars (none for a game",
                                    "they didn't rate).", ""};
  const State state = m_rating_download.Now();
  if (state == State::Downloading || state == State::Reading)
  {
    lines.push_back(RatingsDownloadText() + "...");
  }
  else if (state == State::Failed)
  {
    // Why, in as many lines as the panel needs (Net's plain words, translated here).
    lines.push_back("The download failed:");
    std::istringstream why(WrapText(m_font, 28, PS4Lang::T(m_rating_download.Reason()), 540));
    for (std::string line; std::getline(why, line);)
      lines.push_back(line);
  }
  if (m_rating_copy.there && state != State::Downloading && state != State::Reading)
  {
    lines.push_back("LaunchBox's ratings of " + m_rating_copy.date);
    lines.push_back(std::to_string(m_rating_copy.games) + " games, downloaded " + DateText(m_rating_copy.made));
  }
  else if (!m_rating_copy.there && state != State::Downloading && state != State::Reading)
  {
    lines.push_back("Not downloaded yet: about 110 MB,");
    lines.push_back("from LaunchBox, kept 60 days.");
  }
  lines.push_back("");
  lines.push_back("Information (Triangle) says it");
  lines.push_back("in numbers, with its votes.");
  return lines;
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
      games.push_back(NamesOf(game.rom));  // (00.85: its names only, handed to the thread: no second copy)
    else
      m_dl_skipped++;
  }
  m_dl_progress = 0;
  m_dl_total = static_cast<int>(games.size());
  const std::string covers = CoversSaveDir(true);
  m_dl_thread = std::thread([this, games = std::move(games), closest, covers] {
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
    CoverSaved(Lower(key), path);
    m_cover_why.erase(key);
    // (00.85: the covers looked for already - those drawn so far -, not every game, for each cover that comes.)
    for (auto it = m_covers.begin(); it != m_covers.end();)
    {
      const bool again = it->first >= 0 && it->first < static_cast<int>(m_games.size()) &&
                         m_games[static_cast<size_t>(it->first)].rom.key == key &&
                         it->second.texture == ImTextureID{} && !it->second.requested;
      it = again ? m_covers.erase(it) : std::next(it);  // looked for again next time it is drawn
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
  for (const auto& [key, why] : result.why)
    m_cover_why[key] = why;
  if (shown)
  {
    m_panel.lines = {"", "Downloaded: " + std::to_string(result.downloaded),
                     "Already there: " + std::to_string(result.already),
                     "Not found: " + std::to_string(result.not_found)};
    if (result.failed > 0)
    {
      m_panel.lines.push_back("Failed: " + std::to_string(result.failed));
      // Why, in plain words (00.68; curl's own in the log), over the lines it takes.
      std::istringstream lines(WrapText(m_font, 28, PS4Lang::T(result.reason.empty() ? result.error : result.reason),
                                        550.0f));
      for (std::string line; std::getline(lines, line);)
        m_panel.lines.push_back(line);
    }
    m_panel.lines.push_back("");
    if (result.not_found > 0 || result.failed > 0)
      m_panel.lines.push_back("Why, game by game: Games Without Cover");
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

// Square in a game list, a new game folder, a game deleted, a drive plugged in: the folders are read again - on the
// read's thread (00.85): the lists keep their games meanwhile (their line says the read's count), each its selected
// game when the new ones come (GamesRead), the covers of a new folder's games downloaded then.
void Menu::Impl::RefreshGames()
{
  AskRead(true, true);
}

void Menu::Impl::CheckPlaces(bool now)
{
  // A look at the drives' top folders and the Roms folders only (Library::FindPlaces): the games are read
  // again only when they changed. (00.85: `now`, back from a game, on the read's thread too: a drive that doesn't
  // answer can't hold the menu's first frame; no look while a read is under way - its places are newer.)
  if (now)
  {
    m_places_time = m_time;
    AskRead(false, false);
    return;
  }
  if (m_places_look.valid() || m_time - m_places_time < 1.5f || ReadingGames() >= 0)
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
  RefreshGames();  // (GamesRead says how many there are then)
  ShowToast("Game folders changed: games looked for again", 0);
  PortableLook(places.drives);  // (00.87: a drive with portable data plugged in, the one in use taken out)
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
  m_prefs.Save(UserDir() + "/xmb.ini");
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
      m_prefs.Save(UserDir() + "/xmb.ini");
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
    m_wallpaper = LoadPngTexture(UserDir() + "/themes/" + m_prefs.wallpaper, nullptr, 1920, 1080);
  if (m_prefs.console_order != ConsoleOrderKeys())
    SetConsoleOrder(SavedConsoleOrder(m_prefs.console_order));
  m_prefs.Save(UserDir() + "/xmb.ini");
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
  // (00.87, src/Portable.h) The player's data on a USB drive, to take it to another PS4.
  settings.push_back({"Keep My Data on the USB Drive", PortableDetail(), -1, -1, SET_PORTABLE});
  settings.push_back({"Language", PS4Lang::Current().name, -1, -1, SET_LANGUAGE});
  // Each console's buttons (its normal layout); a game's own from the game: Triangle -> Controls, or the in-game
  // menu's Controls.
  settings.push_back({"Controls", "Each console's buttons on the DualShock 4", -1, -1, SET_CONTROLS});
  // RetroAchievements (00.73, src/Achievements.h): the account (Casual only, 00.78).
  settings.push_back({"RetroAchievements", AchievementsDetail(), -1, -1, SET_ACHIEVEMENTS});
  settings.push_back({"My Stats", MyStatsDetail(), -1, -1, SET_MY_STATS});  // (00.23: src/MyStats.h)
  settings.push_back({"Voice Chat", Voice::Detail(), -1, -1, SET_VOICE});  // (00.23: src/Voice.h)
  // Online play (00.23, src/Netplay.h): Join with Code, Public Rooms, Hide My Address (a room is made from a game).
  settings.push_back({"Online Play", "The Online tab: rooms, codes, your profile", -1, -1, SET_ONLINE});
  // The emulation settings (Video, Graphics, Performance, Emulation, System) are set from a game
  // (Triangle -> Game Settings: for that game, its console or all games), not here.
  settings.push_back({"Game Folders", "A folder for each console's games", -1, -1, SET_FOLDERS});
  // ((01.09 to 01.21: 3DS Updates and DLC here. (01.22, the owner) From the game: Triangle -> Updates, DLC and Mods.)
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
  // (00.80: The Bezel Project's) (01.11: not counted here - BezelsDetail)
  settings.push_back({"Game Bezels", BezelsDetail(false), -1, -1, SET_BEZELS});
  settings.push_back({"Star Ratings", m_prefs.ratings ? "On" : "Off", -1, -1, SET_RATINGS});
  settings.push_back({"Sound Effects", m_prefs.sounds ? "On" : "Off", -1, -1, SET_SOUNDS});
  settings.push_back({"Volume", std::to_string(m_prefs.volume), -1, -1, SET_VOLUME});
  settings.push_back({"Menu Style", Library() ? "Library" : "XMB", -1, -1, SET_STYLE});
  if (const Settings::Setting* size = Settings::Find("menu_size"))
    settings.push_back({"Menu Size", Settings::LabelOf(*size, Settings::Get("menu_size")), -1, -1, SET_MENU_SIZE});
  settings.push_back({"Clock", m_prefs.clock ? "On" : "Off", -1, -1, SET_CLOCK});
  settings.push_back({"Reset All Settings", "Every setting back to the defaults", -1, -1,
                      SET_RESET_ALL});
  settings.push_back({"Check for Updates", UpdatesDetail(Update::Get()), -1, -1, SET_UPDATES});
  // (00.23, src/UsageStats.h) How many use EmuSuNi: a random number of this app, never names or games.
  settings.push_back({"Anonymous Statistics", UsageStats::Enabled() ? "On" : "Off", -1, -1, SET_STATS});
  settings.push_back({"How to Help", "", -1, -1, SET_HELP});  // (00.90)
  settings.push_back({"Send Log", "", -1, -1, SET_SEND_LOG});  // (00.91: src/LogReport.h)
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
  // RetroAchievements' filter (00.80, Touchpad): only the games whose file has achievements - its second step, those
  // with achievements begun and not all unlocked -; a game not checked yet stays out meanwhile. Each list's counts.
  const bool ra_filter = RaFilterOn();
  for (auto& count : m_ra_count)
    count = {};
  const bool view_filter = ViewFilterOn();
  for (int& hidden : m_view_hidden)
    hidden = 0;
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
    if (ra_filter)
    {
      const uint32_t console = Achievements::ConsoleId(game.rom.system);
      const bool has = console != 0 && game.rom.system != SystemId::THREEDS;  // (a console RetroAchievements has)
      for (const int c : {static_cast<int>(Category::Games), ListOfSystem(game.rom.system),
                          m_favorites.count(game.rom.key) ? static_cast<int>(Category::Favorites) : -1})
      {
        if (c < 0)
          continue;
        m_ra_count[c].total++;
        m_ra_count[c].files += has ? 1 : 0;
        m_ra_count[c].unchecked += has && game.ra.state == Achievements::LibraryInfo::State::Unchecked ? 1 : 0;
      }
      if (!RaFilterPasses(game))
        continue;
    }
    // ((01.18) Order and Filters: the games for adults hidden (kept: no count); a genre, the players.)
    if (m_prefs.hide_adult && game.rating.adult)
      continue;
    if (view_filter && !ViewPasses(game))
    {
      for (const int c : {static_cast<int>(Category::Games), ListOfSystem(game.rom.system),
                          m_favorites.count(game.rom.key) ? static_cast<int>(Category::Favorites) : -1})
      {
        if (c >= 0)
          m_view_hidden[c]++;
      }
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
    // (00.80) A list the RetroAchievements filter emptied (its games still being checked, or none with achievements).
    m_ra_emptied[c] = ra_filter && m_ra_count[c].total > 0 && m_items[c].empty();
    if (m_ra_emptied[c])
      m_items[c].push_back({m_prefs.ra_filter == "progress" ? "No achievements in progress here" :
                                                              "No games with achievements here",
                            m_ra_count[c].unchecked > 0 ? "Still checking the files: they show here as they're found" :
                                                          "Touchpad shows every game again"});
    // (01.18) A list the genre or players emptied.
    m_view_emptied[c] = !m_ra_emptied[c] && view_filter && m_view_hidden[c] > 0 && m_items[c].empty();
    if (m_view_emptied[c])
      m_items[c].push_back({"No games of that genre or players here", "Options shows every game again"});
  }
  // (00.85) A list still empty while the games are being read: "Reading your games..." and how many so far
  // (UpdateReadingItems counts on).
  const int reading = ReadingGames();
  m_reading_shown = reading;
  const Item reading_item{"Reading your games...", reading > 0 ? std::to_string(reading) + " found so far" : ""};
  if (games.empty() && !m_search.empty())
    games.push_back({"No games match \"" + m_search + "\"", "Circle clears the search"});
  else if (games.empty() && reading >= 0)
    games.push_back(reading_item);
  else if (games.empty())
    games.push_back({"No games found", "Copy your games to their console's folder (Settings: Game Folders)"});
  for (const System& system : Systems())
  {
    auto& list = m_items[ListOfSystem(system.id)];
    if (list.empty() && !m_search.empty())
      list.push_back({"No games match \"" + m_search + "\"", "Circle clears the search"});
    else if (list.empty() && reading >= 0)
      list.push_back(reading_item);
    else if (list.empty())
      // (00.90: players looked for "Roms/gba" anywhere) Where exactly: the drive's root, or the PS4's storage.
      list.push_back({"No games found", "Copy your games to Roms/" + std::string(system.key) + " on a USB drive or in " +
                                            m_data_dir + "/Roms/" + system.key});
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
  BuildOnlineItems();
  for (int c = 0; c < CATEGORY_COUNT; c++)
    m_selected[c] = std::clamp(m_selected[c], 0, std::max(0, static_cast<int>(m_items[c].size()) - 1));
}

void Menu::Impl::SetLanguage(const std::string& code)
{
  Settings::Set("language", code);
  PS4Lang::Load(m_assets_dir, code);
  Achievements::SetLanguage(code);  // (00.76: the achievements' texts translated to it)
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

ImTextureID Menu::Impl::StatePicture(const std::string& id, int slot, float* aspect)
{
  if (m_state_pictures_game != id)
  {
    ForgetStatePictures();
    m_state_pictures_game = id;
  }
  const std::string key = id + "/" + std::to_string(slot);
  auto it = m_state_pictures.find(key);
  if (it != m_state_pictures.end())
  {
    if (aspect)
      *aspect = m_state_aspects[key];
    return it->second;
  }
  float ratio = 8.0f / 7.0f;
  const ImTextureID texture = LoadPngTexture(Saves::StatePicturePath(id, slot), &ratio, 256, 224);
  m_state_pictures[key] = texture;
  m_state_aspects[key] = ratio;
  if (aspect)
    *aspect = ratio;
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
  m_state_aspects.clear();
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
    row.icon = StatePicture(save.id, slot, &row.icon_aspect);
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
  case SET_MENU_SIZE:
    // (00.85) The setting's choices (Settings.cpp's menu_size), the one in use chosen.
    m_panel.title = "Menu Size";
    if (const Settings::Setting* size = Settings::Find("menu_size"))
    {
      for (const Settings::Choice& choice : size->choices)
      {
        if (Settings::Get("menu_size") == choice.value)
          m_panel.selected = static_cast<int>(m_panel.options.size());
        m_panel.options.push_back(choice.label);
      }
    }
    m_panel.lines = {"", "Smaller games in the menu:", "more of them on the screen at once,", "in the XMB's list and the",
                     "Library's row of covers.", "", "It changes at once."};
    break;
  case SET_CLOCK:
    m_panel.title = "Clock";
    m_panel.options = {"On", "Off"};
    m_panel.selected = m_prefs.clock ? 0 : 1;
    break;
  case SET_STATS:
    m_panel.title = "Anonymous Statistics";
    m_panel.options = {"On", "Off"};
    m_panel.selected = UsageStats::Enabled() ? 0 : 1;
    m_panel.lines = {"", "Counts how many use EmuSuNi and where:", "a random number of this app, its version,",
                     "the PS4's language, the console and the game", "played (a checksum, never a file's name).",
                     "No names, no address kept.", "", "Off: nothing is sent."};
    break;
  case SET_RATINGS:
    m_panel.title = "Star Ratings";
    m_panel.options = {"On", "Off", m_rating_copy.there ? "Download Again" : "Download Now"};
    m_panel.selected = m_prefs.ratings ? 0 : 1;
    m_panel.lines = RatingsPanelLines();  // (UpdateRatings keeps them current while the panel is open)
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
    // (00.90, a player thought his covers had to be moved) With the data on a USB drive (Portable), the first
    // place is the drive's EmuSuNi-Portable/covers, with the saves: named so. Both places are read either way.
    const bool portable = Portable::Active();
    m_panel.options = {portable ? "With My Data (USB Drive)" : "Internal Storage", "USB Drive"};
    m_panel.selected = Settings::Get("covers_on") == "usb" ? 1 : 0;
    m_panel.lines = {"",
                     (portable ? "With your data: " : "Internal: ") + UserDir() + "/covers",
                     m_usb_covers.empty() ? std::string("USB: no drive connected") : "USB: " + m_usb_covers,
                     "",
                     "New covers go there (downloaded or chosen);",
                     "the ones in both places are shown.",
                     "",
                     "Your own: a PNG or JPEG per game,",
                     "named like its file or its title,",
                     "in a folder per console (covers/snes,",
                     "covers/nes, covers/md, covers/psp...).",
                     "Square in the Library reads them again."};
    break;
  }
  case SET_CONSOLES_SHOWN:
    OpenConsolesShown();
    return;
  case SET_ACHIEVEMENTS:
    OpenAchievementsPanel();
    return;
  case SET_VOICE:
    OpenVoicePanel();
    return;
  case SET_ONLINE:
    OpenOnlinePanel();
    return;
  case SET_OL_CREATE:
    m_panel.title = "Create Room";
    m_panel.options = {"With a Game", "Choose Together"};
    m_online_actions = {"with_game", "together"};
    m_panel.lines = {"", "With a game: pick it in your games,", "then Triangle -> Play Online.", "",
                     "Choose Together: a room without a game:", "you pick one you both have."};
    Play("menu");
    return;
  case SET_OL_SETTINGS:
    OpenOnlineSettings();
    Play("menu");
    return;
  case SET_OL_PROFILE:
  {
    const UsageStats::Profile profile = UsageStats::GetProfile();
    m_panel.title = "Online Profile";
    m_panel.options = {"On", "Off"};
    m_online_actions = {"profile_on", "profile_off"};
    m_panel.selected = profile.on ? 0 : 1;
    m_panel.lines = {"", "On: EmuSuNi's server keeps your history -", "time played by game and console, sessions,",
                     "online matches - and gives you a code that", "opens it on EmuSuNi's website.", "",
                     "Off (the default): nothing of yours is kept.", "Delete My Profile deletes it."};
    Play("menu");
    return;
  }
  case SET_OL_PUBLIC:
    m_panel.title = "Public Profile";
    m_panel.options = {"On", "Off"};
    m_online_actions = {"public_on", "public_off"};
    m_panel.selected = UsageStats::GetProfile().is_public ? 0 : 1;
    m_panel.lines = {"", "On: your nickname, country and hours", "are in the website's rankings.", "",
                     "Off: only your code opens your profile."};
    Play("menu");
    return;
  case SET_OL_CODE:
    m_panel.title = "Profile Code";
    m_panel.options = {"OK"};
    m_online_actions = {"back"};
    m_panel.lines = {"", UsageStats::GetProfile().code, "", "Your profile on EmuSuNi's website:",
                     "emusuni.com", "Statistics -> My Profile, with this code.", "",
                     "Anyone with the code sees it: keep it."};
    Play("menu");
    return;
  case SET_OL_DELETE:
    m_panel.title = "Delete My Profile";
    m_panel.options = {"Cancel", "Delete My Profile"};
    m_online_actions = {"back", "delete_profile"};
    m_panel.lines = {"", "Your profile is deleted on EmuSuNi's", "server: its time played, sessions and",
                     "online matches. My Stats on this PS4 stay."};
    Play("menu");
    return;
  case SET_MY_STATS:
    OpenMyStats();
    return;
  case SET_BEZELS:
    OpenBezelsPanel();
    return;
  case SET_PORTABLE:
    OpenPortablePanel();  // (00.87)
    return;
  case SET_UPDATES:
    OpenUpdatePanel(false);
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
  case SET_HELP:
    OpenHelpPanel();
    return;
  case SET_SEND_LOG:
    OpenSendLogPanel();
    return;
  case SET_ABOUT:
    m_panel.title = "About";
    // (00.95, the owner: his name and the version only. LaunchBox's and The Bezel Project's credits are in their own
    // panels - Star Ratings, Game Bezels - and in THIRD_PARTY_NOTICES.md.)
    m_panel.lines = {"EmuSuNi", "", "Created by Arturo Fuentes", std::string("Version ") + App::VERSION};
    break;
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
  // (The 3DS: its own resolution only, 1x, from 00.64 to 00.70, which a second row said; the Quality levels again since
  // 00.71. Flycast's consoles had one fixed level, Standard, from 00.53 to 00.67: the Quality levels again since 00.68.)
  if (m_panel.system == static_cast<int>(SystemId::THREEDS))
    rows.push_back({"Nintendo 3DS: experimental", "", "", true});
  for (const char* category : Settings::Categories())
  {
    if (!m_panel.category.empty() && m_panel.category != category)
      continue;
    bool header = !m_panel.category.empty();  // one category: no header
    for (const Settings::Setting& setting : Settings::All())
    {
      if (std::string(setting.category) != category)
        continue;
      if (!game.empty() && std::string(setting.key) == "language")
        continue;
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

// Moves the selection by direction (0: onto the nearest selectable row), skipping headers. Past the first or the last
// row: round to the other end with `wrap` (a fresh press, 00.76), the list's scroll jumping there; else it stays.
void Menu::Impl::MoveSettingRow(int direction, bool wrap)
{
  const int count = static_cast<int>(m_panel.rows.size());
  const int from = m_panel.selected;
  const int row = MenuNav::Step(from, direction, count, wrap, [this](int i) { return !m_panel.rows[i].header; });
  if (row < 0 || row >= count)
    return;
  m_panel.selected = row;
  if (direction != 0 && row != from && (direction > 0) != (row > from))
    m_panel.snap = true;
}

void Menu::Impl::ChangeSettingRow(int direction)
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.rows.size()))
    return;
  const SettingRow row = m_panel.rows[m_panel.selected];
  const std::string game = m_panel.game_id;
  if (m_panel.setting == SET_LIST_VIEW)
  {
    ChangeViewRow(direction);  // (01.18) Order and Filters
    return;
  }
#ifdef EMUSUNI_3DS
  // (01.09) A game's Updates, DLC and Mods: Mods On / Off (either way), Remove the Update / the DLC ("Remove?" first),
  // the update's or DLC's row (where to install them); (01.22) one of its CIAs (Cross or Right: Open3dsCiaConfirm),
  // Start the Game (after the game closed for it).
  if (m_panel.setting == SET_3DS_EXTRAS)
  {
    if (row.key == "3ds:mods")
    {
      ThreeDsCia::SetModsOn(m_3ds_title, row.tag < 0);
      m_panel.rows[m_panel.selected].tag = row.tag < 0 ? 1 : -1;
    }
    else if (direction > 0 && row.key == "3ds:start")
    {
      StartGameFromPanel(m_3ds_game);
    }
    else if (direction > 0 && row.key.rfind("cia:", 0) == 0)
    {
      Open3dsCiaConfirm(static_cast<size_t>(std::atoi(row.key.c_str() + 4)));
    }
    else if (direction > 0 && (row.key == "3ds:remove_update" || row.key == "3ds:remove_dlc"))
    {
      Ask3dsRemove(row.key == "3ds:remove_dlc");
    }
    else if (direction > 0 && (row.key == "3ds:update" || row.key == "3ds:dlc" || row.key == "3ds:nocia"))
    {
      ShowToast(ThreeDsCia::Available() ? "Copy its CIA files to Roms/3ds/cia: they show here" :
                                          "This version's 3DS emulator can't install CIAs yet.",
                0);
    }
    return;
  }
#endif
  // (01.22) An Arcade game's Patched Romset: Start the Game, Remove It ("Remove?" first), one found (Install?), where
  // they go.
  if (m_panel.setting == SET_ARCADE_PATCHED)
  {
    if (direction <= 0)
      return;
    if (row.key == "patched:start")
      StartGameFromPanel(m_patched_game);
    else if (row.key == "patched:remove")
      AskPatchedRemove();
    else if (row.key.rfind("patched:file:", 0) == 0)
      OpenPatchedConfirm(static_cast<size_t>(std::atoi(row.key.c_str() + 13)));
    else if (row.key == "patched:none")
      ShowToast("Copy it to Roms/arcade/patched, named as the game's set: it shows here", 0);
    return;
  }
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
      AskText(
          "Look For", m_find_query,
          [this](const std::string& text) {
            if (!m_panel.open || m_panel.setting != SET_FIND_COVER)
              return;  // the cover was saved meanwhile: back in Games Without Cover
            if (!text.empty())
              m_find_query = text;
            StartFind();
          },
          100, TextKeyboard::Mode::Text, "The game's name in English often finds more.");
    }
    else if (row.key == "find:kind")
    {
      m_find_titles = !m_find_titles;
      StartFind();
    }
    else if (row.key.rfind("pick:", 0) == 0)
    {
      // The picture as the game's cover (covers/<console>/<file name>.png): over its cover of that name where it is
      // (the internal storage or a USB drive; a JPEG of the player's own stays, the new PNG beside it is newer, so it
      // wins), else where new covers go (Covers Saved On).
      const auto old = m_cover_files.find(Lower(m_find_key));
      std::string target = old != m_cover_files.end() ? old->second :
                                                        CoverFiles::NewFile(CoversSaveDir(true), m_find_key);
      if (Lower(target.substr(target.rfind('.'))) != ".png")
        target = target.substr(0, target.rfind('.')) + ".png";
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
  if (m_panel.setting == SET_VOICE)
  {
    // Voice Chat (00.23): a setting stepped (Voice saves it), Test Microphone started or stopped (the microphone of
    // the PS4's first user, whose controller the menu reads).
    int user = 0;
    sceUserServiceGetInitialUser(&user);
    if (Voice::PanelChange(row.key, direction, user))
    {
      BuildVoiceRows();
      BuildItems();  // (the Settings list's Voice Chat line)
    }
    return;
  }
  if (m_panel.setting == SET_ACHIEVEMENTS)
  {
    // RetroAchievements (00.73): log in (the user name, then the password, on the keyboard: only the token the
    // site gives back is kept), log out. (00.78: no Hardcore Mode - Casual only.) (00.79) What is RetroAchievements?
    if (row.key == "ra:about" && direction > 0)
    {
      OpenAboutPanel();
      Play("menu");
    }
    else if (row.key == "ra:login" && direction > 0)
    {
      AskText(
          "RetroAchievements: User Name", m_ra_user.empty() ? Achievements::GetAccount().user : m_ra_user,
          [this](const std::string& user) {
            if (user.empty())
              return;
            m_ra_user = user;
            m_ra_ask_password = 3;  // (UpdateAchievements, a few frames later)
          },
          64, TextKeyboard::Mode::Plain, "Your user name at retroachievements.org.");
    }
    else if (row.key == "ra:logout" && direction > 0)
    {
      Achievements::LogOut();
      m_ra_login = Achievements::Login::Out;
      BuildItems();
      BuildAchievementsRows();
      ShowToast("Logged out of RetroAchievements", 0);
      Play("confirm");
    }
    else if (row.key == "ra:history" && direction > 0)
    {
      OpenHistoryPanel();  // (00.75)
      Play("menu");
    }
    else if (row.key == "ra:translate")
    {
      // (00.76) The achievements' texts in the app's language (machine translations: Google's, else MyMemory's, 00.79)
      // or as RetroAchievements has them.
      Achievements::SetTranslateSetting(!Achievements::TranslateSetting());
      BuildAchievementsRows();
      Play("confirm");
    }
    else if (row.key == "ra:marks")
    {
      // (00.77) The "RA" marks on the games: hidden (nothing hashed then), or shown (the games handed over again).
      Achievements::SetMarksSetting(!m_ra_marks);
      StartAchievementMarks();
      if (m_prefs.ra_filter != "off")
        RebuildGameLists(true);  // (00.80: the filter works with the marks only)
      BuildAchievementsRows();
      Play("confirm");
    }
    return;
  }
  if (m_panel.setting == SET_RA_HISTORY)
  {
    // My Achievements (00.75): Update Now asks retroachievements.org for the account's games again; a game (00.76)
    // shows every achievement of it.
    if (row.key == "ra:sync" && direction > 0)
    {
      Achievements::UpdateHistory(true);
      BuildHistoryRows();
      Play("confirm");
    }
    else if (row.key.rfind("ra:game:", 0) == 0 && direction > 0)
    {
      OpenGameAchievements(static_cast<uint32_t>(std::strtoul(row.key.c_str() + 8, nullptr, 10)));
      Play("menu");
    }
    return;
  }
  if (m_panel.setting == SET_BEZELS)
  {
    // Game Bezels (00.80): the download for every console or one (Stop the Download while it runs), the deletion.
    if (direction <= 0)
      return;
    if (row.key == "bz:all" && m_bezel_download.Busy())
    {
      m_bezel_download.Stop(true);  // (the player's: not resumed at the next start either)
      m_bezels_counted = false;
      ShowToast("The bezels' download was stopped", 0);
      UpdateBezelDownload();
      BuildBezelRows();
      Play("back");
      return;
    }
    if (row.key == "bz:all" || row.key.rfind("bz:console:", 0) == 0)
    {
      if (m_bezel_download.Busy())
      {
        ShowToast("A download is under way", 0);
        Play("error");
        return;
      }
      const std::vector<std::string> consoles =
          row.key == "bz:all" ? BezelConsoles() : std::vector<std::string>{row.key.substr(11)};
      if (consoles.empty() || !StartBezelDownload(consoles, false))
      {
        ShowToast("No games here that The Bezel Project has bezels for", 0);
        Play("error");
        return;
      }
      ShowToast("Downloading the bezels of your games", 0);
      UpdateBezelDownload();
      BuildBezelRows();
      Play("confirm");
      return;
    }
    if (row.key == "bz:delete")
    {
      if (m_bezel_download.Busy())
      {
        ShowToast("A download is under way", 0);
        Play("error");
        return;
      }
      BezelsDetail();  // (what is there)
      int files = 0;
      int64_t bytes = 0;
      for (const auto& [key, there] : m_bezels_there)
      {
        files += there.files;
        bytes += there.bytes;
      }
      if (files == 0)
      {
        ShowToast("No bezels downloaded", 0);
        Play("error");
        return;
      }
      m_panel = {};
      m_panel.open = true;
      m_panel.setting = SET_BEZELS_DELETE;
      m_panel.title = "Delete Downloaded Bezels";
      m_panel.options = {"Cancel", "Delete"};
      m_panel.lines = {"", std::to_string(files) + " bezels (" + std::to_string((bytes + (1 << 19)) >> 20) + " MB)",
                       "downloaded from The Bezel Project are deleted.", "", "Your own bezels stay."};
      Play("menu");
    }
    return;
  }
  if (m_panel.setting == SET_RA_GAME || m_panel.setting == SET_RA_VERSIONS || m_panel.setting == SET_RA_ABOUT)
    return;  // (their rows say things; Circle goes back)
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
    if (row.key == "menu_buttons")
    {
      // The in-game menu's buttons (00.68), every game's: Left / Right, or Cross, the next choice.
      if (const Settings::Setting* setting = Settings::Find("menu_buttons"))
      {
        Settings::Set("menu_buttons", Settings::Step(*setting, Settings::Get("menu_buttons"), direction));
        m_panel.rows[m_panel.selected].value = Settings::LabelOf(*setting, Settings::Get("menu_buttons"));
      }
      return;
    }
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
      m_prefs.Save(UserDir() + "/xmb.ini");
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
        m_capture_step = 0;
        m_capture_chord = 0;
      }
      else
      {
        Controls::Set(binding, "None", edit, system);
        if (Controls::IsMacro(binding))
          Controls::SetMacroPresses(binding, "", edit, system);
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
  // The buttons that open the in-game menu (00.68), for every game; then each console's controls.
  if (const Settings::Setting* menu_buttons = Settings::Find("menu_buttons"))
  {
    m_panel.rows.push_back({"Every Game", "", "", true});
    m_panel.rows.push_back({menu_buttons->label, Settings::LabelOf(*menu_buttons, Settings::Get("menu_buttons")),
                            "menu_buttons"});  // (its help: the setting's, under the list)
  }
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
    if (Controls::IsMacro(bindings[i]))
    {
      // (00.89) A macro: its button and what it presses, in words ("R1 = Cross + Triangle").
      row.value = Controls::MacroText(bindings[i], edit, m_controls_system);
      row.glyph.clear();
    }
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
  if (m_capture_time > 5.0f && m_capture_chord == 0)
  {
    m_capture = false;
    m_capture_step = 0;
    Play("back");
    return;
  }
  const std::string& key = m_panel.rows[m_capture_row].key;
  const auto& binding = Controls::Bindings(m_controls_system)[std::atoi(key.c_str() + 4)];
  const std::string edit = Controls::HasGameProfile(m_controls_game) ? m_controls_game : "";
  if (m_capture_step == 1)
  {
    // (00.89) A macro's presses: every button held together, taken once all of them are let go.
    m_capture_chord |= Controls::PressedControls(data);
    if (m_capture_chord == 0 || !Controls::IsAtRest(data))
      return;
    Controls::Set(binding, m_capture_trigger, edit, m_controls_system);
    Controls::SetMacroPresses(binding, Controls::NamesOf(m_capture_chord), edit, m_controls_system);
    Log("controls: " + std::string(binding.label) + " = " + Controls::MacroText(binding, edit, m_controls_system));
    BuildControlRows();
    m_capture = false;
    m_capture_step = 0;
    m_input.Poll(0.0f);  // swallow the presses
    Play("confirm");
    return;
  }
  const std::string control = Controls::Capture(data, Controls::TakesSticks(binding));  // (00.90: a stick too)
  if (control.empty())
    return;
  if (Controls::IsMacro(binding))
  {
    // Its button: now its presses (kept with it once they come; 5 s without them: nothing changes).
    m_capture_trigger = control;
    m_capture_step = 1;
    m_capture_chord = 0;
    m_capture_released = false;
    m_capture_time = 0.0f;
    Play("confirm");
    return;
  }
  Controls::Set(binding, control, edit, m_controls_system);
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
  m_cheats_folder = UserDir() + "/cheats/" + GetSystem(game.rom.system).prefix;
  m_cheats_file = game.rom.id + ".cht";
  m_cheat_page = CheatPage::List;
  m_cheats.Load(UserDir(), game.rom.key);
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
      rows.push_back({"Downloading the cheat collection: " + std::to_string(m_finder.DownloadedMb()) +
                          (m_finder.TotalMb() > 0 ? " of " + std::to_string(m_finder.TotalMb()) : std::string()) + " MB",
                      "", "", true});
    else if (state == State::Searching || state == State::Idle)
      rows.push_back({"Looking for this game's cheats...", "", "", true});
    else if (state == State::Failed)
    {
      rows.push_back({m_finder.Error(), "", "", true});
      // Why (00.68: the plain reason), over as many lines as it takes; Find Cheats Online again tries again.
      const std::string reason = m_finder.Reason().empty() ? std::string() : PS4Lang::T("Why: " + m_finder.Reason());
      std::istringstream lines(WrapText(m_bold, 26, reason, 580.0f));
      for (std::string line; std::getline(lines, line);)
        rows.push_back({line, "", "", true});
      rows.push_back({"Circle, then Find Cheats Online: another try", "", "", true});
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
  const int index = items.empty() ? -1 : items[m_selected[m_category]].game;
  if (index < 0 || index >= static_cast<int>(m_games.size()))
  {
    m_panel = {};  // (no game there any more: a list made again meanwhile - 00.80's filter)
    return;
  }
  const Game& game = m_games[index];
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Game";
  // (00.88) Favorites no longer here: L2 on a game adds it or takes it out (the menu fits the screen again).
  m_panel.options = {"Start",
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
  // RetroAchievements (00.79): the game's achievements (a file it has achievements for), or the files it knows of the
  // game (one it doesn't know, or another version). (00.80) For every game of a console it has achievements for, the
  // marks on or off: a file not checked yet is checked when it opens; the 3DS (none yet) has no entry.
  if (Achievements::ConsoleId(game.rom.system) != 0 && game.rom.system != SystemId::THREEDS)
  {
    const auto info = std::find(m_panel.options.begin(), m_panel.options.end(), "Information");
    m_panel.options.insert(info, "Achievements");
  }
  // Online play (00.23, src/Netplay.h): the consoles that play online, after Start.
  if (Netplay::RulesFor(GetSystem(game.rom.system).key).online)
    m_panel.options.insert(m_panel.options.begin() + 1, "Play Online");
#ifdef EMUSUNI_GBLINK
  // (00.95) The Game Boy link cable online (src/GbLink.h): its room for two, the other player's own game in Game Boy 2.
  if (game.rom.system == SystemId::GB || game.rom.system == SystemId::GBC)
    m_panel.options.insert(m_panel.options.begin() + 1, "Link Cable Online");
#endif
#ifdef EMUSUNI_GBALINK
  // (01.04) The Game Boy Advance's (mGBA's link core): the same room, two GBA games.
  if (game.rom.system == SystemId::GBA)
    m_panel.options.insert(m_panel.options.begin() + 1, "Link Cable Online");
#endif
#ifdef EMUSUNI_3DS
  // (01.09) A 3DS game's update and DLC (installed from its CIAs: (01.22) here, until then from Settings), and its mods
  // on or off.
  if (game.rom.system == SystemId::THREEDS)
  {
    const auto info = std::find(m_panel.options.begin(), m_panel.options.end(), "Information");
    m_panel.options.insert(info, "Updates, DLC and Mods");
  }
#endif
  // (01.22) An Arcade game's patched romset (FinalBurn Neo's system/fbneo/patched, 01.05): in use, removed, installed.
  if (game.rom.system == SystemId::ARCADE)
  {
    const auto info = std::find(m_panel.options.begin(), m_panel.options.end(), "Information");
    m_panel.options.insert(info, "Patched Romset");
  }
  m_extras_from_game = false;  // (its panels from here: Start is in this list)
  Play("menu");
}

#ifdef EMUSUNI_3DS
// ---- (01.09, the owner: "Sí, hazlo") The 3DS's updates and DLC from the player's CIAs, a game's mods ----

std::string Menu::Impl::ThreeDsSaveDir() const
{
  return Portable::UserDir() + "/saves/3ds";  // (Emulator.cpp: the core's save folder, its Azahar/sdmc below)
}

std::string Menu::Impl::ThreeDsGameName(uint64_t game_title_id)
{
  if (game_title_id == 0)
    return {};
  for (const Game& game : m_games)
  {
    if (game.rom.system != SystemId::THREEDS)
      continue;
    auto found = m_3ds_titles.find(game.rom.path);
    if (found == m_3ds_titles.end())
      found = m_3ds_titles.emplace(game.rom.path, ThreeDsCia::RomTitleId(game.rom.path)).first;
    if (found->second == game_title_id)
      return game.rom.title;
  }
  return {};
}

std::string Menu::Impl::CiaLine(const ThreeDsCia::File& file)
{
  const ThreeDsCia::Info& info = file.info;
  std::string name = info.read ? ThreeDsGameName(ThreeDsCia::GameOf(info.title_id)) : std::string();
  if (name.empty() && info.read)
    name = info.name;
  if (name.empty())
    name = std::filesystem::path(file.file_name).stem().string();
  return name;
}

// The CIAs of the 3DS's game folders' "cia" folders (made where there is none, so the player sees where they go), each
// with what it is ((01.22) for a game's Updates, DLC and Mods: its own among them; until then Settings -> 3DS Updates
// and DLC listed them all).
void Menu::Impl::List3dsCias()
{
  const System& system = GetSystem(SystemId::THREEDS);
  const std::vector<std::string> folders = ThreeDsCia::Folders(
      Library::ConsoleFolders(system, GamesFolder(SystemId::THREEDS), m_places), true);
  m_cias = ThreeDsCia::List(folders);
  std::string where;
  for (const std::string& folder : folders)
    where += (where.empty() ? "" : ", ") + folder;
  LogF("3ds cia: %zu CIA%s in %s%s", m_cias.size(), m_cias.size() == 1 ? "" : "s",
       where.empty() ? "no folder" : where.c_str(), ThreeDsCia::Available() ? "" : " (this core has no installer)");
}

// A CIA chosen: what it is, the room it takes and the free space; Install / Cancel (only Back without the room).
void Menu::Impl::Open3dsCiaConfirm(size_t index)
{
  if (index >= m_cias.size())
    return;
  const ThreeDsCia::File& file = m_cias[index];
  const ThreeDsCia::Info& info = file.info;
  const int refused = !info.read ? info.code :
                      info.encrypted ? ThreeDsCia::ENCRYPTED :
                      info.kind != ThreeDsCia::Kind::Update && info.kind != ThreeDsCia::Kind::Dlc ?
                                       ThreeDsCia::NOT_INSTALLED :
                                       ThreeDsCia::OK;
  if (refused != ThreeDsCia::OK)
  {
    LogF("3ds cia: %s refused: %s", file.file_name.c_str(), ThreeDsCia::CodeText(refused).c_str());
    ShowToast(ThreeDsCia::CodeText(refused), 0);
    Play("error");
    return;
  }
  m_cia_chosen = index;
  const bool update = info.kind == ThreeDsCia::Kind::Update;
  const int64_t free = ThreeDsCia::FreeBytes(Portable::UserDir());
  const uint64_t need = info.install_bytes + ThreeDsCia::SPARE_BYTES;
  const bool room = free < 0 || static_cast<uint64_t>(free) >= need;
  LogF("3ds cia: %s chosen: %s %s of %s, %s to install, %s free", file.file_name.c_str(), update ? "the update" : "DLC",
       ThreeDsCia::VersionText(info.version).c_str(), ThreeDsCia::Hex(ThreeDsCia::GameOf(info.title_id)).c_str(),
       ThreeDsCia::SizeText(info.install_bytes).c_str(),
       free < 0 ? "not known" : ThreeDsCia::SizeText(static_cast<uint64_t>(free)).c_str());
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_3DS_CIA_CONFIRM;
  m_panel.title = update ? "Install the Update" : "Install the DLC";
  m_panel.options = room ? std::vector<std::string>{"Install", "Cancel"} : std::vector<std::string>{"Back"};
  const auto add = [this](const std::string& translated) {
    std::istringstream lines(WrapText(m_font, 28, translated, 550.0f));
    for (std::string line; std::getline(lines, line);)
      m_panel.lines.push_back(line);
  };
  m_panel.lines = {""};
  add(CiaLine(file));
  add(update ? Tr("Update " + ThreeDsCia::VersionText(info.version)) : Tr("DLC"));
  m_panel.lines.push_back("");
  add(Tr("It takes: " + ThreeDsCia::SizeText(info.install_bytes)));
  add(Tr("Free space: " + (free < 0 ? Tr("not known") : ThreeDsCia::SizeText(static_cast<uint64_t>(free)))));
  m_panel.lines.push_back("");
  if (!room)
    add(Tr("Not enough free space: free some first (the PS4's storage, or the USB drive with Keep My Data on "
                   "the USB Drive)."));
  else if (update)
    add(Tr("An update can change the game's save: copy the save first if you may go back."));
  else
    add(Tr("The game finds its DLC from its next start."));
  Play("menu");
}

// Each frame: the install's panel - its progress, then its result once (the CIA deleted or kept).
void Menu::Impl::Update3dsInstall()
{
  if (!m_panel.open || m_panel.setting != SET_3DS_CIA_INSTALL)
    return;
  const ThreeDsCia::Progress progress = ThreeDsCia::Status();
  if (!progress.busy && !progress.done)
    return;  // (its result said)
  const ThreeDsCia::File* file = m_cia_chosen < m_cias.size() ? &m_cias[m_cia_chosen] : nullptr;
  const std::string name = file ? CiaLine(*file) : std::filesystem::path(progress.path).filename().string();
  const auto add = [this](const std::string& translated) {
    std::istringstream lines(WrapText(m_font, 28, translated, 550.0f));
    for (std::string line; std::getline(lines, line);)
      m_panel.lines.push_back(line);
  };
  m_panel.lines = {""};
  add(name);
  m_panel.lines.push_back("");
  if (progress.busy)
  {
    const int percent = progress.total > 0 ? static_cast<int>(progress.bytes * 100 / progress.total) : 0;
    add(Tr(m_panel.options.empty() ? "Cancelling..." : "Installing...") + " " + std::to_string(percent) + "%");
    if (progress.total > 0)
      add(ThreeDsCia::SizeText(progress.bytes) + " / " + ThreeDsCia::SizeText(progress.total));
    m_panel.lines.push_back("");
    add(Tr("Don't close EmuSuNi or turn off the PS4 until it ends."));
    return;
  }
  // Done: said once (its thread joined: the next frame has nothing to say).
  const int result = progress.result;
  ThreeDsCia::Forget();
  if (result == ThreeDsCia::OK)
  {
    const bool update = file && file->info.kind == ThreeDsCia::Kind::Update;
    add(update ? Tr("The update is installed: " + ThreeDsCia::VersionText(file->info.version)) :
                 Tr("The DLC is installed."));
    add(Tr("The game uses it from its next start."));
    m_panel.lines.push_back("");
    add(Tr("The CIA file isn't needed any more. Delete it from the drive?"));
    m_panel.options = {"Delete the CIA", "Keep the CIA"};
  }
  else
  {
    add(Tr(ThreeDsCia::CodeText(result)));
    m_panel.options = {"Back"};
  }
  m_panel.selected = 0;
  Play(result == ThreeDsCia::OK ? "confirm" : "error");
}

// A 3DS game's Triangle -> Updates, DLC and Mods: its update and its DLC (Remove: its save copied first), its mods,
// and ((01.22) the owner: from the game, not Settings) its own CIAs to install (Cross: Open3dsCiaConfirm); after the
// game closed for one of them (the in-game menu's), Start the Game at its top.
void Menu::Impl::Open3dsExtras(int game_index, const std::string& select_key, bool reread)
{
  if (game_index < 0 || game_index >= static_cast<int>(m_games.size()))
  {
    m_panel = {};  // (no game there any more: the games read again meanwhile)
    return;
  }
  const Game& game = m_games[game_index];
  m_3ds_game = game_index;
  m_3ds_path = game.rom.path;
  auto found = m_3ds_titles.find(game.rom.path);
  if (found == m_3ds_titles.end())
    found = m_3ds_titles.emplace(game.rom.path, ThreeDsCia::RomTitleId(game.rom.path)).first;
  m_3ds_title = found->second;
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_3DS_EXTRAS;
  m_panel.title = "Updates, DLC and Mods";
  if (m_3ds_title == 0)
  {
    m_panel.lines = {"", game.rom.title, ""};
    std::istringstream lines(WrapText(m_font, 28,
                                      Tr("A homebrew game (.3dsx) has no title ID: no updates, DLC or mods."),
                                      550.0f));
    for (std::string line; std::getline(lines, line);)
      m_panel.lines.push_back(line);
    Play("menu");
    return;
  }
  const std::string save_dir = ThreeDsSaveDir();
  std::vector<SettingRow> rows;
  if (m_extras_from_game)
  {
    SettingRow start{"Start the Game", "", "3ds:start"};
    start.help = "The game starts again with what it has installed now.";
    rows.push_back(start);
  }
  rows.push_back({game.rom.title, ThreeDsCia::Hex(m_3ds_title), "", true});
  if (ThreeDsCia::Available())
  {
    const ThreeDsCia::Installed has = ThreeDsCia::InstalledFor(save_dir, m_3ds_title);
    SettingRow update{"Update", Tr("None"), "3ds:update"};
    SettingRow dlc{"DLC", Tr("None"), "3ds:dlc"};
    update.help = dlc.help = "Install them from their CIA files: copied to Roms/3ds/cia, they show here.";
    if (has.read && has.has_update)
    {
      update.value = ThreeDsCia::VersionText(has.update_version) + " (" + ThreeDsCia::SizeText(has.update_bytes) + ")";
      update.help = "The game starts with this update. Another version: install its CIA (below, once in Roms/3ds/cia).";
    }
    if (has.read && has.dlc_titles > 0)
    {
      dlc.value = Tr("Installed") + " (" + ThreeDsCia::SizeText(has.dlc_bytes) + ")";
      dlc.help = Tr("Its DLC's contents installed: " + std::to_string(has.dlc_contents));
    }
    if (!has.read)
      update.help = dlc.help = ThreeDsCia::CodeText(has.code);
    rows.push_back(update);
    rows.push_back(dlc);
    const std::string copy_first =
        "Removed for good. An update or DLC can change the game's save: copy the save first (its folder: saves/3ds/"
        "Azahar/sdmc, by FTP), or the game may not read it after.";
    if (has.read && has.has_update)
    {
      SettingRow remove{"Remove the Update", "", "3ds:remove_update"};
      remove.help = copy_first;
      rows.push_back(remove);
    }
    if (has.read && has.dlc_titles > 0)
    {
      SettingRow remove{"Remove the DLC", "", "3ds:remove_dlc"};
      remove.help = copy_first;
      rows.push_back(remove);
    }
    // Its own CIAs in the "cia" folders (an encrypted one said; one that can't be read has no game to show under).
    if (reread)
      List3dsCias();
    const int64_t free = ThreeDsCia::FreeBytes(Portable::UserDir());
    rows.push_back({"Install from CIA Files",
                    free < 0 ? std::string() : "Free: " + ThreeDsCia::SizeText(static_cast<uint64_t>(free)), "",
                    true});
    size_t own = 0;
    for (size_t i = 0; i < m_cias.size(); i++)
    {
      const ThreeDsCia::File& file = m_cias[i];
      const ThreeDsCia::Info& info = file.info;
      if (!info.read || ThreeDsCia::GameOf(info.title_id) != m_3ds_title || info.kind == ThreeDsCia::Kind::Game)
        continue;
      own++;
      const bool is_update = info.kind == ThreeDsCia::Kind::Update;
      SettingRow row;
      row.label = is_update ? "Update " + ThreeDsCia::VersionText(info.version) : std::string("DLC");
      row.key = "cia:" + std::to_string(i);
      std::string help = file.file_name + "  \xC2\xB7  " + ThreeDsCia::SizeText(file.size);
      if (info.encrypted)
      {
        row.value = Tr("Encrypted");
        help += "\n" + Tr(ThreeDsCia::CodeText(ThreeDsCia::ENCRYPTED));
      }
      else if (is_update && has.read && has.has_update && has.update_version >= info.version)
      {
        row.value = Tr("Installed");
        help += "\n" + Tr("Installed already: " + ThreeDsCia::VersionText(has.update_version));
      }
      else
      {
        row.value = ThreeDsCia::SizeText(info.install_bytes);
        help += "\n" + Tr(!is_update && has.read && has.dlc_titles > 0 ?
                              "This game has DLC installed: Cross installs this one too." :
                              "Cross: install it.");
      }
      row.help = help;
      rows.push_back(row);
    }
    if (own == 0)
    {
      // Where they go: the 3DS's "cia" folders (one made in each game folder: List3dsCias).
      std::string where;
      for (const std::string& folder : ThreeDsCia::Folders(
               Library::ConsoleFolders(GetSystem(SystemId::THREEDS), GamesFolder(SystemId::THREEDS), m_places), false))
        where += (where.empty() ? "" : "\n") + folder;
      SettingRow none{"No CIA Files for It", "", "3ds:nocia"};
      none.help = Tr("Copy its decrypted update or DLC CIAs to the folder cia in Roms/3ds:") + "\n" +
                  (where.empty() ? std::string("USB: Roms/3ds/cia") : where);
      rows.push_back(none);
    }
  }
  else
  {
    SettingRow none{"Updates and DLC", Tr("Not in This Version"), "3ds:update"};
    none.help = ThreeDsCia::CodeText(ThreeDsCia::NO_CORE);
    rows.push_back(none);
  }
  SettingRow mods{"Mods", "", "3ds:mods"};
  mods.tag = ThreeDsCia::ModsOn(m_3ds_title) ? 1 : -1;
  const bool any = ThreeDsCia::HasMods(save_dir, m_3ds_title);
  const std::string folder = "saves/3ds/Azahar/load/mods/" + ThreeDsCia::Hex(m_3ds_title);
  mods.help = any ? "Its mods are loaded when it starts (Off: the game as it is - a mod for another version or region "
                    "can hang it). Their folder: " + folder :
                    "No mods in its folder (LayeredFS: romfs, exefs, code.ips): " + folder;
  rows.push_back(mods);
  m_panel.rows = std::move(rows);
  m_panel.selected = m_extras_from_game ? 0 : 1;
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!select_key.empty() && m_panel.rows[i].key == select_key)
      m_panel.selected = static_cast<int>(i);
  }
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
  else
    MoveSettingRow(0);
  Play("menu");
}

// "Remove?" for the game's update or its DLC (its save copied first).
void Menu::Impl::Ask3dsRemove(bool dlc)
{
  m_3ds_remove = (uint64_t{dlc ? 0x0004008Cu : 0x0004000Eu} << 32) | (m_3ds_title & 0xFFFFFFFFull);
  const std::string title =
      m_3ds_game >= 0 && m_3ds_game < static_cast<int>(m_games.size()) ? m_games[m_3ds_game].rom.title : std::string();
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_3DS_REMOVE;
  m_panel.title = dlc ? "Remove the DLC" : "Remove the Update";
  m_panel.options = {"Cancel", "Remove"};
  m_panel.lines = {"", title, ""};
  std::istringstream lines(WrapText(m_font, 28,
                                    Tr("An update or DLC can change the game's save, and the game may not read it once "
                                       "removed. Copy the save first (by FTP: saves/3ds/Azahar/sdmc)."),
                                    550.0f));
  for (std::string line; std::getline(lines, line);)
    m_panel.lines.push_back(line);
  Play("menu");
}

// Cross on the installer's or a game's options panels (their rows are ChangeSettingRow's).
void Menu::Impl::ThreeDsPanelChoice()
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.options.size()))
    return;
  const std::string choice = m_panel.options[static_cast<size_t>(m_panel.selected)];
  const std::string path = m_cia_chosen < m_cias.size() ? m_cias[m_cia_chosen].path : std::string();
  switch (m_panel.setting)
  {
  case SET_3DS_CIA_CONFIRM:
    if (choice != "Install")
    {
      Open3dsExtras(GameIndexOf(m_3ds_path), "cia:" + std::to_string(m_cia_chosen), false);
      Play("back");
      return;
    }
    MakeDirs(ThreeDsSaveDir());
    if (!ThreeDsCia::Start(ThreeDsSaveDir(), path))
    {
      ShowToast(ThreeDsCia::CodeText(ThreeDsCia::Status().result), 0);
      ThreeDsCia::Forget();
      Play("error");
      return;
    }
    m_panel.setting = SET_3DS_CIA_INSTALL;
    m_panel.options = {"Cancel"};
    m_panel.selected = 0;
    Update3dsInstall();
    Play("confirm");
    return;
  case SET_3DS_CIA_INSTALL:
    if (choice == "Cancel")
    {
      ThreeDsCia::Cancel();
      LogLine("3ds cia: the install cancelled by the player");
      m_panel.options.clear();  // ("Cancelling..." until it stops)
      Play("back");
    }
    else if (choice == "Delete the CIA")
    {
      m_panel = {};
      m_panel.open = true;
      m_panel.setting = SET_3DS_CIA_DELETE;
      m_panel.title = "Delete the CIA";
      m_panel.options = {"Cancel", "Delete"};
      m_panel.lines = {"", std::filesystem::path(path).filename().string(), ""};
      std::istringstream lines(WrapText(m_font, 28,
                                        Tr("It is deleted from the drive for good: the PS4 has no recycle "
                                                   "bin. What it installed stays."),
                                        550.0f));
      for (std::string line; std::getline(lines, line);)
        m_panel.lines.push_back(line);
      Play("menu");
    }
    else
    {
      Open3dsExtras(GameIndexOf(m_3ds_path), "cia:" + std::to_string(m_cia_chosen));
      Play("back");
    }
    return;
  case SET_3DS_CIA_DELETE:
    if (choice == "Delete")
    {
      std::error_code ec;
      const bool deleted = std::filesystem::remove(path, ec);
      LogF("3ds cia: %s deleted: %s", path.c_str(), deleted ? "yes" : ec ? ec.message().c_str() : "not there");
      ShowToast(deleted ? "CIA deleted" : "The CIA couldn't be deleted", 0);
      Open3dsExtras(GameIndexOf(m_3ds_path));
      Play(deleted ? "confirm" : "error");
    }
    else
    {
      Open3dsExtras(GameIndexOf(m_3ds_path), "cia:" + std::to_string(m_cia_chosen), false);
      Play("back");
    }
    return;
  case SET_3DS_REMOVE:
    if (choice == "Remove" && m_3ds_remove != 0)
    {
      const int code = ThreeDsCia::Uninstall(ThreeDsSaveDir(), m_3ds_remove);
      const bool dlc = ThreeDsCia::KindOf(m_3ds_remove) == ThreeDsCia::Kind::Dlc;
      ShowToast(code == ThreeDsCia::OK ? (dlc ? "DLC removed" : "Update removed") : ThreeDsCia::CodeText(code), 0);
      Open3dsExtras(GameIndexOf(m_3ds_path));
      Play(code == ThreeDsCia::OK ? "confirm" : "error");
    }
    else
    {
      Open3dsExtras(GameIndexOf(m_3ds_path), "", false);
      Play("back");
    }
    return;
  default:
    m_panel.open = false;
    Play("back");
  }
}

bool Menu::Impl::ThreeDsPanelBack()
{
  switch (m_panel.setting)
  {
  case SET_3DS_CIA_INSTALL:
    if (ThreeDsCia::Status().busy)
      return true;  // (it goes on: Cancel stops it)
    [[fallthrough]];
  case SET_3DS_CIA_CONFIRM:
  case SET_3DS_CIA_DELETE:
  {
    const bool installed = m_panel.setting == SET_3DS_CIA_INSTALL;  // (what it has may have changed: read again)
    Open3dsExtras(GameIndexOf(m_3ds_path), "cia:" + std::to_string(m_cia_chosen), installed);
    Play("back");
    return true;
  }
  case SET_3DS_REMOVE:
    Open3dsExtras(GameIndexOf(m_3ds_path), "", false);
    Play("back");
    return true;
  case SET_3DS_EXTRAS:
  {
    if (m_extras_from_game)
    {
      // ((01.22) Opened after the game closed for it: the list as it was - the game selected may be another one.)
      m_extras_from_game = false;
      m_panel.open = false;
      Play("back");
      return true;
    }
    OpenGameOptions();  // (the game is still the one selected)
    const auto at = std::find(m_panel.options.begin(), m_panel.options.end(), "Updates, DLC and Mods");
    m_panel.selected = at == m_panel.options.end() ? 0 : static_cast<int>(at - m_panel.options.begin());
    return true;
  }
  default:
    return false;
  }
}
#endif

// ---- (01.22, the owner: "En el menú de triángulo en una carátula y en el menú L2 y R2 en un juego") What a game has
// installed, from the game ----

int Menu::Impl::GameIndexOf(const std::string& path) const
{
  for (size_t i = 0; i < m_games.size(); i++)
  {
    if (m_games[i].rom.path == path)
      return static_cast<int>(i);
  }
  return -1;
}

void Menu::Impl::StartGameFromPanel(int game)
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return;
  m_extras_from_game = false;
  m_panel.open = false;
  ActivateItem(Item{m_games[game].rom.title, "", game});
}

void Menu::Impl::TellInstallFolders(SystemId system)
{
  std::vector<std::string> folders;
  const auto games = [&] { return Library::ConsoleFolders(GetSystem(system), GamesFolder(system), m_places); };
#ifdef EMUSUNI_3DS
  if (system == SystemId::THREEDS)
    folders = ThreeDsCia::Folders(games(), false);
#endif
  if (system == SystemId::ARCADE)
    folders = GameExtras::PatchedFolders(games(), false);
  GameExtras::SetInstallFolders(system, folders);
}

void Menu::Impl::OpenAfterGame(const GameExtras::AfterGame& after)
{
  const int game = GameIndexOf(after.game_path);
  if (game < 0)
  {
    LogLine("game extras: the game isn't in the list any more: " + after.game_path);
    return;
  }
  LogLine("game extras: back from the game for " + after.action);
  m_extras_from_game = true;
  const auto has_row = [this](const std::string& key) {
    return std::any_of(m_panel.rows.begin(), m_panel.rows.end(), [&](const SettingRow& r) { return r.key == key; });
  };
#ifdef EMUSUNI_3DS
  if (after.action.rfind("3ds:", 0) == 0)
  {
    Open3dsExtras(game);
    if (after.action == "3ds:remove_update" || after.action == "3ds:remove_dlc")
    {
      if (has_row(after.action))
        Ask3dsRemove(after.action == "3ds:remove_dlc");
    }
    else if (after.action.rfind("3ds:install:", 0) == 0)
    {
      const std::string path = after.action.substr(12);
      size_t at = m_cias.size();
      for (size_t i = 0; i < m_cias.size(); i++)
      {
        if (m_cias[i].path == path)
          at = i;
      }
      if (at < m_cias.size())
        Open3dsCiaConfirm(at);
      else
        ShowToast("The file isn't there any more", 0);
    }
    return;
  }
#endif
  if (after.action.rfind("arcade:", 0) == 0)
  {
    OpenArcadePatched(game);
    if (after.action == "arcade:remove")
    {
      if (has_row("patched:remove"))
        AskPatchedRemove();
    }
    else if (after.action.rfind("arcade:install:", 0) == 0)
    {
      const std::string path = after.action.substr(15);
      size_t at = m_patched_found.size();
      for (size_t i = 0; i < m_patched_found.size(); i++)
      {
        if (m_patched_found[i].path == path)
          at = i;
      }
      if (at < m_patched_found.size())
        OpenPatchedConfirm(at);
      else
        ShowToast("The file isn't there any more", 0);
    }
  }
}

std::string Menu::Impl::PatchedDir() const
{
  // (Emulator.cpp's: the Arcade's system folder - the player's data on a USB drive too -, its fbneo/patched.)
  return Portable::SystemDir(GetSystem(SystemId::ARCADE).key) + "/fbneo/patched";
}

// An Arcade game's Triangle -> Patched Romset: the one FinalBurn Neo uses instead of the game's zip (Remove It), and
// those of its set in Roms/arcade/patched (made where there is none, so the player sees where they go; Cross: install).
void Menu::Impl::OpenArcadePatched(int game_index, const std::string& select_key)
{
  if (game_index < 0 || game_index >= static_cast<int>(m_games.size()))
  {
    m_panel = {};  // (no game there any more: the games read again meanwhile)
    return;
  }
  const Game& game = m_games[game_index];
  m_patched_game = game_index;
  m_patched_path = game.rom.path;
  const std::string& set = game.rom.id;
  const std::string dir = PatchedDir();
  const GameExtras::PatchedFile in_use = GameExtras::PatchedInUse(dir, set);
  const std::vector<std::string> folders = GameExtras::PatchedFolders(
      Library::ConsoleFolders(GetSystem(SystemId::ARCADE), GamesFolder(SystemId::ARCADE), m_places), true);
  m_patched_found = GameExtras::PatchedFound(folders, set);
  LogF("arcade patched: %s: %s in use, %zu found", set.c_str(), in_use.path.empty() ? "none" : in_use.path.c_str(),
       m_patched_found.size());
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ARCADE_PATCHED;
  m_panel.title = "Patched Romset";
  std::vector<SettingRow> rows;
  if (m_extras_from_game)
  {
    SettingRow start{"Start the Game", "", "patched:start"};
    start.help = "The game starts again with what it has installed now.";
    rows.push_back(start);
  }
  rows.push_back({game.rom.title, set, "", true});
  SettingRow use{"In Use", in_use.path.empty() ? Tr("None") : in_use.name, "patched:inuse"};
  use.help = in_use.path.empty() ?
                 "The game starts from its own zip. A patched romset (a hack with no driver of its own, named as the "
                 "set) takes its place: copied to Roms/arcade/patched, it shows below." :
                 Tr("FinalBurn Neo starts the game from it instead of its own zip (never online, where both consoles "
                    "need the set as the DAT knows it).") +
                     "\n" + in_use.path + "  \xC2\xB7  " + GameExtras::SizeText(in_use.size);
  rows.push_back(use);
  if (!in_use.path.empty())
  {
    SettingRow remove{"Remove It", "", "patched:remove"};
    remove.help = "Deleted from the PS4: the game starts from its own zip again.";
    rows.push_back(remove);
  }
  const int64_t free = GameExtras::FreeBytes(Portable::SystemDir(GetSystem(SystemId::ARCADE).key));
  rows.push_back({"Install from Roms/arcade/patched",
                  free < 0 ? std::string() : "Free: " + GameExtras::SizeText(static_cast<uint64_t>(free)), "", true});
  for (size_t i = 0; i < m_patched_found.size(); i++)
  {
    const GameExtras::PatchedFile& file = m_patched_found[i];
    SettingRow row{file.name, GameExtras::SizeText(file.size), "patched:file:" + std::to_string(i)};
    row.help = file.path + "\n" +
               Tr(in_use.path.empty() ? "Cross: install it." : "Cross: install it (in place of the one in use).");
    rows.push_back(row);
  }
  if (m_patched_found.empty())
  {
    std::string where;
    for (const std::string& folder : folders)
      where += (where.empty() ? "" : "\n") + folder;
    SettingRow none{"None Found", "", "patched:none"};
    none.help = Tr("Copy the patched romset, named as the game's set, to the folder patched in Roms/arcade: " + set +
                   ".zip") +
                "\n" + (where.empty() ? std::string("USB: Roms/arcade/patched") : where);
    rows.push_back(none);
  }
  m_panel.rows = std::move(rows);
  m_panel.selected = m_extras_from_game ? 0 : 1;  // (In Use: not Remove It at once)
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!select_key.empty() && m_panel.rows[i].key == select_key)
      m_panel.selected = static_cast<int>(i);
  }
  m_panel.selected = std::min(m_panel.selected, static_cast<int>(m_panel.rows.size()) - 1);
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
  else
    MoveSettingRow(0);
  Play("menu");
}

// One found chosen: what it takes, the free space; Install / Cancel (only Back without the room).
void Menu::Impl::OpenPatchedConfirm(size_t index)
{
  if (index >= m_patched_found.size() || m_patched_game < 0 || m_patched_game >= static_cast<int>(m_games.size()))
    return;
  m_patched_chosen = index;
  const GameExtras::PatchedFile& file = m_patched_found[index];
  const Game& game = m_games[m_patched_game];
  const std::string dir = PatchedDir();
  const GameExtras::PatchedFile in_use = GameExtras::PatchedInUse(dir, game.rom.id);
  const int64_t free = GameExtras::FreeBytes(Portable::SystemDir(GetSystem(SystemId::ARCADE).key));
  const uint64_t need = file.size + (16ull << 20);
  const bool room = free < 0 || static_cast<uint64_t>(free) >= need;  // (the one in use goes once the copy is whole)
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ARCADE_CONFIRM;
  m_panel.title = "Install the Patched Romset";
  m_panel.options = room ? std::vector<std::string>{"Install", "Cancel"} : std::vector<std::string>{"Back"};
  const auto add = [this](const std::string& translated) {
    std::istringstream lines(WrapText(m_font, 28, translated, 550.0f));
    for (std::string line; std::getline(lines, line);)
      m_panel.lines.push_back(line);
  };
  m_panel.lines = {""};
  add(game.rom.title);
  add(file.name);
  m_panel.lines.push_back("");
  add(Tr("It takes: " + GameExtras::SizeText(file.size)));
  add(Tr("Free space: " + (free < 0 ? Tr("not known") : GameExtras::SizeText(static_cast<uint64_t>(free)))));
  m_panel.lines.push_back("");
  if (!room)
    add(Tr("Not enough free space: free some first (the PS4's storage, or the USB drive with Keep My Data on "
           "the USB Drive)."));
  else if (!in_use.path.empty())
    add(Tr("It takes the place of the one in use: " + in_use.name));
  add(Tr("FinalBurn Neo starts the game from it from its next start (online: its own zip)."));
  Play("menu");
}

void Menu::Impl::AskPatchedRemove()
{
  if (m_patched_game < 0 || m_patched_game >= static_cast<int>(m_games.size()))
    return;
  const Game& game = m_games[m_patched_game];
  const GameExtras::PatchedFile in_use = GameExtras::PatchedInUse(PatchedDir(), game.rom.id);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ARCADE_REMOVE;
  m_panel.title = "Remove the Patched Romset";
  m_panel.options = {"Cancel", "Remove"};
  m_panel.lines = {"", game.rom.title, in_use.name, ""};
  std::istringstream lines(WrapText(m_font, 28,
                                    Tr("It is deleted from the PS4 for good: the game starts from its own zip again. "
                                       "Its copy in Roms/arcade/patched, if any, stays."),
                                    550.0f));
  for (std::string line; std::getline(lines, line);)
    m_panel.lines.push_back(line);
  Play("menu");
}

// Each frame: the copy's panel - its progress, then its result once.
void Menu::Impl::UpdatePatchedCopy()
{
  if (!m_panel.open || m_panel.setting != SET_ARCADE_COPY)
    return;
  const GameExtras::CopyProgress progress = GameExtras::CopyStatus();
  if (!progress.busy && !progress.done)
    return;  // (its result said)
  const auto add = [this](const std::string& translated) {
    std::istringstream lines(WrapText(m_font, 28, translated, 550.0f));
    for (std::string line; std::getline(lines, line);)
      m_panel.lines.push_back(line);
  };
  m_panel.lines = {""};
  add(std::filesystem::path(progress.from).filename().string());
  m_panel.lines.push_back("");
  if (progress.busy)
  {
    const int percent = progress.total > 0 ? static_cast<int>(progress.bytes * 100 / progress.total) : 0;
    add(Tr(m_panel.options.empty() ? "Cancelling..." : "Copying...") + " " + std::to_string(percent) + "%");
    if (progress.total > 0)
      add(GameExtras::SizeText(progress.bytes) + " / " + GameExtras::SizeText(progress.total));
    m_panel.lines.push_back("");
    add(Tr("Don't close EmuSuNi or turn off the PS4 until it ends."));
    return;
  }
  GameExtras::ForgetCopy();  // (said once: its thread joined)
  if (progress.ok)
  {
    add(Tr("The patched romset is installed."));
    add(Tr("The game uses it from its next start."));
  }
  else
  {
    add(Tr(progress.cancelled ? std::string("Cancelled: the game keeps what it had before") : progress.error));
  }
  m_panel.options = {"OK"};
  m_panel.selected = 0;
  Play(progress.ok ? "confirm" : "error");
}

void Menu::Impl::ArcadePanelChoice()
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.options.size()))
    return;
  const std::string choice = m_panel.options[static_cast<size_t>(m_panel.selected)];
  const int game = GameIndexOf(m_patched_path);
  const std::string chosen_key = "patched:file:" + std::to_string(m_patched_chosen);
  switch (m_panel.setting)
  {
  case SET_ARCADE_CONFIRM:
    if (choice != "Install" || game < 0 || m_patched_chosen >= m_patched_found.size())
    {
      OpenArcadePatched(game, chosen_key);
      Play("back");
      return;
    }
    if (!GameExtras::StartCopy(m_patched_found[m_patched_chosen].path, PatchedDir(), m_games[game].rom.id))
    {
      const GameExtras::CopyProgress progress = GameExtras::CopyStatus();
      ShowToast(progress.error.empty() ? std::string("Another copy is going on") : progress.error, 0);
      GameExtras::ForgetCopy();
      Play("error");
      return;
    }
    m_panel.setting = SET_ARCADE_COPY;
    m_panel.options = {"Cancel"};
    m_panel.selected = 0;
    UpdatePatchedCopy();
    Play("confirm");
    return;
  case SET_ARCADE_COPY:
    if (choice == "Cancel")
    {
      GameExtras::CancelCopy();
      LogLine("arcade patched: the copy cancelled by the player");
      m_panel.options.clear();  // ("Cancelling..." until it stops)
      Play("back");
      return;
    }
    OpenArcadePatched(game, chosen_key);
    Play("back");
    return;
  case SET_ARCADE_REMOVE:
    if (choice == "Remove" && game >= 0)
    {
      std::string error;
      const bool removed = GameExtras::RemovePatched(PatchedDir(), m_games[game].rom.id, &error);
      ShowToast(removed ? "Patched romset removed" : "It couldn't be deleted: " + error, 0);
      OpenArcadePatched(game);
      Play(removed ? "confirm" : "error");
      return;
    }
    OpenArcadePatched(game, "patched:remove");
    Play("back");
    return;
  default:
    m_panel.open = false;
    Play("back");
  }
}

bool Menu::Impl::ArcadePanelBack()
{
  const int game = GameIndexOf(m_patched_path);
  switch (m_panel.setting)
  {
  case SET_ARCADE_COPY:
    if (GameExtras::CopyStatus().busy)
      return true;  // (it goes on: Cancel stops it)
    [[fallthrough]];
  case SET_ARCADE_CONFIRM:
    OpenArcadePatched(game, "patched:file:" + std::to_string(m_patched_chosen));
    Play("back");
    return true;
  case SET_ARCADE_REMOVE:
    OpenArcadePatched(game, "patched:remove");
    Play("back");
    return true;
  case SET_ARCADE_PATCHED:
  {
    if (m_extras_from_game)
    {
      // (Opened after the game closed for it: the list as it was - the game selected may be another one.)
      m_extras_from_game = false;
      m_panel.open = false;
      Play("back");
      return true;
    }
    OpenGameOptions();  // (the game is still the one selected)
    const auto at = std::find(m_panel.options.begin(), m_panel.options.end(), "Patched Romset");
    m_panel.selected = at == m_panel.options.end() ? 0 : static_cast<int>(at - m_panel.options.begin());
    return true;
  }
  default:
    return false;
  }
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
  case SET_STATS:
    UsageStats::SetEnabled(option == 0);
    break;
  case SET_STATS_NOTICE:
    StartStats();  // (first: off before the start wouldn't be kept)
    if (option == 1)
      UsageStats::SetEnabled(false);
    Log(std::string("stats: the notice answered: ") + (option == 1 ? "turned off" : "OK"));
    break;
  case SET_RATINGS:
    if (option == 2)
    {
      // Download Now / Again: LaunchBox's ratings of today (in the background; the Library's title line says how far).
      const bool started = !m_rating_download.Busy() && StartRatingsDownload();
      ShowToast(started ? "Downloading LaunchBox's ratings" : "The ratings are being downloaded already", 0);
      return;
    }
    m_prefs.ratings = option == 0;
    if (m_prefs.ratings && !m_rating_download_tried && Ratings::CopyOld(m_rating_copy))
      StartRatingsDownload();  // (turned on without ratings: they come now)
    break;
  case SET_LANGUAGE:
    if (option >= 0 && option < static_cast<int>(PS4Lang::All().size()))
      SetLanguage(PS4Lang::All()[option].code);
    m_first_language = false;
    break;
  case SET_STYLE:
    m_prefs.style = option == 0 ? "library" : "xmb";
    break;
  case SET_MENU_SIZE:
    // (00.85: a global value of settings.ini, as Covers Saved On; the next frame draws it - Draw reads it.)
    if (const Settings::Setting* size = Settings::Find("menu_size"))
    {
      if (option >= 0 && option < static_cast<int>(size->choices.size()))
        Settings::Set("menu_size", size->choices[static_cast<size_t>(option)].value);
    }
    break;
  case SET_COVERS:
  {
    // (Settings::Set: a global value of settings.ini, as the language: Reset All Settings puts it back.)
    Settings::Set("covers_on", option == 1 ? "usb" : "internal");
    const std::string folder = CoversSaveDir(true);  // made now on the drive
    Log("covers: new ones go to " + folder);
    IndexCovers();  // the place saved in wins over the other one now
    ShowToast(option == 1 && folder == UserDir() + "/covers" ? std::string("No USB drive: covers stay internal") :
                                                                "Covers: " + folder,
              0);
    break;
  }
  case SET_RESET_ALL:
    if (option == 1)
    {
      // The games' settings; the menu's own options stay, as the language does (Consoles Shown, Covers
      // Saved On and Menu Size, 00.85, are global values of settings.ini too).
      std::vector<std::pair<std::string, std::string>> menu;
      for (const auto& [key, value] : Settings::GlobalValues())
      {
        if (key == "consoles_shown" || key == "consoles_off" || key == "covers_on" || key == "menu_size" ||
            key == "stats_notice")
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
  if (m_online_ask >= 0)
  {
    OnlineAskInput(pressed);  // (00.86: a player asking to join our room)
    return;
  }
  if (m_panel.open && OnlinePanelInput(pressed))
    return;  // (00.23: online play's panels)
  if (m_panel.open)
  {
    if (m_panel.setting == SET_FIND_COVER)
      m_find_select_first = false;  // the user moved first: no jump to the closest picture
    if (m_panel.setting == SET_CONSOLE_ORDER && ConsoleOrderInput(pressed))
      return;
    if ((pressed & BTN_CIRCLE) && m_panel.setting == SET_PORTABLE_COPY && (PortableCopyBusy() || m_portable_ok))
      return;  // (00.87: the copy of the player's data goes on; once done, Restart Now)
    if (pressed & BTN_CIRCLE)
    {
      if (m_first_language && m_panel.setting == SET_LANGUAGE)
        ApplySettingOption(SET_LANGUAGE, m_panel.selected);
#ifdef EMUSUNI_3DS
      if (ThreeDsPanelBack())
        return;  // ((01.09) the 3DS's installer: back to its list; an install going on stays)
#endif
      if (ArcadePanelBack())
        return;  // ((01.22) an Arcade game's patched romset: back to its panel; a copy going on stays)
      if (m_panel.setting == SET_RA_HISTORY)
      {
        // (My Achievements, 00.75: back to Settings -> RetroAchievements, on its row.)
        OpenAchievementsPanel();
        for (size_t i = 0; i < m_panel.rows.size(); i++)
        {
          if (m_panel.rows[i].key == "ra:history")
            m_panel.selected = static_cast<int>(i);
        }
        Play("back");
        return;
      }
      if (m_panel.setting == SET_RA_ABOUT)
      {
        // (What is RetroAchievements?, 00.79: back to Settings -> RetroAchievements, on its row.)
        OpenAchievementsPanel();
        for (size_t i = 0; i < m_panel.rows.size(); i++)
        {
          if (m_panel.rows[i].key == "ra:about")
            m_panel.selected = static_cast<int>(i);
        }
        Play("back");
        return;
      }
      if (m_panel.setting == SET_RA_VERSIONS || (m_panel.setting == SET_RA_GAME && m_ra_from_game))
      {
        // (A game's Triangle -> Achievements, 00.79: back to its options, on that one.)
        m_ra_from_game = false;
        OpenGameOptions();
        const auto at = std::find(m_panel.options.begin(), m_panel.options.end(), "Achievements");
        m_panel.selected = at == m_panel.options.end() ? 0 : static_cast<int>(at - m_panel.options.begin());
        return;  // (OpenGameOptions' own sound)
      }
      if (m_panel.setting == SET_RA_GAME)
      {
        // (A game's achievements, 00.76: back to My Achievements, on the game's row.)
        OpenHistoryPanel();
        const std::string key = "ra:game:" + std::to_string(m_ra_game);
        for (size_t i = 0; i < m_panel.rows.size(); i++)
        {
          if (m_panel.rows[i].key == key)
            m_panel.selected = static_cast<int>(i);
        }
        Play("back");
        return;
      }
      if (m_panel.setting == SET_DELETE)
      {
        OpenSavesPanel(m_saves_panel);
        Play("back");
        return;
      }
      if (m_panel.setting == SET_BEZELS_DELETE)
      {
        // (Delete Downloaded Bezels' "Delete?", 00.80: back to Game Bezels, on that row.)
        OpenBezelsPanel();
        for (size_t i = 0; i < m_panel.rows.size(); i++)
        {
          if (m_panel.rows[i].key == "bz:delete")
            m_panel.selected = static_cast<int>(i);
        }
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
      // (00.76: round from the first row to the last and back on a fresh press; a held arrow stops at the end.)
      if (pressed & BTN_UP)
      {
        MoveSettingRow(-1, Fresh(BTN_UP));
        Play("move");
      }
      if (pressed & BTN_DOWN)
      {
        MoveSettingRow(1, Fresh(BTN_DOWN));
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
    for (const uint32_t arrow : {BTN_UP, BTN_DOWN})
    {
      // (00.76: round on a fresh press, as the lists.)
      const int next = MenuNav::Step(m_panel.selected, arrow == BTN_UP ? -1 : 1, count, Fresh(arrow));
      if ((pressed & arrow) && next != m_panel.selected)
      {
        m_panel.selected = next;
        Play("move");
      }
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
    if (m_panel.setting == SET_DELETE_GAME && m_panel.options[m_panel.selected].rfind("Saves and States: ", 0) == 0)
    {
      // ((01.19) Delete Game: its saves and states kept or deleted too - the choice changes, the panel stays.)
      m_delete_saves = !m_delete_saves;
      m_panel.options[m_panel.selected] = m_delete_saves ? "Saves and States: Delete Too" : "Saves and States: Keep";
      SetDeleteGameLines();
      Play("category");
      return;
    }
    if (m_panel.setting == SET_DELETE_GAME || m_panel.setting == SET_CLEANUP_CONFIRM)
    {
      const bool go = m_panel.options[m_panel.selected] == "Delete";
      const bool saves = m_panel.setting == SET_DELETE_GAME && m_delete_saves;
      m_panel.open = false;
      if (go)
        DeleteGames(m_pending_delete, saves);
      else
        Play("back");
      m_pending_delete.clear();
      m_delete_saves = false;
      return;
    }
    if (m_panel.setting == SET_BEZELS_DELETE)
    {
      // Delete Downloaded Bezels (00.80): only the files the downloads wrote (their records say which), then back to
      // Game Bezels.
      const bool go = m_panel.options[m_panel.selected] == "Delete" && !m_bezel_download.Busy();
      if (go)
      {
        std::vector<std::string> keys;
        for (const System& system : Systems())
        {
          if (BezelProject::Has(system.key))
            keys.push_back(system.key);
        }
        const BezelProject::Downloaded deleted = BezelProject::DeleteAll(UserDir(), keys);
        m_bezels_counted = false;
        ShowToast("Bezels deleted: " + std::to_string(deleted.files) + " (" +
                      std::to_string((deleted.bytes + (1 << 19)) >> 20) + " MB)",
                  0);
        for (Item& item : m_items[static_cast<int>(Category::Settings)])
        {
          if (item.setting == SET_BEZELS)
            item.detail = BezelsDetail();
        }
      }
      OpenBezelsPanel();
      for (size_t i = 0; i < m_panel.rows.size(); i++)
      {
        if (m_panel.rows[i].key == "bz:delete")
          m_panel.selected = static_cast<int>(i);
      }
      Play(go ? "confirm" : "back");
      return;
    }
    if (m_panel.setting == SET_SEND_LOG || m_panel.setting == SET_SEND_LOG_ASK)
    {
      SendLogChoice();  // (00.91)
      return;
    }
    if (m_panel.setting == SET_UPDATES || m_panel.setting == SET_UPDATE_NOTICE)
    {
      UpdatePanelChoice();
      return;
    }
    if (m_panel.setting >= SET_PORTABLE && m_panel.setting <= SET_PORTABLE_WARNING)
    {
      PortablePanelChoice();  // (00.87)
      return;
    }
#ifdef EMUSUNI_3DS
    if (m_panel.setting >= SET_3DS_CIA && m_panel.setting <= SET_3DS_REMOVE)
    {
      ThreeDsPanelChoice();  // (01.09)
      return;
    }
#endif
    if (m_panel.setting >= SET_ARCADE_PATCHED && m_panel.setting <= SET_ARCADE_REMOVE)
    {
      ArcadePanelChoice();  // (01.22)
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
        m_delete_title = game.rom.title;
        m_delete_file = std::filesystem::path(game.rom.path).filename().string();
        // A .cue's tracks and each disc of a game on several go with it (DeleteGames).
        m_delete_files = Library::GameFiles(game.rom).size();
        m_delete_saves = false;  // ((01.19) its saves stay unless chosen: put back, the game would find them)
        m_panel = {};
        m_panel.open = true;
        m_panel.setting = SET_DELETE_GAME;
        m_panel.title = "Delete Game";
        m_panel.options = {"Cancel", "Delete", "Saves and States: Keep"};
        SetDeleteGameLines();
        Play("menu");
        return;
      }
      if (choice == "Play Online")
      {
        OpenOnlineHost(item.game);  // (00.23: a private or a public room)
        return;
      }
      if (choice == "Link Cable Online")
      {
        OpenOnlineHost(item.game, true);  // (00.95: a Game Boy link cable room)
        return;
      }
      if (choice == "Rename")
      {
        const std::string key = game.rom.key;
        m_panel.open = false;
        AskText(
            "Rename", game.rom.title, [this, key](const std::string& name) { RenameGame(key, name); }, 100,
            TextKeyboard::Mode::Text, "Left empty, the game takes its file's own name again.");
        return;
      }
      if (choice == "Find Cover")
      {
        OpenFindCover(item.game);
        return;
      }
#ifdef EMUSUNI_3DS
      if (choice == "Updates, DLC and Mods")
      {
        Open3dsExtras(item.game);  // (01.09)
        return;
      }
#endif
      if (choice == "Patched Romset")
      {
        OpenArcadePatched(item.game);  // (01.22)
        return;
      }
      if (choice == "Achievements")
      {
        // RetroAchievements (00.79): the page My Achievements has for the game (every achievement, its progress, in
        // the app's language), or - a file it doesn't know - the files it knows of the game. (00.80) A file the marks
        // haven't checked (or with them off): checked now, on that page ("Checking this file...").
        if (m_ra_marks && game.ra.state == Achievements::LibraryInfo::State::Known)
        {
          OpenGameAchievements(game.ra.game_id);
          m_ra_from_game = true;
        }
        else
        {
          OpenVersionsPanel(game);
        }
        Play("menu");
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
        // (01.05, the owner: "Revisar mis juegos") Whether its file is the standard version (No-Intro's, FinalBurn
        // Neo's): once NetplayLibrary has read it (it is read in the background, from here too).
        const std::string console_key = GetSystem(game.rom.system).key;
        if (game.rom.discs.size() <= 1 && PlaysOnline(game.rom.system) && NetplayLibrary::Covered(console_key))
        {
          uint32_t crc = 0;
          uint64_t content = 0;
          std::string line;
          if (NetplayLibrary::Lookup(game.rom.path, &crc, &content))
          {
            const std::string name = NetplayLibrary::StandardName(console_key, crc, content);
            line = name.empty() ? "Standard version: no (" + NetplayLibrary::StandardSource(console_key) + ")" :
                                  "Standard version: " + name;
          }
          else
          {
            line = "Standard version: being checked (Online Settings: Check My Games)";
            StartOnlineLibrary();
          }
          m_panel.lines.insert(m_panel.lines.begin() + 5, line);
        }
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
        // Its rating (src/Ratings.h) after when it was played: its stars on the first line, then where it is from.
        {
          const std::vector<std::string> rating = RatingLines(game);
          auto at = std::find(m_panel.lines.begin(), m_panel.lines.end(), PlayedText(game.played));
          at = at == m_panel.lines.end() ? m_panel.lines.end() : at + 1;
          m_panel.lines.insert(at, rating.begin(), rating.end());
          m_panel.stars = game.rating.half_stars;
          m_panel.stars_line = rating.front();
          // RetroAchievements (00.77): its achievements and the player's progress, after the rating.
          if (m_ra_marks)
          {
            auto after = std::find(m_panel.lines.begin(), m_panel.lines.end(), rating.back());
            after = after == m_panel.lines.end() ? m_panel.lines.end() : after + 1;
            m_panel.lines.insert(after, AchievementLine(game));
          }
        }
      }
      else
      {
        // One default game: setting another replaces it, the same game again removes it.
        const bool remove = m_prefs.default_game == game.rom.path;
        m_prefs.default_game = remove ? "" : game.rom.path;
        m_prefs.Save(UserDir() + "/xmb.ini");
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
  // (00.85) The Online tab's table: its rows and its buttons (the tabs' and the columns' stay as they are).
  if (m_category == static_cast<int>(Category::Online) && OnlineTabInput(pressed))
    return;

  auto& items = m_items[m_category];
  int& selected = m_selected[m_category];
  const int count = static_cast<int>(items.size());
  const bool games = IsGameList(m_category);
  // XMB: Left / Right switch the columns, Up / Down the items, L1 / R1 jump five.
  // Library: L1 / R1 switch the tabs; the games scroll sideways, the other tabs up and down.
  // (A page of Settings is where Settings is.)
  // (00.76) Every one of these goes round on a fresh press - the first tab or column to the last, the first item to
  // the last and back -, while a held button (the arrows, L1 / R1: they repeat) stops at the end once
  // (src/MenuNav.h). The Library's tabs went round before too, held or not. L2 (Favorite) and R2 (Search) stay one
  // press each: they act, they don't move.
  const auto& columns = Library() ? Tabs() : Columns();
  const int column = Library() ? LibraryTab() : ColumnIndex(m_category);
  const int column_count = static_cast<int>(columns.size());
  uint32_t prev_tab = BTN_LEFT, next_tab = BTN_RIGHT, prev_item = BTN_UP, next_item = BTN_DOWN;
  uint32_t prev_page = BTN_L1, next_page = BTN_R1;
  if (Library())
  {
    prev_tab = BTN_L1;
    next_tab = BTN_R1;
    prev_item = games ? BTN_LEFT : BTN_UP;
    next_item = games ? BTN_RIGHT : BTN_DOWN;
    // Up / Down jump ten games in the row.
    prev_page = games ? BTN_UP : 0;
    next_page = games ? BTN_DOWN : 0;
  }
  const int page = Library() ? 10 : 5;
  // The item `to` selected: a move round the list (the first to the last) jumps there at once, so the covers between
  // aren't drawn - and loaded - on the way.
  const auto select = [&](int to) {
    const bool round = std::abs(to - selected) > page;
    selected = to;
    if (round)
      m_item_pos[m_category] = static_cast<float>(to);
    Play("move");
  };
  if (pressed & (prev_tab | next_tab))
  {
    const uint32_t button = (pressed & prev_tab) ? prev_tab : next_tab;
    const int to = MenuNav::Step(column, button == prev_tab ? -1 : 1, column_count, Fresh(button));
    if (to != column && to >= 0 && to < column_count)
    {
      m_category = columns[to];
      Play("category");
      // ((01.07 to 01.08 the Library's L1 / R1 went through the Online tab's pages, starting at its first or its last;
      // (01.09) L2 / R2 only: the tab shows the page it was left on.)
      // Its covers and a look at the drives (a drive plugged in since: its games come) once the tabs settle
      // (UpdateTabs): holding L1 / R1 through them loads nothing on the way.
      m_tab_changed = m_time;
      if (IsGameList(m_category))
        m_places_waiting = true;
    }
  }
  else if ((pressed & prev_item) && count > 0)
  {
    const int to = MenuNav::Step(selected, -1, count, Fresh(prev_item));
    if (to != selected)
      select(to);
  }
  else if ((pressed & next_item) && count > 0)
  {
    const int to = MenuNav::Step(selected, 1, count, Fresh(next_item));
    if (to != selected)
      select(to);
  }
  else if (prev_page && (pressed & prev_page) && count > 0)
  {
    const int to = MenuNav::Jump(selected, -page, count, Fresh(prev_page));
    if (to != selected)
      select(to);
  }
  else if (next_page && (pressed & next_page) && count > 0)
  {
    const int to = MenuNav::Jump(selected, page, count, Fresh(next_page));
    if (to != selected)
      select(to);
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
    // ((01.18) Order and Filters: the order - until 01.17 Options went to the next one at once -, a genre, the
    // players, Hide Adult Games.)
    OpenViewPanel();
  }
  else if ((pressed & BTN_TOUCHPAD) && games)
  {
    // RetroAchievements' filter (00.80): every game, those with achievements, those with achievements in progress -
    // as the marks found them (Settings -> RetroAchievements -> Marks on the Games); kept in xmb.ini.
    if (!m_ra_marks)
    {
      ShowToast("The filter needs Settings -> RetroAchievements -> Marks on the Games", 0);
      Play("error");
    }
    else
    {
      m_prefs.ra_filter = m_prefs.ra_filter == "off" ? "known" : m_prefs.ra_filter == "known" ? "progress" : "off";
      m_prefs.Save(UserDir() + "/xmb.ini");
      RebuildGameLists(true);
      m_sort_shown = 2.5f;
      if (!Library())
        ShowToast(RaFilterOn() ? RaFilterLine(m_category) : std::string("Every game"), 0);  // (the XMB's own line)
      Play("category");
    }
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
    m_board.Open(TextKeyboard::Mode::Search, m_search, SEARCH_MAX);
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
  std::ifstream file(UserDir() + "/favorites.ini");
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
  WriteTextFile(UserDir() + "/favorites.ini", text);
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

// The keyboard (00.80, src/TextKeyboard.h): the search's (R2 in the games) or a text asked for (AskText). Its buttons
// are the menu's in both: Cross types the selected key, Circle goes back (the search: closed, as typed; a text asked
// for: Cancel), Square deletes, Triangle types a space; the others are on the keys they do the same as.
void Menu::Impl::HandleKeyboard(uint32_t pressed)
{
  using TextKeyboard::Button;
  const TextKeyboard::Mode mode = m_board.GetMode();
  if (mode == TextKeyboard::Mode::Search)
    m_board.SetText(m_search);  // (the search may have changed without the keyboard: the tour)
  // (00.76: the rows and the keys go round on a fresh press - as they always did -, a held arrow stops at the edge.)
  if (pressed & (BTN_UP | BTN_DOWN))
  {
    const uint32_t arrow = (pressed & BTN_UP) ? BTN_UP : BTN_DOWN;
    if (m_board.Move(arrow == BTN_UP ? -1 : 1, 0, Fresh(arrow)))
      Play("move");
    return;
  }
  if (pressed & (BTN_LEFT | BTN_RIGHT))
  {
    const uint32_t arrow = (pressed & BTN_LEFT) ? BTN_LEFT : BTN_RIGHT;
    if (m_board.Move(0, arrow == BTN_LEFT ? -1 : 1, Fresh(arrow)))
      Play("move");
    return;
  }
  if (pressed & BTN_CROSS)
  {
    KeyboardDid(m_board.Press(), 1, BTN_CROSS);
    return;
  }
  static constexpr std::pair<uint32_t, Button> BUTTONS[] = {
      {BTN_L2, Button::L2},         {BTN_L1, Button::L1}, {BTN_R1, Button::R1},           {BTN_SQUARE, Button::Square},
      {BTN_TRIANGLE, Button::Triangle}, {BTN_R2, Button::R2}, {BTN_OPTIONS, Button::Options}, {BTN_CIRCLE, Button::Circle}};
  for (const auto& [bit, button] : BUTTONS)
  {
    if (!(pressed & bit))
      continue;
    // (L1 / R1 held: the pages or the games one after another, round only on a fresh press.)
    const int direction = button == Button::L1 ? -1 : 1;
    KeyboardDid(m_board.Do(TextKeyboard::ButtonAction(mode, button), direction, Fresh(bit)), direction, bit);
    return;
  }
}

// What a key or a button did: its sound, the search's lists, the text asked for to whom asked (OK) or forgotten.
void Menu::Impl::KeyboardDid(TextKeyboard::Result result, int direction, uint32_t button)
{
  using TextKeyboard::Result;
  const bool search = m_board.GetMode() == TextKeyboard::Mode::Search;
  switch (result)
  {
  case Result::Typed:
  case Result::Erased:
    if (search)
      SetSearch(m_board.Text());
    Play(result == Result::Typed ? "category" : "back");
    break;
  case Result::Toggled:
    Play("move");
    break;
  case Result::Refused:
    Play("error");
    break;
  case Result::Browse:
  {
    // The games found, one by one, without leaving the keyboard (00.76: held, one after another; round on a fresh
    // press).
    const auto& items = m_items[m_category];
    int& selected = m_selected[m_category];
    const int count = static_cast<int>(items.size());
    const int next = MenuNav::Step(selected, direction, count, Fresh(button));
    if (IsGameList(m_category) && !items.empty() && items[0].game >= 0 && next != selected && next >= 0 &&
        next < count)
    {
      if (std::abs(next - selected) > 1)
        m_item_pos[m_category] = static_cast<float>(next);  // (round: there at once, no covers on the way)
      selected = next;
      Play("move");
    }
    break;
  }
  case Result::Done:
    m_keyboard = false;
    Play("confirm");
    break;
  case Result::Ok:
  {
    // (Taken from the keyboard first: what OK does may ask for another text - the password after the user name.)
    m_keyboard = false;
    auto done = std::move(m_ask_done);
    m_ask_done = nullptr;
    std::string text = m_board.Take();
    Play("confirm");
    if (done)
      done(text);
    std::fill(text.begin(), text.end(), '\0');
    break;
  }
  case Result::Cancel:
    m_keyboard = false;
    if (!search)
    {
      m_ask_done = nullptr;
      m_board.Take();  // (nothing of it kept: a password)
    }
    Play("back");
    break;
  default:
    break;
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
    m_prefs.Save(UserDir() + "/xmb.ini");
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
  std::ifstream file(UserDir() + "/names.ini");
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
  WriteTextFile(UserDir() + "/names.ini", text);
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
  m_ratings.erase(key);  // (its rating by its new name: StartRatings)
  m_rating_asked.erase(key);
  // (00.85) The name on the game now, without reading every folder again (nothing changed on the drive): the games by
  // title again, the stars of its new name, the marks of the games' new order.
  const std::array<std::string, CATEGORY_COUNT> selected = SelectedPaths();
  const std::string online = m_online_game >= 0 && m_online_game < static_cast<int>(m_games.size()) ?
                                 m_games[static_cast<size_t>(m_online_game)].rom.path :
                                 std::string();
  ApplyNames();
  StartRatings();
  StartAchievementMarks();
  GamesChanged(selected, online);
  ShowToast(name.empty() || name == own ? "Its own name again" : "Renamed", 0);  // (the keyboard's OK: its sound)
}

void Menu::Impl::AskText(const std::string& title, const std::string& text,
                         std::function<void(const std::string&)> done, uint32_t max_length, TextKeyboard::Mode mode,
                         const std::string& hint)
{
  // (00.80: always the app's keyboard - with the menu's buttons. The PS4's, until 00.79, chose its keys with the
  // console's Enter button, Circle on some consoles, and can't be told otherwise.)
  m_ask_done = std::move(done);
  m_ask_title = title;
  m_ask_hint = hint;
  m_board.Open(mode == TextKeyboard::Mode::Search ? TextKeyboard::Mode::Text : mode, text, max_length);
  m_keyboard = true;
  Play("menu");
}

// ---------------------------------------------------------------------------------------------
// RetroAchievements (00.73, src/Achievements.h): Settings -> RetroAchievements.

std::string Menu::Impl::AchievementsDetail() const
{
  // (00.75: the session's state, short.)
  const Achievements::Status status = Achievements::GetStatus();
  switch (status.session)
  {
  case Achievements::Session::Connected:
    return "Connected as " + status.user;
  case Achievements::Session::Connecting:
    return "Connecting...";
  case Achievements::Session::Offline:
    return "No connection (trying again)";
  case Achievements::Session::Failed:
    return "The login failed";
  default:
    return "Not logged in";
  }
}

void Menu::Impl::OpenVoicePanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_VOICE;
  m_panel.title = "Voice Chat";
  BuildVoiceRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// Settings -> Voice Chat (00.23): Voice's rows; its notes (Test Microphone's progress and result) over header lines of
// the panel's width, translated here; the level meter's bar as it is. The selection stays on its row.
void Menu::Impl::BuildVoiceRows()
{
  const int at = m_panel.selected;
  const std::string selected =
      at >= 0 && at < static_cast<int>(m_panel.rows.size()) ? m_panel.rows[at].key : std::string();
  std::vector<SettingRow> rows;
  for (const Voice::Row& row : Voice::PanelRows())
  {
    if (!row.key.empty())
    {
      SettingRow setting{row.label, row.value, row.key};
      setting.help = row.help;
      rows.push_back(std::move(setting));
    }
    else if (!row.value.empty() || row.label.empty())
    {
      rows.push_back({row.label, row.value, "", true});  // (the meter: its bar and its level; or a gap)
    }
    else
    {
      const std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(row.label), 880.0f - 120.0f);
      for (size_t start = 0; start <= wrapped.size();)
      {
        const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
        rows.push_back({wrapped.substr(start, end - start), "", "", true});
        start = end + 1;
      }
    }
  }
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!selected.empty() && m_panel.rows[i].key == selected)
      m_panel.selected = static_cast<int>(i);
  }
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

void Menu::Impl::OpenAchievementsPanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ACHIEVEMENTS;
  m_panel.title = "RetroAchievements";
  m_ra_login = Achievements::GetAccount().state;
  BuildAchievementsRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildAchievementsRows()
{
  const Achievements::Account account = Achievements::GetAccount();
  std::vector<SettingRow> rows;
  // A note over header lines of the panel's width (the text translated here).
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  // The session's state first (00.75: connected or not, live - UpdateAchievements).
  const Achievements::Status status = Achievements::GetStatus();
  m_ra_status = Achievements::StatusLine(status);
  note(m_ra_status);
  rows.push_back({"What is RetroAchievements?", "", "ra:about"});  // (00.79: how to start, what it is)
  if (account.state == Achievements::Login::In)
  {
    rows.push_back({"Points", std::to_string(status.points + status.casual_points), "ra:points"});
  }
  else if (account.state != Achievements::Login::Working)
  {
    rows.push_back({"Log In", "", "ra:login"});
  }
  rows.push_back({"My Achievements", "", "ra:history"});
  // (00.76) The achievements' texts in the app's language: not for English, which RetroAchievements has.
  if (PS4Lang::Current().code != std::string("en"))
    rows.push_back({"Translate Achievements", Achievements::TranslateSetting() ? "On" : "Off", "ra:translate"});
  rows.push_back({"Marks on the Games", m_ra_marks ? "On" : "Off", "ra:marks"});  // (00.77)
  if (account.state == Achievements::Login::In || account.saved_token)
    rows.push_back({"Log Out", "", "ra:logout"});
  const int selected = m_panel.selected;
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(selected, 0, std::max(0, static_cast<int>(m_panel.rows.size()) - 1));
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

// What a game's achievements page is made from (made again when it changes): its state, its counts, the translations
// that came and how they go (00.79).
static std::string GamePageKey(const Achievements::GameDetail& d)
{
  return std::to_string(static_cast<int>(d.state)) + "|" + std::to_string(d.entries.size()) + "|" +
         std::to_string(d.unlocked) + "|" + d.error + "|" + std::to_string(Achievements::TranslationsVersion()) + "|" +
         Achievements::TranslationLine(Achievements::GetTranslationState(d.title));
}

// Every frame: rc_client's waiting work (an unlock retried, the session's pings), the password's keyboard after the
// user name's, and the login's progress (the panel and the Settings row follow it).
void Menu::Impl::UpdateAchievements(float dt)
{
  Achievements::Idle();
  // (00.77) The games' marks as the module's thread finds them: once a second at most, when something changed.
  m_ra_lib_time -= dt;
  if (m_ra_marks && m_ra_lib_time <= 0.0f)
  {
    m_ra_lib_time = 1.0f;
    RefreshAchievementMarks();
    if (m_ra_counts_due && m_time - m_ra_rebuilt >= 3.0f && RaFilterOn())
    {
      m_ra_counts_due = false;  // (00.85: the filter's counts that waited)
      m_ra_rebuilt = m_time;
      if (m_panel.open || m_keyboard)
        m_ra_rebuild = true;
      else
        RebuildGameLists(true);
    }
  }
  // (00.80) The filter's lists made again once the panel or the keyboard that held them back is gone.
  if (m_ra_rebuild && !m_panel.open && !m_keyboard)
  {
    m_ra_rebuild = false;
    if (RaFilterOn())
      RebuildGameLists(true);
  }
  if (m_ra_ask_password > 0 && !m_keyboard && --m_ra_ask_password == 0)
  {
    const std::string user = m_ra_user;
    AskText(
        "RetroAchievements: Password", "",
        [this, user](const std::string& password) {
          if (password.empty())
            return;
          Achievements::LogIn(user, password);
          m_ra_login = Achievements::Login::Working;
          BuildItems();
          if (m_panel.open && m_panel.setting == SET_ACHIEVEMENTS)
            BuildAchievementsRows();
        },
        128, TextKeyboard::Mode::Password, "Sent once to log in: EmuSuNi keeps only the key the site gives back.");
  }
  m_ra_refresh -= dt;
  if (m_ra_refresh > 0.0f)
    return;
  m_ra_refresh = 0.25f;
  const bool shown = m_panel.open && m_panel.setting == SET_ACHIEVEMENTS;
  const bool history = m_panel.open && m_panel.setting == SET_RA_HISTORY;
  const bool game_page = m_panel.open && m_panel.setting == SET_RA_GAME;
  // (00.75) While a RetroAchievements panel shows: the connection checked now and then (the module: once a minute at
  // most), and the panel made again when the session's state or the history changes (00.76: or translations came).
  if (shown || history || game_page)
    Achievements::CheckConnection();
  if (history)
  {
    const Achievements::History h = Achievements::GetHistory();
    const std::string key = Achievements::StatusLine(Achievements::GetStatus()) + "|" + (h.syncing ? "1" : "0") + "|" +
                            std::to_string(h.synced) + "|" + h.sync_error + "|" + std::to_string(h.unlocks.size()) +
                            "|" + std::to_string(h.games.size()) + "|" +
                            std::to_string(Achievements::TranslationsVersion());
    if (key != m_ra_history_shown)
      BuildHistoryRows();
  }
  if (game_page)
  {
    if (GamePageKey(Achievements::GetGameDetail()) != m_ra_game_shown)
      BuildGameAchievementRows();
    // (00.79) The achievements on screen translated first: the selected one, then those around it.
    std::vector<uint32_t> ids;
    auto add = [&](int i) {
      if (i >= 0 && i < static_cast<int>(m_panel.rows.size()) && m_panel.rows[i].key.rfind("ra:ach:", 0) == 0)
        ids.push_back(static_cast<uint32_t>(std::strtoul(m_panel.rows[i].key.c_str() + 7, nullptr, 10)));
    };
    add(m_panel.selected);
    for (int d = 1; d <= 10; d++)
    {
      add(m_panel.selected + d);
      if (d <= 5)
        add(m_panel.selected - d);
    }
    Achievements::TranslateFirst(ids);
  }
  if (m_panel.open && m_panel.setting == SET_RA_VERSIONS)
  {
    const Achievements::Versions v = Achievements::GetVersions();
    if (v.state == Achievements::Versions::State::Ready && v.file_state == Achievements::LibraryInfo::State::Known &&
        v.game_id != 0)
    {
      // (00.80) The file checked now has achievements: the game's page (Circle goes back to its options).
      OpenGameAchievements(v.game_id);
      m_ra_from_game = true;
    }
    else if (std::to_string(static_cast<int>(v.state)) + "|" + std::to_string(v.names.size()) != m_ra_versions_shown)
    {
      BuildVersionsRows();
    }
  }
  const Achievements::Login now = Achievements::GetAccount().state;
  const std::string status = Achievements::StatusLine(Achievements::GetStatus());
  if (now == m_ra_login && status == m_ra_status)
    return;
  const Achievements::Login before = m_ra_login;
  m_ra_login = now;
  m_ra_status = status;
  // (The Settings row's short state: the items made again only when it changes - not each second of a countdown.)
  static std::string detail_shown;
  if (const std::string detail = AchievementsDetail(); detail != detail_shown)
  {
    detail_shown = detail;
    BuildItems();
  }
  if (shown)
    BuildAchievementsRows();
  if (before == Achievements::Login::Working && now == Achievements::Login::In && shown)
  {
    ShowToast("Logged in to RetroAchievements", 0);
    Play("confirm");
  }
  else if (before == Achievements::Login::Working && now == Achievements::Login::Failed && shown)
  {
    Play("error");
  }
}

// An achievement's date in the app's language (00.76): "Oct 23, 2026  14:05" in English, "23/10/2026  14:05" else (the
// months' names are English ones).
static std::string AchievementDate(int64_t when)
{
  if (when <= 0)
    return "";
  if (PS4Lang::Current().code == std::string("en"))
    return DateText(when);
  const std::time_t then = static_cast<std::time_t>(when);
  std::tm day = {};
  localtime_r(&then, &day);
  char text[48];
  std::strftime(text, sizeof(text), "%d/%m/%Y  %H:%M", &day);
  return text;
}

// Settings -> RetroAchievements -> My Achievements (00.75): the achievements unlocked in EmuSuNi (kept on the PS4:
// shown offline too), the newest first, then the games - those played in EmuSuNi, the last one first, then the
// account's others (retroachievements.org, updated when older than 10 minutes) - each with its picture.
void Menu::Impl::OpenHistoryPanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_RA_HISTORY;
  m_panel.title = "My Achievements";
  Achievements::UpdateHistory(false);
  BuildHistoryRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildHistoryRows()
{
  const Achievements::Status status = Achievements::GetStatus();
  const Achievements::History history = Achievements::GetHistory();
  std::vector<SettingRow> rows;
  // (A note over header lines of the panel's width, translated here, as Settings -> RetroAchievements' own.)
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  const std::string status_line = Achievements::StatusLine(status);
  note(status_line);
  if (history.syncing)
    note("Updating your account's games from retroachievements.org...");
  else if (!history.sync_error.empty())
    note("Your account's games couldn't be updated: " + history.sync_error);
  else if (history.synced > 0)
    note("Your account's games: updated " + AchievementDate(history.synced));
  else
    note("Your account's games come from retroachievements.org once connected");
  if (status.session == Achievements::Session::Connected && !history.syncing)
    rows.push_back({"Update Now", "", "ra:sync"});
  // The achievements unlocked in EmuSuNi.
  rows.push_back({"Recently Unlocked", std::to_string(history.unlocks.size()), "", true});
  if (history.unlocks.empty())
    note("None unlocked in EmuSuNi yet");
  for (const Achievements::HistoryUnlock& unlock : history.unlocks)
  {
    SettingRow row;
    row.label = Achievements::Translated(unlock.title);  // (the app's language once translated, 00.76)
    row.value = unlock.points == 1 ? std::string("1 point") : std::to_string(unlock.points) + " points";
    row.key = "ra:unlock";
    row.picture = unlock.badge;
    row.help = unlock.game + "\n" + PS4Lang::T("Unlocked " + AchievementDate(unlock.time)) +
               (unlock.description.empty() ? std::string() : "\n" + Achievements::Translated(unlock.description));
    rows.push_back(std::move(row));
  }
  // The games: unlocked of all, in EmuSuNi or anywhere on the account.
  rows.push_back({"Games", std::to_string(history.games.size()), "", true});
  if (history.games.empty())
    note("No games yet");
  for (const Achievements::HistoryGame& game : history.games)
  {
    SettingRow row;
    row.label = game.title.empty() ? "#" + std::to_string(game.game_id) : game.title;
    row.value = std::to_string(game.unlocked) + " / " + std::to_string(game.total);
    row.key = "ra:game:" + std::to_string(game.game_id);  // (Cross: all its achievements, 00.76)
    row.picture = game.badge;
    row.help = Achievements::ConsoleName(game.console) + "\n" +
               PS4Lang::T(std::to_string(game.unlocked) + " of " + std::to_string(game.total) + " achievements") + "\n" +
               (game.played > 0 ? PS4Lang::T("Played in EmuSuNi " + AchievementDate(game.played)) :
                                  PS4Lang::T(std::string("On your retroachievements.org account"))) +
               "\n" + PS4Lang::T(std::string("Cross: all its achievements"));
    rows.push_back(std::move(row));
  }
  m_ra_history_shown = status_line + "|" + (history.syncing ? "1" : "0") + "|" + std::to_string(history.synced) + "|" +
                       history.sync_error + "|" + std::to_string(history.unlocks.size()) + "|" +
                       std::to_string(history.games.size()) + "|" + std::to_string(Achievements::TranslationsVersion());
  const int selected = m_panel.selected;
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(selected, 0, std::max(0, static_cast<int>(m_panel.rows.size()) - 1));
  if (m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

// My Achievements -> a game (00.76): every achievement of it, locked (its grey picture) then unlocked, as the in-game
// page shows them - points, the progress of the last session in EmuSuNi, when unlocked, rarity - from the PS4's copy at
// once, then from retroachievements.org.
void Menu::Impl::OpenGameAchievements(uint32_t game_id)
{
  m_ra_game = game_id;
  m_ra_from_game = false;  // (a game's Triangle -> Achievements sets it after)
  Achievements::OpenGame(game_id);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_RA_GAME;
  m_panel.title = Achievements::GetGameDetail().title;
  m_ra_game_shown.clear();
  BuildGameAchievementRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildGameAchievementRows()
{
  const Achievements::GameDetail detail = Achievements::GetGameDetail();
  std::vector<SettingRow> rows;
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  if (!detail.title.empty())
    m_panel.title = detail.title;
  using State = Achievements::GameDetail::State;
  if (!detail.entries.empty())
  {
    note(std::to_string(detail.unlocked) + " of " + std::to_string(detail.entries.size()) + " achievements  -  " +
         std::to_string(detail.points) + " of " + std::to_string(detail.total_points) + " points");
    // (00.79) The texts still in English, said: being translated, or waiting for the next day.
    if (const std::string line = Achievements::TranslationLine(Achievements::GetTranslationState(detail.title));
        !line.empty())
      note(line);
  }
  if (detail.state == State::Loading)
    note(detail.entries.empty() ? std::string("Asking retroachievements.org for this game's achievements...") :
                                  std::string("Updating from retroachievements.org..."));
  else if (detail.state == State::Offline || detail.state == State::Failed)
  {
    if (detail.entries.empty() && detail.error == "Not logged in")
      note("Not logged in: its achievements show here once you log in (Settings -> RetroAchievements -> Log In)");
    else if (detail.entries.empty())
      note(detail.error.empty() ? std::string("No connection to RetroAchievements") : detail.error);
    else
      note("As last seen on this PS4: " + AchievementDate(detail.updated));
  }
  std::string group;
  for (const Achievements::Entry& entry : detail.entries)
  {
    if (entry.group != group)
    {
      group = entry.group;
      rows.push_back({group, "", "", true});  // ("Locked", "Unlocked": translated as drawn)
    }
    SettingRow row;
    row.label = entry.title;  // (translated by the module once its translation came)
    row.value = entry.points == 1 ? std::string("1 point") : std::to_string(entry.points) + " points";
    row.key = "ra:ach:" + std::to_string(entry.id);  // (its id: those on screen translated first, 00.79)
    row.picture = entry.badge;  // (the grey one while locked)
    row.help = entry.description;
    if (!entry.unlocked && !entry.progress.empty())
      row.help += "\n" + PS4Lang::T("Progress: " + entry.progress);
    if (entry.unlocked)
      row.help += "\n" + (entry.unlock_time > 0 ? PS4Lang::T("Unlocked " + AchievementDate(entry.unlock_time)) :
                                                  PS4Lang::T(std::string("Unlocked on your account")));
    if (entry.rarity > 0.0f)
    {
      char rarity[32];
      std::snprintf(rarity, sizeof(rarity), "%.1f", entry.rarity);
      row.help += "\n" + PS4Lang::T(std::string(rarity) + "% of the players have it");
    }
    rows.push_back(std::move(row));
  }
  m_ra_game_shown = GamePageKey(detail);
  const int selected = m_panel.selected;
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(selected, 0, std::max(0, static_cast<int>(m_panel.rows.size()) - 1));
  if (!m_panel.rows.empty() && m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

// A game's Triangle -> Achievements on a file RetroAchievements doesn't know (00.79): its game of that name (or of that
// other version), the files it supports for it - named after No-Intro's list where it has them -, the player's own.
void Menu::Impl::OpenVersionsPanel(const Game& game)
{
  Achievements::OpenVersions({game.rom.key, game.rom.discs.empty() ? game.rom.path : game.rom.discs.front(),
                              game.rom.system},
                             game.rom.title);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_RA_VERSIONS;
  m_panel.title = "Achievements";
  m_ra_versions_shown.clear();
  BuildVersionsRows();
  m_panel.selected = 0;
  MoveSettingRow(0);
}

void Menu::Impl::BuildVersionsRows()
{
  const Achievements::Versions v = Achievements::GetVersions();
  std::vector<SettingRow> rows;
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  // (A file's name: a row of its own, selectable - a long list scrolls with the selection.)
  auto file = [&](const std::string& name, const char* key) {
    SettingRow row;
    row.label = name;
    row.key = key;
    rows.push_back(std::move(row));
  };
  using FileState = Achievements::LibraryInfo::State;
  if (v.state != Achievements::Versions::State::Ready)
  {
    // (00.80) The file checked now: hashed, the console's list, its state.
    note("Checking this file: \"" + v.file_name + "\"...");
  }
  else if (v.file_state == FileState::Unchecked)
  {
    note("RetroAchievements' list of this console didn't come (no connection?): try again later.");
  }
  else if (v.file_state == FileState::NoAchievements)
  {
    note("RetroAchievements knows this game, \"" + v.title + "\", but it has no achievements for it yet.");
  }
  else if (v.file_state == FileState::Known)
  {
    note("Checking this file: \"" + v.file_name + "\"...");  // (its achievements' page comes next)
  }
  else
  {
    // A file it doesn't take: its game (by this other version, else by the name), the files it does take.
    if (v.other_version)
      note("RetroAchievements knows this file, \"" + v.file_name + "\", as a version of \"" + v.title +
           "\" whose achievements don't work with it.");
    else
      note("RetroAchievements doesn't know this file: \"" + v.file_name + "\".");
    if (v.game_id == 0)
    {
      note("It has no achievements for a game of that name on this console.");
    }
    else
    {
      note("\"" + v.title + "\" has " + std::to_string(v.achievements) + " achievements with these files:");
      for (const std::string& name : v.names)
        file(name, "ra:file");
      if (v.unnamed > 0)
      {
        note(v.names.empty() ? std::to_string(v.unnamed) + " files; their names aren't known here: "
                                                            "retroachievements.org lists them (the game's page -> "
                                                            "Supported Game Files)." :
                               "And " + std::to_string(v.unnamed) + " more, whose names aren't known here "
                                                                      "(retroachievements.org: the game's page -> "
                                                                      "Supported Game Files).");
      }
      if (!v.yours.empty())
      {
        note("In your games already (they have the \"RA\" mark):");
        for (const std::string& name : v.yours)
          file(name, "ra:yours");
      }
      note("A file of one of those (the same dump: a translation or a hack isn't) gets the \"RA\" mark once it is "
           "with your games.");
    }
  }
  m_ra_versions_shown = std::to_string(static_cast<int>(v.state)) + "|" + std::to_string(v.names.size());
  const int selected = m_panel.selected;
  m_panel.rows = std::move(rows);
  m_panel.selected = std::clamp(selected, 0, std::max(0, static_cast<int>(m_panel.rows.size()) - 1));
  if (!m_panel.rows.empty() && m_panel.rows[m_panel.selected].header)
    MoveSettingRow(1);
}

// Settings -> RetroAchievements -> What is RetroAchievements? (00.79): each subject a row, its words under it.
void Menu::Impl::OpenAboutPanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_RA_ABOUT;
  m_panel.title = "What is RetroAchievements?";
  std::vector<SettingRow> rows;
  auto note = [&](const std::string& english) {
    std::string wrapped = WrapText(m_bold, 26, PS4Lang::T(english), 880.0f - 120.0f);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      rows.push_back({wrapped.substr(start, end - start), "", "", true});
      start = end + 1;
    }
  };
  auto subject = [&](const char* title, const std::string& text) {
    SettingRow row;
    row.label = title;
    row.key = "ra:about:subject";
    rows.push_back(std::move(row));
    note(text);
  };
  // The consoles it has achievements for: the app's, in the order of its own list.
  std::string consoles;
  for (const System& system : Systems())
  {
    const uint32_t console = Achievements::ConsoleId(system.id);
    if (console == 0 || system.id == SystemId::THREEDS)
      continue;
    consoles += (consoles.empty() ? "" : ", ") + std::string(system.name);
  }
  subject("What it is",
          "RetroAchievements (retroachievements.org) adds achievements to classic games: goals inside each game - beat "
          "a boss, find a secret, finish a level fast - unlocked as you play, each with its points. It's free, made by "
          "its players.");
  subject("1. A free account",
          "Make one at retroachievements.org, on a phone or a computer, and confirm the e-mail it sends you: until then "
          "it refuses the login.");
  subject("2. Log in here",
          "Settings -> RetroAchievements -> Log In: the keyboard asks for your user name, then your password. "
          "EmuSuNi keeps only the key the site gives back (never the password) and logs in again by itself at each "
          "start.");
  subject("3. Play",
          "A game with achievements says so when it starts; each unlock shows a picture. The in-game menu's "
          "Achievements lists them with your progress; My Achievements keeps what you unlocked; Triangle -> "
          "Achievements on a game shows them all.");
  subject("Casual and points",
          "EmuSuNi plays in RetroAchievements' Casual mode: save states, cheats and any speed are allowed, and the "
          "points go to your account. (Its Hardcore mode waits until RetroAchievements approves EmuSuNi.)");
  subject("The consoles", "With achievements: " + consoles + ". Not yet: Nintendo 3DS.");
  subject("Which files",
          "RetroAchievements knows certain files of each game - most often the usual dumps of its American, European "
          "or Japanese version (No-Intro's for cartridges, Redump's for discs); not translations or hacks. The \"RA\" "
          "mark on a cover says the file is known; Triangle -> Achievements on a game without it shows the files it "
          "knows.");
  subject("Only the games with achievements",
          "On the games, the touchpad shows only those whose file has achievements, then those with achievements "
          "begun and not finished, then every game again (with Marks on the Games on).");
  subject("Without a connection",
          "The game plays as always. Unlocks made offline wait and are sent when the connection comes back, while "
          "EmuSuNi stays open; My Achievements shows what this PS4 kept.");
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  MoveSettingRow(0);
}

// RetroAchievements' marks on the games (00.77): the games to the module (it hashes them on its own thread, once per
// file, waiting while a game runs), their states back.
void Menu::Impl::StartAchievementMarks()
{
  m_ra_marks = Achievements::MarksSetting();
  m_ra_lib_version = -1;
  if (!m_ra_marks)
    return;  // (hidden: nothing hashed either)
  std::vector<Achievements::LibraryFile> files;
  files.reserve(m_games.size());
  for (const Game& game : m_games)
    files.push_back({game.rom.key, game.rom.discs.empty() ? game.rom.path : game.rom.discs.front(), game.rom.system});
  Achievements::SetLibrary(files);
  RefreshAchievementMarks();
}

void Menu::Impl::RefreshAchievementMarks()
{
  const int version = Achievements::LibraryVersion();
  if (version == m_ra_lib_version)
    return;
  m_ra_lib_version = version;
  // (00.85) Only the games whose state may have changed (the files hashed since; every game after a new list, a
  // console's list, the account's progress): before, every game's each second while the files were hashed. Each by
  // its place in the list given to SetLibrary (StartAchievementMarks: m_games' order; its key checks it).
  bool all = false;
  const std::vector<Achievements::LibraryChange> changes = Achievements::TakeLibraryChanges(&all);
  bool changed = false, moved = false;  // moved: a game into or out of the filter's lists
  for (const Achievements::LibraryChange& change : changes)
  {
    if (change.index >= m_games.size() || m_games[change.index].rom.key != change.key)
      continue;
    Game& game = m_games[change.index];
    const Achievements::LibraryInfo& info = change.info;
    const bool differs = info.state != game.ra.state || info.progress != game.ra.progress ||
                         info.unlocked != game.ra.unlocked || info.achievements != game.ra.achievements;
    const bool passed = differs && RaFilterPasses(game);
    game.ra = info;
    changed = changed || differs;
    moved = moved || (differs && passed != RaFilterPasses(game));
  }
  // (00.80) The RetroAchievements filter follows the games as they're checked (each list keeps its selected game) -
  // not under an open panel or the keyboard, whose game must stay where it is: after them (UpdateAchievements).
  // (00.85: a game into or out of the lists at once; the count alone - "checking 1200 of 3585..." - every 3 s at most:
  // each time is every list made again.)
  if (changed && RaFilterOn())
  {
    if (!moved && m_time - m_ra_rebuilt < 3.0f)
    {
      m_ra_counts_due = true;
      return;
    }
    m_ra_counts_due = false;
    m_ra_rebuilt = m_time;
    if (m_panel.open || m_keyboard)
      m_ra_rebuild = true;
    else
      RebuildGameLists(true);
  }
}

// (00.80) The RetroAchievements filter: a game whose file has achievements; in its second step, begun and not all
// unlocked.
bool Menu::Impl::RaFilterPasses(const Game& game) const
{
  const Achievements::LibraryInfo& info = game.ra;
  if (info.state != Achievements::LibraryInfo::State::Known)
    return false;
  return m_prefs.ra_filter != "progress" ||
         (info.progress && info.unlocked > 0 && info.unlocked < info.achievements);
}

// ---- (01.18) Order and Filters: Options in a list of games ----

bool Menu::Impl::ViewPasses(const Game& game) const
{
  const Ratings::Rating& r = game.rating;
  if (m_filter_genre >= 0 && (r.genres & (1u << m_filter_genre)) == 0)
    return false;
  switch (m_filter_players)
  {
  case 1:
    return r.players == 1;
  case 2:
    return r.players >= 2;
  case 5:
    return r.coop;
  default:
    return true;
  }
}

std::string Menu::Impl::PlayersName(int players)
{
  switch (players)
  {
  case 1:
    return "1 Player Only";
  case 2:
    return "2 Players or More";
  case 5:
    return "Cooperative";
  default:
    return "Any Number";
  }
}

std::string Menu::Impl::ViewFilterLine(int c) const
{
  std::string line;
  if (m_filter_genre >= 0)
    line = std::string("Genre: ") + Ratings::GenreName(m_filter_genre);
  if (m_filter_players > 0)
    line += (line.empty() ? "" : "  -  ") + PlayersName(m_filter_players);
  const int hidden = c >= 0 && c < CATEGORY_COUNT ? m_view_hidden[c] : 0;
  return line + "  -  " + std::to_string(hidden) + " hidden  -  Options: change";
}

void Menu::Impl::OpenViewPanel()
{
  m_view_category = m_category;
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_LIST_VIEW;
  m_panel.title = "Order and Filters";
  BuildViewRows();
  m_panel.selected = 0;
  Play("menu");
}

// Its rows: the order, the genre (among the list's games' genres), the players, Hide Adult Games - each one's help
// says how many of the list's games are known (their genres and players come with the star ratings' data).
void Menu::Impl::BuildViewRows()
{
  const int c = m_view_category;
  const bool every = c == static_cast<int>(Category::Games), favorites = c == static_cast<int>(Category::Favorites);
  int counts[Ratings::GENRE_COUNT] = {};
  int total = 0, with_genres = 0, adult = 0;
  m_view_players_known = 0;
  for (const Game& game : m_games)
  {
    if (!every && (favorites ? m_favorites.count(game.rom.key) == 0 : ListOfSystem(game.rom.system) != c))
      continue;
    total++;
    with_genres += game.rating.genres != 0 ? 1 : 0;
    m_view_players_known += game.rating.players != 0 || game.rating.coop ? 1 : 0;
    adult += game.rating.adult ? 1 : 0;
    for (int g = 0; g < Ratings::GENRE_COUNT; g++)
      counts[g] += (game.rating.genres >> g) & 1u;
  }
  m_view_genres.clear();
  for (int g = 0; g < Ratings::GENRE_COUNT; g++)
  {
    if (counts[g] > 0 || g == m_filter_genre)
      m_view_genres.push_back(g);
  }
  // (Without LaunchBox's genres and players yet - none downloaded, or a copy before 01.18 being downloaded again: until
  // then only the arcade's genres are known, and no game's players (01.18's first test: every game hidden) - said so.
  // A line each sentence: each one translated by itself.)
  const std::string coming =
      m_rating_copy.meta ? std::string() :
      m_rating_download.Busy() ? std::string("\nThe star ratings' data is downloading: the others come with it.") :
                                 std::string("\nThe others come with the star ratings' data (Settings: Star Ratings).");
  const std::string genres_line =
      std::to_string(with_genres) + " of " + std::to_string(total) + " games here have their genre known." + coming;
  const std::string players_line = std::to_string(m_view_players_known) + " of " + std::to_string(total) +
                                   " games here have their players known." + coming;
  const int was = m_panel.selected;
  m_panel.rows.clear();
  const auto add = [this](const std::string& label, const std::string& value, const std::string& key,
                          const std::string& help) {
    SettingRow row{label, value, key};
    row.help = help;
    m_panel.rows.push_back(std::move(row));
  };
  add("Order", SortName(), "view:sort",
      "The games' order: A-Z, Recently Played, Region, Console, or only those Without Cover (this session).");
  add("Genre", m_filter_genre >= 0 ? Ratings::GenreName(m_filter_genre) : "Every Genre", "view:genre",
      std::string("Left / Right: the genres of this list's games.") +
          (m_filter_genre >= 0 ? "\n" + std::to_string(counts[m_filter_genre]) + " games of this genre here." :
                                 std::string()) +
          "\n" + genres_line);
  add("Players", PlayersName(m_filter_players), "view:players",
      "1 Player Only, 2 Players or More (at once), or Cooperative play.\n" + players_line);
  add("Hide Adult Games", m_prefs.hide_adult ? "On" : "Off", "view:adult",
      adult > 0 ? "Arcade games for adults (their category says so) left out of every list.\n" + std::to_string(adult) +
                      " of them here." :
                  std::string("Arcade games for adults (their category says so) left out of every list."));
  m_panel.selected = std::clamp(was, 0, static_cast<int>(m_panel.rows.size()) - 1);
}

void Menu::Impl::ChangeViewRow(int direction)
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.rows.size()))
    return;
  const std::string key = m_panel.rows[static_cast<size_t>(m_panel.selected)].key;
  if (key == "view:sort")
  {
    const int n = static_cast<int>(std::size(SORT_NAMES));
    int sort = 0;
    while (sort < n && m_prefs.sort != SORT_NAMES[sort][0])
      sort++;
    m_prefs.sort = SORT_NAMES[((sort + direction) % n + n) % n][0];
    m_prefs.Save(UserDir() + "/xmb.ini");
  }
  else if (key == "view:genre")
  {
    // (Every Genre, then the list's genres, round.)
    std::vector<int> choices = {-1};
    choices.insert(choices.end(), m_view_genres.begin(), m_view_genres.end());
    const int n = static_cast<int>(choices.size());
    int at = static_cast<int>(std::find(choices.begin(), choices.end(), m_filter_genre) - choices.begin());
    m_filter_genre = choices[static_cast<size_t>((((at < n ? at : 0) + direction) % n + n) % n)];
  }
  else if (key == "view:players")
  {
    // ((01.23, the owner: "basta con que salga 2 o más") Any Number, 1 Player Only, 2 Players or More, Cooperative -
    // until 01.22 3 and 4 Players or More too.)
    static constexpr int CHOICES[] = {0, 1, 2, 5};
    constexpr int n = static_cast<int>(std::size(CHOICES));
    const int at = static_cast<int>(std::find(std::begin(CHOICES), std::end(CHOICES), m_filter_players) -
                                    std::begin(CHOICES));
    m_filter_players = CHOICES[(((at < n ? at : 0) + direction) % n + n) % n];
    // ((01.18's first test) No game of the list with its players known yet - the star ratings' data not there, or
    // being downloaded again -: every game is left out until it comes; said, not only in the row's help.)
    if (m_filter_players > 0 && m_view_players_known == 0)
      ShowToast(m_rating_download.Busy() ? "The players aren't known yet: they come with the star ratings' data, "
                                           "downloading now" :
                                           "The players aren't known yet: they come with the star ratings' data "
                                           "(Settings: Star Ratings)",
                0);
  }
  else if (key == "view:adult")
  {
    m_prefs.hide_adult = !m_prefs.hide_adult;
    m_prefs.Save(UserDir() + "/xmb.ini");
  }
  Log("menu: order and filters: " + std::string(m_prefs.sort) + ", genre " +
      (m_filter_genre >= 0 ? Ratings::GenreName(m_filter_genre) : "every one") + ", players " +
      std::to_string(m_filter_players) + ", adults " + (m_prefs.hide_adult ? "hidden" : "shown"));
  ViewChanged();
  BuildViewRows();
}

// The lists made again after a change: the selected game stays selected (Without Cover from a game with its cover:
// the next one without, A-Z), the list's line says the order and the filters a moment.
void Menu::Impl::ViewChanged(int list)
{
  const int c = list >= 0 && list < CATEGORY_COUNT                       ? list :
                m_view_category >= 0 && m_view_category < CATEGORY_COUNT ? m_view_category :
                                                                           m_category;
  const auto& before = m_items[c];
  const int at = m_selected[c];
  const int game = at >= 0 && at < static_cast<int>(before.size()) ? before[static_cast<size_t>(at)].game : -1;
  BuildItems();
  const auto& sorted = m_items[c];
  bool found = false;
  for (size_t i = 0; i < sorted.size(); i++)
  {
    if (game >= 0 && sorted[i].game == game)
    {
      m_selected[c] = static_cast<int>(i);
      found = true;
    }
  }
  if (!found && game >= 0 && m_prefs.sort == "nocover")
  {
    int next = 0;
    while (next + 1 < static_cast<int>(sorted.size()) && sorted[next].game >= 0 && sorted[next].game < game)
      next++;
    m_selected[c] = next;
  }
  m_selected[c] = std::clamp(m_selected[c], 0, std::max(0, static_cast<int>(sorted.size()) - 1));
  m_item_pos[c] = static_cast<float>(m_selected[c]);
  m_sort_shown = 2.5f;
}

std::string Menu::Impl::RaFilterHiddenLine(int c) const
{
  const int shown = m_ra_emptied[c] ? 0 : static_cast<int>(m_items[c].size());
  const int hidden = std::max(0, m_ra_count[c].total - shown);
  return std::string(m_prefs.ra_filter == "progress" ? "Only games with achievements in progress" :
                                                       "Only games with achievements") +
         "  -  " + std::to_string(hidden) + " hidden  -  Touchpad: show every game";
}

// The filter's line under a list's title: its count, and the check still going on ("checking 1200 of 3585...").
std::string Menu::Impl::RaFilterLine(int c) const
{
  if (!RaFilterOn() || c < 0 || c >= CATEGORY_COUNT)
    return {};
  const RaCount& n = m_ra_count[c];
  const int shown = m_ra_emptied[c] ? 0 : static_cast<int>(m_items[c].size());
  std::string line = (m_prefs.ra_filter == "progress" ? "Achievements in progress: " : "With achievements: ") +
                     std::to_string(shown) + " of " + std::to_string(n.total);
  if (n.unchecked > 0)
    line += "  -  checking " + std::to_string(n.files - n.unchecked) + " of " + std::to_string(n.files) + "...";
  return line;
}

std::string Menu::Impl::AchievementLine(const Game& game) const
{
  using State = Achievements::LibraryInfo::State;
  const Achievements::LibraryInfo& info = game.ra;
  switch (info.state)
  {
  case State::Unchecked:
    return "RetroAchievements: not checked yet";
  case State::NotKnown:
    return "RetroAchievements: this file isn't known";
  case State::NoAchievements:
    return "RetroAchievements: no achievements for this game yet";
  case State::OtherVersion:
    return "RetroAchievements: another version of this game has achievements";
  case State::Known:
    break;
  }
  const std::string count = std::to_string(info.achievements);
  if (!info.progress)
    return "RetroAchievements: " + count + " achievements";
  if (info.unlocked >= info.achievements)
    return "RetroAchievements: " + count + " achievements (completed)";
  return "RetroAchievements: " + count + " achievements (" + std::to_string(info.unlocked) + " unlocked)";
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
                                              "libretro's list can't be read: " + Net::Plain(0, result.error);
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
      CoverSaved(Lower(result.key), result.path);
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
                               UserDir() + "/covers/.preview" + std::to_string(++m_preview_serial) + ".png"});
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
  const std::string covers = UserDir() + "/covers", ca = m_assets_dir + "/xmb/cacert.pem";
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
  // (00.85: counted once until the covers or the games change - each game's names looked up: with 30 000 games a
  // moment, which every list made again and every cover downloaded took.)
  if (m_no_cover_count < 0)
  {
    m_no_cover_count = 0;
    for (const Game& game : m_games)
      m_no_cover_count += HasCover(game.rom) ? 0 : 1;
  }
  const int missing = m_no_cover_count;
  return missing == 0 ? std::string("Every game has a cover") :
                        std::to_string(missing) + (missing == 1 ? " game" : " games");
}

// Settings' count again when covers come (BuildItems isn't called for them): counted again in the frames' loop, once
// a second at most (00.85: a download brings covers every frame).
void Menu::Impl::RefreshNoCoverItem()
{
  m_no_cover_count = -1;
  m_no_cover_due = true;
}

void Menu::Impl::UpdateNoCoverItem()
{
  if (!m_no_cover_due || m_time - m_no_cover_time < 1.0f)
    return;
  m_no_cover_due = false;
  m_no_cover_time = m_time;
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
    // (A game's discs are no copies of each other: the Saturn's, each its own game - "(Disc 1)", "(Disc 2)".)
    std::string base;
    const int disc = Library::DiscNumber(m_games[i].rom.id, &base);
    if (disc > 0)
      key += "|disc" + std::to_string(disc);
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
void Menu::Impl::DeleteGames(const std::vector<std::string>& paths, bool saves)
{
  int deleted = 0, partly = 0;
  std::string error;
  // (00.85: the games by file once - Clean Up deletes thousands of a big library's games, each looked for among all.)
  std::unordered_map<std::string, std::vector<size_t>> by_path;
  for (size_t i = 0; i < m_games.size(); i++)
    by_path[m_games[i].rom.path].push_back(i);
  std::set<std::string> gone;
  std::vector<std::pair<Game, std::vector<std::string>>> forget;  // ((01.19) each game gone, with its files deleted)
  for (const std::string& path : paths)
  {
    const auto found = by_path.find(path);
    Library::Game rom;
    rom.path = path;
    if (found != by_path.end())
      rom = m_games[found->second.front()].rom;
    // ((01.19) src/GameDelete.h: a file already gone counts as deleted - until 01.18 it kept the game in the lists -;
    // the game is gone when its own file is (each disc's), whatever of its other files couldn't be deleted (said):
    // until 01.18 such a file kept it in the lists without its own one - its cover there, opened: an error.)
    const GameDelete::Files files = GameDelete::DeleteFiles(rom);
    if (error.empty())
      error = files.error;
    if (files.own_gone)
    {
      deleted++;
      partly += files.failed.empty() ? 0 : 1;
      gone.insert(path);
      if (found != by_path.end())
      {
        for (const size_t i : found->second)
          m_favorites.erase(m_games[i].rom.key);
        forget.emplace_back(m_games[found->second.front()], files.deleted);
      }
      if (m_prefs.default_game == path)
        m_prefs.default_game.clear();
    }
  }
  m_prefs.Save(UserDir() + "/xmb.ini");
  SaveFavorites();
  // (00.85) The games deleted leave the lists at once (their files are gone); the read on its thread then finds what
  // else changed.
  if (!gone.empty())
  {
    const std::array<std::string, CATEGORY_COUNT> selected = SelectedPaths();
    const std::string online = m_online_game >= 0 && m_online_game < static_cast<int>(m_games.size()) ?
                                   m_games[static_cast<size_t>(m_online_game)].rom.path :
                                   std::string();
    for (const Game& game : m_games)
    {
      if (gone.count(game.rom.path) && m_console_games[static_cast<size_t>(game.rom.system)] > 0)
        m_console_games[static_cast<size_t>(game.rom.system)]--;
    }
    m_games.erase(std::remove_if(m_games.begin(), m_games.end(),
                                 [&gone](const Game& game) { return gone.count(game.rom.path) > 0; }),
                  m_games.end());
    // ((01.19) A read that ended before the delete and wasn't taken yet has the games as they were: dropped - it
    // brought them back with their covers until the next read (RefreshGames' below, which reads again).)
    {
      std::lock_guard lock(m_read_mutex);
      if (m_read_done && m_read_done->scanned)
        m_read_done.reset();
    }
    // ((01.19) What else was theirs - what the remaining games keep looked at once -; their downloaded bezels,
    // RetroAchievements' hashes of their files, the recents' lines of their files, each list once.)
    KeptByOthers kept;
    bool pictures = false;
    for (const auto& [game, files] : forget)
    {
      for (const std::string& name : CoverNames(game.rom))
        pictures = pictures || m_cover_files.count(name) > 0;
    }
    for (const Game& other : m_games)
    {
      kept.keys.insert(other.rom.key);
      kept.names.insert(std::string(GetSystem(other.rom.system).key) + "/" + other.rom.id);
      if (!pictures)
        continue;
      for (const std::string& name : CoverNames(other.rom))
      {
        const auto it = m_cover_files.find(name);
        if (it != m_cover_files.end())
          kept.pictures.insert(it->second);
      }
    }
    std::set<std::string> played_gone;
    std::vector<std::string> hashed_gone;
    std::map<std::string, std::vector<std::string>> bezels;  // console -> its games' ids
    for (const auto& [game, files] : forget)
    {
      const std::string went = ForgetGame(game, saves, kept);
      Log("deleted game " + game.rom.key + (went.empty() ? std::string() : ": " + went));
      played_gone.insert(game.rom.path);
      played_gone.insert(game.rom.discs.begin(), game.rom.discs.end());
      hashed_gone.insert(hashed_gone.end(), files.begin(), files.end());
      const std::string console = GetSystem(game.rom.system).key;
      if (!kept.names.count(console + "/" + game.rom.id))
        bezels[console].push_back(game.rom.id);
    }
    for (const auto& [console, ids] : bezels)
      BezelProject::DeleteGameBezels(UserDir(), console, ids);
    Achievements::ForgetLibraryFiles(hashed_gone);
    GameDelete::DropPlayed(UserDir() + "/played.ini", played_gone,
                           [](const std::string& stored) { return Portable::FromStored(stored); });
    m_games_fingerprint = 0;  // (the read's games, whatever they are, come in)
    StartAchievementMarks();  // (the games' new places in m_games)
    GamesChanged(selected, online);
  }
  RefreshGames();
  const int failed = static_cast<int>(paths.size()) - deleted;
  if (failed == 0 && partly == 0)
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
  if (failed == 0)  // ((01.19) gone from the lists, a file of theirs not deleted)
    m_panel.lines = {"", deleted == 1 ? "Game deleted" : std::to_string(deleted) + " games deleted",
                     "Some of their other files couldn't be deleted:", error, "", "Is the drive read-only?"};
  Play("error");
}

std::string Menu::Impl::ForgetGame(const Game& game, bool saves, const KeptByOthers& kept)
{
  const Library::Game& rom = game.rom;
  const std::string& key = rom.key;
  std::vector<std::string> went;
  // (A copy of the game elsewhere - another drive, another folder - has its key: its name, settings, saves stay.)
  const bool key_kept = kept.keys.count(key) > 0;
  // Its cover: the pictures of its names (its key, its title, an arcade set's full name and parent's) no remaining game
  // shows by any of its names - a title's picture another region's game shows too stays.
  const std::set<std::string> pictures = GameDelete::OwnPictures(CoverNames(rom), m_cover_files, kept.pictures);
  if (!pictures.empty())
  {
    int covers = 0;
    for (const std::string& picture : pictures)
    {
      if (std::remove(picture.c_str()) != 0)
      {
        Log("couldn't delete the cover " + picture);
        continue;
      }
      covers++;
      for (auto it = m_cover_files.begin(); it != m_cover_files.end();)
        it = it->second == picture ? m_cover_files.erase(it) : std::next(it);
    }
    if (covers > 0)
      went.push_back(covers == 1 ? "its cover" : std::to_string(covers) + " covers");
  }
  if (!key_kept)
  {
    m_cover_why.erase(key);
    m_ratings.erase(key);  // (its stars: matched again if it comes back)
    m_rating_asked.erase(key);
    if (m_names.erase(key) > 0)
    {
      SaveNames();
      went.push_back("its name");
    }
    if (Settings::IsCustom(key))
    {
      Settings::ResetGame(key);
      went.push_back("its own settings");
    }
    if (Controls::HasGameProfile(key))
    {
      Controls::DeleteGameProfile(key);
      went.push_back("its controls");
    }
    if (std::remove((UserDir() + "/cheats/" + key + ".cht").c_str()) == 0)
      went.push_back("its cheats");
    if (saves)
    {
      int removed = Saves::DeleteSram(key) ? 1 : 0;
      std::remove(Saves::DiscPath(key).c_str());
      for (int slot = 0; slot <= Saves::SLOTS; slot++)
        removed += Saves::DeleteState(key, slot) ? 1 : 0;
      if (removed > 0)
        went.push_back(std::to_string(removed) + (removed == 1 ? " save or state" : " saves and states"));
    }
  }
  // A game named after its folder ("EBOOT.PBP", "disc.gdi" in "Game Name/"): the folder too, once empty - never
  // anything else in it.
  if (GameDelete::RemoveEmptyFolder(rom))
    went.push_back("its folder (empty)");
  std::string text;
  for (const std::string& each : went)
    text += (text.empty() ? "" : ", ") + each;
  return text;
}

void Menu::Impl::SetDeleteGameLines()
{
  m_panel.lines = {"", m_delete_title, "",
                   m_delete_files > 1 ? "Its " + std::to_string(m_delete_files) + " files are deleted from the drive:" :
                                        std::string("Its file is deleted from the drive:"),
                   m_delete_file, "", "And its cover, name, own settings, controls, cheats and bezel.",
                   m_delete_saves ? "Its saves and states are deleted too: they can't be brought back." :
                                    "Its saves and states stay (Saved Data)."};
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
      {"Achievements", "RetroAchievements: every achievement of the game, unlocked or not, with your progress; for a file "
                       "it doesn't know, the files of the game it knows."},
      {"Rename", "A name of your own for the game (it is found by it too, covers included). Empty: its own name again."},
      {"Find Cover", "Choose the game's cover from libretro's pictures: the ones most like its name, or another name."},
      {"Set as Default Game", "This game starts by itself when EmuSuNi opens (hold L1 to get the menu)."},
      {"Remove Default Game", "EmuSuNi opens on the menu again."},
      {"Delete Game", "Deletes the game's files from the drive (a .cue's tracks and every disc too), and its cover, name, "
                      "own settings, controls, cheats and bezel. Its saves stay unless you choose."},
      // (01.19) Delete Game's choice for the saves.
      {"Saves and States: Keep", "Its saves and states stay (Saved Data): put back, the game finds them. Cross: "
                                 "delete them too."},
      {"Saves and States: Delete Too", "Its battery saves and save states are deleted with it: they can't be brought "
                                       "back. Cross: keep them."},
      // (01.22, the owner) What a game has installed, from the game (no longer Settings).
      {"Updates, DLC and Mods", "Its update and DLC (installed from its CIA files in Roms/3ds/cia, or removed) and its "
                                "mods on or off. Also in the in-game menu."},
      {"Patched Romset", "A hack of this set with no driver of its own: the one FinalBurn Neo uses (removed), or one "
                         "installed from Roms/arcade/patched. Also in the in-game menu."},
      {"Cancel", "Nothing is changed."},
      {"Delete", "Deletes for good: it can't be brought back."},
      {"Reset everything", "Every setting, global and per game, back to the default."},
      {"Internal Storage", "New covers are saved in the PS4's own storage, as before."},
      {"With My Data (USB Drive)", "New covers are saved with your data on the USB drive (EmuSuNi-Portable/covers). "
                                   "Nothing to move: the covers of both places are shown."},
      {"USB Drive", "New covers are saved on the USB drive (EmuSuNi/covers): the PS4's storage stays free. "
                    "Without the drive they go to the internal storage."},
      // (Updates, 00.73.)
      {"Update Now", "Downloads the new version, checks it and hands it to the PS4, which installs it (its notifications "
                 "show it). Games can be played meanwhile."},
      {"Later", "Closes this until EmuSuNi starts again."},
      {"Check Again", "Asks GitHub for the newest version again."},
      {"Stop the Download", "Stops it; Update goes on from there next time."},
      {"Include Test Versions: Off", "Also the test versions the creator publishes on GitHub (pre-releases): newer, "
                                     "but they may have problems."},
      {"Include Test Versions: On", "Only the published versions again."},
      {"Close EmuSuNi", "Closes the app so that the PS4 finishes installing the update; open EmuSuNi again "
                        "afterwards."},
      // (Star Ratings, 00.73.)
      {"Download Now", "Downloads LaunchBox's database (about 110 MB, in the background) and keeps its ratings of "
                       "your consoles' games (a few MB). Again after 60 days."},
      {"Download Again", "Downloads LaunchBox's database again (about 110 MB) for its newest ratings."},
      // (Online play, 00.23.)
      {"Play Online", "Makes a room for this game on EmuSuNi's server: a friend joins it with its code (or anyone, a "
                      "public room), and you play together as on one console."},
      // (00.85: the Online tab's table lists every room - a private one without its code.)
      {"Private Room", "Only those you tell its code can join (the Online tab lists it, without its code)."},
      {"Public Room", "Anyone can join it from the Online tab. Voice chat starts off for everyone."},
      {"Choose Together", "A room without a game: once your friend is in, you both see the games you both have (the "
                          "same file) and you pick one; after its match, another one, without joining again."},
      {"Close Room", "Closes the room: no one can join it any more."},
      {"Leave Room", "Leaves the room."},
      // (01.05) The room's chat.
      {"Quick Phrase", "A ready phrase for everyone in the room, each reads it in their language (L2 does the same)."},
      {"Write a Message", "A line of up to 100 letters with the keyboard, for everyone in the room (R2 does the same)."},
      // (01.05) Spectators.
      {"Spectators: Yes", "Up to 4 may watch the match from the Online tab, without voice or chat. Cross: no spectators."},
      {"Spectators: No", "No one watches the match. Cross: up to 4 may watch it from the Online tab."},
      {"Stop Watching", "Leaves the room: the match goes on without you."},
      {"Late Joining: Yes", "A player may join the match under way while a place is left: everyone stops a moment "
                            "while it comes in. Cross: no one joins once it starts."},
      {"Late Joining: No", "No one joins the match once it starts. Cross: a player may join it under way."},
      // (01.14) Free entry.
      {"Entry: Free", "Whoever comes is in the room at once, without asking you. Cross: each one asks first."},
      {"Entry: With Permission", "Each player asks to join and you accept them or not. Cross: whoever comes is in at "
                                 "once."},
      {"Watch the Match", "Watch it as a spectator, about a second behind the players: you play nothing."},
      {"Play Anyway", "Your file of this game isn't the host's (another version or dump): it may go out of step, "
                      "which the match corrects, or not start at all."},
  };
  const auto it = HELP.find(option);
  return it == HELP.end() ? std::string() : it->second;
}

// ---- Online play (00.23, src/Netplay.h) ----

// A game's identity for a room: its console, title, CRC32 and size (each file read once a session). A renamed game
// keeps its own title (the other console knows it by that one).
bool Menu::Impl::OnlineGameInfo(int game, NetProto::GameInfo* info)
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return false;
  const Library::Game& rom = m_games[static_cast<size_t>(game)].rom;
  auto it = m_online_crcs.find(rom.path);
  if (it == m_online_crcs.end())
  {
    uint32_t crc = 0;
    uint64_t size = 0;
    // ((01.05) its content's - NetplayLibrary's copy first: the menu's thread reads nothing it already knows)
    if (!NetplayLibrary::Lookup(rom.path, &crc, &size) &&
        !Netplay::GameCrc(rom.path, &crc, &size, GetSystem(rom.system).key))
      return false;
    it = m_online_crcs.emplace(rom.path, std::make_pair(crc, size)).first;
  }
  info->console = GetSystem(rom.system).key;
  info->title = rom.title;
  if (rom.custom_title)
  {
    std::string region, tags;
    Library::ParseName(rom.id, &info->title, &region, &tags);
  }
  info->crc = it->second.first;
  info->size = it->second.second;
  return true;
}

// (01.06) A game's whole file's CRC32 and size, as v00.23 knew it (Netplay::FileCrc): NetplayLibrary's copy first, else
// read now (once a session) - a file of a room of such an app, or our room's own.
bool Menu::Impl::OnlineFileId(int game, uint32_t* crc, uint64_t* size)
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return false;
  const Library::Game& rom = m_games[static_cast<size_t>(game)].rom;
  auto it = m_online_file_crcs.find(rom.path);
  if (it == m_online_file_crcs.end())
  {
    uint32_t c = 0;
    uint64_t s = 0;
    if (!NetplayLibrary::LookupFile(rom.path, &c, &s) && !Netplay::FileCrc(rom.path, &c, &s))
      return false;
    it = m_online_file_crcs.emplace(rom.path, std::make_pair(c, s)).first;
  }
  *crc = it->second.first;
  *size = it->second.second;
  return true;
}

// (00.95) The players a room for the game may take: those the game takes at once (NetplayLibrary::Players; not known:
// as many as the ports) on the console's ports - 4 with its adapter (Multiplayer.h: the game's Multiplayer Adapter Off,
// 2) or on a console of 4 (the N64, the Dreamcast, the NAOMI, the Atomiswave, the Arcade, the PC Engine's TurboTap), 2
// on the others -, 2 to NetProto::MAX_MEMBERS. A Choose Together room (game -1): MAX_MEMBERS. (01.04) The Atari 2600's
// paddle games: up to 4 (two paddles in each port, Stella's RetroPad ports 1-4: Warlords) - players.bin has more than 2
// for them only (scripts/make-netplay-players.py); a 2600 game not known: 2.
int Menu::Impl::OnlinePlayersMost(int game) const
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return NetProto::MAX_MEMBERS;
  const Library::Game& rom = m_games[static_cast<size_t>(game)].rom;
  const SystemId id = rom.system;
  int ports = 2;
  if (Multiplayer::HasAdapter(id))
    ports = Settings::Get("multiplayer_adapter", rom.key) == "off" ? 2 : 4;
  else if (id == SystemId::N64 || id == SystemId::DC || id == SystemId::NAOMI || id == SystemId::ATOMISWAVE ||
           id == SystemId::ARCADE || id == SystemId::PCE || id == SystemId::A2600)
    ports = 4;
  uint32_t crc = 0;
  uint64_t size = 0;
  NetplayLibrary::Lookup(rom.path, &crc, &size);
  const int players = NetplayLibrary::Players(GetSystem(id).key, rom.id, crc);
  const int unknown = id == SystemId::A2600 ? 2 : ports;
  return std::clamp(players > 0 ? std::min(players, ports) : unknown, 2, NetProto::MAX_MEMBERS);
}

// (00.95) Play Online's Players: what it does, and how far it goes.
std::string Menu::Impl::OnlinePlayersHelp() const
{
  return "Up to " + std::to_string(m_online_players) + " players in the room (Left / Right: 2 to " +
         std::to_string(m_online_players_most) + "). The match starts with those in it when you press Start.";
}

// (01.04) A game of ours for a link cable room of that console (NetProto::GBLINK: Game Boy and Game Boy Color games;
// NetProto::GBALINK: Game Boy Advance games).
static bool LinkGameFits(const std::string& link_console, SystemId system)
{
  if (link_console == NetProto::GBALINK)
    return system == SystemId::GBA;
  return system == SystemId::GB || system == SystemId::GBC;
}

// The room's game among ours: the same file (its CRC32 and size: only our files of its size are read), else a game of
// the same title (another version or dump: an arcade romset's zip made otherwise is the same set - the core checks its
// files - so it counts as the same); *index its m_games' index.
NetProto::LobbyGame Menu::Impl::FindOnlineGame(const NetProto::GameInfo& game, int* index)
{
  using NetProto::LobbyGame;
  *index = -1;
  // (00.95) A Game Boy link cable room: its game among our Game Boy and Game Boy Color games, the same file only (Game
  // Boy 1 runs the same ROM on both consoles). (01.04: a Game Boy Advance link room's among our GBA games.)
  const bool link = game.console == NetProto::GBLINK || game.console == NetProto::GBALINK;
  const System* system = game.console == NetProto::GBLINK  ? &GetSystem(SystemId::GB) :
                         game.console == NetProto::GBALINK ? &GetSystem(SystemId::GBA) :
                                                             SystemByKey(game.console);
  if (!system)
    return LobbyGame::Missing;
  auto title_key = [](const std::string& title) {
    std::string key;
    for (const char c : title)
    {
      if (std::isalnum(static_cast<unsigned char>(c)))
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
  };
  const std::string wanted = title_key(game.title);
  int by_title = -1;
  for (size_t i = 0; i < m_games.size(); i++)
  {
    const Library::Game& rom = m_games[i].rom;
    if (link ? !LinkGameFits(game.console, rom.system) : rom.system != system->id)
      continue;
    NetProto::GameInfo mine;
    if (MayBeContent(rom, game.size) && OnlineGameInfo(static_cast<int>(i), &mine) && mine.crc == game.crc &&
        mine.size == game.size)
    {
      *index = static_cast<int>(i);
      return LobbyGame::Same;
    }
    // (01.06) A room of v00.23's (or 01.04's) says its game by its whole file: ours of that very size, by theirs. (Not
    // a link cable room's: of this version only.)
    uint32_t file_crc = 0;
    uint64_t file_size = 0;
    if (!link && rom.size == game.size && OnlineFileId(static_cast<int>(i), &file_crc, &file_size) &&
        file_crc == game.crc && file_size == game.size)
    {
      *index = static_cast<int>(i);
      return LobbyGame::Same;
    }
    std::string own = rom.title, region, tags;
    if (rom.custom_title)
      Library::ParseName(rom.id, &own, &region, &tags);
    if (by_title < 0 && !wanted.empty() && title_key(own) == wanted)
      by_title = static_cast<int>(i);
  }
  if (by_title < 0 || link)
    return LobbyGame::Missing;
  *index = by_title;
  return IsRomsetSystem(system->id) ? LobbyGame::Same : LobbyGame::Different;
}

// (01.05) Whether a file of ours may hold a content of that size (NetplayLibrary::ContentId's): known already (the
// library's copy: its content's size), else its file's size the content's or the content's with a header (iNES 16,
// with a trainer 528, a78 128, LNX 64, a copier's 512), or a zip - the only files the menu's thread reads to know.
bool Menu::Impl::MayBeContent(const Library::Game& rom, uint64_t size) const
{
  uint32_t crc = 0;
  uint64_t content = 0;
  if (NetplayLibrary::Lookup(rom.path, &crc, &content))
    return content == size;
  const std::string& p = rom.path;
  if (p.size() > 4 && p[p.size() - 4] == '.' && std::tolower(static_cast<unsigned char>(p[p.size() - 3])) == 'z' &&
      std::tolower(static_cast<unsigned char>(p[p.size() - 2])) == 'i' &&
      std::tolower(static_cast<unsigned char>(p[p.size() - 1])) == 'p')
    return true;
  for (const uint64_t header : {0, 16, 64, 128, 512, 528})
  {
    if (rom.size == size + header)
      return true;
  }
  return false;
}

// (01.05, the owner: "que se vea también la versión que tiene cada uno del juego") A game's version as its file's name
// says it: its region and its other groups, without their brackets - "Super Mario World (USA) (Rev 1)" is "USA, Rev 1"
// ("[!]", a good dump's mark, left out). "" when the name says none.
static std::string VersionText(const std::string& region, const std::string& tags)
{
  std::string out = region;
  for (size_t i = 0; i < tags.size();)
  {
    const char open = tags[i];
    const char close = open == '(' ? ')' : open == '[' ? ']' : 0;
    const size_t stop = close ? tags.find(close, i + 1) : std::string::npos;
    if (stop == std::string::npos)
    {
      i++;
      continue;
    }
    const std::string inside = tags.substr(i + 1, stop - i - 1);
    if (!inside.empty() && inside != "!")
      out += (out.empty() ? "" : ", ") + inside;
    i = stop + 1;
  }
  return out;
}

std::string Menu::Impl::OnlineVersionText(int game) const
{
  if (game < 0 || game >= static_cast<int>(m_games.size()))
    return {};
  const Library::Game& rom = m_games[static_cast<size_t>(game)].rom;
  return VersionText(rom.region, rom.tags);
}

int Menu::Impl::StandardMark(const Game& game) const
{
  if (game.standard == 0)
  {
    const std::string key = GetSystem(game.rom.system).key;
    uint32_t crc = 0;
    uint64_t size = 0;
    if (game.rom.discs.size() > 1 || !PlaysOnline(game.rom.system) || !NetplayLibrary::Covered(key))
      game.standard = 3;
    else if (NetplayLibrary::Lookup(game.rom.path, &crc, &size))
      game.standard = NetplayLibrary::StandardName(key, crc, size).empty() ? 2 : 1;
  }
  return game.standard == 1 || game.standard == 2 ? game.standard : 0;
}

float Menu::Impl::DrawStandardMark(ImDrawList* dl, ImVec2 left_center, float r, int mark, float alpha, bool label)
{
  if (mark != 1 && mark != 2)
    return 0.0f;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const uint32_t color = mark == 1 ? (light ? 0x1E7F46 : 0x3FBF6F) : (light ? 0x9A5C00 : 0xE0A030);
  const ImVec2 c(left_center.x + r, left_center.y);
  dl->AddCircleFilled(c, r, Rgba(color, alpha * 0.95f), 20);
  const ImU32 white = Rgba(0xFFFFFF, alpha);
  const float t = std::max(1.5f, r * 0.26f);
  if (mark == 1)
  {
    dl->PathLineTo(ImVec2(c.x - r * 0.48f, c.y + r * 0.02f));
    dl->PathLineTo(ImVec2(c.x - r * 0.12f, c.y + r * 0.38f));
    dl->PathLineTo(ImVec2(c.x + r * 0.50f, c.y - r * 0.34f));
    dl->PathStroke(white, 0, t);
  }
  else
    dl->AddLine(ImVec2(c.x - r * 0.48f, c.y), ImVec2(c.x + r * 0.48f, c.y), white, t);
  if (label)
  {
    const float size = std::max(16.0f, r * 1.6f);
    const std::string word = PS4Lang::T(mark == 1 ? "Standard" : "Another version");
    dl->AddText(m_bold, size, ImVec2(c.x + r + 8.0f, c.y - size * 0.58f), Rgba(color, alpha), word.c_str());
  }
  return StandardMarkWidth(r, mark, label);
}

float Menu::Impl::StandardMarkWidth(float r, int mark, bool label) const
{
  if (mark != 1 && mark != 2)
    return 0.0f;
  if (!label)
    return r * 2.0f;
  const float size = std::max(16.0f, r * 1.6f);
  const std::string word = PS4Lang::T(mark == 1 ? "Standard" : "Another version");
  return r * 2.0f + 8.0f + m_bold->CalcTextSizeA(size, FLT_MAX, 0.0f, word.c_str()).x;
}

// (01.05, the owner: "en el nick de la persona salga una abreviación pequeña del país") A player's country as a small
// tag: its two letters in a rounded box, centred on cy; its width (0: none - not shown).
float Menu::Impl::DrawCountryTag(ImDrawList* dl, float x, float cy, const std::string& country, float size, float a)
{
  if (country.empty())
    return 0.0f;
  const Theme& theme = CurrentTheme();
  const float tw = m_bold->CalcTextSizeA(size, FLT_MAX, 0.0f, country.c_str()).x;
  const float w = tw + size * 0.8f, h = size + 8.0f;
  const ImVec2 p0(x, cy - h * 0.5f), p1(x + w, cy + h * 0.5f);
  dl->AddRectFilled(p0, p1, Rgba(theme.text, 0.10f * a), h * 0.3f);
  dl->AddRect(p0, p1, Rgba(theme.text, 0.40f * a), h * 0.3f, 0, 1.5f);
  dl->AddText(m_bold, size, ImVec2(x + (w - tw) * 0.5f, cy - size * 0.58f), Rgba(theme.text, 0.9f * a),
              country.c_str());
  return w;
}

// (00.95) A Game Boy link cable room: the guest's game for Game Boy 2 among ours - the same file (console, CRC32, size).
int Menu::Impl::FindLinkGame(const NetProto::LibraryGame& game)
{
  const std::string link = Netplay::LinkConsole();
  for (size_t i = 0; i < m_games.size(); i++)
  {
    const Library::Game& rom = m_games[i].rom;
    if (!LinkGameFits(link, rom.system) || GetSystem(rom.system).key != game.console || !MayBeContent(rom, game.size))
      continue;
    NetProto::GameInfo mine;
    if (OnlineGameInfo(static_cast<int>(i), &mine) && mine.crc == game.crc && mine.size == game.size)
      return static_cast<int>(i);
  }
  return -1;
}

// A guest: its game for Game Boy 2 (m_online_link_game) and its battery save - RAM and clock, as mGBA keeps them - to
// the host.
void Menu::Impl::SayLinkGame()
{
  NetProto::GameInfo info;
  if (!OnlineGameInfo(m_online_link_game, &info))
    return;
  NetProto::LibraryGame g;
  g.console = info.console;
  g.crc = info.crc;
  g.size = info.size;
  const Library::Game& rom = m_games[static_cast<size_t>(m_online_link_game)].rom;
  const std::vector<uint8_t> save = GbLink::ReadSave(rom.key, static_cast<uint64_t>(std::time(nullptr)));
  Netplay::SetLinkGame(g, info.title, GbLink::PackSave(save));
  m_online_link_said = true;
  Log("online: our game for Game Boy 2: " + rom.path + " (its save: " + std::to_string(save.size()) + " bytes)");
}

// A guest's own game for Game Boy 2: one of its Game Boy and Game Boy Color games (Circle: back to the room).
void Menu::Impl::OpenLinkGamePicker()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_OL_LINKGAME;
  m_panel.title = "Your Game";
  m_panel.lines = {"", "The game of your Game Boy (Game Boy 2).", "The other console needs its file too."};
  m_online_actions.clear();
  const std::string link = Netplay::LinkConsole();  // (01.04: a GBA link room's: GBA games)
  for (size_t i = 0; i < m_games.size(); i++)
  {
    const Library::Game& rom = m_games[i].rom;
    if (!LinkGameFits(link, rom.system))
      continue;
    if (static_cast<int>(i) == m_online_link_game)
      m_panel.selected = static_cast<int>(m_online_actions.size());
    m_panel.options.push_back(rom.title);
    m_online_actions.push_back("lg:" + std::to_string(i));
  }
  Play("menu");
}

// Before Emulator::Run of a link cable match: both games as this console has them, ours, the guest's save (the host's).
void Menu::Impl::SetGbLinkMatch()
{
  const bool host = Netplay::IsHost();
  GbLink::Match match;
  const int games[2] = {m_online_game, m_online_link_game};
  for (int i = 0; i < 2; i++)
  {
    if (games[i] < 0 || games[i] >= static_cast<int>(m_games.size()))
      return;
    const Library::Game& rom = m_games[static_cast<size_t>(games[i])].rom;
    NetProto::GameInfo info;
    OnlineGameInfo(games[i], &info);
    match.games[i].path = rom.path;
    match.games[i].key = rom.key;
    match.games[i].title = rom.title;
    match.games[i].system = rom.system;
    match.games[i].crc = info.crc;
    match.games[i].size = info.size;
  }
  match.local = host ? 0 : 1;
  if (host)
  {
    const std::vector<uint8_t> packed = Netplay::LinkGuestSave();
    if (!GbLink::UnpackSave(packed.data(), packed.size(), &match.guest_save))
      Log("online: the guest's Game Boy save didn't come whole");
  }
  GbLink::Set(match);
}

// The Online tab (00.23): Settings -> Online Play, and every way back to online play, land there.
void Menu::Impl::OpenOnlinePanel()
{
  m_panel.open = false;
  m_category = static_cast<int>(Category::Online);
  m_category_pos = static_cast<float>(ColumnIndex(m_category));
  m_tab_pos = static_cast<float>(LibraryTab());
  BuildOnlineItems();
  Play("menu");
}

// The Online tab has no rows of items (00.85: its table, BuildOnlineTable, instead; until 00.84 its settings were rows
// here - they are Online Settings' now, Options in the table).
void Menu::Impl::BuildOnlineItems()
{
  m_items[static_cast<int>(Category::Online)].clear();
}

// The Online tab's actions (its table's buttons, Online Settings' rows, and their panels' options). False: not one of
// them.
bool Menu::Impl::OnlineAction(const std::string& action)
{
  // (00.85) Back to Online Settings, on the row of what was done.
  const auto back_to_settings = [this](const std::string& key) { OpenOnlineSettings(key); };
  if (action == "replace_yes" || action == "replace_no")
  {
    // (00.86) "You already have a room open": closed (then what was asked), or kept.
    std::function<void()> then;
    then.swap(m_online_after_close);
    m_panel.open = false;
    if (action == "replace_no")
    {
      Play("back");
      return true;
    }
    Log("online: our room closed to make or join another");
    Netplay::Leave();
    Netplay::TakeError();
    if (then)
      then();
    else
      Play("confirm");
  }
  else if (action == "join")
  {
    if (AskCloseRoom(true, [this] { OnlineAction("join"); }))
      return true;  // (00.86)
    m_panel.open = false;
    AskText(
        "Join with Code", "",
        [this](const std::string& code) {
          if (NetProto::NormalizeCode(code).empty())
            return;
          m_online_found = NetProto::LobbyGame::Looking;
          m_online_game = -1;
          m_online_library_sent = false;
          Netplay::Join(code, m_data_dir);
          OpenOnlineRoom();
        },
        8, TextKeyboard::Mode::Plain, "The 6 letters and numbers of your friend's room.");
  }
  else if (action.rfind("watch:", 0) == 0)
  {
    // (01.05) A match to watch (its room's code): as a spectator, the game found as a guest's is.
    m_online_found = NetProto::LobbyGame::Looking;
    m_online_game = -1;
    m_online_library_sent = false;
    Netplay::Watch(action.substr(6), m_data_dir);
    OpenOnlineRoom();
    Play("confirm");
  }
  else if (action == "entry")
  {
    // (01.14) The host: whoever comes is in at once, or asks first.
    const Netplay::View v = Netplay::GetView();
    Netplay::SetFreeEntry(!v.free_entry);
    ShowToast(!v.free_entry ? "Players come in without asking" : "Players ask you to join first", 0);
    Play("confirm");
  }
  else if (action == "late")
  {
    // (01.05) The host: a player may join the match under way, or not.
    const Netplay::View v = Netplay::GetView();
    Netplay::SetLateAllowed(!v.late_allowed);
    ShowToast(!v.late_allowed ? "A player may join the match under way" : "No one joins the match once it starts", 0);
    Play("confirm");
  }
  else if (action == "watchers")
  {
    // (01.05) The host: spectators let in or not.
    const Netplay::View v = Netplay::GetView();
    Netplay::SetWatchAllowed(!v.watch_allowed);
    ShowToast(!v.watch_allowed ? "Spectators may watch the match" : "No spectators: those watching left", 0);
    Play("confirm");
  }
  else if (action == "together")
  {
    OpenOnlineHost(-1);  // (00.23: a room without a game - Choose Together)
  }
  else if (action == "with_game")
  {
    // A room for one game: Triangle -> Play Online on it (the games' tab).
    m_panel.open = false;
    m_category = static_cast<int>(Category::Games);
    m_category_pos = static_cast<float>(ColumnIndex(m_category));
    m_tab_pos = static_cast<float>(LibraryTab());
    ShowToast("Pick a game, then Triangle -> Play Online", 0);
    Play("confirm");
  }
  else if (action == "nick")
  {
    // (00.23) The name the others see in rooms: the PS4 user's unless one is typed here (16 letters, filtered). (The
    // keyboard over Online Settings: Cancel leaves it there.)
    AskText(
        "Nickname", Netplay::GetNickname(m_data_dir),
        [this, back_to_settings](const std::string& nick) {
          Netplay::SetNickname(m_data_dir, nick);
          back_to_settings("ol:nick");
        },
        static_cast<uint32_t>(NetProto::NICK_LETTERS), TextKeyboard::Mode::Text,
        "The name other players see. Left empty: your PS4 user's name.");
  }
  else if (action == "hide")
  {
    Netplay::SetHideAddress(m_data_dir, !Netplay::HideAddress(m_data_dir));
    back_to_settings("ol:hide");
    Play("confirm");
  }
  else if (action == "bar")
  {
    Netplay::SetLatencyBar(m_data_dir, !Netplay::LatencyBar(m_data_dir));
    back_to_settings("ol:bar");
    Play("confirm");
  }
  else if (action == "chattext")  // (01.05)
  {
    Netplay::SetChatText(m_data_dir, !Netplay::ChatText(m_data_dir));
    back_to_settings("ol:chattext");
    Play("confirm");
  }
  else if (action == "country")  // (01.05)
  {
    Netplay::SetShowCountry(m_data_dir, !Netplay::ShowCountry(m_data_dir));
    back_to_settings("ol:country");
    Play("confirm");
  }
  else if (action == "unblock")
  {
    Netplay::UnblockAll(m_data_dir);
    ShowToast("Every blocked player unblocked", 0);
    back_to_settings("ol:nick");
    Play("confirm");
  }
  else if (action == "world")  // (01.07: the world chat shown or hidden)
  {
    Netplay::SetWorldChatOn(m_data_dir, !Netplay::WorldChatOn(m_data_dir));
    back_to_settings("ol:world");
    Play("confirm");
  }
  else if (action == "ranking")  // (01.07: the players' ranking)
  {
    Netplay::SetRanked(m_data_dir, !Netplay::Ranked(m_data_dir));
    back_to_settings("ol:ranking");
    Play("confirm");
  }
  else if (action == "worldmuted")
  {
    Netplay::ClearWorldMuted(m_data_dir);
    ShowToast("The players muted in the world chat are read again", 0);
    back_to_settings("ol:world");
    Play("confirm");
  }
  else if (action == "profile_on" || action == "profile_off")
  {
    UsageStats::SetProfile(action == "profile_on");
    BuildItems();  // (Settings -> Anonymous Statistics: on with it)
    back_to_settings("ol:profile");
    if (action == "profile_on")
      ShowToast("Your profile's code comes in a moment", 0);
    Play("confirm");
  }
  else if (action == "public_on" || action == "public_off")
  {
    UsageStats::SetPublicProfile(action == "public_on");
    back_to_settings("ol:public");
    Play("confirm");
  }
  else if (action == "delete_profile")
  {
    UsageStats::DeleteProfile();
    back_to_settings("ol:profile");
    ShowToast("Your profile is being deleted", 0);
    Play("confirm");
  }
  else if (action == "back")
  {
    // (Profile Code's OK, Delete My Profile's Cancel: back to Online Settings, on that row.)
    back_to_settings(m_panel.setting == SET_OL_CODE ? "ol:code" : "ol:delete");
    Play("back");
  }
  else
  {
    return false;
  }
  return true;
}

// ---- My Stats (00.23, src/MyStats.h): this PS4's own numbers, never sent ----
std::string Menu::Impl::MyStatsDetail() const
{
  int64_t seconds = 0;
  for (const MyStats::Game& g : MyStats::Games())
    seconds += g.seconds;
  if (seconds <= 0)
    return "Nothing played yet";
  char hours[32];
  std::snprintf(hours, sizeof(hours), "%.1f", static_cast<double>(seconds) / 3600.0);
  return std::string(hours) + " h played";
}

// (01.05, the owner: "Revisar mis juegos") Check My Games: each console of the standard versions' list
// (NetplayLibrary::Covered) with games here - how many of them are standard -, then (Cross) a console's games, those not
// standard first, each with its standard name. Its files are read on NetplayLibrary's thread (its copy: once); the rows
// come again as it goes (UpdateOnline: CheckSignature).
void Menu::Impl::OpenCheckGames(const std::string& console, const std::string& select)
{
  const float slide = m_panel.open ? m_panel.slide : 0.0f;
  m_panel = {};
  m_panel.open = true;
  m_panel.slide = slide;
  m_check_console = console;
  m_panel.setting = console.empty() ? SET_OL_CHECK : SET_OL_CHECK_LIST;
  const System* system = console.empty() ? nullptr : SystemByKey(console);
  m_panel.title = system ? system->name : "Check My Games";
  if (console.empty())
  {
    StartOnlineLibrary();
    Log("online: Check My Games");
  }
  BuildCheckRows();
  for (size_t i = 0; i < m_panel.rows.size() && !select.empty(); i++)
  {
    if (m_panel.rows[i].key == select)
      m_panel.selected = static_cast<int>(i);
  }
}

std::string Menu::Impl::CheckSignature() const
{
  const NetplayLibrary::Progress p = NetplayLibrary::GetProgress();
  return std::to_string(p.done) + "/" + std::to_string(p.total) + (p.finished ? "+" : "") + m_check_console;
}

void Menu::Impl::BuildCheckRows()
{
  const std::string was = m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.rows.size()) ?
                              m_panel.rows[static_cast<size_t>(m_panel.selected)].key :
                              std::string();
  std::vector<SettingRow> rows;
  const NetplayLibrary::Progress progress = NetplayLibrary::GetProgress();
  if (!progress.finished)
  {
    SettingRow row{"Looking at Your Games",
                   std::to_string(progress.done) + " of " + std::to_string(progress.total) + " read", "check:reading"};
    row.help = "Each file is read once, in the background (its copy is kept): the menu goes on meanwhile.";
    rows.push_back(std::move(row));
  }
  struct Count
  {
    int total = 0, standard = 0, unread = 0;
  };
  std::map<std::string, Count> counts;
  std::vector<std::string> order;  // (the consoles in the library's order)
  struct Item
  {
    int state = 0;  // 0 not standard, 1 not read yet, 2 standard
    std::string title, help;
    int game = -1;
  };
  std::vector<Item> items;
  for (size_t i = 0; i < m_games.size(); i++)
  {
    const Library::Game& rom = m_games[i].rom;
    const std::string key = GetSystem(rom.system).key;
    if (rom.discs.size() > 1 || !PlaysOnline(rom.system) || !NetplayLibrary::Covered(key))
      continue;
    if (!m_check_console.empty() && key != m_check_console)
      continue;
    uint32_t crc = 0;
    uint64_t size = 0;
    const bool read = NetplayLibrary::Lookup(rom.path, &crc, &size);
    const std::string name = read ? NetplayLibrary::StandardName(key, crc, size) : std::string();
    Count& c = counts[key];
    if (c.total == 0)
      order.push_back(key);
    c.total++;
    c.standard += name.empty() ? 0 : 1;
    c.unread += read ? 0 : 1;
    if (m_check_console.empty())
      continue;
    Item item;
    item.game = static_cast<int>(i);
    item.title = rom.title;
    item.state = !read ? 1 : name.empty() ? 0 : 2;
    const std::string file = std::filesystem::path(rom.path).filename().string();
    char id[16];
    std::snprintf(id, sizeof(id), "%08x", crc);
    item.help = item.state == 2 ? "Standard (" + NetplayLibrary::StandardSource(key) + "): " + name :
                item.state == 1 ? std::string("Being read...") :
                                  "Not the standard version (" + NetplayLibrary::StandardSource(key) + "): " + file +
                                      ", CRC32 " + id + ". Online it finds only players with this same file.";
    items.push_back(std::move(item));
  }
  if (m_check_console.empty())
  {
    for (const std::string& key : order)
    {
      const Count& c = counts[key];
      const System* system = SystemByKey(key);
      SettingRow row{system ? system->name : key,
                     std::to_string(c.standard) + " of " + std::to_string(c.total) + " standard", "check:" + key};
      row.help = c.unread > 0 ? "Still reading " + std::to_string(c.unread) + " of them. Cross: each game." :
                 c.standard == c.total ? std::string("Every one is the standard version. Cross: each game.") :
                                         std::to_string(c.total - c.standard) +
                                             " aren't the standard version: online they find only players with the "
                                             "same file. Cross: which.";
      rows.push_back(std::move(row));
    }
    if (order.empty())
    {
      SettingRow row{"No games to check", "", "check:none"};
      row.help = "The list has the cartridges of the consoles that play online (No-Intro) and the Arcade's sets "
                 "(FinalBurn Neo 1.0.0.03); discs aren't in it.";
      rows.push_back(std::move(row));
    }
  }
  else
  {
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      return a.state != b.state ? a.state < b.state : a.title < b.title;
    });
    for (const Item& item : items)
    {
      SettingRow row{item.title, item.state == 2 ? "Standard" : item.state == 1 ? "..." : "Not standard",
                     "game:" + std::to_string(item.game)};
      row.help = item.help;
      rows.push_back(std::move(row));
    }
  }
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!was.empty() && m_panel.rows[i].key == was)
      m_panel.selected = static_cast<int>(i);
  }
  m_check_signature = CheckSignature();
}

// (01.05, the owner: "haz una tabla con los más jugados") The Online tab's table of the games most played online
// (Touchpad: the rooms' or this one) - Netplay::TopGames, the main server's count of the last days' matches by game:
// each its title and version as the standard list names it (NetplayLibrary::StandardName by its CRC32; else a public
// room's title of it), its console, its matches, and whether it is in our games - the same version (the same content:
// the library's copy of the files' ids) or another of that title. Made again as the server's list comes and as the
// files are read (UpdateOnline: every 2 s at most).
void Menu::Impl::BuildTopTable()
{
  const Netplay::TopList top = Netplay::TopGames();
  m_top_serial = top.serial;
  m_top_built = m_time;
  std::vector<TopRow> rows;
  for (const Netplay::TopGame& g : top.games)
  {
    TopRow row;
    row.top = g;
    const bool gb_link = g.console == NetProto::GBLINK, gba_link = g.console == NetProto::GBALINK;
    row.link = gb_link || gba_link;
    const std::string key = gb_link ? "gb" : gba_link ? "gba" : g.console;
    row.standard = NetplayLibrary::StandardName(key, g.crc, 0);
    if (!row.standard.empty())
    {
      std::string region, tags;
      Library::ParseName(row.standard, &row.title, &region, &tags);
      row.version = VersionText(region, tags);
      // (An Arcade set's: its set's name, the one its zip has.)
      const size_t open = row.standard.rfind('['), close = row.standard.rfind(']');
      if (key == "arcade" && open != std::string::npos && close != std::string::npos && close > open)
        row.version = row.standard.substr(open + 1, close - open - 1);
    }
    else
      row.title = g.title;
    if (row.title.empty())
    {
      char id[16];
      std::snprintf(id, sizeof(id), "%08x", g.crc);
      row.title = std::string("CRC32 ") + id;
    }
    row.system = SystemByKey(key);
    row.tab = gb_link ? std::string("GB Link") : gba_link ? std::string("GBA Link") :
              row.system ? std::string(row.system->tab) :
                           g.console;
    const std::string want = NetplayLibrary::TitleKey(row.title);
    for (size_t i = 0; i < m_games.size(); i++)
    {
      const Library::Game& rom = m_games[i].rom;
      const SystemId s = rom.system;
      const bool fits = gb_link ? (s == SystemId::GB || s == SystemId::GBC) : gba_link ? s == SystemId::GBA :
                                                                               GetSystem(s).key == key;
      if (!fits)
        continue;
      uint32_t crc = 0;
      uint64_t size = 0;
      if (NetplayLibrary::Lookup(rom.path, &crc, &size) && crc == g.crc)
      {
        row.game = static_cast<int>(i);
        row.found = 1;
        break;
      }
      if (row.found == 0 && !want.empty() && NetplayLibrary::TitleKey(rom.title) == want)
      {
        row.game = static_cast<int>(i);
        row.found = 2;
      }
    }
    rows.push_back(std::move(row));
  }
  // ((01.07, the owner's 01.05 log said nothing of it) How many of the list are standard versions and ours, once a
  // list.)
  if (top.answered && top.serial != m_top_logged)
  {
    m_top_logged = top.serial;
    int standard = 0, same = 0, other = 0;
    for (const TopRow& r : rows)
    {
      standard += r.standard.empty() ? 0 : 1;
      same += r.found == 1 ? 1 : 0;
      other += r.found == 2 ? 1 : 0;
    }
    Log("online: the games most played online: " + std::to_string(rows.size()) + ", " + std::to_string(standard) +
        " standard versions (the others' version unknown), " + std::to_string(same) + " in our games (" +
        std::to_string(other) + " more by another version of their title)");
  }
  m_top_rows = std::move(rows);
  m_top_index = std::clamp(m_top_index, 0, std::max(0, static_cast<int>(m_top_rows.size()) - 1));
}

// The table in the rooms' box (y: under the summary line): its title, the columns' names, the games - rank, cover (ours)
// or a tile in its console's colour, title and standard name, console, version, matches, in our games or not -, the
// selected one lit; instead, before the server's answer or without matches, why.
void Menu::Impl::DrawTopTable(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const uint32_t GREEN = light ? 0x1E7F46 : 0x5BD38A, AMBER = light ? 0x9A5C00 : 0xFFC870,
                 RED = light ? 0xB3261E : 0xFF8A80;
  const auto raw = [dl](ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    dl->AddText(font, size, at, color, text.c_str());
  };
  const auto raw_w = [](ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  };
  const Netplay::TopList top = Netplay::TopGames();
  const std::string days = std::to_string(top.days > 0 ? top.days : NetProto::TOP_DAYS);
  Text(dl, m_bold, 26, ImVec2(x0 + 2, y + 4), Rgba(theme.wave, alpha), "Most played online in the last " + days + " days",
       false);
  y += 60.0f;
  const float w = x1 - x0;
  const float tx = x0 + 132.0f, col_console = x0 + w * 0.47f, col_version = x0 + w * 0.56f,
              col_matches = x0 + w * 0.74f, col_mine = x0 + w * 0.83f;
  constexpr float HEAD = 22.0f;
  const ImU32 head = Rgba(theme.text, alpha * 0.55f);
  Text(dl, m_bold, HEAD, ImVec2(x0 + 22.0f, y), head, "Game", false);
  Text(dl, m_bold, HEAD, ImVec2(col_console, y), head, "Console", false);
  Text(dl, m_bold, HEAD, ImVec2(col_version, y), head, "Version", false);
  Text(dl, m_bold, HEAD, ImVec2(col_matches, y), head, "Matches", false);
  Text(dl, m_bold, HEAD, ImVec2(col_mine, y), head, "In Your Games", false);
  y += 32.0f;
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Rgba(theme.text, alpha * 0.12f), 1.5f);
  y += 12.0f;
  const float list_top = y;
  const int count = static_cast<int>(m_top_rows.size());
  if (count == 0)
  {
    const float cx = (x0 + x1) * 0.5f, cy = list_top + std::min(170.0f, (bottom - list_top) * 0.35f);
    const std::string first = PS4Lang::T(!top.answered ? "Asking the server..." : "No matches these days");
    raw(m_bold, 32, ImVec2(cx - raw_w(m_bold, 32, first) * 0.5f, cy), Rgba(theme.text, alpha), first);
    const std::string second = PS4Lang::T("EmuSuNi's server counts the matches of the last " + days +
                                          " days by game: only the games and how many - nothing of anyone.");
    const std::string fitted = Fit(m_font, 24, second, w - 80.0f);
    raw(m_font, 24, ImVec2(cx - raw_w(m_font, 24, fitted) * 0.5f, cy + 54.0f), Rgba(theme.text, alpha * 0.65f), fitted);
    return;
  }
  constexpr float ROW_H = 78.0f, GAP = 8.0f;
  const float visible = std::max(1.0f, std::floor((bottom - list_top) / ROW_H));
  const float scroll = std::clamp(static_cast<float>(m_top_index) - std::floor((visible - 1.0f) * 0.5f), 0.0f,
                                  std::max(0.0f, static_cast<float>(count) - visible));
  dl->PushClipRect(ImVec2(x0 - 4.0f, list_top - 4.0f), ImVec2(x1 + 4.0f, bottom + 4.0f), true);
  for (int i = 0; i < count; i++)
  {
    const float ry = list_top + (static_cast<float>(i) - scroll) * ROW_H;
    if (ry < list_top - ROW_H || ry > bottom)
      continue;
    const TopRow& row = m_top_rows[static_cast<size_t>(i)];
    const bool sel = i == m_top_index;
    const float rh = ROW_H - GAP, cy = ry + rh * 0.5f;
    const float edge = std::clamp(std::min((ry - list_top) / 36.0f, (bottom - ry - rh) / 36.0f) + 1.0f, 0.0f, 1.0f);
    const float ra = alpha * edge, a = ra * (sel || row.found == 1 ? 1.0f : 0.8f);
    const ImVec2 r0(x0, ry), r1(x1, ry + rh);
    if (sel)
    {
      dl->AddRectFilled(r0, r1, Rgba(theme.wave, ra * 0.20f), 14.0f);
      dl->AddRect(r0, r1, Rgba(theme.wave, ra), 14.0f, 0, 2.5f);
    }
    else
      dl->AddRectFilled(r0, r1, light ? Rgba(0xFFFFFF, ra * 0.55f) : Rgba(0x000000, ra * 0.22f), 14.0f);
    // Its rank; its cover when it is one of ours, else a tile in its console's colour.
    const std::string rank = std::to_string(i + 1);
    raw(m_bold, 28, ImVec2(x0 + 40.0f - raw_w(m_bold, 28, rank) * 0.5f, cy - 17.0f), Rgba(theme.wave, a), rank);
    const ImVec2 b0(x0 + 68.0f, cy - 30.0f), b1(x0 + 114.0f, cy + 30.0f);
    float aspect = 0.7f;
    const ImTextureID cover = row.game >= 0 ? Cover(row.game, &aspect) : ImTextureID{};
    if (cover != ImTextureID{})
    {
      const float bw = b1.x - b0.x, bh = b1.y - b0.y;
      float cw = bh * std::max(0.2f, aspect), ch = bh;
      if (cw > bw)
      {
        cw = bw;
        ch = bw / std::max(0.2f, aspect);
      }
      const ImVec2 p0((b0.x + b1.x - cw) * 0.5f, cy - ch * 0.5f);
      dl->AddImageRounded(ImTextureRef(cover), p0, ImVec2(p0.x + cw, p0.y + ch), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, static_cast<int>(a * 255)), 4.0f);
    }
    else
    {
      const uint32_t tile = row.system ? row.system->color : 0x5C6070;
      dl->AddRectFilled(b0, b1, Rgba(tile, a * 0.9f), 6.0f);
      dl->AddRect(b0, b1, Rgba(0xFFFFFF, a * 0.2f), 6.0f, 0, 1.5f);
      DrawCartridgeIcon(dl, ImVec2((b0.x + b1.x) * 0.5f, cy), 28.0f, Rgba(0xFFFFFF, a * 0.85f), Rgba(tile, a));
    }
    // Its title, and under it its standard name (or that it isn't in the list).
    const float title_w = col_console - tx - 18.0f;
    raw(m_bold, 28, ImVec2(tx, ry + 7.0f), Rgba(theme.text, a), Fit(m_bold, 28, row.title, title_w));
    const std::string under = row.standard.empty() ? PS4Lang::T("Not in the standard list") : row.standard;
    raw(m_font, 22, ImVec2(tx, ry + 41.0f), Rgba(theme.text, a * 0.6f), Fit(m_font, 22, under, title_w));
    // Its console: a pill in its colour.
    {
      const uint32_t pill = row.system ? row.system->color : 0x6A6F7D;
      const float pw = raw_w(m_bold, 22, row.tab) + 24.0f;
      const ImVec2 p0(col_console, cy - 16.0f), p1(col_console + pw, cy + 16.0f);
      dl->AddRectFilled(p0, p1, Rgba(pill, a * 0.95f), 16.0f);
      dl->AddRect(p0, p1, Rgba(0xFFFFFF, a * 0.25f), 16.0f, 0, 1.5f);
      raw(m_bold, 22, ImVec2(col_console + 12.0f, cy - 14.0f), Rgba(0xFFFFFF, a), row.tab);
    }
    // Its version, its matches, whether it is in our games.
    // ((01.07, the owner's 01.05 test: blank) A game not in standard.bin - an old app's room by its whole file, a
    // hack -: "Unknown version".)
    const bool unknown = row.version.empty() && row.standard.empty();
    raw(m_font, 24, ImVec2(col_version, cy - 15.0f), Rgba(theme.text, a * (unknown ? 0.6f : 1.0f)),
        Fit(m_font, 24, unknown ? PS4Lang::T("Unknown version") : row.version.empty() ? std::string("-") : row.version,
            col_matches - col_version - 18.0f));
    raw(m_bold, 26, ImVec2(col_matches, cy - 16.0f), Rgba(theme.text, a), std::to_string(row.top.matches));
    const char* mine = row.found == 1 ? "Same version" : row.found == 2 ? "Another version" : "Not in your games";
    const uint32_t mine_color = row.found == 1 ? GREEN : row.found == 2 ? AMBER : RED;
    raw(m_font, 24, ImVec2(col_mine, cy - 15.0f), Rgba(mine_color, a * 0.95f),
        Fit(m_font, 24, PS4Lang::T(mine), x1 - col_mine - 16.0f));
  }
  dl->PopClipRect();
}

void Menu::Impl::OpenMyStats()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_MY_STATS;
  m_panel.title = "My Stats";
  const std::vector<MyStats::Game> games = MyStats::Games();
  int64_t seconds = 0;
  int sessions = 0, online = 0;
  std::map<std::string, int64_t> consoles;
  for (const MyStats::Game& g : games)
  {
    seconds += g.seconds;
    sessions += g.sessions;
    online += g.online;
    consoles[g.console] += g.seconds;
  }
  auto hours = [](int64_t s) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f h", static_cast<double>(s) / 3600.0);
    return std::string(text);
  };
  auto title_of = [this](const std::string& path) {
    for (const Game& game : m_games)
    {
      if (game.rom.path == path)
        return game.rom.title;
    }
    const size_t slash = path.find_last_of('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return name.substr(0, name.rfind('.'));
  };
  std::vector<std::string>& lines = m_panel.lines;
  lines.push_back("");
  lines.push_back("Time played: " + hours(seconds));
  lines.push_back("Games played: " + std::to_string(games.size()));
  lines.push_back("Sessions: " + std::to_string(sessions));
  lines.push_back("Online matches: " + std::to_string(online));
  const Achievements::History history = Achievements::GetHistory();
  uint32_t unlocked = 0, with = 0;
  for (const Achievements::HistoryGame& g : history.games)
  {
    unlocked += g.unlocked;
    with += g.unlocked > 0 ? 1 : 0;
  }
  const Achievements::Account account = Achievements::GetAccount();
  if (!account.user.empty())
    lines.push_back("Achievements: " + std::to_string(unlocked) + " in " + std::to_string(with) + " games (" +
                    std::to_string(account.casual_points + account.points) + " points)");
  else
    lines.push_back("Achievements: log in to RetroAchievements");
  if (!games.empty())
  {
    lines.push_back("");
    lines.push_back("Most played:");
    for (size_t i = 0; i < games.size() && i < 5; i++)
      lines.push_back(title_of(games[i].path) + ": " + hours(games[i].seconds));
    const auto last = std::max_element(games.begin(), games.end(),
                                       [](const MyStats::Game& a, const MyStats::Game& b) { return a.last < b.last; });
    lines.push_back("");
    lines.push_back("Last played: " + title_of(last->path) + " (" + AchievementDate(last->last) + ")");
    std::vector<std::pair<int64_t, std::string>> by;
    for (const auto& [key, s] : consoles)
      by.emplace_back(s, key);
    std::sort(by.rbegin(), by.rend());
    std::string line = "Consoles: ";
    for (size_t i = 0; i < by.size() && i < 3; i++)
    {
      const System* system = SystemByKey(by[i].second);
      line += (i ? ", " : "") + std::string(system ? system->tab : by[i].second.c_str()) + " " + hours(by[i].first);
    }
    lines.push_back(line);
  }
  Play("menu");
}

// A game's Triangle -> Play Online: a private room or a public one. (game -1: a Choose Together room.)
// (00.86) Our room is open (being made, or waiting in the room - not a match): it stays while we go elsewhere.
bool Menu::Impl::HostingRoom() const
{
  return Netplay::Hosting();
}

// (00.86) One room at a time: while ours is open, making or joining another asks first - "close it?" (Yes: closed, then
// `then`; No or Circle: nothing). False: no room of ours, `then` is the caller's to do.
bool Menu::Impl::AskCloseRoom(bool join, std::function<void()> then)
{
  if (!HostingRoom())
    return false;
  m_online_after_close = std::move(then);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_OL_REPLACE;
  m_panel.title = join ? "Join a Room" : "Create Room";
  m_panel.lines = {"", "You already have a room open.",
                   join ? "Close it and join this one?" : "Close it and create a new one?"};
  m_panel.options = {"Yes", "No"};
  m_online_actions = {"replace_yes", "replace_no"};
  m_panel.selected = 1;  // (No: a room with someone in it isn't closed by one press too many)
  Play("menu");
  return true;
}

void Menu::Impl::OpenOnlineHost(int game, bool link)
{
  if (game >= static_cast<int>(m_games.size()))
    return;
  if (AskCloseRoom(false, [this, game, link] { OpenOnlineHost(game, link); }))
    return;  // (00.86)
  m_online_game = game;
  m_online_link = link && game >= 0;
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ONLINE_HOST;
  m_panel.title = game < 0 ? "Choose Together" : "Play Online";
  m_panel.options = {"Private Room", "Public Room"};
  m_online_actions = {"private", "public"};
  // (00.95) Players: how many the room takes - at first the game's (2 when they aren't known), Left / Right change it;
  // not there when the game or its console takes 2.
  m_online_players_most = OnlinePlayersMost(game);
  m_online_players = 2;
  if (game >= 0)
  {
    const Library::Game& rom = m_games[static_cast<size_t>(game)].rom;
    uint32_t crc = 0;
    uint64_t size = 0;
    NetplayLibrary::Lookup(rom.path, &crc, &size);
    const int players = NetplayLibrary::Players(GetSystem(rom.system).key, rom.id, crc);
    m_online_players = std::clamp(players, 2, m_online_players_most);
  }
  if (m_online_players_most > 2)
  {
    m_panel.options.push_back("Players: " + std::to_string(m_online_players));
    m_online_actions.push_back("players");
    m_panel.helps = {"", "", OnlinePlayersHelp()};
  }
  if (game < 0)
    m_panel.lines = {"", "A room without a game: once your", "friend is in, you both see the games",
                     "you both have, and you pick one.", "",
                     "Private: your friend joins with its code.", "Public: anyone can join it."};
  else if (m_online_link)
  {
    // (00.95) A Game Boy link cable room: two players, each with a Game Boy of its own.
    m_panel.title = "Link Cable Online";
    m_panel.lines = {"", m_games[static_cast<size_t>(game)].rom.title, "",
                     "Your Game Boy plays this game; your", "friend's, the game it chooses.",
                     "Each console needs both games.", "", "Private: your friend joins with its code.",
                     "Public: anyone can join it."};
  }
  else
    m_panel.lines = {"", m_games[static_cast<size_t>(game)].rom.title, "",
                     "Private: your friend joins with its code.", "Public: anyone can join it.", "",
                     "The other console needs this game too."};
  Play("menu");
}

// The games of the consoles that play online, their files hashed on NetplayLibrary's thread (the files known by their
// size and time aren't read again): the Choose Together rooms' lists.
void Menu::Impl::StartOnlineLibrary()
{
  std::vector<NetplayLibrary::File> files;
  for (const Game& game : m_games)
  {
    const System& system = GetSystem(game.rom.system);
    // ((01.05) the link cable rooms' too - the Game Boys': Check My Games, the marks under the stars)
    if (PlaysOnline(game.rom.system) && game.rom.discs.size() <= 1)
      files.push_back({system.key, game.rom.path, game.rom.id});
  }
  NetplayLibrary::Start(m_data_dir, files);
  m_online_library_sent = false;
}

void Menu::Impl::OpenOnlineRoom()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_ONLINE_ROOM;
  m_online_actions.clear();  // (00.86: the panel before's aren't this one's selection)
  BuildOnlinePanel();
}

// (00.86) The notice of a player asking to join our room: Left / Right (Up / Down) choose Accept or Reject, Cross does
// it - Accept opens the room (on Start once the player is ready) -, Circle answers later (in the room, while the
// request lasts: the Online tab's table has it on top).
void Menu::Impl::OnlineAskInput(uint32_t pressed)
{
  const Netplay::View v = Netplay::GetView();
  if (v.asking < 0)
  {
    m_online_ask = -1;
    return;
  }
  if (pressed & (BTN_LEFT | BTN_RIGHT | BTN_UP | BTN_DOWN))
  {
    m_online_ask_choice = 1 - m_online_ask_choice;
    Play("move");
    return;
  }
  if (pressed & BTN_CIRCLE)
  {
    m_online_ask = -1;
    Log("online: the request to join left for later");
    ShowToast("Answer it in your room: Online tab", 0);
    Play("back");
    return;
  }
  if (!(pressed & BTN_CROSS))
    return;
  const int member = v.asking;
  m_online_ask = -1;
  if (m_online_ask_choice == 0)
  {
    Netplay::Admit(member);
    m_online_prefer = "start";
    OpenOnlineRoom();
    Play("confirm");
  }
  else
  {
    Netplay::Refuse(member);
    ShowToast(v.asking_nick + " was not accepted", 0);
    Play("back");
  }
}

// (00.86) The notice over the screen's middle: who asks (nickname, time zone), the room's game, the seconds left, Accept
// and Reject; the screen behind dimmed. Its buttons are the hints' (Cross, Circle: Later).
void Menu::Impl::DrawOnlineAsk(ImDrawList* dl, float dt)
{
  if (m_online_ask < 0)
  {
    m_online_ask_slide = 0.0f;
    return;
  }
  Approach(m_online_ask_slide, 1.0f, 14.0f, dt);
  const Netplay::View v = Netplay::GetView();
  if (v.asking < 0)
    return;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const float a = std::min(1.0f, m_online_ask_slide * 1.5f);
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(0x000000, 0.50f * a));
  constexpr float BOX_W = 1000.0f, BOX_H = 400.0f, PAD = 44.0f;
  const float x0 = W * 0.5f - BOX_W * 0.5f;
  const float y0 = H * 0.5f - BOX_H * 0.5f + (1.0f - m_online_ask_slide) * 40.0f;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + BOX_W, y0 + BOX_H),
                    light ? Rgba(0xFFFFFF, 0.96f * a) : Rgba(0x0B0A14, 0.94f * a), 22.0f);
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + BOX_W, y0 + BOX_H), Rgba(theme.wave, 0.9f * a), 22.0f, 0, 2.5f);
  float y = y0 + PAD - 6.0f;
  Text(dl, m_bold, 34, ImVec2(x0 + PAD, y), Rgba(theme.wave, a), "Join Request", false);
  y += 62.0f;
  // ((01.05) its country and time zone: "Ana (CL, UTC-3) wants to join your room")
  const std::string where =
      v.asking_country + (v.asking_country.empty() || v.asking_region.empty() ? "" : ", ") + v.asking_region;
  const std::string who = v.asking_nick + (where.empty() ? "" : " (" + where + ")") + " wants to join your room";
  Text(dl, m_bold, 32, ImVec2(x0 + PAD, y), Rgba(theme.text, a), Fit(m_bold, 32, who, BOX_W - PAD * 2), false);
  y += 52.0f;
  const System* system = SystemByKey(v.game.console);
  const std::string game = v.together ? std::string("Choose Together: you pick a game you both have.") :
                                        "Game: " + v.game.title + (system ? std::string(" (") + system->tab + ")" : "");
  Text(dl, m_font, 28, ImVec2(x0 + PAD, y), Rgba(theme.text, 0.85f * a), Fit(m_font, 28, game, BOX_W - PAD * 2),
       false);
  y += 44.0f;
  Text(dl, m_font, 26, ImVec2(x0 + PAD, y), Rgba(theme.text, 0.6f * a),
       "Answer within " + std::to_string(v.asking_seconds) + " s", false);
  // Accept and Reject, side by side.
  constexpr float BTN_W = 300.0f, BTN_H = 66.0f, GAP = 40.0f;
  const float by = y0 + BOX_H - PAD - BTN_H;
  const float bx = W * 0.5f - BTN_W - GAP * 0.5f;
  static const char* const LABELS[] = {"Accept", "Reject"};
  for (int i = 0; i < 2; i++)
  {
    const bool sel = i == m_online_ask_choice;
    const ImVec2 p0(bx + static_cast<float>(i) * (BTN_W + GAP), by), p1(p0.x + BTN_W, by + BTN_H);
    dl->AddRectFilled(p0, p1, sel ? Rgba(theme.wave, 0.35f * a) : Rgba(theme.text, 0.06f * a), 14.0f);
    dl->AddRect(p0, p1, sel ? Rgba(theme.wave, a) : Rgba(theme.text, 0.25f * a), 14.0f, 0, sel ? 2.5f : 1.5f);
    const float tw = TextWidth(sel ? m_bold : m_font, 32, LABELS[i]);
    Text(dl, sel ? m_bold : m_font, 32, ImVec2(p0.x + (BTN_W - tw) * 0.5f, by + (BTN_H - 32.0f) * 0.5f - 4.0f),
         Rgba(theme.text, a), LABELS[i], false);
  }
}

// The room's panel as it is now (every frame it shows: UpdateOnline).
void Menu::Impl::BuildOnlinePanel()
{
  using NetProto::LobbyGame;
  using Netplay::Phase;
  const Netplay::View v = Netplay::GetView();
  std::vector<std::string> options, actions, lines;
  // The room. (A Choose Together room with its two players: the list of the games both have. 00.86: the other one in
  // it - accepted by the host -, not one asking to join or a guest waiting for the host's answer.)
  bool other = false;
  for (const Netplay::PlayerView& p : v.players)
    other = other || (!p.me && !p.asking);
  if (v.together && v.phase == Phase::Room && other && !v.waiting_host)
  {
    m_online_prefer.clear();  // (its rows: no Start to wait for)
    BuildTogetherRows(v);
    return;
  }
  m_together.on = false;
  m_panel.rows.clear();
  const std::string you = " (you)";
  size_t notes_from = SIZE_MAX;  // (01.05) the lines after the players' (the room screen's notes)
  if (v.phase == Phase::Off)
  {
    m_panel.title = "Online Play";
    lines.push_back("");
    const std::string wrapped = WrapText(m_font, 28, PS4Lang::T(v.error.empty() ? "The room closed" : v.error), 540);
    for (size_t start = 0; start <= wrapped.size();)
    {
      const size_t end = std::min(wrapped.find('\n', start), wrapped.size());
      lines.push_back(wrapped.substr(start, end - start));
      start = end + 1;
    }
    options = {"OK"};
    actions = {"close"};
    // (01.05) Its match started, and its host lets spectators watch it.
    if (!v.watch_offer.empty())
    {
      options.insert(options.begin(), "Watch the Match");
      actions.insert(actions.begin(), "watch:" + v.watch_offer);
    }
  }
  else if (v.phase == Phase::Connecting)
  {
    m_panel.title = v.host ? "Play Online" : "Online Room";
    lines = {"", "Connecting to the online server..."};
    options = {v.host ? "Close Room" : "Leave Room"};
    actions = {"leave"};
  }
  else
  {
    m_panel.title = v.host ? "Play Online" : "Online Room";
    const System* system = SystemByKey(v.game.console);
    if (v.host)
    {
      lines.push_back(v.is_public ? "Public room: anyone can join it." : "Private room: tell your friend the code.");
    }
    else
    {
      lines.push_back("Room " + v.code);
      for (const Netplay::PlayerView& p : v.players)
      {
        if (p.id == 0)
        {
          // ((01.05) its country and time zone: "(CL, UTC-3)")
          const std::string where = p.country + (p.country.empty() || p.region.empty() ? "" : ", ") + p.region;
          lines.push_back("Host: " + p.nick + (where.empty() ? "" : " (" + where + ")"));
        }
      }
    }
    // (Short: under the code and the options the panel holds about seven lines above its help text.)
    if (v.together)
      lines.push_back("Choose Together: you pick a game you both have.");
    else if (v.link)
      lines.push_back("Link cable: " + v.game.title + " (Game Boy 1)");  // (00.95)
    else
      lines.push_back("Game: " + v.game.title + (system ? std::string(" (") + system->tab + ")" : std::string()));
    if (!v.host && !v.together)
    {
      // ((01.05) ours as its file's name says it - "Europe, Rev 1" -, else its title)
      std::string mine = OnlineVersionText(m_online_game);
      if (mine.empty() && m_online_game >= 0 && m_online_game < static_cast<int>(m_games.size()))
        mine = m_games[static_cast<size_t>(m_online_game)].rom.title;
      lines.push_back(m_online_found == LobbyGame::Same      ? std::string("You have this game.") :
                      m_online_found == LobbyGame::Missing   ? std::string("You haven't got this game.") :
                      m_online_found == LobbyGame::Looking   ? std::string("Looking for it in your games...") :
                                                                "Yours is another version: " + mine);
    }
    int guests = 0, in_room = 0;
    for (const Netplay::PlayerView& p : v.players)
      in_room += p.asking ? 0 : 1;
    // (00.95) A room of three or four: how many are in it; a guest who has the game says nothing more (the lines fit).
    if (v.max_players > 2)
      lines.push_back("Players: " + std::to_string(in_room) + " of " + std::to_string(v.max_players));
    for (const Netplay::PlayerView& p : v.players)
    {
      if (p.me)
      {
        lines.push_back(p.nick + you);
        continue;
      }
      if (p.asking)
        continue;  // (00.86: asking to join - the request's lines below, not one of the room's players yet)
      guests += p.id != 0 ? 1 : 0;
      lines.push_back(p.nick + " - " + (p.ping_ms >= 0 ? std::to_string(p.ping_ms) + " ms" : std::string("...")));
      if (v.host && !(v.max_players > 2 && p.game == LobbyGame::Same))
      {
        lines.push_back(p.game == LobbyGame::Same      ? p.nick + " has the game" :
                        p.game == LobbyGame::Different ? p.nick + " plays another version" :
                        p.game == LobbyGame::Missing   ? p.nick + " hasn't got the game" :
                        p.game == LobbyGame::Asking    ? p.nick + " has another version: choosing" :
                                                         p.nick + " is looking for the game");
      }
    }
    notes_from = lines.size();  // (01.05: the room screen shows what follows as the room's notes)
    // (01.23) Back from its match: why it ended ("Ana ended the match", "Ana left the room"), until the next one.
    if (!v.together && !v.notice.empty() && v.phase == Phase::Room)
      lines.push_back(v.notice);
    // (00.95) A Game Boy link cable room: the game of Game Boy 2 - the guest's own -, and whether the host has it.
    if (v.link)
    {
      if (!v.link_game_in && v.host)
        lines.push_back("Your friend is choosing its game...");
      else if (v.link_game_in)
        lines.push_back((v.host ? std::string("Their game: ") : std::string("Your game: ")) + v.link_title +
                        " (Game Boy 2)");
      if (v.link_game_in && v.link_found == LobbyGame::Missing)
        lines.push_back(v.host ? "You haven't got it: they choose another." : "The host hasn't got it: choose another.");
    }
    if (v.hidden)
      lines.push_back("Your address is hidden.");
    if (v.phase == Phase::Starting || v.phase == Phase::Playing)
    {
      lines.push_back("Starting...");
    }
    else if (v.host)
    {
      if (v.asking >= 0)
      {
        // (00.86) A player asks to join: Accept or Reject, first. ((01.05) Its country and time zone.)
        const std::string where = v.asking_country +
                                  (v.asking_country.empty() || v.asking_region.empty() ? "" : ", ") + v.asking_region;
        lines.push_back(v.asking_nick + (where.empty() ? "" : " (" + where + ")") + " wants to join your room");
        lines.push_back("Answer within " + std::to_string(v.asking_seconds) + " s");
        options.push_back("Accept");
        actions.push_back("accept:" + std::to_string(v.asking));
        options.push_back("Reject");
        actions.push_back("reject:" + std::to_string(v.asking));
      }
      else
      {
        // ((01.14) Alone, with Late Joining: Start plays now, whoever comes joins the match.)
        lines.push_back(guests == 0 && v.can_start ? "Press Start to play now: whoever comes joins the match." :
                        guests == 0                ? "Waiting for a player..." :
                        v.can_start                ? "Press Start to play." :
                        guests == 1  ? "Waiting for the player..." :
                                       "Waiting for the players...");  // (00.95)
      }
      if (v.can_start)
      {
        options.push_back("Start");
        actions.push_back("start");
      }
      // (01.05) The chat, and each player's actions (mute, block, remove: a picker - until 01.04 Remove alone here).
      options.push_back("Quick Phrase");
      actions.push_back("chat:phrase");
      options.push_back("Write a Message");
      actions.push_back("chat:text");
      for (const Netplay::PlayerView& p : v.players)
      {
        if (!p.me && !p.asking)
        {
          options.push_back(p.nick + "...");
          actions.push_back("player:" + std::to_string(p.id));
        }
      }
      // (01.14) Whoever comes is in at once, or asks first.
      options.push_back(v.free_entry ? "Entry: Free" : "Entry: With Permission");
      actions.push_back("entry");
      // (01.05) A player joining the match under way: let or not.
      if (v.late_possible)
      {
        options.push_back(v.late_allowed ? "Late Joining: Yes" : "Late Joining: No");
        actions.push_back("late");
      }
      // (01.05) Spectators: let in or not, and who watches.
      if (v.watch_possible)
      {
        options.push_back(v.watch_allowed ? "Spectators: Yes" : "Spectators: No");
        actions.push_back("watchers");
        if (!v.watchers.empty())
        {
          std::string who;
          for (const Netplay::PlayerView& w : v.watchers)
            who += (who.empty() ? "" : ", ") + w.nick;
          lines.push_back("Watching: " + who);
        }
      }
      options.push_back("Close Room");
      actions.push_back("leave");
    }
    else if (v.watching)
    {
      // (01.05) A spectator: it waits for the match (or for its state, the match on), then watches it.
      lines.push_back("You watch this room's match: you play nothing.");
      lines.push_back("It shows here once the host starts it.");
      options.push_back("Stop Watching");
      actions.push_back("leave");
    }
    else if (v.waiting_host)
    {
      // (00.86) Our request to join, until the host answers (Circle or Leave Room: we stop asking).
      lines.push_back("Waiting for the host to accept you...");
      lines.push_back("The host has " + std::to_string(v.waiting_seconds) + " s to answer");
      options.push_back("Leave Room");
      actions.push_back("leave");
    }
    else
    {
      lines.push_back("Waiting for the host to start...");
      if (v.link && m_online_found == LobbyGame::Same)
      {
        options.push_back("Choose Your Game");  // (00.95: Game Boy 2's)
        actions.push_back("linkgame");
      }
      if (m_online_found == LobbyGame::Asking)
      {
        options.push_back("Play Anyway");
        actions.push_back("anyway");
      }
      // (01.05) The chat, and each player's actions (mute, block).
      options.push_back("Quick Phrase");
      actions.push_back("chat:phrase");
      options.push_back("Write a Message");
      actions.push_back("chat:text");
      for (const Netplay::PlayerView& p : v.players)
      {
        if (!p.me && !p.asking)
        {
          options.push_back(p.nick + "...");
          actions.push_back("player:" + std::to_string(p.id));
        }
      }
      options.push_back("Leave Room");
      actions.push_back("leave");
    }
  }
  // (01.05) The room screen's notes: what the room says once its players are listed (the game found too, a guest's).
  m_room_notes.clear();
  if (!v.host && !v.together && v.phase != Phase::Off && v.phase != Phase::Connecting && notes_from > 0)
  {
    for (const std::string& l : lines)
    {
      if (l.rfind("You have this game", 0) == 0 || l.rfind("You haven't got", 0) == 0 || l.rfind("Looking for it", 0) == 0 ||
          l.rfind("Yours is another version", 0) == 0)
        m_room_notes.push_back(l);
    }
  }
  for (size_t i = notes_from; i < lines.size(); i++)
    m_room_notes.push_back(lines[i]);
  // (The selection stays on its action.)
  const std::string was = m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_online_actions.size()) ?
                              m_online_actions[static_cast<size_t>(m_panel.selected)] :
                              std::string();
  m_panel.options = std::move(options);
  m_panel.lines = std::move(lines);
  m_online_actions = std::move(actions);
  // (01.05) A player's entry says what its picker does (the other options: OptionHelp's for their text).
  m_panel.helps.assign(m_online_actions.size(), std::string());
  for (size_t i = 0; i < m_online_actions.size(); i++)
  {
    if (m_online_actions[i].rfind("player:", 0) == 0)
      m_panel.helps[i] = v.host ? "Mute this player (voice and chat), block them on this PS4, or remove them." :
                                  "Mute this player (voice and chat), or block them on this PS4.";
  }
  m_panel.selected = 0;
  bool kept = false;
  for (size_t i = 0; i < m_online_actions.size(); i++)
  {
    if (m_online_actions[i] == was)
    {
      m_panel.selected = static_cast<int>(i);
      kept = true;
    }
  }
  // (00.86) A request that came while the panel shows: on Accept. A player just accepted: on Start once the player is
  // ready. A player's option gone under the selection (answered, expired, left): none selected until the player moves
  // ("-") - a second Cross on that place isn't Remove or Close Room.
  if (!kept && m_online_prefer.empty() &&
      (was.rfind("accept:", 0) == 0 || was.rfind("reject:", 0) == 0 || was.rfind("kick:", 0) == 0))
    m_online_prefer = "-";
  if (!m_online_prefer.empty())
  {
    bool found = false;
    for (size_t i = 0; i < m_online_actions.size() && !found; i++)
    {
      if (m_online_actions[i].rfind(m_online_prefer, 0) == 0)
      {
        m_panel.selected = static_cast<int>(i);
        found = true;
      }
    }
    if (found || v.phase != Phase::Room)
      m_online_prefer.clear();
    else
      m_panel.selected = -1;
  }
}

// A Choose Together room with its players (00.92: the whole screen, DrawTogetherScreen): how far the lists are, the
// games both have - titles and covers from this console's own games -, those known to take 2 players or more first (a
// known single-player game left out), those not known after them while Show All Common Games is on (Square), by
// console in tabs (L1 / R1: every console's first, then each console's with games, in the consoles' order, with how
// many); the host picks one (Cross), a guest suggests one (the host sees it marked). The tab and the selected game
// stay (by their console, by the game) while the lists change.
void Menu::Impl::BuildTogetherRows(const Netplay::View& v)
{
  m_panel.options.clear();
  m_panel.lines.clear();
  m_panel.rows.clear();
  m_online_actions.clear();
  m_panel.title = v.host ? "Room " + v.code : std::string("Online Room");
  TogetherScreen& t = m_together;
  t.on = true;
  t.status.clear();
  t.known = t.unknown = 0;
  std::vector<TogetherCard> all;
  // (00.95) The room's players (each console's): a game for fewer is left out as a single-player one - three or four
  // play only games for that many.
  int need = 0;
  for (const Netplay::PlayerView& p : v.players)
    need += p.asking ? 0 : std::max(1, p.locals);
  need = std::max(need, 2);
  t.need = need;
  const NetplayLibrary::Progress progress = NetplayLibrary::GetProgress();
  if (!progress.finished)
    t.status = "Looking at your games: " + std::to_string(progress.done) + " of " + std::to_string(progress.total);
  else if (!v.common_ready)
    t.status = v.host ? (need > 2 ? "Waiting for the other players' games..." : "Waiting for the other player's games...") :
                        (need > 2 ? "Waiting for the games you all have..." : "Waiting for the games you both have...");
  else
  {
    for (const NetProto::LibraryGame& g : v.common)
    {
      const auto at = m_online_library.find(g);
      if (at == m_online_library.end())
        continue;
      const Library::Game& rom = m_games[static_cast<size_t>(at->second)].rom;
      TogetherCard card;
      card.game = at->second;
      card.players = NetplayLibrary::Players(g.console, rom.id, g.crc);
      card.suggested = v.has_suggestion && v.suggested == g;
      if (card.players > 0 && card.players < need)
        continue;  // (a game for one player: nothing to play together; 00.95: for fewer than the room's players)
      t.unknown += card.players == 0 ? 1 : 0;
      t.known += card.players > 0 ? 1 : 0;
      if (card.players == 0 && !m_online_show_all)
        continue;
      all.push_back(card);
    }
    std::stable_sort(all.begin(), all.end(), [this](const TogetherCard& a, const TogetherCard& b) {
      if ((a.players == 0) != (b.players == 0))
        return a.players != 0;
      return m_games[static_cast<size_t>(a.game)].rom.title < m_games[static_cast<size_t>(b.game)].rom.title;
    });
    if (all.empty() && need > 2)
      t.status = t.unknown > 0 ? "No game you all have is known to take " + std::to_string(need) +
                                     " players: Square shows them all" :
                                 "You have no games for " + std::to_string(need) + " players in common";
    else if (all.empty())
      t.status = t.unknown > 0 ? "No game you both have is known to take 2 players: Square shows them all" :
                                 "No games in common";  // (01.04: and why, under it)
  }
  // (00.92) A new room: on every console's tab ((01.07) the Online tab's console filter is no more).
  if (v.code != m_together_room)
  {
    m_together_room = v.code;
    m_together_tab.clear();
    m_together_game = -1;
    m_together_said.clear();
  }
  // The tabs: every console's, then each console with games.
  t.tabs = {""};
  t.counts = {static_cast<int>(all.size())};
  for (const System& system : Systems())
  {
    int n = 0;
    for (const TogetherCard& card : all)
      n += GetSystem(m_games[static_cast<size_t>(card.game)].rom.system).key == std::string(system.key) ? 1 : 0;
    if (n > 0)
    {
      t.tabs.push_back(system.key);
      t.counts.push_back(n);
    }
  }
  t.tab = 0;
  for (size_t i = 0; i < t.tabs.size(); i++)
  {
    if (t.tabs[i] == m_together_tab)
      t.tab = static_cast<int>(i);
  }
  t.cards.clear();
  for (const TogetherCard& card : all)
  {
    if (t.tab == 0 || GetSystem(m_games[static_cast<size_t>(card.game)].rom.system).key == t.tabs[static_cast<size_t>(t.tab)])
      t.cards.push_back(card);
  }
  t.index = 0;
  for (size_t i = 0; i < t.cards.size(); i++)
  {
    if (t.cards[i].game == m_together_game)
      t.index = static_cast<int>(i);
  }
  m_together_game = t.cards.empty() ? -1 : t.cards[static_cast<size_t>(t.index)].game;
  // (00.92) In the log once the lists are in, and when the tab or the counts change: each tab's games, the one shown.
  if (v.common_ready)
  {
    std::string said = "online: Choose Together: " + std::to_string(t.known) + " games for " + std::to_string(need) +
                       " players or more, " +
                       std::to_string(t.unknown) + " not known (" + (m_online_show_all ? "shown" : "hidden") +
                       "); tabs:";
    for (size_t i = 0; i < t.tabs.size(); i++)
    {
      const System* system = t.tabs[i].empty() ? nullptr : SystemByKey(t.tabs[i]);
      said += std::string(i == 0 ? " " : ", ") + (system ? system->tab : "All") + " " + std::to_string(t.counts[i]);
    }
    const std::string& tab = t.tabs[static_cast<size_t>(t.tab)];
    const System* shown = tab.empty() ? nullptr : SystemByKey(tab);
    said += std::string("; showing ") + (shown ? shown->tab : "All") + " (" + std::to_string(t.cards.size()) + ")";
    if (said != m_together_said)
    {
      m_together_said = said;
      Log(said);
    }
  }
}

// (00.92) A Choose Together room's screen, the whole screen over the menu: the title and the room's code (the host's),
// the players' latency bar (or, hidden, their pings), the console tabs with how many games each, the tab's games as
// cards - cover, title, console and players - scrolling by rows with the selection; the lists' progress instead while
// they come. Its buttons are DrawHints'.
void Menu::Impl::DrawTogetherScreen(ImDrawList* dl, float dt)
{
  const float a = m_panel.slide;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const uint32_t AMBER = light ? 0x9A5C00 : 0xFFC870;
  const Netplay::View v = Netplay::GetView();
  TogetherScreen& t = m_together;
  // (Players' and games' own words - nicknames, titles - drawn as they are, not looked up in the language file.)
  const auto raw = [dl](ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    dl->AddText(font, size, at, color, text.c_str());
  };
  const auto raw_w = [](ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  };
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), light ? Rgba(0xF4F4F8, 0.97f * a) : Rgba(0x06060A, 0.95f * a));
  constexpr float X0 = 80.0f;
  const float x1 = W - 80.0f;
  // (01.05) The games' grid left of the room's chat (TogetherScreen::COLS cards a row there).
  const float gx1 = x1 - 500.0f;
  // The title, the room's code (the host's), the versions' check and how many games; what the room says.
  Text(dl, m_bold, 44, ImVec2(X0, 52), Rgba(theme.text, a), "Choose Together");
  if (v.host && !v.code.empty())
    raw(m_bold, 32, ImVec2(X0 + TextWidth(m_bold, 44, "Choose Together") + 30.0f, 62), Rgba(theme.wave, a),
        PS4Lang::T("Room " + v.code));
  std::string sub = PS4Lang::T(std::string(t.need > 2 ? "Same EmuSuNi version on all: " : "Same EmuSuNi version on both: ") +
                               App::VERSION);
  if (t.status.empty())
    sub += "  \xC2\xB7  " + PS4Lang::T(t.known == 1 ? std::string("1 game you can play together") :
                                                      std::to_string(t.known) + " games you can play together");
  raw(m_font, 24, ImVec2(X0, 118), Rgba(theme.text, 0.7f * a), sub);
  if (!v.notice.empty())
    raw(m_font, 24, ImVec2(X0, 152), Rgba(AMBER, a), Fit(m_font, 24, PS4Lang::T(v.notice), x1 - X0 - 440.0f));
  // The players: the latency bar, or (hidden) each one's nickname and ping.
  std::vector<Netplay::PlayerView> players;
  for (const Netplay::PlayerView& p : v.players)
  {
    if (!p.asking)
      players.push_back(p);
  }
  if (Netplay::GetStatus().latency_bar)
  {
    NetplayUi::DrawLatencyBar(dl, m_font, m_bold, players, ImVec2(x1, players.size() > 3 ? 28.0f : 40.0f), 0, a);
  }
  else
  {
    float py = 52.0f;
    for (const Netplay::PlayerView& p : players)
    {
      if (p.me)
        continue;
      // ((01.05) its country beside the ping)
      const std::string line = p.nick + (p.region.empty() ? "" : " (" + p.region + ")") + "  " +
                               (p.country.empty() ? "" : p.country + "  ") +
                               (p.ping_ms >= 0 ? std::to_string(p.ping_ms) + " ms" : std::string("..."));
      raw(m_font, 26, ImVec2(x1 - raw_w(m_font, 26, line), py), Rgba(theme.text, 0.85f * a), line);
      py += 36.0f;
    }
  }
  // (01.05) The room's chat, at the right.
  DrawChatBox(dl, v, gx1 + 50.0f, 196.0f, x1, HELP_END - 10.0f, a);
  // The tabs: L1, every console's and each console's with its count, R1 (scrolled to keep the chosen one in view).
  constexpr float TAB_Y = 196.0f, TAB_H = 46.0f, TAB_FONT = 24.0f, GLYPH = 36.0f;
  std::vector<std::string> labels;
  std::vector<float> widths;
  for (size_t i = 0; i < t.tabs.size(); i++)
  {
    const System* system = t.tabs[i].empty() ? nullptr : SystemByKey(t.tabs[i]);
    labels.push_back((system ? std::string(system->tab) : PS4Lang::T("All")) + "  " + std::to_string(t.counts[i]));
    widths.push_back(raw_w(m_bold, TAB_FONT, labels.back()) + 36.0f);
  }
  float x = X0 + PS4Glyphs::Draw(dl, m_bold, "L1", X0, TAB_Y + TAB_H * 0.5f, GLYPH, a) + 14.0f;
  const float tabs_end = gx1 - PS4Glyphs::Width("R1", GLYPH) - 14.0f;
  size_t first = 0;
  const auto span = [&](size_t from, size_t to) {
    float w = 0.0f;
    for (size_t i = from; i <= to && i < widths.size(); i++)
      w += widths[i] + 10.0f;
    return w;
  };
  while (first < static_cast<size_t>(t.tab) && span(first, static_cast<size_t>(t.tab)) > tabs_end - x)
    first++;
  for (size_t i = first; i < labels.size() && x + widths[i] <= tabs_end; i++)
  {
    const bool on = static_cast<int>(i) == t.tab;
    const ImVec2 p0(x, TAB_Y), p1(x + widths[i], TAB_Y + TAB_H);
    const System* system = t.tabs[i].empty() ? nullptr : SystemByKey(t.tabs[i]);
    if (on)
    {
      dl->AddRectFilled(p0, p1, Rgba(system ? system->color : theme.wave, a * 0.9f), TAB_H * 0.5f);
      dl->AddRect(p0, p1, Rgba(0xFFFFFF, a * 0.5f), TAB_H * 0.5f, 0, 2.0f);
    }
    else
    {
      dl->AddRectFilled(p0, p1, light ? Rgba(0xFFFFFF, a * 0.7f) : Rgba(0xFFFFFF, a * 0.06f), TAB_H * 0.5f);
      dl->AddRect(p0, p1, Rgba(theme.text, a * 0.18f), TAB_H * 0.5f, 0, 1.5f);
    }
    raw(m_bold, TAB_FONT, ImVec2(x + 18.0f, TAB_Y + (TAB_H - TAB_FONT) * 0.5f - 3.0f),
        on ? Rgba(0xFFFFFF, a) : Rgba(theme.text, a * 0.7f), labels[i]);
    x += widths[i] + 10.0f;
  }
  PS4Glyphs::Draw(dl, m_bold, "R1", x + 4.0f, TAB_Y + TAB_H * 0.5f, GLYPH, a);
  // The games, or why there are none yet.
  const float top = 272.0f, bottom = H - 128.0f;
  if (t.cards.empty())
  {
    const std::string status = t.status.empty() ? std::string("No games in common") : t.status;
    // ((01.05) Centred in the grid's place, left of the chat; wrapped to it.)
    const float gw = gx1 - X0;
    const std::string text = WrapText(m_bold, 32, PS4Lang::T(status), gw);
    const float tw = std::min(gw, raw_w(m_bold, 32, text));
    raw(m_bold, 32, ImVec2(X0 + (gw - tw) * 0.5f, (top + bottom) * 0.5f - 20.0f), Rgba(theme.text, 0.8f * a), text);
    if (status == "No games in common")
    {
      // (01.04, the owner: strangers met in Choose Together and left - the same file is needed, and few know it.)
      const std::string why =
          WrapText(m_font, 24, PS4Lang::T("You all need the same file of the game (the same version and region)."), gw);
      const float ww = std::min(gw, raw_w(m_font, 24, why));
      raw(m_font, 24, ImVec2(X0 + (gw - ww) * 0.5f, (top + bottom) * 0.5f + 26.0f), Rgba(theme.text, 0.65f * a), why);
    }
    if (m_room_picker.open)
      DrawRoomPicker(dl, a);  // (01.05)
    return;
  }
  constexpr int COLS = TogetherScreen::COLS;
  constexpr float GAP = 20.0f, CARD_H = 212.0f, BOX_H = 136.0f;  // (three rows: 15 games in view)
  const float card_w = (gx1 - X0 - GAP * static_cast<float>(COLS - 1)) / static_cast<float>(COLS);
  const int count = static_cast<int>(t.cards.size());
  const int rows = (count + COLS - 1) / COLS;
  const float visible = std::max(1.0f, std::floor((bottom - top + GAP) / (CARD_H + GAP)));
  const float target = std::clamp(static_cast<float>(t.index / COLS) - std::floor((visible - 1.0f) * 0.5f), 0.0f,
                                  std::max(0.0f, static_cast<float>(rows) - visible));
  Approach(t.scroll, target, 14.0f, dt);
  dl->PushClipRect(ImVec2(X0 - 8.0f, top - 8.0f), ImVec2(gx1 + 8.0f, bottom + 8.0f), true);
  for (int i = 0; i < count; i++)
  {
    const float cy = top + (static_cast<float>(i / COLS) - t.scroll) * (CARD_H + GAP);
    if (cy > bottom || cy + CARD_H < top)
      continue;
    const TogetherCard& card = t.cards[static_cast<size_t>(i)];
    const Library::Game& rom = m_games[static_cast<size_t>(card.game)].rom;
    const System& system = GetSystem(rom.system);
    const float cx = X0 + static_cast<float>(i % COLS) * (card_w + GAP);
    const bool sel = i == t.index;
    // (A card half out of the box fades at its edge.)
    const float edge = std::clamp(std::min((cy - top) / 60.0f, (bottom - cy - CARD_H) / 60.0f) + 1.0f, 0.25f, 1.0f);
    const float ca = a * edge;
    const ImVec2 c0(cx, cy), c1(cx + card_w, cy + CARD_H);
    if (sel)
    {
      dl->AddRectFilled(c0, c1, Rgba(theme.wave, ca * 0.22f), 14.0f);
      dl->AddRect(c0, c1, Rgba(theme.wave, ca), 14.0f, 0, 3.0f);
    }
    else
    {
      dl->AddRectFilled(c0, c1, light ? Rgba(0xFFFFFF, ca * 0.8f) : Rgba(0xFFFFFF, ca * 0.05f), 14.0f);
    }
    // The cover, fit in its box (its own shape), else a tile in its console's colour.
    const ImVec2 b0(cx + 12.0f, cy + 10.0f), b1(cx + card_w - 12.0f, cy + 10.0f + BOX_H);
    float aspect = 0.7f;
    const ImTextureID cover = Cover(card.game, &aspect);
    if (cover != ImTextureID{})
    {
      const float bw = b1.x - b0.x, bh = b1.y - b0.y;
      float w = bh * std::max(0.2f, aspect), h = bh;
      if (w > bw)
      {
        w = bw;
        h = bw / std::max(0.2f, aspect);
      }
      const ImVec2 p0((b0.x + b1.x - w) * 0.5f, (b0.y + b1.y - h) * 0.5f);
      dl->AddImageRounded(ImTextureRef(cover), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, static_cast<int>(ca * 255)), 6.0f);
    }
    else
    {
      dl->AddRectFilled(b0, b1, Rgba(system.color, ca * 0.9f), 8.0f);
      DrawCartridgeIcon(dl, ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), 48.0f, Rgba(0xFFFFFF, ca * 0.85f),
                        Rgba(system.color, ca));
    }
    // The title; under it the console (every console's tab), the players, the guest's suggestion.
    raw(m_bold, 23, ImVec2(cx + 12.0f, cy + BOX_H + 16.0f), Rgba(theme.text, ca),
        Fit(m_bold, 23, rom.title, card_w - 24.0f));
    std::string info = card.players > 0 ? std::to_string(card.players) + "P" : std::string("?");
    if (t.tab == 0)
      info = std::string(system.tab) + "  \xC2\xB7  " + info;
    raw(m_font, 21, ImVec2(cx + 12.0f, cy + BOX_H + 46.0f), Rgba(theme.text, ca * 0.65f), info);
    if (card.suggested)
    {
      const std::string& mark = PS4Lang::T("Suggested");
      const float mw = raw_w(m_bold, 19, mark) + 18.0f;
      const ImVec2 m0(cx + card_w - 12.0f - mw, cy + BOX_H + 44.0f), m1(cx + card_w - 12.0f, cy + BOX_H + 70.0f);
      dl->AddRectFilled(m0, m1, Rgba(theme.wave, ca * 0.9f), 14.0f);
      raw(m_bold, 19, ImVec2(m0.x + 9.0f, m0.y + 3.0f), Rgba(0xFFFFFF, ca), mark);
    }
  }
  dl->PopClipRect();
  // More rows than fit: where the view is, at the right.
  if (static_cast<float>(rows) > visible)
  {
    const float track_x = gx1 + 24.0f, h = bottom - top;
    dl->AddRectFilled(ImVec2(track_x, top), ImVec2(track_x + 4.0f, top + h), Rgba(theme.text, a * 0.10f), 2.0f);
    const float thumb = std::max(30.0f, h * visible / static_cast<float>(rows));
    const float at = top + (h - thumb) * t.scroll / std::max(1.0f, static_cast<float>(rows) - visible);
    dl->AddRectFilled(ImVec2(track_x, at), ImVec2(track_x + 4.0f, at + thumb), Rgba(theme.wave, a * 0.8f), 2.0f);
  }
  // The selected game: when its number of players isn't known, said under the cards.
  if (t.cards[static_cast<size_t>(t.index)].players == 0)
    Text(dl, m_font, 22, ImVec2(X0, bottom + 14.0f), Rgba(theme.text, 0.6f * a),
         "How many players it takes isn't known.");
  if (m_room_picker.open)
    DrawRoomPicker(dl, a);  // (01.05)
}

// (00.92) A Choose Together room's screen: arrows through the cards, L1 / R1 the console tabs, Cross plays the game
// (the host) or suggests it (a guest), Square Show All Common Games, Triangle Close Room (a guest: Leave Room). (Circle
// is OnlinePanelInput's: the host's room stays open, a guest leaves.)
// ---- (01.05, the owner) The room on the whole screen, and its chat ----

// The room (waiting, starting): the title and the room's kind on top, the room's code (the host's) at the right; the
// game's cover, its title and what the room says on the left; its players - nickname, time zone, ping, voice - and the
// options (BuildOnlinePanel's: Start, the chat, a player's actions, Leave) in the middle; the room's chat on the
// right. Readable on a TV: 26 px at least, the players and the options 30.
bool Menu::Impl::RoomSetting(const std::string& action)
{
  return action == "entry" || action == "late" || action == "watchers";
}

std::vector<Menu::Impl::RoomPlace> Menu::Impl::RoomPlaces() const
{
  std::vector<RoomPlace> places(m_online_actions.size());
  bool settings = false;
  std::vector<int> ids;  // (the cards top to bottom: by member id, as drawn)
  for (const std::string& a : m_online_actions)
  {
    settings = settings || RoomSetting(a) || a.rfind("reject:", 0) == 0;
    if (a.rfind("player:", 0) == 0)
      ids.push_back(std::atoi(a.c_str() + 7));
  }
  std::sort(ids.begin(), ids.end());
  int rows[2] = {0, 0}, spread = 0;
  for (size_t i = 0; i < m_online_actions.size(); i++)
  {
    const std::string& a = m_online_actions[i];
    if (a.rfind("player:", 0) == 0)
    {
      const int id = std::atoi(a.c_str() + 7);
      places[i] = {-1, static_cast<int>(std::find(ids.begin(), ids.end(), id) - ids.begin())};
    }
    else if (settings)
    {
      const int column = RoomSetting(a) || a.rfind("reject:", 0) == 0 ? 1 : 0;
      places[i] = {column, rows[column]++};
    }
    else
    {
      places[i] = {spread % 2, spread / 2};
      spread++;
    }
  }
  return places;
}

bool Menu::Impl::RoomScreenShown() const
{
  if (!m_panel.open || m_panel.setting != SET_ONLINE_ROOM || m_together.on)
    return false;
  const Netplay::Phase phase = Netplay::GetView().phase;
  return phase == Netplay::Phase::Room || phase == Netplay::Phase::Starting;
}

void Menu::Impl::MoveRoomSelection(uint32_t pressed)
{
  const std::vector<RoomPlace> places = RoomPlaces();
  const int n = static_cast<int>(places.size());
  if (n == 0)
    return;
  const auto at = [&](int column, int row) {
    for (int i = 0; i < n; i++)
    {
      if (places[static_cast<size_t>(i)].column == column && places[static_cast<size_t>(i)].row == row)
        return i;
    }
    return -1;
  };
  const int selected = m_panel.selected;
  int to = selected;
  if (selected < 0 || selected >= n)
    to = at(0, 0) >= 0 ? at(0, 0) : 0;  // (none selected - an option gone from under it: the first one)
  else if (pressed & (BTN_UP | BTN_DOWN))
  {
    // The cards, then the column's options, top to bottom (from a card: the actions' column).
    const uint32_t arrow = (pressed & BTN_UP) ? BTN_UP : BTN_DOWN;
    const int column = std::max(0, places[static_cast<size_t>(selected)].column);
    std::vector<int> stack;
    for (const int c : {-1, column})
    {
      for (int row = 0; at(c, row) >= 0; row++)
        stack.push_back(at(c, row));
    }
    const int pos = static_cast<int>(std::find(stack.begin(), stack.end(), selected) - stack.begin());
    if (!stack.empty())
      to = stack[static_cast<size_t>(
          MenuNav::Step(std::min(pos, static_cast<int>(stack.size()) - 1), arrow == BTN_UP ? -1 : 1,
                        static_cast<int>(stack.size()), Fresh(arrow)))];
  }
  else if (places[static_cast<size_t>(selected)].column >= 0)
  {
    // Left / Right: the other column, on the same row (or its last).
    const int other = 1 - places[static_cast<size_t>(selected)].column;
    int rows = 0;
    while (at(other, rows) >= 0)
      rows++;
    if (rows > 0)
      to = at(other, std::min(places[static_cast<size_t>(selected)].row, rows - 1));
  }
  if (to != selected)
  {
    m_panel.selected = to;
    m_online_prefer.clear();  // (00.86: the player's choice from now on)
    Play("move");
  }
}

void Menu::Impl::DrawRoomScreen(ImDrawList* dl, float dt)
{
  (void)dt;
  const float a = m_panel.slide;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const uint32_t AMBER = light ? 0x9A5C00 : 0xFFC870, GREEN = light ? 0x1E7F46 : 0x5BD38A,
                 RED = light ? 0xB3261E : 0xFF8A80;
  const Netplay::View v = Netplay::GetView();
  const auto raw = [dl](ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    dl->AddText(font, size, at, color, text.c_str());
  };
  const auto raw_w = [](ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  };
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), light ? Rgba(0xF4F4F8, 0.97f * a) : Rgba(0x06060A, 0.95f * a));
  constexpr float X0 = 80.0f, LEFT_W = 520.0f, MID_X = 660.0f, MID_W = 540.0f, CHAT_X = 1260.0f, TOP = 196.0f;
  const float x1 = W - 80.0f;
  const ImU32 text = Rgba(theme.text, a), dim = Rgba(theme.text, 0.65f * a);
  const ImU32 card = light ? Rgba(0x000000, 0.05f * a) : Rgba(0xFFFFFF, 0.06f * a);

  // ---- The title, the room's kind, its code ----
  const std::string title = v.host ? "Play Online" : v.watching ? "Watching" : "Online Room";
  Text(dl, m_bold, 44, ImVec2(X0, 52), text, title);
  std::string kind = v.together ? "Choose Together" : v.link ? "Link cable" : v.is_public ? "Public room" :
                                                                                           "Private room";
  std::string sub = PS4Lang::T(kind);
  int in_room = 0;
  for (const Netplay::PlayerView& p : v.players)
    in_room += p.asking || (v.watching && p.me) ? 0 : 1;
  if (!v.watching)  // ((01.05) a spectator sees the host only)
    sub += "  \xC2\xB7  " + PS4Lang::T("Players: " + std::to_string(in_room) + " of " + std::to_string(v.max_players));
  // ((01.05, the owner) how many watch - every member knows it, the server says it)
  if (v.watcher_count > 0)
    sub += "  \xC2\xB7  " + PS4Lang::T(std::to_string(v.watcher_count) + " watching");
  if (v.hidden)
    sub += std::string("  \xC2\xB7  ") + PS4Lang::T("Your address is hidden.");
  raw(m_font, 26, ImVec2(X0, 118), dim, sub);
  if (!v.code.empty() && (v.host || !v.is_public))
  {
    const std::string label = PS4Lang::T("Room code");
    const float cw = raw_w(m_bold, 64, v.code);
    raw(m_bold, 64, ImVec2(x1 - cw, 44), Rgba(theme.wave, a), v.code);
    raw(m_font, 24, ImVec2(x1 - cw - raw_w(m_font, 24, label) - 18.0f, 74), dim, label);
  }
  dl->AddLine(ImVec2(X0, 168), ImVec2(x1, 168), Rgba(theme.wave, 0.35f * a), 1.5f);

  // ---- Left: the game, its cover, what the room says ----
  {
    float y = TOP;
    const int game = m_online_game;
    float aspect = 0.0f;
    const ImTextureID cover = game >= 0 && game < static_cast<int>(m_games.size()) ? Cover(game, &aspect) : ImTextureID{};
    constexpr float BOX_H = 330.0f;
    if (cover != ImTextureID{})
    {
      float h = BOX_H, w = BOX_H * (aspect > 0.0f ? aspect : 0.75f);
      if (w > LEFT_W)
      {
        w = LEFT_W;
        h = w / (aspect > 0.0f ? aspect : 0.75f);
      }
      const ImVec2 p0(X0 + (LEFT_W - w) * 0.5f, y + (BOX_H - h) * 0.5f);
      dl->AddRectFilled(ImVec2(p0.x - 6, p0.y - 6), ImVec2(p0.x + w + 6, p0.y + h + 6), Rgba(0x000000, 0.45f * a), 10.0f);
      dl->AddImage(ImTextureRef(cover), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32(255, 255, 255, static_cast<int>(255 * a)));
    }
    else
    {
      dl->AddRectFilled(ImVec2(X0, y), ImVec2(X0 + LEFT_W, y + BOX_H), card, 12.0f);
      const std::string none = PS4Lang::T(v.together ? "You choose a game together" : "No cover");
      raw(m_font, 26, ImVec2(X0 + (LEFT_W - raw_w(m_font, 26, none)) * 0.5f, y + BOX_H * 0.5f - 14.0f), dim, none);
    }
    y += BOX_H + 22.0f;
    const System* system = SystemByKey(v.game.console);
    const std::string game_title = v.together ? PS4Lang::T("Choose Together") : v.game.title;
    raw(m_bold, 34, ImVec2(X0, y), text, Fit(m_bold, 34, game_title, LEFT_W));
    y += 46.0f;
    if (system)
    {
      raw(m_font, 26, ImVec2(X0, y), dim, system->tab);
      y += 40.0f;
    }
    else if (v.link)
    {
      raw(m_font, 26, ImVec2(X0, y), dim, std::string(PS4Lang::T("Link cable")) + " (Game Boy 1)");
      y += 40.0f;
    }
    y += 10.0f;
    for (const std::string& note : m_room_notes)
    {
      const bool warn = note.find("haven't") != std::string::npos || note.find("another version") != std::string::npos ||
                        note.find("wants to join") != std::string::npos;
      const std::string wrapped = WrapText(m_font, 26, PS4Lang::T(note), LEFT_W);
      const float h = 34.0f * static_cast<float>(std::count(wrapped.begin(), wrapped.end(), '\n') + 1);
      if (y + h > HELP_END - 3.0f * HELP_LINE - 12.0f)
        break;  // (above the selected option's help: three lines at most)
      dl->AddText(m_font, 26, ImVec2(X0, y), warn ? Rgba(AMBER, a) : Rgba(theme.text, 0.85f * a), wrapped.c_str());
      y += h;
    }
  }

  // ---- Middle: the players, then the options ----
  float y = TOP;
  Text(dl, m_bold, 30, ImVec2(MID_X, y), text, "Players");
  y += 48.0f;
  std::vector<Netplay::PlayerView> players = v.players;
  std::stable_sort(players.begin(), players.end(), [](const Netplay::PlayerView& l, const Netplay::PlayerView& r) {
    return l.asking != r.asking ? !l.asking : l.id < r.id;
  });
  // (01.05, the owner) A room with a game: each card says that player's file of it too - the same as the host's, another
  // version, none - and its version as the file's name says it, under the nickname (its country beside it).
  const bool with_game = !v.together && !v.watching;
  // ((01.15) Every option on the screen: the options' columns' height first - RoomPlaces -, then the cards, smaller
  // when they and the columns don't fit together (a room of 4 with a request to join).)
  const std::vector<RoomPlace> places = RoomPlaces();
  const int count = static_cast<int>(m_panel.options.size());
  constexpr float OPT_H = 48.0f, SET_H = 64.0f, OPT_GAP = 8.0f, COL_GAP = 14.0f;
  const float col_w = (MID_W - COL_GAP) * 0.5f, bottom = HELP_END - 90.0f;
  float column_h[2] = {0.0f, 0.0f};
  for (int i = 0; i < count && i < static_cast<int>(places.size()); i++)
  {
    const RoomPlace& place = places[static_cast<size_t>(i)];
    if (place.column >= 0)
      column_h[place.column] += (RoomSetting(m_online_actions[static_cast<size_t>(i)]) ? SET_H : OPT_H) + OPT_GAP;
  }
  const float grid_h = std::max(0.0f, std::max(column_h[0], column_h[1]) - OPT_GAP);
  float cards_h = 0.0f;
  for (const Netplay::PlayerView& p : players)
    cards_h += (with_game && !p.asking ? 96.0f : 70.0f) + 10.0f;
  const bool compact = y + cards_h + 14.0f + grid_h > bottom;
  for (const Netplay::PlayerView& p : players)
  {
    const float CARD_H = with_game && !p.asking ? (compact ? 84.0f : 96.0f) : (compact ? 62.0f : 70.0f);
    // (Its line's places: the nickname, the second line, the game's line - closer on a smaller card.)
    const float NAME_Y = compact ? 2.0f : 6.0f, SECOND_Y = compact ? 34.0f : 40.0f, GAME_Y = compact ? 58.0f : 66.0f;
    // ((01.15) A player's card is its option: its actions - mute, block, remove.)
    int option = -1;
    for (size_t i = 0; i < m_online_actions.size(); i++)
    {
      if (m_online_actions[i] == "player:" + std::to_string(p.id))
        option = static_cast<int>(i);
    }
    const bool card_sel = option >= 0 && option == m_panel.selected;
    dl->AddRectFilled(ImVec2(MID_X, y), ImVec2(MID_X + MID_W, y + CARD_H), card_sel ? Rgba(theme.wave, 0.30f * a) : card,
                      10.0f);
    if (card_sel)
      dl->AddRect(ImVec2(MID_X, y), ImVec2(MID_X + MID_W, y + CARD_H), Rgba(theme.wave, 0.9f * a), 10.0f, 0, 2.0f);
    if (p.id == 0)
      dl->AddRectFilled(ImVec2(MID_X, y), ImVec2(MID_X + 6, y + CARD_H), Rgba(theme.wave, a), 3.0f);
    std::string name = p.nick + (p.me ? PS4Lang::T(" (you)") : std::string());
    const float tag_w = p.country.empty() ? 0.0f : raw_w(m_bold, 20, p.country) + 16.0f + 12.0f;
    name = Fit(m_bold, 30, name, MID_W - 190 - tag_w);
    raw(m_bold, 30, ImVec2(MID_X + 20, y + NAME_Y), p.asking ? Rgba(AMBER, a) : text, name);
    DrawCountryTag(dl, MID_X + 20 + raw_w(m_bold, 30, name) + 12.0f, y + NAME_Y + 18.0f, p.country, 20, a);
    if (with_game && !p.asking)
    {
      using NetProto::LobbyGame;
      const std::string what = p.id == 0                         ? "The room's game" :
                               p.game == LobbyGame::Same         ? "Same file" :
                               p.game == LobbyGame::Different    ? "Another version" :
                               p.game == LobbyGame::Asking       ? "Another version, choosing" :
                               p.game == LobbyGame::Missing      ? (p.me ? "Not in your games" : "Hasn't got the game") :
                                                                   "Looking for the game...";
      const bool versioned = p.game != LobbyGame::Missing && p.game != LobbyGame::Looking && !p.version.empty();
      const uint32_t color = p.id == 0                      ? theme.text :
                             p.game == LobbyGame::Same      ? GREEN :
                             p.game == LobbyGame::Missing   ? RED :
                             p.game == LobbyGame::Looking   ? theme.text :
                                                              AMBER;
      const std::string line = PS4Lang::T(versioned ? what + ": " + p.version : what);
      raw(m_font, 22, ImVec2(MID_X + 20, y + GAME_Y), Rgba(color, (color == theme.text ? 0.65f : 0.95f) * a),
          Fit(m_font, 22, line, MID_W - 40));
    }
    std::string second = p.id == 0 ? PS4Lang::T("Host") : std::string();
    if (!p.region.empty())
      second += (second.empty() ? "" : "  \xC2\xB7  ") + p.region;
    if (p.asking)
      second = PS4Lang::T("wants to join");
    else if (!p.me && p.blocked)
      second += std::string(second.empty() ? "" : "  \xC2\xB7  ") + PS4Lang::T("blocked");
    else if (!p.me && p.muted)
      second += std::string(second.empty() ? "" : "  \xC2\xB7  ") + PS4Lang::T("muted");
    else if (p.talking)
      second += std::string(second.empty() ? "" : "  \xC2\xB7  ") + PS4Lang::T("talking");
    else if (!p.voice_on)
      second += std::string(second.empty() ? "" : "  \xC2\xB7  ") + PS4Lang::T("voice off");
    raw(m_font, 22, ImVec2(MID_X + 20, y + SECOND_Y), p.talking ? Rgba(GREEN, a) : dim,
        Fit(m_font, 22, second, MID_W - 190));
    if (!p.me)
    {
      const std::string ping = p.ping_ms >= 0 ? std::to_string(p.ping_ms) + " ms" : std::string("...");
      const uint32_t q = p.quality == 0 ? GREEN : p.quality == 1 ? AMBER : p.quality == 2 ? RED : theme.text;
      const float ping_y = compact ? 12.0f : 18.0f;
      raw(m_bold, 30, ImVec2(MID_X + MID_W - 20 - raw_w(m_bold, 30, ping), y + ping_y),
          Rgba(q, p.quality < 0 ? 0.6f * a : a), ping);
      if (p.relay)
      {
        const std::string relay = PS4Lang::T("relay");
        raw(m_font, 20, ImVec2(MID_X + MID_W - 20 - raw_w(m_font, 20, relay), y + ping_y + 32.0f), dim, relay);
      }
    }
    y += CARD_H + 10.0f;
  }
  // The options under the cards (at the same place for 1 to 4 players while they fit): the actions as buttons, the
  // room's settings as their name, their value and a switch - Cross changes it. The selected one lit; its help under
  // them, above the button hints.
  const float grid_y = std::max(y + 4.0f, std::min(TOP + 48.0f + 4.0f * 80.0f + 14.0f, bottom - grid_h));
  float column_y[2] = {grid_y, grid_y};
  for (int i = 0; i < count && i < static_cast<int>(places.size()); i++)
  {
    const RoomPlace& place = places[static_cast<size_t>(i)];
    if (place.column < 0)
      continue;  // (a player's card: drawn above)
    const std::string& action = m_online_actions[static_cast<size_t>(i)];
    const bool sel = i == m_panel.selected, setting = RoomSetting(action);
    const float x = MID_X + (place.column == 0 ? 0.0f : col_w + COL_GAP), top = column_y[place.column];
    const float h = setting ? SET_H : OPT_H;
    column_y[place.column] += h + OPT_GAP;
    const ImVec2 r0(x, top), r1(x + col_w, top + h);
    const std::string shown = PS4Lang::T(m_panel.options[static_cast<size_t>(i)]);
    if (!setting)
    {
      // An action: a button.
      dl->AddRectFilled(r0, r1, sel ? Rgba(theme.wave, 0.35f * a) : card, 10.0f);
      if (sel)
        dl->AddRect(r0, r1, Rgba(theme.wave, 0.9f * a), 10.0f, 0, 2.0f);
      ImFont* font = sel ? m_bold : m_font;
      const std::string label = Fit(font, 26, shown, col_w - 32.0f);
      raw(font, 26, ImVec2(x + (col_w - raw_w(font, 26, label)) * 0.5f, top + 9.0f), text, label);
      continue;
    }
    // A setting: its name small, its value ("Name: Value", translated) under it, a switch at the right - on: Yes,
    // Free.
    const bool on = action == "entry" ? v.free_entry : action == "late" ? v.late_allowed : v.watch_allowed;
    const size_t split = shown.rfind(": ");
    const std::string name = split == std::string::npos ? shown : shown.substr(0, split);
    const std::string value = split == std::string::npos ? std::string() : shown.substr(split + 2);
    dl->AddRectFilled(r0, r1, sel ? Rgba(theme.wave, 0.30f * a) : light ? Rgba(0x000000, 0.025f * a) :
                                                                      Rgba(0xFFFFFF, 0.03f * a), 10.0f);
    dl->AddRect(r0, r1, sel ? Rgba(theme.wave, 0.9f * a) : Rgba(theme.text, 0.22f * a), 10.0f, 0, sel ? 2.0f : 1.5f);
    constexpr float SW_W = 46.0f, SW_H = 26.0f;
    const float sw_x = x + col_w - 16.0f - SW_W, sw_y = top + (h - SW_H) * 0.5f;
    raw(m_font, 20, ImVec2(x + 16.0f, top + 6.0f), dim, Fit(m_font, 20, name, sw_x - x - 26.0f));
    raw(m_bold, 26, ImVec2(x + 16.0f, top + 29.0f), on ? Rgba(GREEN, a) : Rgba(theme.text, 0.85f * a),
        Fit(m_bold, 26, value, sw_x - x - 26.0f));
    dl->AddRectFilled(ImVec2(sw_x, sw_y), ImVec2(sw_x + SW_W, sw_y + SW_H),
                      on ? Rgba(GREEN, 0.9f * a) : Rgba(theme.text, 0.25f * a), SW_H * 0.5f);
    dl->AddCircleFilled(ImVec2(on ? sw_x + SW_W - SW_H * 0.5f : sw_x + SW_H * 0.5f, sw_y + SW_H * 0.5f),
                        SW_H * 0.5f - 3.0f, Rgba(0xFFFFFF, a));
  }
  if (m_panel.selected >= 0 && m_panel.selected < count)
  {
    const size_t at = static_cast<size_t>(m_panel.selected);
    const std::string help = at < m_panel.helps.size() && !m_panel.helps[at].empty() ? m_panel.helps[at] :
                                                                                  OptionHelp(m_panel.options[at]);
    if (!help.empty())
    {
      const std::string wrapped = WrapText(m_font, 25, PS4Lang::T(help), MID_X + MID_W - X0);
      const float lines = static_cast<float>(std::count(wrapped.begin(), wrapped.end(), '\n') + 1);
      dl->AddText(m_font, 25, ImVec2(X0, HELP_END - HELP_LINE * lines), Rgba(theme.text, 0.8f * a), wrapped.c_str());
    }
  }

  // ---- Right: the chat ----
  DrawChatBox(dl, v, CHAT_X, TOP, x1, HELP_END - 10.0f, a);
  if (m_room_picker.open)
    DrawRoomPicker(dl, a);
}

// The room's chat in a box: its lines, the newest at the bottom (older ones off its top), each with its player's
// nickname (ours in the theme's colour); a quick phrase in this console's language, a line of text as it came
// (filtered). Under it how to write, and that the lines go from console to console unencrypted, kept nowhere.
void Menu::Impl::DrawChatBox(ImDrawList* dl, const Netplay::View& v, float x0, float y0, float x1, float y1, float a)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const ImU32 dim = Rgba(theme.text, 0.6f * a);
  Text(dl, m_bold, 30, ImVec2(x0, y0), Rgba(theme.text, a), "Chat");
  // The note under the hints (its lines: the box ends higher for more).
  // ((01.07, the owner) A server that keeps the room's chat: its notice - the lines go to it from then on.)
  const std::string note = WrapText(
      m_font, 20,
      PS4Lang::T(v.chat_stored ? "The rooms' chat and the world chat are kept 30 days for moderation; your IP address "
                                 "isn't kept. Voice isn't recorded." :
                 v.chat_text   ? "From console to console, not encrypted; EmuSuNi's server keeps nothing." :
                                 "The others' lines of text are hidden (Online Settings); their quick phrases show."),
      x1 - x0);
  if (v.chat_stored && !v.watching)
    Netplay::RoomNoticeShown();
  const float note_h = 24.0f * static_cast<float>(std::count(note.begin(), note.end(), '\n') + 1);
  const float top = y0 + 48.0f, bottom = y1 - 56.0f - note_h, w = x1 - x0 - 36.0f;
  dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), light ? Rgba(0x000000, 0.05f * a) : Rgba(0xFFFFFF, 0.05f * a),
                    12.0f);
  // Each player's colour: ours the theme's, the others' by member id.
  static const uint32_t COLOURS[] = {0x62E6FF, 0xFFC870, 0x46F5AE, 0xFF8AC8};
  float y = bottom - 14.0f;
  dl->PushClipRect(ImVec2(x0, top + 6), ImVec2(x1, bottom - 6), true);
  for (auto it = v.chat.rbegin(); it != v.chat.rend() && y > top; ++it)
  {
    if (it->notice)
    {
      // (01.06) A note of ours (a player of an older EmuSuNi, without the chat): dim, no nickname.
      const std::string wrapped = WrapText(m_font, 24, PS4Lang::T(it->text), w);
      y -= 30.0f * static_cast<float>(std::count(wrapped.begin(), wrapped.end(), '\n') + 1) + 6.0f;
      dl->AddText(m_font, 24, ImVec2(x0 + 18, y), dim, wrapped.c_str());
      continue;
    }
    const std::string body = it->phrase ? PS4Lang::ChatPhraseText(it->phrase, it->param) : it->text;
    const std::string nick = it->mine ? PS4Lang::T("You") : it->nick;
    // ((01.15) The nickname in bold and its colour, the words after it: NetplayUi::LayoutChatLine.)
    const NetplayUi::ChatLine line = NetplayUi::LayoutChatLine(m_font, m_bold, 26, 34.0f, nick, body, w);
    y -= line.height + 6.0f;
    const ImU32 who = it->mine ? Rgba(theme.wave, a) : Rgba(COLOURS[std::clamp(it->from, 0, 3)], a);
    NetplayUi::DrawChatLine(dl, m_font, m_bold, 26, 34.0f, line, ImVec2(x0 + 18, y), who,
                            Rgba(theme.text, (it->phrase ? 0.95f : 0.9f) * a));
  }
  dl->PopClipRect();
  if (v.watching)
  {
    // (01.05) A spectator neither reads nor writes the room's chat.
    const std::string none = WrapText(m_font, 24, PS4Lang::T("Spectators watch without chat or voice."), w);
    dl->AddText(m_font, 24, ImVec2(x0 + 18, top + 18), dim, none.c_str());
    return;
  }
  if (v.chat.empty())
  {
    const std::string none = WrapText(m_font, 24, PS4Lang::T("No messages yet. L2: a quick phrase. R2: write one."), w);
    dl->AddText(m_font, 24, ImVec2(x0 + 18, top + 18), dim, none.c_str());
  }
  // How to write, and what the chat is.
  float hx = x0;
  hx += PS4Glyphs::Draw(dl, m_bold, "L2", hx, bottom + 30.0f, 34, a) + 10.0f;
  Text(dl, m_font, 26, ImVec2(hx, bottom + 14.0f), Rgba(theme.text, 0.85f * a), "Quick Phrase");
  hx += TextWidth(m_font, 26, "Quick Phrase") + 34.0f;
  hx += PS4Glyphs::Draw(dl, m_bold, "R2", hx, bottom + 30.0f, 34, a) + 10.0f;
  Text(dl, m_font, 26, ImVec2(hx, bottom + 14.0f), Rgba(theme.text, 0.85f * a), "Write");
  dl->AddText(m_font, 20, ImVec2(x0, bottom + 52.0f), dim, note.c_str());
}

// A picker over the room: its title, its items (the selected one lit).
void Menu::Impl::DrawRoomPicker(ImDrawList* dl, float a)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const RoomPicker& p = m_room_picker;
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(0x000000, 0.55f * a));
  constexpr float BOX_W = 720.0f, ITEM_H = 56.0f;
  // ((01.07) 20 quick phrases: more than the screen's height - the ones about the selected, scrolling with it.)
  const int count = static_cast<int>(p.items.size());
  const int fits = std::max(1, static_cast<int>((H - 120.0f - 110.0f) / ITEM_H));
  const int shown = std::min(count, fits);
  const int first = std::clamp(p.selected - shown / 2, 0, std::max(0, count - shown));
  const float box_h = 110.0f + ITEM_H * static_cast<float>(shown);
  const float x0 = (W - BOX_W) * 0.5f, y0 = std::max(60.0f, (H - box_h) * 0.5f - 30.0f);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + BOX_W, y0 + box_h), light ? Rgba(0xFFFFFF, 0.97f * a) :
                                                                          Rgba(0x0B0A14, 0.96f * a), 20.0f);
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + BOX_W, y0 + box_h), Rgba(theme.wave, 0.9f * a), 20.0f, 0, 2.5f);
  Text(dl, m_bold, 34, ImVec2(x0 + 36, y0 + 26), Rgba(theme.wave, a), p.title);
  // (More above or below: a small triangle at the box's right.)
  const float tx = x0 + BOX_W - 48.0f;
  if (first > 0)
    dl->AddTriangleFilled(ImVec2(tx, y0 + 48.0f), ImVec2(tx + 20.0f, y0 + 48.0f), ImVec2(tx + 10.0f, y0 + 34.0f),
                          Rgba(theme.text, 0.5f * a));
  if (first + shown < count)
    dl->AddTriangleFilled(ImVec2(tx, y0 + box_h - 34.0f), ImVec2(tx + 20.0f, y0 + box_h - 34.0f),
                          ImVec2(tx + 10.0f, y0 + box_h - 20.0f), Rgba(theme.text, 0.5f * a));
  float y = y0 + 86.0f;
  for (size_t i = static_cast<size_t>(first); i < static_cast<size_t>(first + shown); i++, y += ITEM_H)
  {
    const bool sel = static_cast<int>(i) == p.selected;
    if (sel)
      dl->AddRectFilled(ImVec2(x0 + 20, y - 4), ImVec2(x0 + BOX_W - 20, y + ITEM_H - 8), Rgba(theme.wave, 0.30f * a),
                        10.0f);
    const std::string item = i < p.raw.size() && p.raw[i] ? p.items[i] : PS4Lang::T(p.items[i]);
    dl->AddText(sel ? m_bold : m_font, 30, ImVec2(x0 + 44, y + 6), Rgba(theme.text, a), item.c_str());
  }
}

bool Menu::Impl::RoomPickerInput(uint32_t pressed)
{
  RoomPicker& p = m_room_picker;
  const int count = static_cast<int>(p.items.size());
  if (pressed & BTN_CIRCLE)
  {
    p.open = false;
    Play("back");
    return true;
  }
  for (const uint32_t arrow : {BTN_UP, BTN_DOWN})
  {
    if ((pressed & arrow) && count > 0)
    {
      p.selected = MenuNav::Step(p.selected, arrow == BTN_UP ? -1 : 1, count, Fresh(arrow));
      Play("move");
    }
  }
  if ((pressed & BTN_CROSS) && p.selected >= 0 && p.selected < count)
  {
    const std::string action = p.actions[static_cast<size_t>(p.selected)];
    p.open = false;
    // ((01.07) Its actions are PickerAction's: until 01.06 only OnlineAction's were tried - which knows none of them -,
    // so a quick phrase picked in the room, or a player's mute, block or removal, did nothing.)
    if (!PickerAction(action))
      OnlineAction(action);
  }
  return true;
}

// The picker's actions (the room's and (01.07) the world chat's); false: not one of them.
bool Menu::Impl::PickerAction(const std::string& action)
{
  if (action.rfind("phrase:", 0) == 0)
    ChatSaid(Netplay::SendChatPhrase(std::atoi(action.c_str() + 7)));
  else if (action.rfind("mute:", 0) == 0 || action.rfind("unmute:", 0) == 0)
  {
    const bool mute = action[0] == 'm';
    Netplay::SetPeerMuted(std::atoi(action.c_str() + (mute ? 5 : 7)), mute);
    Play("confirm");
  }
  else if (action.rfind("block:", 0) == 0)
  {
    Netplay::BlockPeer(std::atoi(action.c_str() + 6));
    ShowToast("Blocked on this PS4: you won't hear or read them again (Online Settings: Unblock All)", 0);
    Play("confirm");
  }
  else if (action.rfind("kick:", 0) == 0)
  {
    Netplay::Kick(std::atoi(action.c_str() + 5));
    Play("confirm");
  }
  else if (action.rfind("wphrase:", 0) == 0)  // (01.07) the world chat
    WorldSaid(Netplay::WorldSayPhrase(std::atoi(action.c_str() + 8)));
  else if (action.rfind("wmute:", 0) == 0)
  {
    Netplay::WorldMute(action.substr(6));
    m_world_index = -1;
    ShowToast("Muted in the world chat (Online Settings: Muted in the World Chat)", 0);
    Play("confirm");
  }
  else if (action.rfind("wreport:", 0) == 0)
  {
    Netplay::WorldReport(static_cast<uint32_t>(std::strtoul(action.c_str() + 8, nullptr, 10)));
    ShowToast("Reported to EmuSuNi's moderation", 0);
    Play("confirm");
  }
  else
    return false;
  return true;
}

// ---- (01.07, the owner) The Online tab's pages and the world chat ----

// The pages' tabs: L2, Rooms, Chat (its new lines' count while another page shows), Most Played, Players, R2 - the one
// shown lit, as the tabs' bar lights its tab. ((01.08) L2 / R2, the pages' buttons; (01.09) the only ones.)
void Menu::Impl::DrawOnlinePages(ImDrawList* dl, float alpha, float x0, float x1, float y)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  constexpr float CHIP_H = 38.0f, CHIP_FONT = 22.0f, GLYPH_H = 34.0f, BADGE = 18.0f;
  const ImU32 plain = light ? Rgba(0xFFFFFF, alpha * 0.55f) : Rgba(0x000000, alpha * 0.22f);
  const Netplay::WorldChatView world = Netplay::GetWorldChat();
  static const char* const NAMES[] = {"Rooms", "Chat", "Most Played", "Players"};
  float x = x0;
  x += PS4Glyphs::Draw(dl, m_bold, "L2", x, y + CHIP_H * 0.5f, GLYPH_H, alpha) + 12.0f;
  for (int i = 0; i < ONLINE_PAGES && x < x1; i++)
  {
    const bool on = i == m_online_page;
    const int unread = i == PAGE_CHAT && !on && world.on ? std::min(world.unread, 99) : 0;
    const std::string badge = unread > 0 ? std::to_string(unread) : std::string();
    const float badge_w = badge.empty() ? 0.0f : TextWidth(m_bold, BADGE, badge) + 18.0f;
    const float w = TextWidth(m_bold, CHIP_FONT, NAMES[i]) + 30.0f + (badge.empty() ? 0.0f : badge_w + 8.0f);
    const ImVec2 p0(x, y), p1(x + w, y + CHIP_H);
    if (on)
    {
      dl->AddRectFilled(p0, p1, Rgba(theme.wave, alpha * 0.45f), CHIP_H * 0.5f);
      dl->AddRect(p0, p1, Rgba(theme.wave, alpha * 0.95f), CHIP_H * 0.5f, 0, 2.0f);
    }
    else
    {
      dl->AddRectFilled(p0, p1, plain, CHIP_H * 0.5f);
      dl->AddRect(p0, p1, Rgba(theme.text, alpha * 0.18f), CHIP_H * 0.5f, 0, 1.5f);
    }
    Text(dl, m_bold, CHIP_FONT, ImVec2(x + 15.0f, y + (CHIP_H - CHIP_FONT) * 0.5f - 3.0f),
         Rgba(theme.text, alpha * (on ? 1.0f : 0.65f)), NAMES[i], false);
    if (!badge.empty())
    {
      const float bx = x + w - 15.0f - badge_w, by = y + (CHIP_H - 26.0f) * 0.5f;
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badge_w, by + 26.0f), Rgba(theme.wave, alpha), 13.0f);
      dl->AddText(m_bold, BADGE, ImVec2(bx + 9.0f, by + 3.0f), Rgba(light ? 0xFFFFFF : 0x000000, alpha),
                  badge.c_str());
    }
    x += w + 8.0f;
  }
  PS4Glyphs::Draw(dl, m_bold, "R2", x + 4.0f, y + CHIP_H * 0.5f, GLYPH_H, alpha);
}

// The world chat's page: its lines (the newest at the bottom; one selected, Up / Down, lit - Cross: mute its player or
// report it), each "nickname [country]: text" in its player's colour (ours "You"); under them the notice - the
// owner's: the lines kept 30 days for moderation, never the address - and how to write. Hidden (Online Settings), not
// on the server, or no line yet: why.
void Menu::Impl::DrawWorldChat(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const Netplay::WorldChatView world = Netplay::GetWorldChat();
  const ImU32 dim = Rgba(theme.text, 0.6f * alpha);
  const float w = x1 - x0;
  const auto centered = [&](ImFont* font, float size, float at, ImU32 color, const std::string& english) {
    const std::string wrapped = WrapText(font, size, PS4Lang::T(english), std::min(1100.0f, w - 80.0f));
    std::stringstream lines(wrapped);
    for (std::string line; std::getline(lines, line); at += size + 10.0f)
      dl->AddText(font, size, ImVec2((x0 + x1 - TextWidth(font, size, line)) * 0.5f, at), color, line.c_str());
  };
  // The notice (its lines: the box ends higher for more): a server that keeps the lines says so before anything is
  // said; the app sends nothing before it showed it (Netplay::WorldNoticeShown).
  const std::string notice = WrapText(
      m_font, 20,
      PS4Lang::T(world.stored ? "The rooms' chat and the world chat are kept 30 days for moderation; your IP address "
                                "isn't kept. Voice isn't recorded." :
                                "The world chat's messages go through EmuSuNi's server; it doesn't keep them."),
      w - 20.0f);
  const float notice_h = 24.0f * static_cast<float>(std::count(notice.begin(), notice.end(), '\n') + 1);
  const float top = y, box_bottom = bottom - notice_h - 18.0f;
  dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, box_bottom),
                    light ? Rgba(0x000000, 0.05f * alpha) : Rgba(0xFFFFFF, 0.05f * alpha), 12.0f);
  if (world.on && world.answered)
  {
    dl->AddText(m_font, 20, ImVec2(x0 + 10.0f, box_bottom + 10.0f), dim, notice.c_str());
    Netplay::WorldNoticeShown(world.stored);
  }
  const float cy = top + std::min(150.0f, (box_bottom - top) * 0.3f);
  if (!world.on)
  {
    centered(m_bold, 28, cy, Rgba(theme.text, alpha * 0.85f), "The world chat is hidden (Online Settings)");
    return;
  }
  if (!world.answered)
  {
    // (A server before 01.07 drops the question: said so after a few seconds.)
    const bool long_wait = m_time - m_online_entered > 8.0f;
    centered(m_bold, 28, cy, Rgba(theme.text, alpha * 0.85f),
             long_wait ? "The world chat isn't on EmuSuNi's server yet" : "Connecting to the world chat...");
    return;
  }
  if (world.lines.empty())
  {
    centered(m_font, 26, cy, dim, "No messages yet. Square: a quick phrase. Triangle: write one.");
    return;
  }
  // Each player's colour by its code; ours the theme's.
  static const uint32_t COLOURS[] = {0x62E6FF, 0xFFC870, 0x46F5AE, 0xFF8AC8, 0xB7A1FF, 0xFF9E6B, 0x9BE37A, 0x6FC3FF};
  const auto colour_of = [](const std::string& code) {
    uint32_t h = 2166136261u;
    for (const char ch : code)
      h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    return COLOURS[h % (sizeof(COLOURS) / sizeof(COLOURS[0]))];
  };
  const int n = static_cast<int>(world.lines.size());
  int selected = -1;
  for (int i = 0; i < n; i++)
  {
    if (static_cast<int64_t>(world.lines[static_cast<size_t>(i)].serial) == m_world_index)
      selected = i;
  }
  // (The selected line a little above the bottom; none: the newest at the bottom.)
  const int last = selected < 0 ? n - 1 : std::min(n - 1, selected + 2);
  const float wrap_w = w - 36.0f;
  float yy = box_bottom - 12.0f;
  dl->PushClipRect(ImVec2(x0, top + 6.0f), ImVec2(x1, box_bottom - 6.0f), true);
  for (int i = last; i >= 0 && yy > top; i--)
  {
    const Netplay::WorldChatLine& l = world.lines[static_cast<size_t>(i)];
    std::string body = l.phrase ? PS4Lang::ChatPhraseText(l.phrase, l.param) : l.text;
    const std::string who = (l.mine ? PS4Lang::T("You") : l.nick) + (l.country.empty() ? "" : " [" + l.country + "]");
    // ((01.15) The nickname and country in bold and the player's colour, the words after them.)
    const NetplayUi::ChatLine line = NetplayUi::LayoutChatLine(m_font, m_bold, 26, 34.0f, who, body, wrap_w);
    yy -= line.height + 8.0f;
    if (i == selected)
      dl->AddRectFilled(ImVec2(x0 + 6.0f, yy - 4.0f), ImVec2(x1 - 6.0f, yy + line.height + 2.0f),
                        Rgba(theme.wave, 0.28f * alpha), 8.0f);
    NetplayUi::DrawChatLine(dl, m_font, m_bold, 26, 34.0f, line, ImVec2(x0 + 18.0f, yy),
                            l.mine ? Rgba(theme.wave, alpha) : Rgba(colour_of(l.code), alpha),
                            Rgba(theme.text, 0.92f * alpha));
  }
  dl->PopClipRect();
}

// (01.07, the owner) The players' ranking: its place, the player's nickname and country, its hours in matches and the
// console it played most - the last 7 days or since ever (Triangle) -, the selected row lit; not answered (a server
// before it): said after a few seconds.
void Menu::Impl::DrawPlayersTable(ImDrawList* dl, float alpha, float x0, float x1, float y, float bottom)
{
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const Netplay::PlayersList list = Netplay::TopPlayers(m_players_days);
  const auto raw = [dl](ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    dl->AddText(font, size, at, color, text.c_str());
  };
  const std::string period = m_players_days ? "Last 7 days" : "Since ever";
  Text(dl, m_bold, 26, ImVec2(x0 + 2.0f, y), Rgba(theme.wave, alpha), period);
  const float pw = TextWidth(m_bold, 26, period);
  PS4Glyphs::Draw(dl, m_bold, "Triangle", x0 + pw + 18.0f, y + 15.0f, 30.0f, alpha * 0.8f);
  y += 50.0f;
  const float cx = (x0 + x1) * 0.5f;
  const auto centered = [&](ImFont* font, float size, float at, ImU32 color, const std::string& english) {
    const std::string& shown = PS4Lang::T(english);
    raw(font, size, ImVec2(cx - TextWidth(font, size, shown) * 0.5f, at), color, shown);
  };
  if (list.players.empty())
  {
    const float cy = y + std::min(150.0f, (bottom - y) * 0.3f);
    DrawCategoryIcon(dl, Category::Online, ImVec2(cx, cy), 84.0f, Rgba(theme.text, alpha * 0.22f));
    const bool waited = m_time - m_online_entered > 8.0f;
    centered(m_bold, 28, cy + 62.0f, Rgba(theme.text, alpha * 0.85f),
             list.answered ? "No one in the ranking yet: play a match online" :
             waited        ? "The players' ranking isn't on EmuSuNi's server yet" :
                             "Looking for the ranking...");
    return;
  }
  const float w = x1 - x0;
  const float col_player = x0 + 90.0f, col_hours = x0 + w * 0.58f, col_console = x0 + w * 0.76f;
  const ImU32 head = Rgba(theme.text, alpha * 0.55f);
  Text(dl, m_bold, 22, ImVec2(x0 + 22.0f, y), head, "#", false);
  Text(dl, m_bold, 22, ImVec2(col_player, y), head, "Player", false);
  Text(dl, m_bold, 22, ImVec2(col_hours, y), head, "Time Played", false);
  Text(dl, m_bold, 22, ImVec2(col_console, y), head, "Console Played Most", false);
  y += 32.0f;
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Rgba(theme.text, alpha * 0.12f), 1.5f);
  y += 10.0f;
  constexpr float ROW_H = 58.0f;
  const int n = static_cast<int>(list.players.size());
  m_players_index = std::clamp(m_players_index, 0, n - 1);
  const int visible = std::max(1, static_cast<int>((bottom - y) / ROW_H));
  const int first = std::clamp(m_players_index - visible / 2, 0, std::max(0, n - visible));
  for (int i = first; i < n && i < first + visible; i++, y += ROW_H)
  {
    const Netplay::TopPlayer& p = list.players[static_cast<size_t>(i)];
    const bool sel = i == m_players_index;
    const ImVec2 r0(x0, y), r1(x1, y + ROW_H - 8.0f);
    dl->AddRectFilled(r0, r1, sel ? Rgba(theme.wave, alpha * 0.22f) :
                                    light ? Rgba(0xFFFFFF, alpha * 0.5f) : Rgba(0x000000, alpha * 0.2f), 12.0f);
    if (sel)
      dl->AddRect(r0, r1, Rgba(theme.wave, alpha), 12.0f, 0, 2.0f);
    const float ty = y + (ROW_H - 8.0f - 30.0f) * 0.5f;
    const uint32_t medal = i == 0 ? 0xE0B030 : i == 1 ? 0xB8B8C8 : i == 2 ? 0xC8803C : theme.text;
    raw(m_bold, 28, ImVec2(x0 + 22.0f, ty - 2.0f), Rgba(medal, alpha * (i < 3 ? 1.0f : 0.7f)), std::to_string(i + 1));
    const std::string nick =
        Fit(m_font, 28, p.nick.empty() ? std::string("-") : p.nick, col_hours - col_player - 90.0f);
    raw(m_font, 28, ImVec2(col_player, ty - 2.0f), Rgba(theme.text, alpha), nick);
    DrawCountryTag(dl, col_player + TextWidth(m_font, 28, nick) + 12.0f, ty + 15.0f, p.country, 18.0f, alpha);
    const uint32_t minutes = p.seconds / 60;
    raw(m_bold, 26, ImVec2(col_hours, ty), Rgba(theme.text, alpha * 0.9f),
        minutes >= 60 ? std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min" :
                        std::to_string(minutes) + " min");
    const bool gba_link = p.console == NetProto::GBALINK, gb_link = p.console == NetProto::GBLINK;
    const System* system = gb_link || gba_link ? nullptr : SystemByKey(p.console);
    const std::string console = gba_link ? std::string("GBA Link") : gb_link ? std::string("GB Link") :
                                system      ? std::string(system->tab) : p.console;
    if (system)
    {
      const float pill_w = TextWidth(m_bold, 22, console) + 24.0f;
      dl->AddRectFilled(ImVec2(col_console, ty - 2.0f), ImVec2(col_console + pill_w, ty + 30.0f),
                        Rgba(system->color, alpha * 0.95f), 16.0f);
      Text(dl, m_bold, 22, ImVec2(col_console + 12.0f, ty), Rgba(0xFFFFFF, alpha), console, false);
    }
    else
      raw(m_bold, 22, ImVec2(col_console, ty), Rgba(theme.text, alpha * 0.85f), console);
  }
}

bool Menu::Impl::WorldChatInput(uint32_t pressed)
{
  const Netplay::WorldChatView world = Netplay::GetWorldChat();
  const int n = static_cast<int>(world.lines.size());
  int at = -1;
  for (int i = 0; i < n; i++)
  {
    if (static_cast<int64_t>(world.lines[static_cast<size_t>(i)].serial) == m_world_index)
      at = i;
  }
  if (pressed & (BTN_UP | BTN_DOWN))
  {
    // Up from none: the newest line; Down past the newest: none again (the chat follows its new lines).
    if (n > 0)
    {
      const int to = at < 0 ? ((pressed & BTN_UP) ? n - 1 : -1) :
                     (pressed & BTN_UP) ? std::max(0, at - 1) :
                     at + 1 >= n        ? -1 :
                                          at + 1;
      if (to != at)
      {
        m_world_index = to < 0 ? -1 : static_cast<int>(world.lines[static_cast<size_t>(to)].serial);
        Play("move");
      }
    }
    return true;
  }
  if ((pressed & BTN_CIRCLE) && m_world_index >= 0)
  {
    m_world_index = -1;
    Play("back");
    return true;
  }
  if (pressed & BTN_CROSS)
  {
    if (at >= 0)
      OpenWorldLine(at);
    else
      Play("error");
    return true;
  }
  // ((01.08) Square a quick phrase, Triangle a line typed - until 01.07 L2 / R2, the room's own: they are the pages
  // now. So on this page Square doesn't make a room nor Triangle join one by its code: the rooms' page does.)
  if (pressed & (BTN_SQUARE | BTN_TRIANGLE))
  {
    if (!world.on || !world.answered)
    {
      ShowToast(!world.on ? "The world chat is hidden (Online Settings)" :
                            "The world chat isn't on EmuSuNi's server yet",
                0);
      Play("error");
    }
    else if (pressed & BTN_SQUARE)
      OpenWorldPhrases();
    else
      WriteWorldLine();
    return true;
  }
  return false;
}

// The quick phrases for the world chat, in this console's language (they go as their number).
void Menu::Impl::OpenWorldPhrases()
{
  RoomPicker& p = m_room_picker;
  p = RoomPicker();
  p.open = true;
  p.title = "Quick Phrase";
  const std::string country = Netplay::GetWorldChat().my_country;
  for (int i = 1; i <= NetProto::CHAT_PHRASES; i++)
  {
    // ((01.07) "I'm from ...": with our country as the server shows it, only.)
    const bool from = i == NetProto::PHRASE_FROM;
    if (from && country.empty())
      continue;
    p.items.push_back(from ? PS4Lang::ChatPhraseText(i, country) : std::string(NetProto::ChatPhrase(i)));
    p.actions.push_back("wphrase:" + std::to_string(i));
    p.raw.push_back(from);
  }
  Play("menu");
}

// A line's actions: mute its player (this PS4's: Online Settings hears them again), report it to EmuSuNi's moderation.
void Menu::Impl::OpenWorldLine(int index)
{
  const Netplay::WorldChatView world = Netplay::GetWorldChat();
  if (index < 0 || index >= static_cast<int>(world.lines.size()))
    return;
  const Netplay::WorldChatLine& l = world.lines[static_cast<size_t>(index)];
  if (l.mine)
  {
    ShowToast("That message is yours", 0);
    Play("error");
    return;
  }
  RoomPicker& p = m_room_picker;
  p = RoomPicker();
  p.open = true;
  p.title = l.nick;
  p.items = {"Mute This Player", "Report This Message"};
  p.actions = {"wmute:" + l.code, "wreport:" + std::to_string(l.serial)};
  p.raw = {false, false};
  Play("menu");
}

void Menu::Impl::WriteWorldLine()
{
  AskText("Write a Message", "", [this](const std::string& typed) {
    if (!typed.empty())
      WorldSaid(Netplay::WorldSayText(typed));
  }, static_cast<uint32_t>(NetProto::CHAT_LETTERS), TextKeyboard::Mode::Chat,
          "Everyone on the Online tab reads it. One message every 3 seconds.");
}

// A line said: on its way (its answer is a toast then, or the confirm sound), or why not.
void Menu::Impl::WorldSaid(Netplay::WorldSayResult result)
{
  using Netplay::WorldSayResult;
  if (result == WorldSayResult::Sending)
    return;
  ShowToast(result == WorldSayResult::Busy     ? "The last message is still on its way" :
            result == WorldSayResult::Empty    ? "Nothing to send" :
            result == WorldSayResult::NoNotice ? "The world chat's notice must show first" :
                                                 "The world chat isn't on EmuSuNi's server yet",
            0);
  Play("error");
}

// The quick phrases, in this console's language (they go as their number: each console shows them in its own).
void Menu::Impl::OpenQuickPhrases()
{
  RoomPicker& p = m_room_picker;
  p = RoomPicker();
  p.open = true;
  p.title = "Quick Phrase";
  const std::string country = Netplay::GetView().my_country;
  for (int i = 1; i <= NetProto::CHAT_PHRASES; i++)
  {
    // ((01.07) "I'm from ...": with our country as the room shows it, only.)
    const bool from = i == NetProto::PHRASE_FROM;
    if (from && country.empty())
      continue;
    p.items.push_back(from ? PS4Lang::ChatPhraseText(i, country) : std::string(NetProto::ChatPhrase(i)));
    p.actions.push_back("phrase:" + std::to_string(i));
    p.raw.push_back(from);
  }
  Play("menu");
}

// A player's actions: mute (its voice and its chat) or hear again, block on this PS4, (the host) remove.
void Menu::Impl::OpenPlayerActions(int member)
{
  const Netplay::View v = Netplay::GetView();
  for (const Netplay::PlayerView& pl : v.players)
  {
    if (pl.id != member || pl.me)
      continue;
    RoomPicker& p = m_room_picker;
    p = RoomPicker();
    p.open = true;
    p.title = pl.nick;
    const std::string id = std::to_string(member);
    if (!pl.blocked)
    {
      p.items.push_back(pl.muted ? "Hear Again (Voice and Chat)" : "Mute (Voice and Chat)");
      p.actions.push_back((pl.muted ? "unmute:" : "mute:") + id);
      p.items.push_back("Block on This PS4");
      p.actions.push_back("block:" + id);
    }
    if (v.host)
    {
      p.items.push_back("Remove from the Room");
      p.actions.push_back("kick:" + id);
    }
    p.raw.assign(p.items.size(), false);
    if (p.items.empty())
      p.open = false;
    else
      Play("menu");
    return;
  }
}

// A line typed on the app's keyboard (its letters at most: the chat's).
void Menu::Impl::WriteChatLine()
{
  AskText("Write a Message", "", [this](const std::string& typed) {
    if (!typed.empty())
      ChatSaid(Netplay::SendChatText(typed));
  }, static_cast<uint32_t>(NetProto::CHAT_LETTERS), TextKeyboard::Mode::Chat,
          "Everyone in the room reads it. One line every 2 seconds.");
}

void Menu::Impl::ChatSaid(Netplay::ChatResult result)
{
  if (result == Netplay::ChatResult::Sent)
    Play("confirm");
  else
  {
    ShowToast(result == Netplay::ChatResult::TooSoon   ? "One message every 2 seconds" :
              result == Netplay::ChatResult::NotInRoom ? "Not in the room yet" :
                                                         "Nothing to send",
              0);
    Play("error");
  }
}

bool Menu::Impl::TogetherInput(uint32_t pressed)
{
  TogetherScreen& t = m_together;
  constexpr int COLS = TogetherScreen::COLS;
  const int count = static_cast<int>(t.cards.size());
  const auto select = [&](int to) {
    if (to == t.index || to < 0 || to >= count)
      return;
    t.index = to;
    m_together_game = t.cards[static_cast<size_t>(to)].game;
    Play("move");
  };
  if (pressed & (BTN_LEFT | BTN_RIGHT))
  {
    const uint32_t arrow = (pressed & BTN_LEFT) ? BTN_LEFT : BTN_RIGHT;
    select(MenuNav::Step(t.index, arrow == BTN_LEFT ? -1 : 1, count, Fresh(arrow)));
    return true;
  }
  if (pressed & (BTN_UP | BTN_DOWN))
  {
    // (A row up or down; down to a shorter last row: its last card.)
    if (pressed & BTN_UP)
      select(t.index - COLS);
    else if (t.index + COLS < count)
      select(t.index + COLS);
    else if (t.index / COLS < (count - 1) / COLS)
      select(count - 1);
    return true;
  }
  if (pressed & (BTN_L1 | BTN_R1))
  {
    const uint32_t button = (pressed & BTN_L1) ? BTN_L1 : BTN_R1;
    const int tabs = static_cast<int>(t.tabs.size());
    const int next = MenuNav::Step(t.tab, button == BTN_L1 ? -1 : 1, tabs, Fresh(button));
    if (tabs > 1 && next != t.tab)
    {
      m_together_tab = t.tabs[static_cast<size_t>(next)];
      m_together_game = -1;
      t.scroll = 0.0f;
      BuildOnlinePanel();
      Play("category");
    }
    return true;
  }
  if (pressed & BTN_SQUARE)
  {
    if (t.unknown > 0)
    {
      m_online_show_all = !m_online_show_all;
      BuildOnlinePanel();
      ShowToast(m_online_show_all ? (t.need > 2 ? "Every game you all have, also those whose players aren't known" :
                                                  "Every game you both have, also those whose players aren't known") :
                                    "The games known to take " + std::to_string(t.need) + " players or more",
                0);
      Play("category");
    }
    return true;
  }
  if (pressed & BTN_TRIANGLE)
  {
    Netplay::Leave();
    Netplay::TakeError();
    m_panel.open = false;
    Play("back");
    return true;
  }
  if ((pressed & BTN_CROSS) && count > 0)
    TogetherPick(t.cards[static_cast<size_t>(t.index)].game);
  return true;
}

// A Choose Together room's game chosen: the host plays it with the other player now, a guest suggests it.
void Menu::Impl::TogetherPick(int game)
{
  NetProto::GameInfo info;
  if (!OnlineGameInfo(game, &info))
  {
    Play("error");
    return;
  }
  if (Netplay::GetView().host)
  {
    if (!Netplay::PickGame(info))
    {
      ShowToast("The other player isn't ready yet", 0);
      Play("error");
    }
    return;
  }
  NetProto::LibraryGame g;
  g.console = info.console;
  g.crc = info.crc;
  g.size = info.size;
  Netplay::Suggest(g);
  ShowToast("Suggested to the host", 0);
  Play("confirm");
}

// (00.23) The players with EmuSuNi open now, as the server counts the anonymous statistics' heartbeats.
std::string Menu::Impl::OnlineNowLine() const
{
  const int n = UsageStats::OnlineNow();
  if (n < 0)
    return "";
  return n == 1 ? std::string("1 player online now") : std::to_string(n) + " players online now";
}

// ---- The Online tab's table (00.85) ----

// The console filter's choices: every console (""), then each one that plays online, in the consoles' list's order.
std::vector<std::string> Menu::Impl::OnlineConsoles() const
{
  std::vector<std::string> keys = {""};
  for (const System& system : Systems())
  {
    if (Netplay::RulesFor(system.key).online)
      keys.push_back(system.key);
  }
  return keys;
}

// Under the tab's title: the players online now and the rooms there are, each part translated ("Play with friends
// anywhere" until either is known).
std::string Menu::Impl::OnlineSummary() const
{
  const std::string now = OnlineNowLine();
  std::string line = now.empty() ? std::string() : PS4Lang::T(now);
  if (m_online_listed)
  {
    const int rooms = std::max(m_online_last_total, static_cast<int>(m_online_last.size()));
    line += (line.empty() ? "" : "  \xC2\xB7  ") +
            PS4Lang::T(rooms == 1 ? std::string("1 room") : std::to_string(rooms) + " rooms");
    // ((01.16) Refreshes the server didn't answer: the last list stays - said here; until 01.15 the table emptied.)
    const Netplay::View v = Netplay::GetView();
    if (v.phase == Netplay::Phase::Browsing && v.list_late > 0)
      line += std::string("  \xC2\xB7  ") + PS4Lang::T("The server isn't answering: the list may be out of date");
  }
  return line.empty() ? PS4Lang::T("Play with friends anywhere") : line;
}

// The table as it is now (every frame the tab shows): the player's own room on top while there is one, then the last
// list that came - each room's game looked for among ours once (its file's CRC32 and size, else its title) - through
// the filters. The selection stays on its room (by its number in the list) wherever the room moves, else on its row.
void Menu::Impl::BuildOnlineTable()
{
  using NetProto::LobbyGame;
  using Netplay::Phase;
  const Netplay::View v = Netplay::GetView();
  // (00.86: the host's room open lists them too, on its connection.)
  if ((v.phase == Phase::Browsing || HostingRoom()) && v.rooms_done)
  {
    m_online_last = v.rooms;
    m_online_last_total = v.rooms_total;
    m_online_listed = true;
  }
  const bool in_room =
      v.phase == Phase::Connecting || v.phase == Phase::Room || v.phase == Phase::Starting || v.phase == Phase::Playing;
  std::vector<OnlineRow> rows;
  if (in_room)
  {
    // Ours, as the room is: its game, host, players, the other player's ping (the filters leave it there).
    OnlineRow mine;
    mine.key = "mine";
    mine.mine = true;
    mine.together = v.together;
    mine.found = LobbyGame::Same;
    mine.game = v.together ? -1 : m_online_game;
    mine.room.code = v.code;
    mine.room.game = v.game;
    if (v.together)
      mine.room.game.console = NetProto::TOGETHER;
    mine.room.is_public = v.is_public;
    mine.room.playing = v.phase == Phase::Starting || v.phase == Phase::Playing;
    // (00.86: its players - not one asking to join.)
    size_t players = 0;
    for (const Netplay::PlayerView& p : v.players)
      players += p.asking ? 0 : 1;
    mine.room.players = static_cast<uint8_t>(std::clamp<size_t>(players, 1, NetProto::MAX_MEMBERS));
    mine.room.max_players = static_cast<uint8_t>(v.max_players);  // (00.95: 2 to 4)
    for (const Netplay::PlayerView& p : v.players)
    {
      if (p.id == 0)
      {
        mine.room.host = p.nick;
        mine.room.region = p.region;
      }
      if (!p.me && p.ping_ms >= 0)
        mine.ping_ms = p.ping_ms;
    }
    rows.push_back(std::move(mine));
  }
  // (00.86) Ours in the list, private (listed without its code): by its host - us -, time zone and game.
  std::string my_nick;
  std::string my_region;
  for (const Netplay::PlayerView& p : v.players)
  {
    if (p.me)
    {
      my_nick = NetProto::CleanNick(p.nick);
      my_region = p.region;
    }
  }
  for (const Netplay::RoomView& r : m_online_last)
  {
    const NetProto::RoomEntry& room = r.room;
    if (in_room && !v.code.empty() && room.code == v.code)
      continue;  // (ours: on top)
    if (in_room && v.host && !room.is_public && !v.is_public && room.host == my_nick && room.region == my_region &&
        room.game.console == (v.together ? std::string(NetProto::TOGETHER) : v.game.console) &&
        room.game.crc == (v.together ? 0u : v.game.crc) && room.game.size == (v.together ? 0u : v.game.size))
      continue;
    const bool together = room.game.console == NetProto::TOGETHER;
    OnlineRow row;
    row.room = room;
    row.key = room.id != 0 ? "id:" + std::to_string(room.id) : "code:" + room.code;
    row.ping_ms = r.ping_ms;
    row.together = together;
    if (together)
    {
      row.found = LobbyGame::Same;  // (the games both have: once in)
    }
    else
    {
      const std::string key = room.game.console + '\n' + std::to_string(room.game.crc) + '\n' +
                              std::to_string(room.game.size) + '\n' + room.game.title;
      auto mark = m_online_marks.find(key);
      if (mark == m_online_marks.end())
      {
        int index = -1;
        const LobbyGame found = FindOnlineGame(room.game, &index);
        mark = m_online_marks.emplace(key, std::make_pair(found, index)).first;
      }
      row.found = mark->second.first;
      row.game = mark->second.second;
    }
    rows.push_back(std::move(row));
  }
  // ((01.07) No filters: every room; the list's counts are in the log - Netplay's RoomsDone.)
  const int c = static_cast<int>(Category::Online);
  const int count = static_cast<int>(rows.size());
  int at = -1;
  for (int i = 0; i < count && !m_online_row_key.empty(); i++)
  {
    if (rows[static_cast<size_t>(i)].key == m_online_row_key)
      at = i;
  }
  if (at < 0)
    at = std::clamp(m_online_index, 0, std::max(0, count - 1));
  else if (at != m_online_index)
    m_item_pos[c] += static_cast<float>(at - m_online_index);  // (rooms came or went above it: the view with it)
  m_online_index = at;
  m_selected[c] = at;
  m_online_row_key = count > 0 ? rows[static_cast<size_t>(at)].key : std::string();
  m_online_rows = std::move(rows);
}

// (01.08, the owner: "que L2 y R2 muevan entre las pestañas de adentro") L2 / R2: the Online tab's page before or after
// (Rooms, Chat, Most Played, Players), in both styles; past the last page the first again, never the menu's tabs. (They
// don't repeat held: each press one page.) (01.09, the owner: "debes deshabilitar cambiar de pestaña interna con l1 y
// r1, debes dejar solo l2 y r2") Only them: L1 / R1, the pages from 01.07 to 01.08, are the Library's tabs again, and
// nothing in the XMB's Online column (OnlineTabInput).
bool Menu::Impl::OnlinePageInput(uint32_t pressed)
{
  if (!(pressed & (BTN_L2 | BTN_R2)))
    return false;
  const int to = (m_online_page + ((pressed & BTN_L2) ? -1 : 1) + ONLINE_PAGES) % ONLINE_PAGES;
  m_online_page = to;
  m_world_index = -1;
  m_room_picker.open = false;
  if (to == PAGE_TOP)
  {
    Netplay::WantTopGames();
    BuildTopTable();
  }
  static const char* const NAMES[] = {"the rooms", "the world chat", "the games most played online", "the players"};
  Log(std::string("online: ") + NAMES[to]);
  Play("category");
  return true;
}

// The tab's buttons (true: the press was its): (01.08) L2 / R2 its pages; in the rooms' Up / Down the rooms, Cross
// joins the selected one, Square makes a room, Triangle joins one by its code, Options opens Online Settings. Left /
// Right (the XMB's columns) and (01.09) the Library's L1 / R1 (its tabs) stay the menu's.
bool Menu::Impl::OnlineTabInput(uint32_t pressed)
{
  const int c = static_cast<int>(Category::Online);
  // (01.07) A line's actions or a quick phrase over the world chat: its own buttons (on its page only - one left open
  // by the room's screen is closed here).
  if (m_room_picker.open && !m_panel.open)
  {
    if (m_online_page == PAGE_CHAT)
      return RoomPickerInput(pressed);
    m_room_picker.open = false;
  }
  if (OnlinePageInput(pressed))
    return true;
  // ((01.09) L1 / R1: the Library's tabs; in the XMB nothing here - its "five items" aren't this table's rows.)
  if (pressed & (BTN_L1 | BTN_R1))
    return !Library();
  if (m_online_page == PAGE_CHAT && WorldChatInput(pressed))
    return true;
  // ((01.07) The players' ranking: Up / Down its rows, Triangle the last 7 days or since ever.)
  if (m_online_page == PAGE_PLAYERS)
  {
    const int n = static_cast<int>(Netplay::TopPlayers(m_players_days).players.size());
    if (pressed & (BTN_UP | BTN_DOWN))
    {
      const uint32_t button = (pressed & BTN_UP) ? BTN_UP : BTN_DOWN;
      const int to = n == 0 ? 0 : MenuNav::Step(m_players_index, button == BTN_UP ? -1 : 1, n, Fresh(button));
      if (to != m_players_index)
      {
        m_players_index = to;
        Play("move");
      }
      return true;
    }
    if (pressed & BTN_TRIANGLE)
    {
      m_players_days = m_players_days ? 0 : NetProto::TOP_DAYS;
      m_players_index = 0;
      Netplay::WantTopPlayers(m_players_days);
      Log(m_players_days ? "online: the players' ranking of the last 7 days" :
                           "online: the players' ranking since ever");
      Play("category");
      return true;
    }
    if (pressed & (BTN_CROSS | BTN_CIRCLE))
      return true;
  }
  // (01.05, the owner: "haz una tabla con los más jugados") The games most played online: Up / Down its games, Cross
  // makes a room for the selected one when it is ours (Play Online's), Circle back to the rooms; Square, Triangle and
  // Options as ever.
  if (m_online_page == PAGE_TOP)
  {
    const int n = static_cast<int>(m_top_rows.size());
    if (pressed & (BTN_UP | BTN_DOWN))
    {
      const uint32_t button = (pressed & BTN_UP) ? BTN_UP : BTN_DOWN;
      const int to = n == 0 ? 0 : MenuNav::Step(m_top_index, button == BTN_UP ? -1 : 1, n, Fresh(button));
      if (to != m_top_index)
      {
        m_top_index = to;
        Play("move");
      }
      return true;
    }
    if (pressed & BTN_CIRCLE)
    {
      m_online_page = PAGE_ROOMS;
      Play("back");
      return true;
    }
    if (pressed & BTN_CROSS)
    {
      if (m_top_index >= 0 && m_top_index < n && m_top_rows[static_cast<size_t>(m_top_index)].game >= 0)
      {
        const TopRow& row = m_top_rows[static_cast<size_t>(m_top_index)];
        OpenOnlineHost(row.game, row.link);
        Play("menu");
      }
      else
      {
        ShowToast(n == 0 ? "No game selected" : "Not in your games", 0);
        Play("error");
      }
      return true;
    }
  }
  const int count = static_cast<int>(m_online_rows.size());
  const auto select = [&](int to) {
    if (to == m_online_index || to < 0 || to >= count)
      return;
    if (std::abs(to - m_online_index) > 5)
      m_item_pos[c] = static_cast<float>(to);  // (round the list: there at once)
    m_online_index = to;
    m_selected[c] = to;
    m_online_row_key = m_online_rows[static_cast<size_t>(to)].key;
    Play("move");
  };
  if (m_online_page != PAGE_ROOMS && (pressed & (BTN_UP | BTN_DOWN | BTN_CROSS)))
    return true;  // (another page's)
  // ((01.07) Held Up / Down go on through the rooms, one at a time; the pages are (01.08) L2 / R2.)
  if (pressed & (BTN_UP | BTN_DOWN))
  {
    const uint32_t arrow = (pressed & BTN_UP) ? BTN_UP : BTN_DOWN;
    if (count > 0)
      select(MenuNav::Step(m_online_index, arrow == BTN_UP ? -1 : 1, count, Fresh(arrow)));
    return true;
  }
  if ((pressed & (BTN_CROSS | BTN_SQUARE | BTN_TRIANGLE)) || ((pressed & BTN_TOUCHPAD) && m_online_page == PAGE_ROOMS))
  {
    // In a room whose panel something else closed (an error said at the menu's start): back to it, not out of it.
    // (00.86: the host's room open while the host is here is the table's own - Cross on it goes back to it, another
    // room or a new one asks first to close it.)
    const Netplay::Phase phase = Netplay::GetView().phase;
    if (phase != Netplay::Phase::Off && phase != Netplay::Phase::Browsing && !HostingRoom())
    {
      OpenOnlineRoom();
      Play("menu");
      return true;
    }
  }
  if (pressed & BTN_CROSS)
  {
    if (m_online_index >= 0 && m_online_index < count)
      JoinOnlineRow(m_online_rows[static_cast<size_t>(m_online_index)]);
    else
      Play("error");
    return true;
  }
  if (pressed & BTN_SQUARE)
  {
    if (!AskCloseRoom(false, [this] { OpenSettingPanel(SET_OL_CREATE); }))
      OpenSettingPanel(SET_OL_CREATE);
    return true;
  }
  if (pressed & BTN_TRIANGLE)
  {
    OnlineAction("join");
    return true;
  }
  if (pressed & BTN_OPTIONS)
  {
    OpenSettingPanel(SET_OL_SETTINGS);
    return true;
  }
  if (pressed & BTN_TOUCHPAD)
  {
    // ((01.15, the owner) Watch the selected room's match as a spectator, a player's place left or not - a room whose
    // host lets them. (01.07: Touchpad's Most Played is no more; the filters' L2 / R2 are (01.08) the pages.))
    if (m_online_page == PAGE_ROOMS && m_online_index >= 0 && m_online_index < count &&
        !m_online_rows[static_cast<size_t>(m_online_index)].mine)
      JoinOnlineRow(m_online_rows[static_cast<size_t>(m_online_index)], true);
    return true;
  }
  return false;
}

// Cross on a room: a public one is joined at once (its code is in the list), a private one with its code - asked for,
// as its host told it. One that can't be joined says why. ((01.15) `as_watcher`, the Touchpad: watched, a player's place
// left or not.)
void Menu::Impl::JoinOnlineRow(const OnlineRow& row, bool as_watcher)
{
  using NetProto::LobbyGame;
  const NetProto::RoomEntry& room = row.room;
  if (row.mine)
  {
    OpenOnlineRoom();
    Play("menu");
    return;
  }
  std::string why;
  // (01.05) A match under way with a place left whose host lets one join: joined (it loads the game, then plays). Else a
  // match started (or a room full) whose host lets spectators watch: watched instead.
  const bool late = !as_watcher && room.playing && room.late && room.players < room.max_players;
  const bool watch = as_watcher || (!late && room.watchable && room.watchers < NetProto::MAX_WATCHERS &&
                                    (room.playing || room.players >= room.max_players));
  // ((01.06) Another version's when this one plays its console's matches alike: Netplay::CanJoinVersion.)
  if (!Netplay::CanJoinVersion(room))
    why = "This room is for EmuSuNi " + room.app_version +
          (Netplay::RoomIsOlder(room.app_version) ? ": its host needs to update" : "");
  // ((01.15) Watched by choice: a room without spectators (its host's choice, or an EmuSuNi before 01.05), or with all
  // of them, says so - the server's own words when it refuses.)
  else if (as_watcher && (row.together || !room.watchable))
    why = "That room's host doesn't let anyone watch";
  else if (as_watcher && room.watchers >= NetProto::MAX_WATCHERS)
    why = "That room has its spectators already";
  else if ((watch || late) && row.found == LobbyGame::Missing)
    why = "You haven't got this game";
  else if (watch || late)
    why = "";
  else if (room.playing)
    why = "This room's match has started";
  else if (room.players >= room.max_players)
    why = "This room is full";
  else if (!row.together && row.found == LobbyGame::Missing)
    why = "You haven't got this game";
  if (!why.empty())
  {
    ShowToast(why, 0);
    Play("error");
    return;
  }
  if (AskCloseRoom(true, [this, row, as_watcher] { JoinOnlineRow(row, as_watcher); }))
    return;  // (00.86: our room first)
  const auto join = [this, watch](const std::string& code) {
    m_online_found = LobbyGame::Looking;
    m_online_game = -1;
    m_online_library_sent = false;
    if (watch)
      Netplay::Watch(code, m_data_dir);
    else
      Netplay::Join(code, m_data_dir);
    OpenOnlineRoom();
  };
  if (room.is_public && !room.code.empty())
  {
    join(room.code);
    Play("confirm");
    return;
  }
  AskText(
      "Room Code", "",
      [this, join](const std::string& typed) {
        const std::string code = NetProto::NormalizeCode(typed);
        if (code.empty())
          return;
        if (!NetProto::ValidCode(code))
        {
          ShowToast("That isn't a room code: 6 letters and numbers", 0);
          Play("error");
          return;
        }
        join(code);
      },
      8, TextKeyboard::Mode::Plain, "This room is private: type the code its host gave you.");
}

// Options in the table: Online Settings (until 00.84 the tab's own rows). Made again in place (a toggle) keeps the
// panel where it is; `select`: the row to be on.
void Menu::Impl::OpenOnlineSettings(const std::string& select)
{
  const bool again = m_panel.open && m_panel.setting == SET_OL_SETTINGS;
  const float slide = again ? m_panel.slide : 0.0f;
  std::string row = select;
  if (row.empty() && again && m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.rows.size()))
    row = m_panel.rows[static_cast<size_t>(m_panel.selected)].key;
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_OL_SETTINGS;
  m_panel.title = "Online Settings";
  m_panel.slide = slide;
  BuildOnlineSettingsRows();
  m_panel.selected = 0;
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (m_panel.rows[i].key == row)
      m_panel.selected = static_cast<int>(i);
  }
}

// What Online Settings' rows show that changes by itself (the profile's code comes, its deletion ends).
std::string Menu::Impl::OnlineSettingsSignature() const
{
  const UsageStats::Profile profile = UsageStats::GetProfile();
  return std::string(profile.on ? "1" : "0") + (profile.is_public ? "1" : "0") + (profile.deleting ? "1" : "0") +
         profile.code;
}

void Menu::Impl::BuildOnlineSettingsRows()
{
  const std::string was = m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.rows.size()) ?
                              m_panel.rows[static_cast<size_t>(m_panel.selected)].key :
                              std::string();
  const UsageStats::Profile profile = UsageStats::GetProfile();
  const bool hide = Netplay::HideAddress(m_data_dir);
  std::vector<SettingRow> rows;
  const auto add = [&rows](const std::string& label, const std::string& value, const std::string& key,
                           const std::string& help) {
    SettingRow row{label, value, key};
    row.help = help;
    rows.push_back(std::move(row));
  };
  add("Nickname", Netplay::GetNickname(m_data_dir), "ol:nick",
      "The name other players see in rooms (16 letters at most). Left empty: your PS4 user's name.");
  add("Hide My Address", hide ? "On" : "Off", "ol:hide",
      hide ? "Everything goes through EmuSuNi's server: the other player never sees your address (a little more delay)." :
             "Your connection goes straight to the other console when it can (the least delay).");
  // (01.05, the owner) Our country beside our nickname, in the rooms and the Online tab's list.
  add("Show My Country", Netplay::ShowCountry(m_data_dir) ? "Yes" : "No", "ol:country",
      Netplay::ShowCountry(m_data_dir) ?
          "The others see your country's two letters (CL, MX...) beside your nickname, from the next room. EmuSuNi's "
          "server looks it up from your connection; your address is never shown or kept." :
          "The others don't see your country, from the next room.");
  // (00.92) Each player's ping and connection, in the room and over the game.
  add("Latency Bar", Netplay::LatencyBar(m_data_dir) ? "On" : "Off", "ol:bar",
      "Each player's ping and connection - green good, amber fair, red bad - in the room and in the game's corner: it "
      "shows whose connection makes the match wait.");
  // (01.05) The rooms' chat: the others' typed lines shown or not (their quick phrases always), the players blocked.
  add("Others' Typed Messages", Netplay::ChatText(m_data_dir) ? "Shown" : "Hidden", "ol:chattext",
      "In the rooms' chat: the other players' typed lines as they wrote them, or only their quick phrases. Messages go "
      "from console to console, not encrypted; a server that keeps them (the room says so under its chat) keeps them "
      "30 days for moderation, never your address.");
  const int blocked = Netplay::BlockedCount(m_data_dir);
  if (blocked > 0)
    add("Unblock All", std::to_string(blocked), "ol:unblock",
        "The players you blocked (a player's actions in a room) are heard and read again in the next rooms.");
  // (01.07, the owner) The world chat (the Online tab's Chat): shown or hidden - hidden, nothing is asked of the server
  // -, and the players muted in it.
  add("World Chat", Netplay::WorldChatOn(m_data_dir) ? "Visible" : "Not Shown", "ol:world",
      "Visible: the Online tab's Chat page shows the messages of every player on the Online tab, through EmuSuNi's "
      "server. Not Shown: nothing is asked of the server, nothing is shown.");
  const int world_muted = Netplay::WorldMutedCount(m_data_dir);
  if (world_muted > 0)
    add("Muted in the World Chat", std::to_string(world_muted), "ol:worldmuted",
        "The players you muted in the world chat (a message's actions): Cross reads them again.");
  // (01.07, the owner) The players' ranking (the Online tab's Players page).
  add("Appear in the Ranking", Netplay::Ranked(m_data_dir) ? "Yes" : "No", "ol:ranking",
      "Yes: EmuSuNi's server counts your time in online matches for the players' ranking (your nickname, your country "
      "if you show it, the console you play most) - never your address. No: nothing of yours is counted, from the next "
      "room.");
  add("Online Profile", profile.on ? "On" : "Off", "ol:profile",
      "On: EmuSuNi's server keeps your history (time played by game and console, sessions, online matches) and gives "
      "you a code that opens it on EmuSuNi's website. Off (the default): nothing of yours is kept.");
  if (profile.on)
    add("Public Profile", profile.is_public ? "On" : "Off", "ol:public",
        "On: your nickname, country and hours are in the website's rankings. Off: only your code opens your profile.");
  if (!profile.code.empty())
    add("Profile Code", profile.code, "ol:code",
        "Opens your profile on EmuSuNi's website (Statistics -> My Profile). Anyone with the code sees it: keep it.");
  if (!profile.code.empty() || profile.on || profile.deleting)
    add("Delete My Profile", profile.deleting ? "Being deleted..." : "", "ol:delete",
        "Deletes your profile on EmuSuNi's server: its time played, sessions and online matches. My Stats on this PS4 "
        "stay.");
  add("My Stats", MyStatsDetail(), "ol:stats",
      "This PS4's own numbers: time played, games, sessions, online matches. They are never sent.");
  // (01.05, the owner: "Revisar mis juegos")
  add("Check My Games", "", "ol:check",
      "Which of your games are the standard versions (No-Intro's; FinalBurn Neo 1.0.0.03's for the Arcade), the files "
      "most players have: a room finds the same game only with the same version. Your files are read once, in the "
      "background.");
  m_panel.rows = std::move(rows);
  m_panel.selected = 0;
  for (size_t i = 0; i < m_panel.rows.size(); i++)
  {
    if (!was.empty() && m_panel.rows[i].key == was)
      m_panel.selected = static_cast<int>(i);
  }
  m_online_signature = OnlineSettingsSignature();
}

// The table in a box (x0 to x1, top to bottom): (01.07) the pages' tabs (DrawOnlinePages: where the filters' chips
// were), then the page - the rooms: the columns' names, then the rooms - each its game (its cover when it is one of
// ours, and whether it is), console, host and time zone, players, type, status and ping -, scrolling with the
// selection; instead, when there are none, why (looking, the server's answer, none yet: Square makes one); the world
// chat (DrawWorldChat), the games most played (DrawTopTable), the players. summary: the players and rooms line first
// (the XMB's; the Library's page title has it). Its sizes are those the menus draw already (the font atlas's).
void Menu::Impl::DrawOnlineTable(ImDrawList* dl, float alpha, float x0, float x1, float top, float bottom,
                                 bool summary)
{
  using NetProto::LobbyGame;
  if (alpha <= 0.01f)
    return;
  const Theme& theme = CurrentTheme();
  const int c = static_cast<int>(Category::Online);
  const Netplay::View v = Netplay::GetView();
  // (A light theme - dark text -: darker greens and ambers, and its rows and chips lighter than the page.)
  const bool light = theme.text != 0xFFFFFF;
  const uint32_t GREEN = light ? 0x1E7F46 : 0x5BD38A, AMBER = light ? 0x9A5C00 : 0xFFC870,
                 RED = light ? 0xB3261E : 0xFF8A80, BLUE = light ? 0x245FA8 : 0x5AA9FF;
  // (Players' and games' own words - nicknames, titles - drawn as they are, not looked up in the language file.)
  const auto raw = [dl](ImFont* font, float size, ImVec2 at, ImU32 color, const std::string& text) {
    dl->AddText(font, size, at, color, text.c_str());
  };
  float y = top;
  if (summary)
  {
    raw(m_bold, 26, ImVec2(x0 + 2, y), Rgba(theme.wave, alpha), OnlineSummary());
    y += 46.0f;
  }
  // ((01.07, the owner) The pages' tabs (L2 / R2) where the console filter was; then the page.)
  DrawOnlinePages(dl, alpha, x0, x1, y);
  y += 38.0f + 22.0f;
  if (m_online_page == PAGE_TOP)
  {
    DrawTopTable(dl, alpha, x0, x1, y, bottom);
    return;
  }
  if (m_online_page == PAGE_CHAT)
  {
    DrawWorldChat(dl, alpha, x0, x1, y, bottom);
    if (m_room_picker.open && !m_panel.open)
      DrawRoomPicker(dl, alpha);  // (a line's actions, a quick phrase)
    return;
  }
  if (m_online_page == PAGE_PLAYERS)
  {
    DrawPlayersTable(dl, alpha, x0, x1, y, bottom);
    return;
  }
  constexpr float GLYPH_H = 34.0f;

  // The columns: the game the widest, then console, host, players, type, status, and the ping at the right.
  const float w = x1 - x0;
  const float col_console = x0 + w * 0.40f, col_host = x0 + w * 0.49f, col_players = x0 + w * 0.655f,
              col_type = x0 + w * 0.73f, col_status = x0 + w * 0.825f, ping_r = x1 - 22.0f;
  constexpr float HEAD = 22.0f;
  const ImU32 head = Rgba(theme.text, alpha * 0.55f);
  Text(dl, m_bold, HEAD, ImVec2(x0 + 22.0f, y), head, "Game", false);
  Text(dl, m_bold, HEAD, ImVec2(col_console, y), head, "Console", false);
  Text(dl, m_bold, HEAD, ImVec2(col_host, y), head, "Host", false);
  Text(dl, m_bold, HEAD, ImVec2(col_players, y), head, "Players", false);
  Text(dl, m_bold, HEAD, ImVec2(col_type, y), head, "Room Type", false);
  Text(dl, m_bold, HEAD, ImVec2(col_status, y), head, "Status", false);
  Text(dl, m_bold, HEAD, ImVec2(ping_r - TextWidth(m_bold, HEAD, "Ping"), y), head, "Ping", false);
  y += 32.0f;
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), Rgba(theme.text, alpha * 0.12f), 1.5f);
  y += 12.0f;

  const float list_top = y;
  const int count = static_cast<int>(m_online_rows.size());
  if (count == 0)
  {
    // None: looking (a turning arc), the server's answer and the next try, or none yet (Square makes one) - or none
    // the filters leave.
    const float cx = (x0 + x1) * 0.5f, cy = list_top + std::min(170.0f, (bottom - list_top) * 0.35f);
    const auto centered = [&](ImFont* font, float size, float at, ImU32 color, const std::string& english) {
      const std::string& shown = PS4Lang::T(english);
      raw(font, size, ImVec2(cx - TextWidth(font, size, shown) * 0.5f, at), color, shown);
    };
    const bool failed = v.phase == Netplay::Phase::Off && !v.error.empty();
    if (!failed && !m_online_listed)
    {
      const float turn = m_time * 5.0f;
      dl->PathArcTo(ImVec2(cx, cy), 26.0f, turn, turn + 4.4f, 32);
      dl->PathStroke(Rgba(theme.wave, alpha), 0, 5.0f);
      centered(m_bold, 28, cy + 52.0f, Rgba(theme.text, alpha * 0.85f), "Looking for rooms...");
      return;
    }
    DrawCategoryIcon(dl, Category::Online, ImVec2(cx, cy), 84.0f, Rgba(theme.text, alpha * 0.22f));
    if (failed)
    {
      std::stringstream wrapped(WrapText(m_bold, 28, PS4Lang::T(v.error), std::min(1100.0f, w - 120.0f)));
      float at = cy + 66.0f;
      for (std::string line; std::getline(wrapped, line); at += 38.0f)
        raw(m_bold, 28, ImVec2(cx - TextWidth(m_bold, 28, line) * 0.5f, at), Rgba(theme.text, alpha * 0.9f), line);
      centered(m_font, 24, at + 10.0f, Rgba(theme.text, alpha * 0.6f), "Trying again in a moment...");
      return;
    }
    centered(m_bold, 32, cy + 62.0f, Rgba(theme.text, alpha), "No rooms yet");
    const float ly = cy + 116.0f;
    const ImU32 line = Rgba(theme.text, alpha * 0.8f);
    {
      // "Press [Square] to create one" (two pieces translated: the button between them).
      const std::string& before = PS4Lang::T("Press");
      const std::string& after = PS4Lang::T("to create one");
      const float bw = TextWidth(m_font, 26, before), gw = PS4Glyphs::Width("Square", GLYPH_H);
      float lx = cx - (bw + 10.0f + gw + 10.0f + TextWidth(m_font, 26, after)) * 0.5f;
      raw(m_font, 26, ImVec2(lx, ly), line, before);
      lx += bw + 10.0f;
      lx += PS4Glyphs::Draw(dl, m_bold, "Square", lx, ly + 16.0f, GLYPH_H, alpha) + 10.0f;
      raw(m_font, 26, ImVec2(lx, ly), line, after);
    }
    return;
  }

  constexpr float ROW_H = 78.0f, GAP = 8.0f;
  // (Scrolled by whole rows once the selection settles - the top row whole, the selected one about the middle -, the
  // one under the last whole row fading out: more below.)
  const float visible = std::max(1.0f, std::floor((bottom - list_top) / ROW_H));
  const float scroll = std::clamp(m_item_pos[c] - std::floor((visible - 1.0f) * 0.5f), 0.0f,
                                  std::max(0.0f, static_cast<float>(count) - visible));
  // (A room's panel over the table: ours lit, the others dimmed - the last list, not asked for meanwhile. 00.86: the
  // host's room open while its panel is closed leaves the table as it is - lists, selection - ours on top.)
  const bool in_room = (v.phase == Netplay::Phase::Connecting || v.phase == Netplay::Phase::Room ||
                        v.phase == Netplay::Phase::Starting || v.phase == Netplay::Phase::Playing) &&
                       !(HostingRoom() && !(m_panel.open && m_panel.setting == SET_ONLINE_ROOM));
  dl->PushClipRect(ImVec2(x0 - 4.0f, list_top - 4.0f), ImVec2(x1 + 4.0f, bottom + 4.0f), true);
  for (int i = 0; i < count; i++)
  {
    const float ry = list_top + (static_cast<float>(i) - scroll) * ROW_H;
    if (ry < list_top - ROW_H || ry > bottom)
      continue;
    const OnlineRow& row = m_online_rows[static_cast<size_t>(i)];
    const NetProto::RoomEntry& room = row.room;
    const bool sel = i == m_online_index && !in_room;
    const float rh = ROW_H - GAP, cy = ry + rh * 0.5f;
    const ImVec2 r0(x0, ry), r1(x1, ry + rh);
    // ((01.06) Another version's that plays its console's matches alike is as ours: Netplay::CanJoinVersion.)
    const bool other_version = !Netplay::CanJoinVersion(room);
    const bool full = room.players >= room.max_players;
    // ((01.05) A match spectators may watch is as bright: Cross watches it.)
    const bool watchable = !row.mine && !other_version && room.watchable && (room.playing || full) &&
                           room.watchers < NetProto::MAX_WATCHERS && row.found != LobbyGame::Missing;
    // ((01.05) One whose match takes a player more too.)
    const bool late = !row.mine && !other_version && room.playing && room.late && !full &&
                      row.found != LobbyGame::Missing;
    const bool joinable = (!row.mine && !room.playing && !full && !other_version &&
                           (row.together || row.found != LobbyGame::Missing)) ||
                          watchable || late;
    // (A row half out of the list's box fades out at its edge.)
    const float edge = std::clamp(std::min((ry - list_top) / 36.0f, (bottom - ry - rh) / 36.0f) + 1.0f, 0.0f, 1.0f);
    const float ra = alpha * edge;
    const float a = ra * (row.mine || sel || (joinable && !in_room) ? 1.0f : 0.55f);
    if (sel)
    {
      dl->AddRectFilled(r0, r1, Rgba(theme.wave, ra * 0.20f), 14.0f);
      dl->AddRect(r0, r1, Rgba(theme.wave, ra), 14.0f, 0, 2.5f);
    }
    else
    {
      dl->AddRectFilled(r0, r1, light ? Rgba(0xFFFFFF, ra * 0.55f) : Rgba(0x000000, ra * 0.22f), 14.0f);
      if (row.mine)
        dl->AddRect(r0, r1, Rgba(theme.wave, ra * 0.7f), 14.0f, 0, 2.0f);
    }
    if (row.mine)
      dl->AddRectFilled(ImVec2(x0 + 4.0f, ry + 12.0f), ImVec2(x0 + 10.0f, ry + rh - 12.0f), Rgba(theme.wave, ra),
                        3.0f);
    // The game: its cover when it is one of ours (fit in the box, its own shape), else a tile in its console's
    // colour; a Choose Together room's two cards.
    // (00.95: a Game Boy link cable room; 01.04: a Game Boy Advance's)
    const bool gba_link_room = room.game.console == NetProto::GBALINK;
    const bool link_room = room.game.console == NetProto::GBLINK || gba_link_room;
    const System* system = link_room ? &GetSystem(gba_link_room ? SystemId::GBA : SystemId::GB) :
                                       SystemByKey(room.game.console);
    const ImVec2 b0(x0 + 22.0f, cy - 30.0f), b1(x0 + 68.0f, cy + 30.0f);
    float aspect = 0.7f;
    const ImTextureID cover =
        row.game >= 0 && row.game < static_cast<int>(m_games.size()) ? Cover(row.game, &aspect) : ImTextureID{};
    if (row.together)
    {
      dl->AddRectFilled(ImVec2(b0.x + 12.0f, b0.y), ImVec2(b1.x + 2.0f, b1.y - 10.0f), Rgba(theme.wave, a * 0.45f),
                        6.0f);
      dl->AddRectFilled(ImVec2(b0.x, b0.y + 10.0f), ImVec2(b1.x - 10.0f, b1.y), Rgba(theme.wave, a * 0.9f), 6.0f);
      const uint32_t wave = theme.wave;  // (its "?" dark on a bright wave colour - the Super Famicom's gold -)
      const float bright =
          (0.299f * ((wave >> 16) & 0xFF) + 0.587f * ((wave >> 8) & 0xFF) + 0.114f * (wave & 0xFF)) / 255.0f;
      raw(m_bold, 26, ImVec2(b0.x + 10.0f, b0.y + 20.0f), bright > 0.62f ? Rgba(0x000000, a * 0.75f) : Rgba(0xFFFFFF, a),
          "?");
    }
    else if (cover != ImTextureID{})
    {
      const float bw = b1.x - b0.x, bh = b1.y - b0.y;
      float cw = bh * std::max(0.2f, aspect), ch = bh;
      if (cw > bw)
      {
        cw = bw;
        ch = bw / std::max(0.2f, aspect);
      }
      const ImVec2 p0((b0.x + b1.x - cw) * 0.5f, cy - ch * 0.5f);
      dl->AddImageRounded(ImTextureRef(cover), p0, ImVec2(p0.x + cw, p0.y + ch), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, static_cast<int>(a * 255)), 4.0f);
    }
    else
    {
      const uint32_t tile = system ? system->color : 0x5C6070;
      dl->AddRectFilled(b0, b1, Rgba(tile, a * 0.9f), 6.0f);
      dl->AddRect(b0, b1, Rgba(0xFFFFFF, a * 0.2f), 6.0f, 0, 1.5f);
      DrawCartridgeIcon(dl, ImVec2((b0.x + b1.x) * 0.5f, cy), 28.0f, Rgba(0xFFFFFF, a * 0.85f), Rgba(tile, a));
    }
    const float tx = x0 + 86.0f, title_w = col_console - tx - 18.0f;
    std::string title = row.together ? PS4Lang::T("Choose Together") : room.game.title;
    if (row.mine && !row.together && room.game.console.empty())
      title = PS4Lang::T("Joining the room...");
    raw(m_bold, 28, ImVec2(tx, ry + 7.0f), Rgba(theme.text, a), Fit(m_bold, 28, title, title_w));
    // Under it: ours, another version's, whether the game is in ours.
    std::string sub = "Not in your games";
    uint32_t sub_color = RED;
    std::string sub_after;  // (00.86: a private room's code after "Your room")
    if (row.mine && v.asking >= 0)
    {
      sub = v.asking_nick + " wants to join";  // (00.86: Cross - the room, its Accept and Reject)
      sub_color = AMBER;
    }
    else if (row.mine)
    {
      sub = "Your room";
      sub_color = theme.wave;
      if (!v.is_public && !v.code.empty())
        sub_after = "  \xC2\xB7  " + v.code;
    }
    else if (other_version)
    {
      // ((01.06) who updates: the host of an older one, else we)
      sub = Netplay::RoomIsOlder(room.app_version) ? "EmuSuNi " + room.app_version + ": its host needs to update" :
                                                     "Needs EmuSuNi " + room.app_version;
      sub_color = AMBER;
    }
    else if (row.together)
    {
      sub = "Any game you both have";
      sub_color = theme.text;
    }
    else if (row.found == LobbyGame::Same)
    {
      sub = "In your games";
      sub_color = GREEN;
    }
    else if (row.found == LobbyGame::Different)
    {
      sub = "Another version in your games";
      sub_color = AMBER;
    }
    // ((01.05, the owner) the versions as the files' names say them: the same file - its one -, ours and the host's
    // - the list's, a server before it says none -, the host's when we haven't the game)
    if (!row.mine && !other_version && !row.together)
    {
      const std::string mine = OnlineVersionText(row.game);
      if (row.found == LobbyGame::Same && !(mine.empty() && room.version.empty()))
        sub += ": " + (mine.empty() ? room.version : mine);
      else if (row.found == LobbyGame::Different && !mine.empty())
        sub += ": " + mine + (room.version.empty() ? std::string() : " (the host's: " + room.version + ")");
      else if ((row.found == LobbyGame::Different || row.found == LobbyGame::Missing) && !room.version.empty())
        sub += " (the host's: " + room.version + ")";
    }
    // ((01.06) A room of another version we play with: its version after it.)
    if (!row.mine && !other_version && !room.app_version.empty() && room.app_version != App::VERSION)
      sub_after += "  \xC2\xB7  EmuSuNi " + room.app_version;
    raw(m_font, 22, ImVec2(tx, ry + 41.0f), Rgba(sub_color, a * (sub_color == theme.text ? 0.6f : 0.95f)),
        Fit(m_font, 22, PS4Lang::T(sub) + sub_after, title_w));
    // The console: a pill in its colour, its tab's name (a Choose Together room: any).
    if (row.together || system)
    {
      const std::string label = row.together  ? std::string("Any") :
                                gba_link_room ? std::string("GBA Link") :
                                link_room     ? std::string("GB Link") :
                                                std::string(system->tab);
      const uint32_t pill = row.together ? 0x6A6F7D : system->color;
      const float pw = TextWidth(m_bold, 22, label) + 24.0f;
      const ImVec2 p0(col_console, cy - 16.0f), p1(col_console + pw, cy + 16.0f);
      dl->AddRectFilled(p0, p1, Rgba(pill, a * 0.95f), 16.0f);
      dl->AddRect(p0, p1, Rgba(0xFFFFFF, a * 0.25f), 16.0f, 0, 1.5f);
      Text(dl, m_bold, 22, ImVec2(col_console + 12.0f, cy - 14.0f), Rgba(0xFFFFFF, a), label, false);
    }
    // The host and its time zone. ((01.05, the owner) Its country beside its nickname, as a small tag.)
    const float host_w = col_players - col_host - 16.0f;
    const std::string host_country = row.mine ? std::string() : room.country;
    const auto raw_w = [](ImFont* font, float size, const std::string& text) {
      return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    };
    const float tag_w = host_country.empty() ? 0.0f : raw_w(m_bold, 18, host_country) + 14.4f + 10.0f;
    const std::string host_name = Fit(m_font, 26, room.host.empty() ? std::string("-") : room.host, host_w - tag_w);
    raw(m_font, 26, ImVec2(col_host, ry + 9.0f), Rgba(theme.text, a), host_name);
    DrawCountryTag(dl, col_host + raw_w(m_font, 26, host_name) + 10.0f, ry + 25.0f, host_country, 18, a);
    if (!room.region.empty())
      raw(m_font, 22, ImVec2(col_host, ry + 42.0f), Rgba(theme.text, a * 0.55f), room.region);
    // The players: a figure for each place (lit: taken), and how many. (00.95: four smaller and closer, to fit.)
    const int places = std::clamp<int>(room.max_players, 1, NetProto::MAX_MEMBERS);
    const float step = places > 3 ? 17.0f : 22.0f, size = places > 3 ? 0.8f : 1.0f;
    for (int k = 0; k < places; k++)
    {
      const float px = col_players + 10.0f + static_cast<float>(k) * step;
      const ImU32 figure = Rgba(theme.text, a * (k < room.players ? 0.95f : 0.28f));
      dl->AddCircleFilled(ImVec2(px, cy - 9.0f * size), 6.0f * size, figure, 16);
      dl->PathArcTo(ImVec2(px, cy + 12.0f * size), 10.0f * size, IM_PI, 2.0f * IM_PI, 12);
      dl->PathFillConvex(figure);
    }
    raw(m_bold, 24, ImVec2(col_players + 10.0f + static_cast<float>(places) * step + 2.0f, cy - 15.0f),
        Rgba(theme.text, a), std::to_string(room.players) + "/" + std::to_string(room.max_players));
    // Its type: a globe (public) or a padlock (private: its code from its host).
    const float ix = col_type + 12.0f;
    const ImU32 icon = Rgba(theme.text, a * 0.9f);
    if (room.is_public)
    {
      DrawCategoryIcon(dl, Category::Online, ImVec2(ix, cy), 24.0f, icon);
    }
    else
    {
      dl->AddRectFilled(ImVec2(ix - 9.0f, cy - 3.0f), ImVec2(ix + 9.0f, cy + 12.0f), icon, 3.0f);
      dl->PathArcTo(ImVec2(ix, cy - 3.0f), 6.0f, IM_PI, 2.0f * IM_PI, 12);
      dl->PathStroke(icon, 0, 3.0f);
    }
    Text(dl, m_font, 24, ImVec2(col_type + 32.0f, cy - 15.0f), Rgba(theme.text, a),
         room.is_public ? "Public" : "Private", false);
    // Its status: waiting for players, full, playing (ours: connecting first).
    const bool connecting = row.mine && v.phase == Netplay::Phase::Connecting;
    const char* status = connecting ? "Connecting..." : room.playing ? "Playing" : full ? "Full" : "Waiting";
    const uint32_t dot = connecting ? 0x8A90A0 : room.playing ? BLUE : full ? AMBER : GREEN;
    // ((01.05, the owner) under it: a place left in the match under way, how many watch - or that it may be watched)
    std::string status_more;
    if (late)
      status_more = PS4Lang::T("A place left");
    if (room.watchers > 0)
      status_more += (status_more.empty() ? "" : "  \xC2\xB7  ") + PS4Lang::T(std::to_string(room.watchers) + " watching");
    else if (watchable && !late)
      status_more = PS4Lang::T("Can be watched");
    const float status_y = status_more.empty() ? cy - 15.0f : cy - 26.0f;
    dl->AddCircleFilled(ImVec2(col_status + 8.0f, status_y + 15.0f), 7.0f, Rgba(dot, a), 16);
    Text(dl, m_font, 24, ImVec2(col_status + 24.0f, status_y), Rgba(theme.text, a), status, false);
    if (!status_more.empty())
    {
      const std::string ping_now = row.ping_ms > 0 ? "~" + std::to_string(row.ping_ms) + " ms" : "-";
      const float room_w = ping_r - raw_w(m_bold, 24, ping_now) - 14.0f - (col_status + 24.0f);
      raw(m_font, 20, ImVec2(col_status + 24.0f, cy + 4.0f), Rgba(theme.text, a * 0.7f),
          Fit(m_font, 20, status_more, room_w));
    }
    // The ping: estimated (the host's round trip to the server and ours) - in our room, the other player's own.
    const std::string ping = row.ping_ms > 0 ? (row.mine ? "" : "~") + std::to_string(row.ping_ms) + " ms" : "-";
    const uint32_t ping_color = row.ping_ms <= 0 ? theme.text : row.ping_ms < 90 ? GREEN : row.ping_ms < 170 ? AMBER : RED;
    raw(m_bold, 24, ImVec2(ping_r - TextWidth(m_bold, 24, ping), cy - 15.0f),
        Rgba(ping_color, a * (row.ping_ms > 0 ? 1.0f : 0.5f)), ping);
  }
  dl->PopClipRect();
  // More rooms than fit: where the view is, at the right.
  if (static_cast<float>(count) > visible)
  {
    const float track_x = x1 + 12.0f, h = bottom - list_top - GAP;
    dl->AddRectFilled(ImVec2(track_x, list_top), ImVec2(track_x + 4.0f, list_top + h), Rgba(theme.text, alpha * 0.10f),
                      2.0f);
    const float thumb = std::max(30.0f, h * visible / static_cast<float>(count));
    const float at = list_top + (h - thumb) * scroll / std::max(1.0f, static_cast<float>(count) - visible);
    dl->AddRectFilled(ImVec2(track_x, at), ImVec2(track_x + 4.0f, at + thumb), Rgba(theme.wave, alpha * 0.8f), 2.0f);
  }
}

// Every frame: the room's network while there is one; a guest's game looked for once the room's game is known; the
// match's start (the game played: the host's, or the one the guest found); the panel as the room is now.
void Menu::Impl::UpdateOnline()
{
  using NetProto::LobbyGame;
  // (00.23) The Online tab: its games' files hashed (for Choose Together) and the players online asked for when it
  // is entered. (00.85) Online Settings' rows made again when what they show changes (the profile's code comes).
  const bool on_tab = m_category == static_cast<int>(Category::Online);
  if (on_tab && !m_online_tab_entered)
  {
    StartOnlineLibrary();
    UsageStats::Refresh();
    m_online_entered = m_time;
    m_online_marks.clear();  // (the rooms' games looked for again: the games may have changed meanwhile)
  }
  m_online_tab_entered = on_tab;
  if (m_panel.open && m_panel.setting == SET_OL_SETTINGS && OnlineSettingsSignature() != m_online_signature)
    BuildOnlineSettingsRows();
  // ((01.05) Check My Games as the files are read; the games most played online once the server's list comes)
  if (m_panel.open && (m_panel.setting == SET_OL_CHECK || m_panel.setting == SET_OL_CHECK_LIST) &&
      m_time - m_check_at > 0.5 && CheckSignature() != m_check_signature)
  {
    m_check_at = m_time;  // (twice a second at most while the files are read: a library of thousands)
    BuildCheckRows();
  }
  if (m_online_page == PAGE_TOP && on_tab &&
      (Netplay::TopGames().serial != m_top_serial || m_time - m_top_built > 2.0f))
    BuildTopTable();
  // ((01.07) The world chat asked for every 2 s while its page shows, every 10 s else; its answers said. The players'
  // ranking while its page shows.)
  Netplay::WorldChatShown(on_tab && m_online_page == PAGE_CHAT && !m_panel.open);
  if (on_tab && m_online_page == PAGE_PLAYERS)
    Netplay::WantTopPlayers(m_players_days);
  if (on_tab)
  {
    const Netplay::WorldChatView world = Netplay::GetWorldChat();
    if (world.said_serial != m_world_said_seen)
    {
      m_world_said_seen = world.said_serial;
      using NetProto::WorldResult;
      if (world.said == WorldResult::Sent)
        Play("confirm");
      else
      {
        const std::string wait = std::to_string(std::max(1, world.said_seconds));
        ShowToast(world.said == WorldResult::TooSoon  ? "One message every 3 seconds: wait " + wait + " s" :
                  world.said == WorldResult::TooMany  ? "Too many messages: wait " + wait + " s" :
                  world.said == WorldResult::Busy     ? "The world chat is busy: try again in a moment" :
                  world.said == WorldResult::Blocked  ? (world.said_seconds == 0xFFFF ?
                                                             std::string("EmuSuNi's moderation blocked you from the "
                                                                         "world chat") :
                                                             "EmuSuNi's moderation blocked you from the world chat "
                                                             "for " + std::to_string((world.said_seconds + 59) / 60) +
                                                                 " min") :
                  world.said == WorldResult::NoNotice ? "The world chat's notice must show first" :
                                                        "The message couldn't be sent",
                  0);
        Play("error");
      }
    }
  }
  const bool shown = m_panel.open && m_panel.setting == SET_ONLINE_ROOM;
  // (00.86) Our room ended while we were elsewhere (the server lost it; a game played alone that took long to load):
  // said once the menu shows (closed by us - Close Room, another room - nothing to say).
  {
    const bool hosting = HostingRoom();
    if (m_online_was_hosting && !hosting && !shown && !m_intro)
    {
      const Netplay::View v = Netplay::GetView();
      if (v.phase == Netplay::Phase::Off && !v.error.empty())
      {
        Log("online: our room closed while we were elsewhere: " + v.error);
        ShowToast("Your room closed: " + v.error, 0);
        Netplay::TakeError();
      }
      m_online_was_hosting = false;
    }
    else if (hosting)
    {
      m_online_was_hosting = true;
    }
  }
  // (00.85) The table's list: asked for once the tab is there a moment (an XMB column passed on the way asks for
  // nothing), again every few seconds, a while after a failure; let go when the tab is left or a game starts. Not
  // while the room's panel shows: the room has the connection then. (00.86: our room open, its panel closed - the
  // list on the room's connection.)
  {
    const Netplay::View v = Netplay::GetView();
    if (v.phase == Netplay::Phase::Browsing && (!on_tab || m_launch >= 0))
    {
      Netplay::Leave();
    }
    else if (on_tab && m_launch < 0 && !shown && m_time - m_online_entered >= 0.3f)
    {
      if (v.phase == Netplay::Phase::Off && !v.error.empty() && m_online_retry_at <= 0.0f)
      {
        m_online_retry_at = m_time + 10.0f;  // (the table says why meanwhile)
        m_online_last.clear();
        m_online_listed = false;
      }
      if (v.phase == Netplay::Phase::Off && (v.error.empty() || m_time >= m_online_retry_at))
      {
        m_online_retry_at = 0.0f;
        m_online_refresh_at = m_time + 5.0f;
        Netplay::Browse("", m_data_dir);
      }
      else if (v.phase == Netplay::Phase::Browsing && v.rooms_done && m_time >= m_online_refresh_at)
      {
        m_online_refresh_at = m_time + 5.0f;
        Netplay::RefreshRooms();
      }
      else if (v.host && v.phase == Netplay::Phase::Room && m_time >= m_online_refresh_at)
      {
        m_online_refresh_at = m_time + 5.0f;
        Netplay::RefreshRooms();  // (00.86: ignored while one is on its way)
      }
    }
  }
  if (!shown && Netplay::GetView().phase == Netplay::Phase::Off)
  {
    m_online_ask = -1;
    if (on_tab)
      BuildOnlineTable();
    return;
  }
  Netplay::Update();
  const Netplay::View v = Netplay::GetView();
  // (00.86) A player asking to join our room: a notice over whatever shows - once a keyboard, a launch or the start's
  // animation is over -, not over the room's panel (its Accept, selected, and Reject are there). Gone with the request
  // (answered, expired, the player left); Circle leaves it for the room.
  if (v.asking < 0 || shown)
    m_online_ask = -1;
  if (v.asking >= 0 && v.asking_serial != m_online_ask_serial)
  {
    const bool can_show = !m_keyboard && !m_capture && m_launch < 0 && !m_intro && !Tour::Active();
    if (shown || can_show)
    {
      m_online_ask_serial = v.asking_serial;
      Log("online: " + v.asking_nick + " asks to join our room");
      if (shown)
      {
        m_online_prefer = "accept";
      }
      else
      {
        m_online_ask = v.asking;
        m_online_ask_choice = 0;
      }
      Play("menu");
    }
  }
  // A Choose Together room: this console's games to it once every file is hashed (each one's file found again by its
  // console, CRC32 and size: the common list's titles and covers are this console's).
  if (v.together && v.phase == Netplay::Phase::Room && !m_online_library_sent && NetplayLibrary::GetProgress().finished)
  {
    m_online_library_sent = true;
    m_online_library.clear();
    std::vector<NetProto::LibraryGame> mine;
    for (size_t i = 0; i < m_games.size(); i++)
    {
      const System& system = GetSystem(m_games[i].rom.system);
      NetProto::LibraryGame g;
      g.console = system.key;
      if (!Netplay::RulesFor(system.key).online || !NetplayLibrary::Lookup(m_games[i].rom.path, &g.crc, &g.size))
        continue;
      if (m_online_library.emplace(g, static_cast<int>(i)).second)
        mine.push_back(g);
    }
    Netplay::SetLibrary(mine, true);
  }
  // A Choose Together match: the game the host picked, found among ours by its file.
  if (v.together && v.phase != Netplay::Phase::Room)
  {
    NetProto::LibraryGame g;
    g.console = v.game.console;
    g.crc = v.game.crc;
    g.size = v.game.size;
    const auto at = m_online_library.find(g);
    m_online_game = at == m_online_library.end() ? -1 : at->second;
  }
  if (v.phase == Netplay::Phase::Room && !v.host && !v.together && m_online_found == LobbyGame::Looking &&
      !v.game.console.empty())
  {
    m_online_found = FindOnlineGame(v.game, &m_online_game);
    if (m_online_found == LobbyGame::Different)
      m_online_found = LobbyGame::Asking;  // (the player says: Play Anyway)
    // ((01.05) our file's version, for every player's card)
    Netplay::SetGameVersion(m_online_found == LobbyGame::Missing ? std::string() : OnlineVersionText(m_online_game));
    Netplay::SetLobbyGame(m_online_found);
    Log(std::string("online: the room's game ") + v.game.title + ": " +
        (m_online_found == LobbyGame::Same     ? "the same file here" :
         m_online_found == LobbyGame::Asking   ? "another version here" :
                                                  "not here"));
    m_online_link_game = -1;
    m_online_link_said = false;
  }
  // (00.95) A Game Boy link cable room. A guest: its game for Game Boy 2 - at first the room's - and its save, said
  // once it has the room's game. The host: the guest's game looked for among ours.
  if (v.link && v.phase == Netplay::Phase::Room && !v.host && m_online_found == LobbyGame::Same && !m_online_link_said)
  {
    if (m_online_link_game < 0)
      m_online_link_game = m_online_game;
    SayLinkGame();
  }
  if (v.link && v.phase == Netplay::Phase::Room && v.host && v.link_game_in && v.link_found == LobbyGame::Looking)
  {
    m_online_link_game = FindLinkGame(v.link_game);
    Netplay::SetLinkGameFound(m_online_link_game >= 0);
    Log("online: the guest's game for Game Boy 2 " + v.link_title + ": " +
        (m_online_link_game >= 0 ? "the same file here" : "not here"));
  }
  if (Netplay::TakeStart())
  {
    // ((01.05) A spectator only with the host's very file: another version's frames would go otherwise each time.)
    const bool watch_other = Netplay::GetView().watching && m_online_found != LobbyGame::Same;
    if (watch_other)
    {
      Netplay::Leave();
      ShowToast("Your file of this game isn't the host's: it can't be watched", 0);
    }
    else if (m_online_game >= 0 && m_online_game < static_cast<int>(m_games.size()) && m_launch < 0)
    {
      m_panel.open = false;
      m_launch = m_online_game;
      m_launch_online = true;  // (00.86: its end's error is the match's)
      m_launch_time = 0.0f;
      m_games[static_cast<size_t>(m_online_game)].played = static_cast<int64_t>(std::time(nullptr));
      SavePlayed();
      Play("launch");
      Log("online: the match starts with " + m_games[static_cast<size_t>(m_online_game)].rom.path);
    }
    else
    {
      Netplay::Leave();
      ShowToast("The game isn't in your games: you left the room", 0);
    }
  }
  if (on_tab)
    BuildOnlineTable();
  if (shown && m_launch < 0)
    BuildOnlinePanel();
}

// The online panels' buttons (true: one of them is open, the press is theirs).
bool Menu::Impl::OnlinePanelInput(uint32_t pressed)
{
  const int setting = m_panel.setting;
  if (setting != SET_ONLINE && setting != SET_ONLINE_HOST && setting != SET_ONLINE_ROOM && setting != SET_OL_SETTINGS &&
      setting != SET_OL_CREATE && setting != SET_OL_PROFILE && setting != SET_OL_PUBLIC && setting != SET_OL_CODE &&
      setting != SET_OL_DELETE && setting != SET_MY_STATS && setting != SET_OL_REPLACE && setting != SET_OL_LINKGAME &&
      setting != SET_OL_CHECK && setting != SET_OL_CHECK_LIST)
    return false;
  // (01.05) Check My Games: Up / Down its rows; Cross a console's games; Circle back (Check My Games to Online
  // Settings, a console's games to Check My Games).
  if (setting == SET_OL_CHECK || setting == SET_OL_CHECK_LIST)
  {
    if (pressed & BTN_CIRCLE)
    {
      if (setting == SET_OL_CHECK_LIST)
        OpenCheckGames("", "check:" + m_check_console);
      else
        OpenOnlineSettings("ol:check");
      Play("back");
      return true;
    }
    if (pressed & (BTN_UP | BTN_DOWN))
    {
      MoveSettingRow((pressed & BTN_UP) ? -1 : 1, Fresh((pressed & BTN_UP) ? BTN_UP : BTN_DOWN));
      Play("move");
    }
    if (setting == SET_OL_CHECK && (pressed & BTN_CROSS) && m_panel.selected >= 0 &&
        m_panel.selected < static_cast<int>(m_panel.rows.size()))
    {
      const std::string key = m_panel.rows[static_cast<size_t>(m_panel.selected)].key;
      if (key.rfind("check:", 0) == 0)
      {
        OpenCheckGames(key.substr(6));
        Play("confirm");
      }
    }
    return true;
  }
  // (01.05) The room screen's picker - a quick phrase, a player's actions - takes the buttons; L2 a quick phrase, R2 a
  // line typed (in a room we are in: not while asking the host to join).
  if (setting == SET_ONLINE_ROOM && m_room_picker.open)
    return RoomPickerInput(pressed);
  if (setting == SET_ONLINE_ROOM && (pressed & (BTN_L2 | BTN_R2)))
  {
    const Netplay::View v = Netplay::GetView();
    if ((v.phase == Netplay::Phase::Room || v.phase == Netplay::Phase::Starting) && !v.waiting_host && !v.watching)
    {
      if (pressed & BTN_L2)
        OpenQuickPhrases();
      else
        WriteChatLine();
      return true;
    }
  }
  // (00.95) A link cable room's game for Game Boy 2: Circle back to the room.
  if (setting == SET_OL_LINKGAME && (pressed & BTN_CIRCLE))
  {
    OpenOnlineRoom();
    Play("back");
    return true;
  }
  // (00.85) Online Settings' pages: Circle back to it, on their row (My Stats: Cross too - it has nothing to choose).
  const char* settings_row = setting == SET_OL_PROFILE ? "ol:profile" : setting == SET_OL_PUBLIC ? "ol:public" :
                             setting == SET_OL_CODE    ? "ol:code" :
                             setting == SET_OL_DELETE  ? "ol:delete" :
                             setting == SET_MY_STATS   ? "ol:stats" :
                                                         nullptr;
  if (settings_row && ((pressed & BTN_CIRCLE) || (setting == SET_MY_STATS && (pressed & BTN_CROSS))))
  {
    OpenOnlineSettings(settings_row);
    Play("back");
    return true;
  }
  if (setting == SET_MY_STATS)
    return true;
  if (pressed & BTN_CIRCLE)
  {
    // Out of the room (it is left; why it ended, if it did, was said here: the Online tab's table doesn't say it again).
    // (00.86) The host's room stays open: back to the menus - the Online tab's table has it on top (Close Room closes
    // it). A guest leaves, as before (its request to join too).
    if (setting == SET_ONLINE_ROOM && HostingRoom())
    {
      m_panel.open = false;
      m_online_prefer.clear();
      Log("online: our room stays open while we are elsewhere");
      ShowToast("Your room stays open: the Online tab takes you back to it", 0);
      Play("back");
      return true;
    }
    if (setting == SET_ONLINE_ROOM)
    {
      Netplay::Leave();
      Netplay::TakeError();
    }
    if (setting == SET_OL_REPLACE)
      m_online_after_close = nullptr;  // (00.86: kept - nothing else done)
    if (setting == SET_ONLINE_HOST)
    {
      if (m_online_game < 0)
        OpenOnlinePanel();  // (Choose Together: back to Online Play)
      else
        OpenGameOptions();
      return true;
    }
    m_panel.open = false;
    Play("back");
    return true;
  }
  if (setting == SET_ONLINE_ROOM && m_together.on)
    return TogetherInput(pressed);  // (00.92: a Choose Together room's screen)
  if (setting == SET_OL_SETTINGS)
  {
    // (00.85) Online Settings: Cross on a row opens its page (or does it: Nickname's keyboard, Hide My Address on or
    // off - Left / Right too).
    if (pressed & (BTN_UP | BTN_DOWN))
    {
      MoveSettingRow((pressed & BTN_UP) ? -1 : 1, Fresh((pressed & BTN_UP) ? BTN_UP : BTN_DOWN));
      Play("move");
    }
    if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.rows.size()))
      return true;
    const std::string key = m_panel.rows[static_cast<size_t>(m_panel.selected)].key;
    if (key == "ol:hide" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("hide");
    else if (key == "ol:bar" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("bar");
    else if (key == "ol:chattext" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("chattext");  // (01.05)
    else if (key == "ol:country" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("country");  // (01.05)
    else if (key == "ol:unblock" && (pressed & BTN_CROSS))
      OnlineAction("unblock");
    else if (key == "ol:world" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("world");  // (01.07)
    else if (key == "ol:ranking" && (pressed & (BTN_CROSS | BTN_LEFT | BTN_RIGHT)))
      OnlineAction("ranking");
    else if (key == "ol:worldmuted" && (pressed & BTN_CROSS))
      OnlineAction("worldmuted");
    else if (!(pressed & BTN_CROSS))
      return true;
    else if (key == "ol:nick")
      OnlineAction("nick");
    else if (key == "ol:stats")
      OpenMyStats();
    else if (key == "ol:check")
    {
      OpenCheckGames("");  // (01.05)
      Play("confirm");
    }
    else
      OpenSettingPanel(key == "ol:profile" ? SET_OL_PROFILE : key == "ol:public" ? SET_OL_PUBLIC :
                       key == "ol:code"    ? SET_OL_CODE :
                                             SET_OL_DELETE);
    return true;
  }
  const int count = static_cast<int>(m_panel.options.size());
  // (00.95) Play Online's Players: Left / Right (Cross: one more, round to 2).
  if (setting == SET_ONLINE_HOST && m_panel.selected >= 0 &&
      m_panel.selected < static_cast<int>(m_online_actions.size()) &&
      m_online_actions[static_cast<size_t>(m_panel.selected)] == "players" &&
      (pressed & (BTN_LEFT | BTN_RIGHT | BTN_CROSS)))
  {
    const int span = m_online_players_most - 1;
    const int step = (pressed & BTN_LEFT) ? -1 : 1;
    m_online_players = 2 + ((m_online_players - 2 + step) % span + span) % span;
    m_panel.options[static_cast<size_t>(m_panel.selected)] = "Players: " + std::to_string(m_online_players);
    if (static_cast<size_t>(m_panel.selected) < m_panel.helps.size())
      m_panel.helps[static_cast<size_t>(m_panel.selected)] = OnlinePlayersHelp();
    Play("move");
    return true;
  }
  // ((01.15) The room screen: its options in columns under the cards - RoomPlaces.)
  if (RoomScreenShown())
  {
    if (pressed & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))
      MoveRoomSelection(pressed);
  }
  else
  {
    for (const uint32_t arrow : {BTN_UP, BTN_DOWN})
    {
      const int next = count > 0 ? MenuNav::Step(m_panel.selected, arrow == BTN_UP ? -1 : 1, count, Fresh(arrow)) : 0;
      if ((pressed & arrow) && next != m_panel.selected)
      {
        m_panel.selected = next;
        m_online_prefer.clear();  // (00.86: the player's choice from now on)
        Play("move");
      }
    }
  }
  if (!(pressed & BTN_CROSS) || m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_online_actions.size()))
    return true;
  const std::string action = m_online_actions[static_cast<size_t>(m_panel.selected)];
  if (OnlineAction(action))
    return true;
  if (action == "linkgame")
  {
    OpenLinkGamePicker();  // (00.95)
    return true;
  }
  if (action.rfind("lg:", 0) == 0)
  {
    m_online_link_game = std::atoi(action.c_str() + 3);
    SayLinkGame();  // (the host looks for it again)
    OpenOnlineRoom();
    Play("confirm");
    return true;
  }
  if ((action == "private" || action == "public") && m_online_game < 0)
  {
    // A Choose Together room (its games' list once the other player is in).
    m_online_library_sent = false;
    StartOnlineLibrary();
    Netplay::HostTogether(action == "public", m_data_dir, m_online_players);
    OpenOnlineRoom();
    Play("confirm");
  }
  else if (action == "private" || action == "public")
  {
    NetProto::GameInfo info;
    if (!OnlineGameInfo(m_online_game, &info))
    {
      ShowToast("The game's file can't be read", 0);
      Play("error");
      return true;
    }
    m_online_found = NetProto::LobbyGame::Same;
    // (00.95) A Game Boy link cable room: its console the link's, two players.
    m_online_link_game = -1;
    m_online_link_said = false;
    if (m_online_link)  // (01.04: a GBA game's room is a Game Boy Advance link room)
      info.console = m_games[static_cast<size_t>(m_online_game)].rom.system == SystemId::GBA ? NetProto::GBALINK :
                                                                                             NetProto::GBLINK;
    // ((01.06) Its whole file's CRC32 too: a player of v00.23 knows the game by it - not a link cable room's, which
    // plays with its own version only.)
    else
      OnlineFileId(m_online_game, &info.file_crc, &info.file_size);
    Netplay::Host(info, action == "public", m_data_dir, m_online_link ? 2 : m_online_players);
    Netplay::SetGameVersion(OnlineVersionText(m_online_game));  // ((01.05) with the room's Create: the list shows it)
    OpenOnlineRoom();
    Play("confirm");
  }
  else if (action == "start")
  {
    if (!Netplay::StartMatch())
    {
      ShowToast("The other player isn't ready yet", 0);
      Play("error");
    }
  }
  else if (action == "chat:phrase")  // (01.05) the chat
    OpenQuickPhrases();
  else if (action == "chat:text")
    WriteChatLine();
  else if (action.rfind("player:", 0) == 0)
    OpenPlayerActions(std::atoi(action.c_str() + 7));
  else if (PickerAction(action))
  {
    // ((01.07) a quick phrase, a player's mute, block or removal: the picker's actions - PickerAction)
  }
  else if (action.rfind("accept:", 0) == 0)
  {
    // (00.86) The player asking to join is let in; the selection waits for Start (BuildOnlinePanel).
    Netplay::Admit(std::atoi(action.c_str() + 7));
    m_online_prefer = "start";
    Play("confirm");
  }
  else if (action.rfind("reject:", 0) == 0)
  {
    const Netplay::View v = Netplay::GetView();
    Netplay::Refuse(std::atoi(action.c_str() + 7));
    ShowToast(v.asking_nick + " was not accepted", 0);
    Play("back");
  }
  else if (action == "anyway")
  {
    m_online_found = NetProto::LobbyGame::Different;
    Netplay::SetLobbyGame(NetProto::LobbyGame::Different);
    Play("confirm");
  }
  else if (action == "leave" || action == "close")
  {
    Netplay::Leave();
    Netplay::TakeError();  // (00.85: said here already - the table doesn't say it again)
    m_panel.open = false;
    Play("back");
  }
  return true;
}

// ---- Updates (00.73, src/Update.h) ----

std::string Menu::Impl::UpdatesDetail(const Update::Status& status) const
{
  if (status.newer)
    return "Version " + status.newest.version + " is available";
  return std::string("This version: ") + App::VERSION;
}

// Settings -> Check for Updates (a check now, unless one ran in the last two minutes or a download runs), or the
// start's notice of a newer version (shown once a session: Later only closes it).
void Menu::Impl::OpenUpdatePanel(bool notice)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = notice ? SET_UPDATE_NOTICE : SET_UPDATES;
  if (notice)
  {
    Update::NoticeShown();
  }
  else
  {
    const Update::Status status = Update::Get();
    // (Nor over an update under way or one ready to finish: its page shows.)
    const bool working = status.busy || status.phase == Update::Phase::Checking ||
                         status.phase == Update::Phase::Downloading || status.phase == Update::Phase::Verifying ||
                         status.phase == Update::Phase::Installing || status.phase == Update::Phase::Installed ||
                         status.phase == Update::Phase::Saved;
    const bool recent = status.phase == Update::Phase::Checked && status.message.find("failed") == std::string::npos &&
                        std::time(nullptr) - status.checked_at < 120;
    if (!working && !recent)
      Update::Check();
  }
  m_update_serial = -1;
  BuildUpdatePanel();
  Play("menu");
}

void Menu::Impl::BuildUpdatePanel()
{
  const Update::Status s = Update::Get();
  m_update_serial = s.serial;
  const bool notice = m_panel.setting == SET_UPDATE_NOTICE;
  const std::string running = App::VERSION;
  std::vector<std::string> options, lines;
  // A text in the panel's lines, translated first and wrapped to its width; at most `most` lines of it.
  auto add = [&](const std::string& text, size_t most) {
    std::istringstream in(WrapText(m_font, 28, PS4Lang::T(text), 550.0f));
    size_t count = 0;
    for (std::string line; std::getline(in, line) && count < most; count++)
      lines.push_back(line);
  };
  // The release notes' first lines, `most` lines of the panel in all (clear of the help text at the bottom).
  auto notes = [&](size_t most) {
    const size_t end = lines.size() + most;
    for (const std::string& note : Update::NoteLines(s.newest.notes, most))
    {
      if (lines.size() >= end)
        break;
      add(note, std::min<size_t>(2, end - lines.size()));
    }
  };
  if (notice)
  {
    m_panel.title = s.important ? "Important Update" : "Update Available";
    options = {"Update Now", "Later"};
    lines.push_back("");
    add("EmuSuNi " + s.newest.version + " is out (you have " + running + ").", 2);
    if (s.important)
      add("This update is important: please install it.", 2);
    lines.push_back("");
    notes(7);
  }
  else
  {
    m_panel.title = "Check for Updates";
    lines.push_back("");
    lines.push_back("This version: " + running);
    if (!s.newest.tag.empty())
      lines.push_back("Newest: " + s.newest.version + (s.newest.prerelease ? " (test version)" : ""));
    lines.push_back("");
    const std::string tests = s.tests ? "Include Test Versions: On" : "Include Test Versions: Off";
    char progress[96] = "";
    if (s.total > 0)
    {
      snprintf(progress, sizeof(progress), "%.1f|%.1f|%d", static_cast<double>(s.done) / (1024.0 * 1024.0),
               static_cast<double>(s.total) / (1024.0 * 1024.0),
               static_cast<int>(std::clamp<int64_t>(s.done * 100 / s.total, 0, 100)));
    }
    std::string done_mb, total_mb, percent;
    {
      std::istringstream parts(progress);
      std::getline(parts, done_mb, '|');
      std::getline(parts, total_mb, '|');
      std::getline(parts, percent, '|');
    }
    switch (s.phase)
    {
    case Update::Phase::Idle:
    case Update::Phase::Checking:
      lines.push_back("Checking for updates...");
      break;
    case Update::Phase::Downloading:
      add(s.message, 2);
      if (!percent.empty())
        lines.push_back("Downloaded: " + done_mb + " of " + total_mb + " MB (" + percent + "%)");
      if (!s.saved.empty())
        add("File: " + s.saved, 2);
      lines.push_back("");
      lines.push_back("Circle closes this; the download goes on.");
      options = {"Stop the Download"};
      break;
    case Update::Phase::Verifying:
      add(s.message, 2);
      if (!percent.empty())
        lines.push_back("Checked: " + percent + "%");
      break;
    case Update::Phase::Installing:
      add(s.message, 3);
      if (!percent.empty())
        lines.push_back("Copied by the PS4: " + percent + "%");
      if (!s.detail.empty())
        add(s.detail, 3);
      break;
    case Update::Phase::Installed:
      add(s.message, 2);
      if (!s.detail.empty())
        add(s.detail, 3);
      options = {"Close EmuSuNi"};
      break;
    case Update::Phase::Saved:
    case Update::Phase::Failed:
      add(s.message, 2);
      if (!s.detail.empty())
        add(s.detail, 4);
      if (s.phase == Update::Phase::Failed && s.newer)
        options = {"Update Now", "Check Again", tests};
      else
        options = {"Check Again", tests};
      break;
    case Update::Phase::Checked:
      add(s.message.empty() ? std::string(s.newer ? "A newer version is available" : "You have the newest version") :
                              s.message,
          2);
      if (s.newer)
      {
        if (s.important)
          add("This update is important: please install it.", 2);
        notes(3);
        options = {"Update Now", "Check Again", tests};
      }
      else
      {
        options = {"Check Again", tests};
      }
      break;
    }
  }
  if (options != m_panel.options)
  {
    m_panel.options = options;
    m_panel.selected = 0;
  }
  m_panel.lines = lines;
}

// Cross on one of the update panels' options.
void Menu::Impl::UpdatePanelChoice()
{
  if (m_panel.selected < 0 || m_panel.selected >= static_cast<int>(m_panel.options.size()))
    return;
  const std::string choice = m_panel.options[m_panel.selected];
  if (choice == "Later")
  {
    Log("update: the player chose Later");
    m_panel.open = false;
    Play("back");
    return;
  }
  if (choice == "Update Now")
  {
    Update::StartUpdate();
    m_panel.setting = SET_UPDATES;  // (the notice becomes the download's page)
    BuildUpdatePanel();
    Play("confirm");
    return;
  }
  if (choice == "Check Again")
  {
    Update::Check();
    BuildUpdatePanel();
    Play("confirm");
    return;
  }
  if (choice == "Stop the Download")
  {
    Update::Cancel();
    Play("back");
    return;
  }
  if (choice == "Close EmuSuNi")
  {
    // The PS4 finishes installing the update once the app is closed (as with a game's update).
    Log("update: the player closes EmuSuNi to finish the update");
    Netplay::Leave();  // (00.86: our room, open in the background, closes with the app - the others told)
    LogSync();
    sceSystemServiceLoadExec("exit", nullptr);
    return;
  }
  if (choice.rfind("Include Test Versions", 0) == 0)
  {
    Update::SetTests(!Update::Get().tests);
    BuildUpdatePanel();
    Play("confirm");
  }
}

// Every frame: the update panel and the Settings row follow the updater; the start's notice when a newer version is
// known - in the menu, with nothing else open (not over the first start's language list, the tour or a keyboard).
void Menu::Impl::UpdateUpdater()
{
  if (!m_update_started)
    return;
  const int serial = Update::Serial();
  if (serial != m_update_serial)
  {
    if (m_panel.open && (m_panel.setting == SET_UPDATES || m_panel.setting == SET_UPDATE_NOTICE))
      BuildUpdatePanel();
    else
      m_update_serial = serial;
    const std::string detail = UpdatesDetail(Update::Get());
    for (Item& item : m_items[static_cast<int>(Category::Settings)])
    {
      if (item.setting == SET_UPDATES)
        item.detail = detail;
    }
  }
  // (00.89) The statistics' notice first (once), then the update's; the statistics start once it is gone.
  const bool idle = !m_panel.open && !m_intro && !m_keyboard && !m_capture && m_launch < 0 && !Tour::Active();
  // (00.91) A send under way or done: its panel follows it.
  if (m_panel.open && m_panel.setting == SET_SEND_LOG && LogReport::GetStatus().serial != m_report_serial)
    BuildSendLogPanel();
  if (idle && m_stats_notice)
    OpenStatsNotice();
  else if (idle && m_report_ask)
    OpenCrashQuestion();
  else if (m_stats_waiting && !m_stats_notice && !(m_panel.open && m_panel.setting == SET_STATS_NOTICE))
    StartStats();
  else if (idle && Update::NoticeWanted())
    OpenUpdatePanel(true);
}

// ---- Portable data (00.87, src/Portable.h) ----

std::string Menu::Impl::PortableDetail() const
{
  if (Portable::Lost())
    return "Drive removed: saving to this PS4";
  if (Portable::Active())
    return "On: " + Portable::DriveRoot();
  return "Off";
}

namespace
{
// A panel's text, translated first and wrapped to its width, line by line into `lines` (as the update panel's).
void AddWrapped(const std::function<std::string(const std::string&)>& wrap, const std::string& text,
                std::vector<std::string>* lines)
{
  std::istringstream in(wrap(PS4Lang::T(text)));
  for (std::string line; std::getline(in, line);)
    lines->push_back(line);
}
}  // namespace

// Settings -> Keep My Data on the USB Drive: on a drive (Turn On, or a drive's data: Use It, Add My Data), in use (Turn
// Off, with the copy back or not), or the drive in use taken out (Restart Now).
void Menu::Impl::OpenPortablePanel()
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_PORTABLE;
  m_panel.title = "Data on a USB Drive";
  m_portable_actions.clear();
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  auto add = [&](const std::string& text) { AddWrapped(wrap, text, &lines); };
  auto option = [this](const std::string& label, const std::string& action, const std::string& help) {
    m_panel.options.push_back(label);
    m_portable_actions.push_back(action);
    m_panel.helps.push_back(help);
  };
  if (Portable::Lost())
  {
    add("The USB drive with your data was removed: saving to this PS4 until EmuSuNi starts again with it.");
    option("Restart Now", "restart",
           "EmuSuNi closes and opens again: with the drive's data if it is connected, else with this PS4's.");
    option("OK", "back", "");
  }
  else if (Portable::Active())
  {
    add("On: your saves, settings and game list are on the USB drive:");
    lines.push_back(Portable::DataFolder());
    lines.push_back("");
    add("BIOS files: put yours in EmuSuNi-Portable/system on the drive to use them on any PS4.");
    option("Turn Off and Copy Back", "off_copy",
           "Copies the drive's data to this PS4 (what it hasn't got, or has older; nothing is deleted), then EmuSuNi "
           "restarts with this PS4's data.");
    option("Turn Off", "off", "EmuSuNi restarts with this PS4's own data, as it was. The drive keeps its data.");
    option("Cancel", "back", "");
  }
  else
  {
    // The drives the menu knows now (its last look), never one that looks like the PS4's own storage.
    std::vector<std::string> drives;
    for (const std::string& drive : m_places.drives)
    {
      if (!Library::LooksLikePs4Storage(drive))
        drives.push_back(drive);
    }
    const std::vector<Portable::DriveData> found = Portable::Look(drives);
    const bool one = drives.size() == 1;
    for (const std::string& drive : drives)
    {
      const bool data = std::any_of(found.begin(), found.end(),
                                    [&drive](const Portable::DriveData& d) { return d.root == drive; });
      if (!data)
      {
        option(one ? std::string("Turn On") : "Turn On: " + drive, "on:" + drive,
               "Copies your data to the drive (its folder EmuSuNi-Portable; nothing here is deleted), then EmuSuNi "
               "restarts and uses it.");
        continue;
      }
      option(one ? std::string("Use the Drive's Data") : "Use the Data on " + drive, "use:" + drive,
             "EmuSuNi restarts and plays with the drive's data (from this PS4 or another). This PS4's own data isn't "
             "changed.");
      option(one ? std::string("Add My Data and Use It") : "Add My Data to " + drive, "merge:" + drive,
             "Copies to the drive what it hasn't got of your data (its own files are kept), then EmuSuNi restarts and "
             "uses it.");
    }
    if (drives.empty())
    {
      add("Connect a USB drive (exFAT or FAT32) to keep your data on it.");
      lines.push_back("");
    }
    add("Your saves, states, settings, cheats, bezels, covers and game list in EmuSuNi-Portable on a USB drive: take "
        "it to another PS4 with EmuSuNi.");
    if (m_panel.options.size() < 3)
    {
      lines.push_back("");
      add("Not copied: your RetroAchievements login, online profile and statistics. BIOS files neither: put yours in "
          "EmuSuNi-Portable/system on the drive.");
    }
    option("Cancel", "back", "");
  }
  m_panel.lines = std::move(lines);
  Play("menu");
}

// A drive with portable data the menu's look found: never answered for on this PS4 (Use it? Yes / No), or used here
// (Restart Now / Later: the next start uses it).
void Menu::Impl::OpenPortableQuestion(bool notice)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.title = "Data on a USB Drive";
  m_portable_actions.clear();
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  auto add = [&](const std::string& text) { AddWrapped(wrap, text, &lines); };
  auto option = [this](const std::string& label, const std::string& action, const std::string& help) {
    m_panel.options.push_back(label);
    m_portable_actions.push_back(action);
    m_panel.helps.push_back(help);
  };
  if (!notice)
  {
    m_panel.setting = SET_PORTABLE_ASK;
    add("This USB drive (" + m_portable_drive + ") has EmuSuNi data: saves, settings, cheats, covers and a game list.");
    lines.push_back("");
    add("Use the data on this drive?");
    lines.push_back("");
    add("This PS4's own data isn't changed, and is used again when the drive isn't there.");
    option("Yes", "yes", "EmuSuNi restarts with the drive's data (this PS4 remembers it: next time at once).");
    option("No", "no", "This PS4's own data. Settings -> Keep My Data on the USB Drive can still use the drive's.");
  }
  else
  {
    m_panel.setting = SET_PORTABLE_NOTICE;
    add("The USB drive with your EmuSuNi data (" + m_portable_drive + ") is connected: it is used from the next start.");
    option("Restart Now", "restart", "EmuSuNi closes and opens again with the drive's data.");
    option("Later", "back", "");
  }
  m_panel.lines = std::move(lines);
  Play("menu");
}

void Menu::Impl::OpenPortableWarning(const std::string& warning)
{
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_PORTABLE_WARNING;
  m_panel.title = "Data on a USB Drive";
  m_portable_actions.clear();
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  AddWrapped(wrap, warning, &lines);
  if (Portable::Lost())
  {
    lines.push_back("");
    AddWrapped(wrap, "Games start again once EmuSuNi restarts.", &lines);
    m_panel.options.push_back("Restart Now");
    m_portable_actions.push_back("restart");
    m_panel.helps.push_back(
        "EmuSuNi closes and opens again: with the drive's data if it is connected, else with this PS4's.");
  }
  m_panel.options.push_back("OK");
  m_portable_actions.push_back("back");
  m_panel.helps.push_back("");
  m_panel.lines = std::move(lines);
  for (Item& item : m_items[static_cast<int>(Category::Settings)])
  {
    if (item.setting == SET_PORTABLE)
      item.detail = PortableDetail();
  }
  Play("error");
}

// ---- The anonymous statistics' notice (00.89) ----

// Whether it goes is the owner's (STATS_NOTICE). Once: at the first start with the statistics - a new install, or the
// first after the update that brought them (v00.23) -, before any heartbeat: UsageStats starts once it is answered or
// closed (its first heartbeat 10 s later), so Turn Them Off sends nothing at all. settings.ini "stats_notice" says it
// was shown. data_dir/stats-notice-test shows it at every start (to see it on the PS4 before it goes).
constexpr bool STATS_NOTICE = false;

bool Menu::Impl::StatsNoticeWanted() const
{
  if (FileExists(m_data_dir + "/stats-notice-test"))
    return true;
  if (!STATS_NOTICE)
    return false;
  for (const auto& [key, value] : Settings::GlobalValues())
  {
    if (key == "stats_notice")
      return false;
  }
  return true;
}

void Menu::Impl::OpenStatsNotice()
{
  m_stats_notice = false;
  Settings::Set("stats_notice", "shown");
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_STATS_NOTICE;
  m_panel.title = "Anonymous Statistics";
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  auto add = [&](const std::string& text) { AddWrapped(wrap, text, &lines); };
  add("EmuSuNi sends anonymous statistics: how many use it and from which countries, its version, the PS4's "
      "language, the console and the game played (a checksum or a disc's code, never a file's name).");
  lines.push_back("");
  add("Never sent: names, PSN, your list of games, your files. The server keeps no address.");
  lines.push_back("");
  add("Off, nothing at all is sent. Change it any time: Settings -> Anonymous Statistics.");
  m_panel.options = {"OK", "Turn Them Off"};
  m_panel.helps = {"They stay as they are. They show which consoles and games to improve first.",
                   "Nothing is sent. Online play still works; the online profile needs them."};
  m_panel.lines = std::move(lines);
  Log("stats: the notice shown");
  Play("menu");
}

void Menu::Impl::StartStats()
{
  if (!m_stats_waiting)
    return;
  m_stats_waiting = false;
  UsageStats::Start(m_data_dir, App::VERSION);
}

// ---- How to Help (00.90) ----

// Settings -> How to Help: EmuSuNi is free, and asks for no money (FinalBurn Neo's licence forbids asking for donations
// for a project with its code: assets/licenses): a star on GitHub, a report with the log, a word to others. The PS4
// opens no link: the project's website (00.95: emusuni.com - before, its GitHub page) as a QR code under its address
// (assets/xmb/help-qr.png, scripts/make-help-qr.py), drawn by DrawSidePanel.
void Menu::Impl::OpenHelpPanel()
{
  m_panel.title = "How to Help";
  std::vector<std::string> lines = {""};
  AddWrapped([this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); },
             "EmuSuNi is free. You can help it without spending anything:", &lines);
  lines.push_back("");
  for (const char* text : {"Give it a star on GitHub.",
                           "Report what fails in GitHub's Issues, with /data/EmuSuNi/emusuni.log attached.",
                           "Tell others about it and share your videos."})
  {
    // A dash, the lines after it under its text.
    std::istringstream in(WrapText(m_font, 28, PS4Lang::T(text), 520.0f));
    std::string line;
    for (bool first = true; std::getline(in, line); first = false)
      lines.push_back((first ? "- " : "   ") + line);
  }
  lines.push_back("");
  m_panel.lines = std::move(lines);
  // (Each QR code under the lines, its name under it: DrawSidePanel.)
  if (m_help_qr == ImTextureID{})
    m_help_qr = LoadPngTexture(m_assets_dir + "/xmb/help-qr.png");
  if (m_telegram_qr == ImTextureID{})
    m_telegram_qr = LoadPngTexture(m_assets_dir + "/xmb/telegram-qr.png");  // (01.04)
}

// ---- Send Log (00.91, src/LogReport.h) ----

// Settings -> Send Log: what goes and Send (a send under way: how it goes).
void Menu::Impl::OpenSendLogPanel()
{
  m_report_confirm = LogReport::GetStatus().state != LogReport::State::Sending;
  BuildSendLogPanel();
}

// The panel as the send is: asked (what goes, Send / Cancel), sending, sent (its code, big: DrawSidePanel), or not sent
// (why, Try Again / Close). Made again when the send's state changes (UpdateUpdater); the slide as it is.
void Menu::Impl::BuildSendLogPanel()
{
  const LogReport::Status status = LogReport::GetStatus();
  m_report_serial = status.serial;
  m_panel.open = true;
  m_panel.setting = SET_SEND_LOG;
  m_panel.title = "Send Log";
  m_panel.options.clear();
  m_panel.helps.clear();
  m_panel.rows.clear();
  m_panel.selected = 0;
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  auto add = [&](const std::string& text) { AddWrapped(wrap, text, &lines); };
  if (m_report_confirm || status.state == LogReport::State::Idle)
  {
    add("EmuSuNi's log goes to its creator, to find what went wrong: what the app did this time and the time before "
        "(games started, settings, errors), how it closed (crash.log) and its version.");
    lines.push_back("");
    add("Not sent: your games, saves or other files. Your nickname and user names are taken out; the server keeps "
        "only the country, never your address, and deletes the log after 90 days.");
    lines.push_back("");
    add("You get a code: write it in your report on GitHub (Issues) or Telegram.");
    m_panel.options = {"Send", "Cancel"};
    m_panel.helps = {"Sends the log now (a few seconds).", "Nothing is sent."};
  }
  else if (status.state == LogReport::State::Sending)
  {
    add("Sending the log to EmuSuNi's server...");
  }
  else if (status.state == LogReport::State::Sent)
  {
    m_panel.title = "Log Sent";
    lines.clear();  // (under the code)
    add("This is its code: write it in your report on GitHub (Issues) or Telegram, with what happened.");
    // (01.04, the owner) The group on Telegram as a QR code under it (DrawSidePanel).
    if (m_telegram_qr == ImTextureID{})
      m_telegram_qr = LoadPngTexture(m_assets_dir + "/xmb/telegram-qr.png");
    m_panel.options = {"OK"};
    m_panel.helps = {"Closes this. Settings -> Send Log sends a new one."};
  }
  else
  {
    add(status.plain);
    m_panel.options = {"Try Again", "Close"};
    m_panel.helps = {"Sends the log again.", "Nothing is sent."};
  }
  m_panel.lines = std::move(lines);
}

void Menu::Impl::SendLogChoice()
{
  const std::string option = m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.options.size()) ?
                                 m_panel.options[static_cast<size_t>(m_panel.selected)] :
                                 std::string();
  if (option == "Send" || option == "Try Again" || option == "Yes, Send It")
  {
    StartSendingLog();
    return;
  }
  m_panel.open = false;
  Play(option == "OK" ? "confirm" : "back");
}

// The send begun (src/LogReport.h): this player's own names taken out by the app, the server's address on its thread.
void Menu::Impl::StartSendingLog()
{
  LogReport::Ask ask;
  ask.data_dir = m_data_dir;
  const std::string ra_user = Achievements::GetAccount().user;
  if (!ra_user.empty())
    ask.names.emplace_back(ra_user, "<ra-user>");
  const std::string nickname = Netplay::GetNickname(m_data_dir);
  if (!nickname.empty())
    ask.names.emplace_back(nickname, "<player>");
  const std::string data_dir = m_data_dir;
  ask.server = [data_dir] {
    const std::vector<std::string> servers = Netplay::MainServers(data_dir, 5.0);
    return servers.empty() ? std::string() : servers.front();
  };
  m_report_confirm = false;
  LogReport::Send(ask);
  BuildSendLogPanel();
  Play("confirm");
}

// At the start after EmuSuNi closed by itself (src/LogReport.h CrashToAsk), once the menu is free: send its log? Asked
// once (Circle too counts as No).
void Menu::Impl::OpenCrashQuestion()
{
  m_report_ask = false;
  LogReport::CrashAnswered(m_data_dir);
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_SEND_LOG_ASK;
  m_panel.title = "EmuSuNi Closed";
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  AddWrapped(wrap, "EmuSuNi closed by itself last time.", &lines);
  lines.push_back("");
  AddWrapped(wrap,
             "Send its log to its creator, to find why? It has what the app did, how it closed and its version. Your "
             "nickname and user names are taken out; the server keeps only the country, never your address, for 90 "
             "days.",
             &lines);
  m_panel.options = {"Yes, Send It", "No"};
  m_panel.helps = {"Sends it now: you get a code to write in a report on GitHub or Telegram.",
                   "Nothing is sent. Settings -> Send Log can send it later."};
  m_panel.lines = std::move(lines);
  Log("report: asked after EmuSuNi closed by itself");
  Play("menu");
}

void Menu::Impl::PortablePanelChoice()
{
  const size_t at = static_cast<size_t>(m_panel.selected);
  const std::string action = at < m_portable_actions.size() ? m_portable_actions[at] : std::string("back");
  const auto after = [&action](const std::string& head) {
    return action.compare(0, head.size(), head) == 0 ? action.substr(head.size()) : std::string();
  };
  if (action == "back")
  {
    m_panel.open = false;
    Play("back");
  }
  else if (action == "restart")
  {
    RestartApp("the player chose Restart Now");
  }
  else if (action == "stop")
  {
    if (m_portable_progress)
      m_portable_progress->stop = true;
    Play("back");
  }
  else if (action == "yes" || action == "no")
  {
    Portable::Remember(m_portable_id, action);
    if (action == "yes")
    {
      RestartApp("the data on " + m_portable_drive + " is used");
      return;
    }
    m_panel.open = false;
    ShowToast("This PS4's data is used", 0);
    Play("back");
  }
  else if (action == "off")
  {
    Portable::Remember(Portable::DriveId(), "off");
    RestartApp("the data on the USB drive is turned off");
  }
  else if (action == "off_copy")
  {
    StartPortableCopy(Portable::Copying::Back, Portable::DriveRoot(), Portable::Policy::Newer);
  }
  else if (const std::string drive = after("on:"); !drive.empty())
  {
    // (A drive without portable data: what a copy cut before left there is its own - newer files go over it.)
    StartPortableCopy(Portable::Copying::ToDrive, drive, Portable::Policy::Newer);
  }
  else if (const std::string drive = after("merge:"); !drive.empty())
  {
    StartPortableCopy(Portable::Copying::ToDrive, drive, Portable::Policy::Missing);
  }
  else if (const std::string drive = after("use:"); !drive.empty())
  {
    const std::string id = Portable::DriveDataId(drive);
    if (id.empty())
    {
      ShowToast("The drive's data can't be read", 0);
      Play("error");
      return;
    }
    Portable::Remember(id, "yes");
    RestartApp("the data on " + drive + " is used");
  }
}

// The copy on a thread of its own, its page as it goes: this PS4's data to a drive (Turn On, Add My Data) or the drive's
// to this PS4 (Turn Off and Copy Back). The page stays until it ends (Stop stops it at its next file).
void Menu::Impl::StartPortableCopy(Portable::Copying copying, const std::string& drive, Portable::Policy policy)
{
  if (m_portable_thread.joinable() || drive.empty())
    return;
  Portable::CopyPlan plan;
  plan.copying = copying;
  plan.policy = policy;
  plan.drive = drive;
  if (copying == Portable::Copying::ToDrive)
  {
    plan.from = m_data_dir;
    plan.to = Portable::FolderOf(drive);
    plan.writer = Portable::NewWord();
  }
  else
  {
    plan.from = Portable::DataFolder();
    plan.to = m_data_dir;
  }
  m_prefs.Save(UserDir() + "/xmb.ini");  // (the menu's as it is now: in the copy)
  m_portable_plan = plan;
  m_portable_progress = std::make_unique<Portable::CopyProgress>();
  m_portable_ok = m_portable_ended = false;
  Log("portable: copying " + plan.from + " -> " + plan.to +
      (policy == Portable::Policy::Missing ? " (what it hasn't got)" : " (what it hasn't got or has older)"));
  Portable::CopyProgress* progress = m_portable_progress.get();
  m_portable_thread = std::thread([plan, progress] { Portable::CopyData(plan, progress); });
  m_panel = {};
  m_panel.open = true;
  m_panel.setting = SET_PORTABLE_COPY;
  BuildPortableCopyPanel();
  Play("confirm");
}

bool Menu::Impl::PortableCopyBusy() const
{
  return m_portable_progress && !m_portable_ended;
}

void Menu::Impl::BuildPortableCopyPanel()
{
  m_portable_built = m_time;
  if (!m_portable_progress)
    return;
  Portable::CopyProgress& p = *m_portable_progress;
  const bool to_drive = m_portable_plan.copying == Portable::Copying::ToDrive;
  m_panel.title = to_drive ? "Copying Your Data" : "Copying to This PS4";
  m_panel.options.clear();
  m_panel.helps.clear();
  m_portable_actions.clear();
  std::vector<std::string> lines = {""};
  const auto wrap = [this](const std::string& text) { return WrapText(m_font, 28, text, 550.0f); };
  auto add = [&](const std::string& text) { AddWrapped(wrap, text, &lines); };
  auto mb = [](uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20); };
  if (!m_portable_ended)
  {
    lines.push_back(to_drive ? m_portable_plan.to : m_portable_plan.from);
    lines.push_back("");
    if (p.listing.load())
    {
      lines.push_back("Looking at your data...");
    }
    else
    {
      lines.push_back(std::to_string(p.looked.load()) + " of " + std::to_string(p.files.load()) + " files");
      lines.push_back(mb(p.bytes_done.load()) + " of " + mb(p.bytes_total.load()) + " MB");
    }
    lines.push_back("");
    add("A copy: nothing is deleted.");
    m_panel.options = {"Stop"};
    m_portable_actions = {"stop"};
    m_panel.helps = {"Stops the copy: nothing is turned on or off (what was copied stays)."};
  }
  else if (m_portable_ok)
  {
    add(to_drive ? "Done: your data is on the drive." : "Done: the drive's data is on this PS4.");
    lines.push_back("");
    lines.push_back(std::to_string(p.copied.load()) + " files copied");
    if (p.kept.load() > 0)
      add(std::to_string(p.kept.load()) +
          (to_drive ? " of your files were on the drive already: the drive's were kept." :
                      " files of this PS4 are newer: they were kept."));
    if (p.refused.load() > 0)
      add(std::to_string(p.refused.load()) + " files have names a USB drive doesn't take: they stay on this PS4.");
    lines.push_back("");
    add(to_drive ? "EmuSuNi restarts to use the drive." : "EmuSuNi restarts with this PS4's data.");
    m_panel.options = {"Restart Now"};
    m_portable_actions = {"restart"};
    m_panel.helps = {"EmuSuNi closes and opens again with the data chosen."};
  }
  else
  {
    std::string error;
    {
      std::lock_guard lock(p.mutex);
      error = p.error;
    }
    if (error == "stopped")
    {
      add("Stopped: nothing was turned on or off.");
    }
    else
    {
      add(to_drive ? "Not turned on: a file couldn't be copied." : "Not turned off: a file couldn't be copied.");
      lines.push_back("");
      AddWrapped(wrap, error, &lines);
      lines.push_back("");
      add("Is the drive full? Nothing was deleted.");
    }
    m_panel.options = {"OK"};
    m_portable_actions = {"back"};
    m_panel.helps = {""};
  }
  m_panel.lines = std::move(lines);
}

// Every frame: the copy's page as it goes and its end; then, with the menu free, a warning waiting (the drive in use
// taken out, a write to it failed, it didn't answer at the start), a drive's question or notice.
void Menu::Impl::UpdatePortable()
{
  if (m_portable_progress && !m_portable_ended && m_portable_progress->done.load())
  {
    if (m_portable_thread.joinable())
      m_portable_thread.join();
    m_portable_ended = true;
    Portable::CopyProgress& p = *m_portable_progress;
    std::string error;
    {
      std::lock_guard lock(p.mutex);
      error = p.error;
    }
    m_portable_ok = p.failed.load() == 0 && error.empty();
    const bool to_drive = m_portable_plan.copying == Portable::Copying::ToDrive;
    if (m_portable_ok && to_drive)
    {
      // The drive's portable.ini last (a copy cut before it isn't portable data), this PS4's answer: turned on here.
      const std::string id = Portable::MarkDrive(m_portable_plan.drive);
      if (id.empty())
      {
        m_portable_ok = false;
        std::lock_guard lock(p.mutex);
        p.error = Portable::FolderOf(m_portable_plan.drive) + "/" + Portable::MARKER + ": can't be written";
      }
      else
      {
        Portable::Remember(id, "home");
        Portable::SetLibraryWriter(m_portable_plan.writer, id);
      }
    }
    else if (m_portable_ok)
    {
      Portable::Remember(Portable::DriveId(), "off");
    }
    LogF("portable: the copy %s: %d files, %d copied, %d kept (the other side's), %d the same, %d names refused, %d "
         "failed%s%s",
         m_portable_ok ? "is done" : "ended without turning anything on or off", p.files.load(), p.copied.load(),
         p.kept.load(), p.same.load(), p.refused.load(), p.failed.load(), error.empty() ? "" : ": ", error.c_str());
    m_panel = {};
    m_panel.open = true;
    m_panel.setting = SET_PORTABLE_COPY;
    BuildPortableCopyPanel();
    Play(m_portable_ok ? "confirm" : "error");
  }
  else if (PortableCopyBusy() && m_panel.open && m_panel.setting == SET_PORTABLE_COPY && m_time - m_portable_built > 0.25f)
  {
    BuildPortableCopyPanel();
  }
  if (const std::string warning = Portable::TakeWarning(); !warning.empty())
  {
    m_portable_warning = warning;
    for (Item& item : m_items[static_cast<int>(Category::Settings)])
    {
      if (item.setting == SET_PORTABLE)
        item.detail = PortableDetail();
    }
  }
  if (m_panel.open || m_intro || m_keyboard || m_capture || m_launch >= 0 || Tour::Active())
    return;
  if (!m_portable_warning.empty())
  {
    OpenPortableWarning(m_portable_warning);
    m_portable_warning.clear();
  }
  else if (!m_portable_pending.empty())
  {
    OpenPortableQuestion(m_portable_pending == "notice");
    m_portable_pending.clear();
  }
}

// The drives the menu found (its reads, its looks): the one in use still there; another's portable data asked about
// (never answered on this PS4) or told about (used here: the next start uses it), once a session each.
void Menu::Impl::PortableLook(const std::vector<std::string>& drives)
{
  if (m_portable_looked && drives == m_portable_drives)
    return;
  m_portable_looked = true;
  m_portable_drives = drives;
  Portable::CheckDrive();  // (the one in use taken out: its warning, the fallback)
  for (const Portable::DriveData& data : Portable::Look(drives))
  {
    if (data.in_use || !m_portable_said.insert(data.id).second)
      continue;
    if (Portable::Active() || Portable::Lost())
    {
      Log("portable: " + data.root + " has portable data too (" + data.id + "): one drive's at a time, not used");
      continue;
    }
    if (data.answer == "no" || data.answer == "off")
    {
      Log("portable: " + data.root + " has portable data (" + data.id + "), \"" + data.answer +
          "\" on this PS4: not used (Settings -> Keep My Data on the USB Drive)");
      continue;
    }
    m_portable_drive = data.root;
    m_portable_id = data.id;
    m_portable_pending = data.answer.empty() ? "ask" : "notice";
    Log("portable: " + data.root + " has portable data (" + data.id + "): " +
        (data.answer.empty() ? "the player is asked whether to use it" : "used from the next start (said so)"));
    break;
  }
}

// EmuSuNi again, to read the player's data from where it is now (as the start watchdog restarts it: src/StartGuard.cpp).
// Still here after 5 s: it closes instead (the next start uses that data).
void Menu::Impl::RestartApp(const std::string& why)
{
  Log("portable: EmuSuNi restarts: " + why);
  m_prefs.Save(UserDir() + "/xmb.ini");
  Netplay::Leave();
  LogSync();
  const int result = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
  LogF("portable: restart asked (%#x)", static_cast<unsigned>(result));
  LogSync();
  for (int i = 0; i < 50; i++)
    sceKernelUsleep(100000);
  Log("portable: EmuSuNi wasn't restarted: it closes (its next start uses the data chosen)");
  LogSync();
  sceSystemServiceLoadExec("exit", nullptr);
}

// A command of the screenshot tour (Tour.h).
void Menu::Impl::TourCommand(const std::string& command)
{
  const size_t space = command.find(' ');
  const std::string word = command.substr(0, space);
  const std::string rest = space == std::string::npos ? std::string() : command.substr(space + 1);
  const int games = static_cast<int>(Category::Games);
  if (word == "play" && rest.empty())
  {
    // (01.05) The game selected on the page shown ("tab atarijaguar rayman", then "play": a title in several consoles
    // - "play Rayman" starts the first in the whole library, the Game Boy Color's).
    const int at = m_selected[m_category];
    const int game = at >= 0 && at < static_cast<int>(m_items[m_category].size()) ? m_items[m_category][at].game : -1;
    if (game < 0)
    {
      Log("tour: play: no game selected on this page");
      return;
    }
    m_panel.open = false;
    m_launch = game;
    m_launch_time = 0.0f;
    Play("launch");
  }
  else if (word == "select" || word == "play")
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
    m_board.Open(TextKeyboard::Mode::Search, m_search, SEARCH_MAX);
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
    // A page by its console's key ("arcade"), or all / favorites / online / settings (a console not shown: All), and on
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
    else if (name == "online")
      page = static_cast<int>(Category::Online);
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
                        rest == "nocover"  ? SET_NO_COVER :  // (00.68: each game's reason under the list)
                        rest == "bezels"   ? SET_BEZELS :    // (00.80: Game Bezels)
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

// data_dir/played.ini: "<Unix time> <game path>" per line. (00.87: a game on a USB drive with the player's portable
// data: its path relative to the drive, "<drive>/Roms/...", src/Portable.h.)
void Menu::Impl::LoadPlayed()
{
  std::map<std::string, int64_t> played;
  std::ifstream file(UserDir() + "/played.ini");
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    const size_t space = line.find(' ');
    if (space != std::string::npos && space > 0)
      played[Portable::FromStored(line.substr(space + 1))] = std::atoll(line.c_str());  // (00.87: src/Portable.h)
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
    std::ifstream file(UserDir() + "/played.ini");
    std::string line;
    while (std::getline(file, line))
    {
      const size_t space = line.find(' ');
      if (space != std::string::npos && space > 0)
        played[Portable::FromStored(line.substr(space + 1))] = std::atoll(line.c_str());
    }
  }
  for (const Game& game : m_games)
  {
    if (game.played > 0)
      played[game.rom.path] = game.played;
  }
  std::string text;
  for (const auto& [path, when] : played)
    text += std::to_string(when) + " " + Portable::ToStored(path) + "\n";
  WriteTextFile(UserDir() + "/played.ini", text);
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
  case Category::SaturnGames:
  {
    // The Saturn's planet: a ball with its ring across it, tilted (the console's logo).
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    dl->AddCircleFilled(c, r * 0.62f, color, 40);
    ImVec2 ring[33];
    for (int k = 0; k < 33; k++)
    {
      const float t = static_cast<float>(k) / 32.0f * IM_PI * 2.0f;
      const float x = std::cos(t) * r * 1.0f, y = std::sin(t) * r * 0.28f;
      ring[k] = ImVec2(c.x + x * 0.94f - y * 0.34f, c.y + x * 0.34f + y * 0.94f);
    }
    dl->AddPolyline(ring, 33, hole, ImDrawFlags_None, r * 0.2f);
    dl->AddPolyline(ring, 33, color, ImDrawFlags_None, r * 0.1f);
    break;
  }
  case Category::PceGames:
  {
    // The PC Engine seen from above: the small square body, the HuCard half out of its slot at the front, the
    // expansion port's grille at the back and the power switch (no overlap: the colour can be see-through).
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 p0(c.x - r * 0.88f, c.y - r * 0.5f), p1(c.x + r * 0.88f, c.y + r * 0.92f);
    dl->AddRectFilled(p0, p1, color, r * 0.1f);
    dl->AddRectFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.96f), ImVec2(c.x + r * 0.42f, c.y - r * 0.58f), color,
                      r * 0.05f, ImDrawFlags_RoundCornersTop);  // the HuCard
    dl->AddRectFilled(ImVec2(c.x - r * 0.52f, p0.y + r * 0.06f), ImVec2(c.x + r * 0.52f, p0.y + r * 0.14f), hole);
    for (int k = 0; k < 4; k++)
    {
      const float y = c.y + r * (0.2f + k * 0.16f);
      dl->AddRectFilled(ImVec2(c.x - r * 0.62f, y), ImVec2(c.x + r * 0.3f, y + r * 0.07f), hole);
    }
    dl->AddRectFilled(ImVec2(c.x + r * 0.5f, c.y + r * 0.2f), ImVec2(c.x + r * 0.7f, c.y + r * 0.42f), hole,
                      r * 0.03f);
    break;
  }
  case Category::A2600Games:
  {
    // (00.90) The 2600's joystick seen from the front: its square base, the fire button at its top left, the rubber
    // boot in the middle and the stick standing up from it (no overlap: the colour can be see-through).
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y - r * 0.02f), ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), color, r * 0.14f);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.56f, c.y + r * 0.24f), r * 0.16f, hole, 16);  // the fire button
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.46f), r * 0.3f, hole, 24);             // the boot
    dl->AddRectFilled(ImVec2(c.x - r * 0.1f, c.y - r * 0.92f), ImVec2(c.x + r * 0.1f, c.y - r * 0.07f), color,
                      r * 0.08f, ImDrawFlags_RoundCornersTop);  // the stick
    break;
  }
  case Category::A7800Games:
  {
    // (00.90) The 7800's joystick: its low base with a groove, the tall grip on it with a button on either side of its
    // top.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y + r * 0.44f), ImVec2(c.x + r * 0.9f, c.y + r * 0.92f), color, r * 0.14f);
    dl->AddRectFilled(ImVec2(c.x - r * 0.62f, c.y + r * 0.62f), ImVec2(c.x + r * 0.62f, c.y + r * 0.7f), hole);
    dl->AddRectFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.92f), ImVec2(c.x + r * 0.3f, c.y + r * 0.4f), color, r * 0.28f,
                      ImDrawFlags_RoundCornersTop);  // the grip
    for (const float side : {-1.0f, 1.0f})
      dl->AddCircleFilled(ImVec2(c.x + side * r * 0.3f, c.y - r * 0.52f), r * 0.13f, hole, 16);  // its buttons
    break;
  }
  case Category::JaguarGames:
  {
    // (00.90) The Jaguar's pad: its wide body with the D-pad at the left and A, B, C at the right, the number pad's
    // twelve keys below in the middle.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    const ImVec2 p0(c.x - r * 0.95f, c.y - r * 0.72f), p1(c.x + r * 0.95f, c.y + r * 0.12f);
    dl->AddRectFilled(p0, p1, color, r * 0.36f);
    dl->AddRectFilled(ImVec2(c.x - r * 0.46f, p1.y), ImVec2(c.x + r * 0.46f, c.y + r * 0.94f), color, r * 0.14f,
                      ImDrawFlags_RoundCornersBottom);  // the number pad's part
    const ImVec2 dp(c.x - r * 0.55f, c.y - r * 0.32f);
    const float arm = r * 0.22f, thick = r * 0.07f;
    dl->AddRectFilled(ImVec2(dp.x - arm, dp.y - thick), ImVec2(dp.x + arm, dp.y + thick), hole);
    dl->AddRectFilled(ImVec2(dp.x - thick, dp.y - arm), ImVec2(dp.x + thick, dp.y + arm), hole);
    for (int k = 0; k < 3; k++)
      dl->AddCircleFilled(ImVec2(c.x + r * (0.32f + k * 0.2f), c.y - r * (0.16f + k * 0.16f)), r * 0.09f, hole, 12);
    for (int row = 0; row < 4; row++)
    {
      for (int col = 0; col < 3; col++)
      {
        const ImVec2 k0(c.x + r * (-0.36f + col * 0.26f), c.y + r * (0.2f + row * 0.18f));
        dl->AddRectFilled(k0, ImVec2(k0.x + r * 0.2f, k0.y + r * 0.11f), hole, r * 0.03f);
      }
    }
    break;
  }
#ifdef EMUSUNI_OPENBOR
  case Category::OpenborGames:
  {
    // (with-openbor) A beat 'em up's fist seen from the front: four knuckles on the back of the hand, the thumb across
    // it (no overlap: the colour can be see-through). A stand-in until Frontend draws OpenBOR's own icon.
    const ImU32 hole = IM_COL32(0, 0, 0, 130);
    for (int k = 0; k < 4; k++)
    {
      const float x0 = c.x + r * (-0.72f + k * 0.37f);
      dl->AddRectFilled(ImVec2(x0, c.y - r * 0.78f), ImVec2(x0 + r * 0.33f, c.y - r * 0.12f), color, r * 0.15f);
    }
    dl->AddRectFilled(ImVec2(c.x - r * 0.72f, c.y - r * 0.06f), ImVec2(c.x + r * 0.76f, c.y + r * 0.86f), color,
                      r * 0.2f);  // the back of the hand
    dl->AddRectFilled(ImVec2(c.x - r * 0.62f, c.y + r * 0.1f), ImVec2(c.x + r * 0.3f, c.y + r * 0.26f), hole,
                      r * 0.08f);  // the thumb's edge
    break;
  }
#endif
  case Category::Favorites:
    DrawStar(dl, c, r * 0.95f, color);
    break;
  case Category::Online:
  {
    // (00.23) A globe: its outline, the equator and two meridians.
    dl->AddCircle(c, r * 0.85f, color, 48, r * 0.12f);
    dl->AddLine(ImVec2(c.x - r * 0.85f, c.y), ImVec2(c.x + r * 0.85f, c.y), color, r * 0.1f);
    dl->AddEllipse(c, ImVec2(r * 0.38f, r * 0.85f), color, 0.0f, 40, r * 0.1f);
    dl->AddLine(ImVec2(c.x - r * 0.72f, c.y - r * 0.45f), ImVec2(c.x + r * 0.72f, c.y - r * 0.45f), color, r * 0.07f);
    dl->AddLine(ImVec2(c.x - r * 0.72f, c.y + r * 0.45f), ImVec2(c.x + r * 0.72f, c.y + r * 0.45f), color, r * 0.07f);
    break;
  }
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

// (00.88) The mark of a console that plays online: the Online tab's globe, white on a disc of the theme's colour.
void Menu::Impl::DrawOnlineMark(ImDrawList* dl, ImVec2 c, float size, float alpha)
{
  const Theme& theme = CurrentTheme();
  const float r = size * 0.5f;
  dl->AddCircleFilled(c, r, Rgba(theme.wave, 0.95f * alpha), 32);
  dl->AddCircle(c, r, Rgba(0x000000, 0.35f * alpha), 32, std::max(1.0f, size * 0.04f));
  DrawCategoryIcon(dl, Category::Online, c, size * 0.78f, Rgba(0xFFFFFF, alpha));
}

void Menu::Impl::DrawItem(ImDrawList* dl, const Item& item, float y, bool selected, float alpha)
{
  const Theme& theme = CurrentTheme();
  const float x = 470.0f;
  // (00.85) Menu Size: the row drawn at its usual size, then shrunk around its icon's middle (ScaleDrawn) - inside a
  // clip as big as can be meanwhile, so that no text near the screen's edge is left out as it is made (ImGui drops the
  // lines outside the clip), which shrunk would be in view.
  const float menu_scale = s_menu_scale;
  const int first_vertex = dl->VtxBuffer.Size;
  if (menu_scale != 1.0f)
    dl->PushClipRect(ImVec2(-W, -H), ImVec2(W * 2.0f, H * 2.0f), false);
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
  // (The screen's right margin as it is once shrunk: a smaller Menu Size has room for more of a long name.)
  Text(dl, m_bold, title_size, ImVec2(tx, y - title_size * 0.62f), text,
       Fit(m_bold, title_size, item.label, x + (W - 80 - x) / menu_scale - tx));
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
    // Its rating (Settings -> Star Ratings): five small stars beside the badge.
    if (m_prefs.ratings && game.rating.half_stars >= 0)
    {
      constexpr float R = 11.0f;
      DrawRating(dl, m_star, ImVec2(sx + RatingWidth(R) * 0.5f, sub_y + 17), R, game.rating.half_stars,
                 Rgba(theme.text, alpha * 0.30f), alpha);
      sx += RatingWidth(R) + 18;
    }
    // (01.05, the owner) Its file the standard version or not, right after the stars (under them is the next row): its
    // word too for the game selected.
    if (const int mark = StandardMark(game))
      sx += DrawStandardMark(dl, ImVec2(sx, sub_y + 17), 10.0f, mark, alpha, selected) + 16;
    // RetroAchievements' mark (00.77): its achievements' "RA" after them.
    if (m_ra_marks && game.ra.state == Achievements::LibraryInfo::State::Known)
    {
      constexpr float MARK_H = 26.0f;
      DrawAchievementMark(dl, m_bold, ImVec2(sx + MARK_H * 1.75f, sub_y + 4 + MARK_H), MARK_H, game.ra, alpha);
      sx += MARK_H * 1.75f + 16;
    }
  }
  if (!item.detail.empty())
    Text(dl, m_font, 26.0f, ImVec2(sx, sub_y + 1), Rgba(theme.text, alpha * 0.75f), item.detail);
  if (menu_scale != 1.0f)
  {
    ScaleDrawn(dl, first_vertex, ImVec2(x, y), menu_scale);
    dl->PopClipRect();
  }
}

void Menu::Impl::DrawSidePanel(ImDrawList* dl, float dt)
{
  Approach(m_panel.slide, m_panel.open ? 1.0f : 0.0f, 16.0f, dt);
  if (m_panel.slide < 0.01f)
    return;
  // (00.92) A Choose Together room with its players: the whole screen.
  if (m_panel.setting == SET_ONLINE_ROOM && m_together.on)
  {
    DrawTogetherScreen(dl, dt);
    return;
  }
  // (01.05, the owner) A room - waiting, starting -: the whole screen too.
  if (m_panel.setting == SET_ONLINE_ROOM)
  {
    const Netplay::Phase phase = Netplay::GetView().phase;
    if (phase == Netplay::Phase::Room || phase == Netplay::Phase::Starting)
    {
      DrawRoomScreen(dl, dt);
      return;
    }
  }
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
  // (00.23) Online play's room: its code, big (the host reads it out to a friend).
  const Netplay::View online = m_panel.setting == SET_ONLINE_ROOM ? Netplay::GetView() : Netplay::View();
  if (m_panel.setting == SET_ONLINE_ROOM && online.host && !online.code.empty())
  {
    Text(dl, m_bold, 120, ImVec2(x0 + 50, 200), Rgba(theme.wave, m_panel.slide), online.code);
    y = 360;
  }
  // (00.92) The room's players: the latency bar, top right, once another one is in.
  if (m_panel.setting == SET_ONLINE_ROOM && online.phase == Netplay::Phase::Room && Netplay::GetStatus().latency_bar)
  {
    std::vector<Netplay::PlayerView> players;
    bool other = false;
    for (const Netplay::PlayerView& p : online.players)
    {
      if (p.asking)
        continue;
      players.push_back(p);
      other = other || !p.me;
    }
    if (other)
      NetplayUi::DrawLatencyBar(dl, m_font, m_bold, players, ImVec2(x0 + width - 30.0f, 30.0f), 0, m_panel.slide);
  }
  // (00.91) A log sent: its code, big (the player writes it in a report).
  if (m_panel.setting == SET_SEND_LOG && LogReport::GetStatus().state == LogReport::State::Sent)
  {
    Text(dl, m_bold, 120, ImVec2(x0 + 50, 200), Rgba(theme.wave, m_panel.slide), LogReport::GetStatus().code);
    y = 360;
  }
  // (00.95) A room of three or four, its players' lines and options: everything closer, clear of the help text.
  const bool crowded = m_panel.setting == SET_ONLINE_ROOM && m_panel.lines.size() + m_panel.options.size() > 10;
  if (!m_panel.options.empty())
  {
    // (00.88) The options closer when they would reach the help text at the bottom (a long list: the Game menu).
    const float step = std::clamp((H - 196.0f - y) / static_cast<float>(m_panel.options.size()),
                                  crowded ? 46.0f : 50.0f, crowded ? 50.0f : 62.0f);
    for (size_t i = 0; i < m_panel.options.size(); i++, y += step)
    {
      const bool sel = static_cast<int>(i) == m_panel.selected;
      if (sel)
      {
        dl->AddRectFilled(ImVec2(x0 + 30, y - 6), ImVec2(W - 30, y + step - 10),
                          Rgba(theme.wave, 0.30f * m_panel.slide), 10.0f);
      }
      Text(dl, sel ? m_bold : m_font, 34, ImVec2(x0 + 60, y), text, m_panel.options[i]);
    }
    // What the selected option does, at the bottom.
    if (m_panel.selected >= 0 && m_panel.selected < static_cast<int>(m_panel.options.size()))
    {
      const size_t at = static_cast<size_t>(m_panel.selected);
      const std::string help = at < m_panel.helps.size() && !m_panel.helps[at].empty() ? m_panel.helps[at] :
                                                                                    OptionHelp(m_panel.options[at]);
      if (!help.empty())
      {
        const std::string wrapped = WrapText(m_font, 25, PS4Lang::T(help), width - 100);
        // ((01.05) A long one higher: it ends above the button hints.)
        const float lines = static_cast<float>(std::count(wrapped.begin(), wrapped.end(), '\n') + 1);
        dl->AddText(m_font, 25, ImVec2(x0 + 50, std::min(H - 176.0f, HELP_END - HELP_LINE * lines)),
                    Rgba(theme.text, 0.8f * m_panel.slide), wrapped.c_str());
      }
    }
  }
  // A long panel (About) closer, clear of the button hints at the bottom.
  const float line_step = crowded ? 34.0f : m_panel.lines.size() > 18 ? 38.0f : 42.0f;
  const float line_size = crowded ? 26.0f : 28.0f;
  for (const std::string& line : m_panel.lines)
  {
    // Information's rating: its stars at the line's end.
    constexpr float STAR_R = 12.0f;
    const bool stars = m_panel.stars >= 0 && line == m_panel.stars_line;
    const float room = stars ? RatingWidth(STAR_R) + 20.0f : 0.0f;
    Text(dl, m_font, line_size, ImVec2(x0 + 50, y), text, Fit(m_font, line_size, line, width - 90 - room));
    if (stars)
    {
      DrawRating(dl, m_star, ImVec2(x0 + width - 40 - RatingWidth(STAR_R) * 0.5f, y + 17), STAR_R, m_panel.stars,
                 Rgba(theme.text, 0.30f * m_panel.slide), m_panel.slide);
    }
    y += line_step;
  }
  // A QR code at its own size (205: sharp), its name under it (centered), clear of the button hints.
  constexpr float QR = 205.0f, QR_LABEL = 36.0f;
  const auto draw_qr = [&](ImTextureID qr, ImVec2 p0, const std::string& label) {
    dl->AddImage(ImTextureRef(qr), p0, ImVec2(p0.x + QR, p0.y + QR), ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, static_cast<int>(255 * m_panel.slide)));
    if (!label.empty())
      Text(dl, m_bold, 26, ImVec2(p0.x + (QR - TextWidth(m_bold, 26, label)) * 0.5f, p0.y + QR + 6), text, label);
  };
  if (m_panel.setting == SET_HELP && m_help_qr != ImTextureID{})
  {
    // (00.90) How to Help: the project's website as a QR code; (01.04, the owner) the group on Telegram's beside it.
    const ImVec2 p0(x0 + 50, std::min(y + 8, H - 160 - QR - QR_LABEL));
    draw_qr(m_help_qr, p0, "emusuni.com");
    if (m_telegram_qr != ImTextureID{})
      draw_qr(m_telegram_qr, ImVec2(p0.x + QR + 50, p0.y), "Telegram");
  }
  if (m_panel.setting == SET_SEND_LOG && LogReport::GetStatus().state == LogReport::State::Sent &&
      m_telegram_qr != ImTextureID{})
  {
    // (01.04) A log sent: the group on Telegram's QR code under the lines, what to tell there beside it.
    const ImVec2 p0(x0 + 50, std::min(y + 8, H - 160 - QR - QR_LABEL));
    draw_qr(m_telegram_qr, p0, "Telegram");
    const std::string wrapped =
        WrapText(m_font, 26, PS4Lang::T("Tell us what happened on Telegram, with this code."), width - 100 - QR - 30);
    dl->AddText(m_font, 26, ImVec2(p0.x + QR + 30, p0.y + 20), text, wrapped.c_str());
  }
  if (!m_panel.rows.empty())
  {
    // Settings list, scrolling with the selection; the setting's help text below it - (01.05) the list shorter when the
    // text needs more than its room, so that the text ends above the button hints (HELP_END).
    const float row_h = 58.0f, top = 220.0f;
    const float bottom = std::max(top + 3.0f * row_h, std::min(H - 200.0f, HELP_END - 24.0f - HELP_LINE *
                                                                              static_cast<float>(m_panel.help_lines)));
    const float visible = (bottom - top) / row_h;
    const float target =
        std::clamp(static_cast<float>(m_panel.selected) - visible * 0.5f + 1.0f, 0.0f,
                   std::max(0.0f, static_cast<float>(m_panel.rows.size()) - visible));
    if (m_panel.snap)
      m_panel.scroll = target;  // (the list went round: there at once, not through every row's pictures)
    m_panel.snap = false;
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
        // (00.89) In its own proportion, centred in the 52 x 45 box on a dark ground (not stretched).
        constexpr float BOX_W = 52.0f, BOX_H = 45.0f;
        float w = BOX_W, h = BOX_H;
        if (row.icon_aspect > 0.0f)
        {
          h = BOX_W / row.icon_aspect;
          if (h > BOX_H)
          {
            h = BOX_H;
            w = BOX_H * row.icon_aspect;
          }
          dl->AddRectFilled(ImVec2(label_x, ry + 1), ImVec2(label_x + BOX_W, ry + 1 + BOX_H),
                            Rgba(0x000000, 0.35f * m_panel.slide), 4.0f);
        }
        const ImVec2 q0(label_x + (BOX_W - w) * 0.5f, ry + 1 + (BOX_H - h) * 0.5f);
        dl->AddImage(ImTextureRef(row.icon), q0, ImVec2(q0.x + w, q0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, static_cast<int>(255 * m_panel.slide)));
        label_x += 66;
      }
      else if (!row.picture.empty())
      {
        // RetroAchievements' pictures (00.75): those of the rows shown, square, once downloaded (their room kept).
        const ImTextureID picture = AchievementsUi::Picture(m_display, row.picture);
        if (picture != ImTextureID{})
          dl->AddImage(ImTextureRef(picture), ImVec2(label_x + 3, ry + 1), ImVec2(label_x + 49, ry + 47), ImVec2(0, 0),
                       ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * m_panel.slide)));
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
      if (m_capture && m_capture_step == 1)
        help = "Now hold together the buttons it presses, then let them go...  (5 s: cancel)";
      else if (m_capture)
        help = "Press the DualShock 4 button for " + row.label + "...  (5 s: cancel)";
      else if (!row.help.empty())
        help = row.help;  // (made with the row: RetroAchievements' history, 00.75)
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
               index < bindings.size() && Controls::TakesSticks(bindings[index]) ?
                   "Cross: press a new button or push a stick for it. Left: none. D-Pad: Left/Right to choose." :
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
        // (00.89) Macros: several buttons in one.
        if (index < bindings.size() && Controls::IsMacro(bindings[index]))
          help = "Several buttons in one (fighting games). Cross: press the macro's button, then hold together the "
                 "buttons it presses and let them go. Left: none. Its button stops doing what it did.";
        // (00.90, GitHub #45) The N64's sticks: what they do whatever the rows say.
        if (m_controls_system == SystemId::N64 && index < bindings.size())
        {
          if (bindings[index].stick_group)
            help = "Left/Right: which controls move the N64's D-pad. The left stick is always its control stick.";
          else if (std::string(bindings[index].key).rfind("c_", 0) == 0)
            help = "Cross: press a new button or push a stick. Left: none. The right stick presses the C buttons in "
                   "any case.";
        }
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
      {
        help = "Cross: choose this game's cover (Find Cover). Circle there brings you back here.";
        // Why the last download left it without one (00.68: no picture named like it, the closest one unsure, the
        // list or the picture not downloaded and why).
        if (const auto why = m_cover_why.find(row.key.substr(8)); why != m_cover_why.end())
          help += "\n" + why->second;
      }
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
      // RetroAchievements (00.73).
      else if (row.key == "ra:about")
        help = "How to start (a free account, the login here), what achievements and points are, the consoles and the "
               "files it knows.";
      else if (row.key == "ra:file")
        help = "A file's name in No-Intro's list (the usual dump): the same file, renamed as you like, works.";
      else if (row.key == "ra:yours")
        help = "One of your files RetroAchievements has achievements for: start it from your games.";
      else if (row.key == "ra:login")
        help = "Your account of retroachievements.org (free). The keyboard asks for the user name, then the "
               "password, which is sent once to log in: EmuSuNi keeps only the token the site gives back.";
      else if (row.key == "ra:logout")
        help = "Forgets the login on this PS4. What you unlocked stays on your account.";
      else if (row.key == "ra:points")
        help = "Your account's points on retroachievements.org.";
      else if (row.key == "ra:history")
        help = "The achievements you unlocked in EmuSuNi (kept on this PS4, shown offline too) and your account's "
               "games with their progress, with their pictures.";
      else if (row.key == "ra:sync")
        help = "Asks retroachievements.org for your account's games and their progress again.";
      else if (row.key == "ra:translate")
        help = "On: the achievements' names and descriptions in your language, machine-translated - by Google's "
               "translator, a whole game at once, or MyMemory when Google can't (about 5000 characters a day) - each "
               "game's once, kept on this PS4. Off: as RetroAchievements has them (English).";
      else if (row.key == "ra:marks")
        help = "On: \"RA\" on the covers of the games whose file RetroAchievements has achievements for (gold when "
               "you unlocked them all, a line for what you unlocked), found in the background - no account needed. "
               "Off: none, and the games' files aren't read for it.";
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
      // (01.05) Its lines for the list's end next frame (a new selection: one frame at the old end, then there).
      m_panel.help_lines =
          static_cast<int>(std::count(wrapped.begin(), wrapped.end(), '\n')) + (wrapped.empty() ? 0 : 1);
      dl->AddText(m_font, 25, ImVec2(x0 + 50, bottom + 24), Rgba(theme.text, 0.8f * m_panel.slide),
                  wrapped.c_str());
    }
    else
      m_panel.help_lines = 0;
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
  auto hint = [&](const std::string& label, const char* glyph) {
    const float tw = TextWidth(m_font, 28, label);
    x -= tw;
    Text(dl, m_font, 28, ImVec2(x, y - 17), text, label);
    const float gw = PS4Glyphs::Width(glyph, 36);
    x -= gw + 12;
    PS4Glyphs::Draw(dl, m_bold, glyph, x, y, 36, 0.9f);
    x -= 40;
  };
  if (m_keyboard)
  {
    // (00.80) The keyboard's buttons (src/TextKeyboard.h), from the right - over a panel too (a text asked for from
    // it: the keyboard has the buttons).
    const auto hints = TextKeyboard::Hints(m_board.GetMode());
    for (const TextKeyboard::Hint& h : hints)
    {
      if (h.right)
        hint(h.label, h.glyph);
    }
    for (auto it = hints.rbegin(); it != hints.rend(); ++it)
    {
      if (it->right || std::string(it->glyph) == "D-Pad")
        continue;
      if (std::string(it->glyph) != "L1/R1")
      {
        hint(it->label, it->glyph);
        continue;
      }
      hint(it->label, "R1");
      x += 32.0f - PS4Glyphs::Width("L1", 36);
      PS4Glyphs::Draw(dl, m_bold, "L1", x, y, 36, 0.9f);
      x -= 40.0f;
    }
  }
  else if (m_online_ask >= 0)
  {
    // (00.86) A player asking to join: the notice's buttons.
    hint("Later", "Circle");
    hint("Select", "Cross");
  }
  else if (m_panel.open && m_panel.setting == SET_ONLINE_ROOM && m_together.on)
  {
    // (00.92) A Choose Together room's screen.
    const bool host = Netplay::GetView().host;
    hint("Back", "Circle");
    if (!m_together.cards.empty())
      hint(host ? "Play" : "Suggest", "Cross");
    if (m_together.unknown > 0)
      hint(m_online_show_all ? "Only " + std::to_string(m_together.need) + "+ Players" : std::string("Show All"),
           "Square");
    hint(host ? "Close Room" : "Leave Room", "Triangle");
    if (m_together.tabs.size() > 1)
    {
      hint("Console", "R1");
      x += 32.0f - PS4Glyphs::Width("L1", 36);
      PS4Glyphs::Draw(dl, m_bold, "L1", x, y, 36, 0.9f);
      x -= 40.0f;
    }
  }
  else if (m_panel.open)
  {
    hint("Back", "Circle");
    if (!m_panel.options.empty())
      hint("Select", "Cross");
    if (!m_panel.rows.empty())
      hint(m_panel.setting == SET_NO_COVER ? "Select" : "Change", "Cross");
  }
  else if (m_category == static_cast<int>(Category::Online))
  {
    // (00.85) The Online column's table. ((01.08) Its pages: L2 / R2, (01.09) the only ones.)
    hint("Online Settings", "Options");
    hint("Page", "R2");
    x += 32.0f - PS4Glyphs::Width("L2", 36);  // (L2 before R2: the Together screen's way)
    PS4Glyphs::Draw(dl, m_bold, "L2", x, y, 36, 0.9f);
    x -= 40.0f;
    if (m_online_page == PAGE_TOP)
    {
      hint("Create Room", "Cross");
      return;
    }
    if (m_online_page == PAGE_CHAT)
    {
      hint("Write", "Triangle");
      hint("Quick Phrase", "Square");
      if (m_world_index >= 0)
        hint("Mute or Report", "Cross");
      return;
    }
    if (m_online_page == PAGE_PLAYERS)
    {
      hint(m_players_days ? "Since Ever" : "Last 7 Days", "Triangle");  // ((01.07) the ranking's period)
      return;
    }
    hint("Join with Code", "Triangle");
    hint("Create Room", "Square");
    // (00.86: Cross on our own room goes back to it.)
    const bool mine = m_online_index >= 0 && m_online_index < static_cast<int>(m_online_rows.size()) &&
                      m_online_rows[static_cast<size_t>(m_online_index)].mine;
    // ((01.15) A room that takes spectators: the Touchpad watches it, a player's place left or not.)
    if (!mine && OnlineRowCanWatch() && !OnlineRowWatch())
      hint("Watch", "Touchpad");
    hint(mine ? "Go to Your Room" : OnlineRowWatch() ? "Watch" : "Join", "Cross");
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
      hint("Order and Filters", "Options");  // ((01.18) until 01.17: Sort, the next order at once)
      if (m_ra_marks)
        hint("RA Filter", "Touchpad");  // (00.80)
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
  s_menu_scale = MenuScaleOf(Settings::Get("menu_size"));  // (00.85: Settings -> Menu Size, at once)
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
      if (PlaysOnline(c))
        DrawOnlineMark(dl, ImVec2(x + size * 0.46f, row_y - size * 0.40f), size * 0.36f, alpha);
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
    // (00.85) Menu Size: the rows as much closer as they are smaller (DrawItem shrinks each), fading as far down the
    // screen as before - so more of them in view.
    const float menu_scale = s_menu_scale;
    for (int i = 0; i < static_cast<int>(items.size()); i++)
    {
      const float rel = static_cast<float>(i) - m_item_pos[c];
      float y;
      float alpha;
      if (rel >= -0.5f)
      {
        y = 470.0f + (rel * 120.0f + (rel > 0.5f ? 40.0f : rel * 80.0f)) * menu_scale;
        alpha = rel < 0.5f ? 1.0f : std::max(0.0f, 0.85f - (rel - 1.0f) * 0.12f * menu_scale);
      }
      else
      {
        y = 120.0f + (rel + 1.0f) * 100.0f * menu_scale;
        alpha = std::max(0.0f, 0.55f + (rel + 1.0f) * 0.2f * menu_scale);
      }
      if (y < -80.0f || y > H + 80.0f || alpha <= 0.01f)
        continue;
      DrawItem(dl, items[i], y, i == m_selected[c], alpha * switch_alpha);
    }
    // (00.85) The Online column: its table under the icons, the whole width (no items of its own).
    if (c == static_cast<int>(Category::Online))
      DrawOnlineTable(dl, switch_alpha, 150.0f, W - 90.0f, 384.0f, 935.0f, true);
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
    // (00.89) The RetroAchievements filter on in a list of games: said all the time (it is kept between runs) with
    // how to turn it off - not only in a toast when it changes.
    if (RaFilterOn() && IsGameList(c))
      Text(dl, m_font, 26, ImVec2(150.0f, H - 190.0f), Rgba(0xFFC83C, switch_alpha), RaFilterHiddenLine(c));
    // ((01.18) Order and Filters' genre or players, the same way: above it when both are on.)
    if (ViewFilterOn() && IsGameList(c))
      Text(dl, m_font, 26, ImVec2(150.0f, H - (RaFilterOn() ? 224.0f : 190.0f)), Rgba(0xFFC83C, switch_alpha),
           ViewFilterLine(c));
    DrawEmblem(dl, ImVec2(110, 90), 70, 0.9f);
    // The version under the emblem.
    const std::string version = std::string("v") + App::VERSION;
    Text(dl, m_font, 22, ImVec2(110 - TextWidth(m_font, 22, version) * 0.5f, 132), Rgba(theme.text, 0.6f),
         version, false);
  }

  // (A text asked for from a panel - Rename, Look For, the login -: the keyboard over it; the search's under one.)
  if (m_board.GetMode() != TextKeyboard::Mode::Search)
  {
    DrawSidePanel(dl, dt);
    DrawKeyboard(dl, dt);
  }
  else
  {
    DrawKeyboard(dl, dt);
    DrawSidePanel(dl, dt);
  }
  DrawOnlineAsk(dl, dt);  // (00.86: a player asking to join our room, over everything)
  DrawToast(dl, dt);
  if (Library() && m_panel.slide < 0.5f && m_online_ask < 0)
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
                           float alpha, int focus, float stars_x)
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
  // Its rating under it (Settings -> Star Ratings; none for a game without one): five stars as big as the cover
  // (27 pixels wide for the selected one) on its reflection's top, under the part of it in view (stars_x), following
  // its tilted bottom edge; gone while the search keyboard is up (the title is under the covers then).
  const float rating_alpha = alpha * (1.0f - m_keyboard_slide);
  const float star_r = std::clamp(h * 0.032f, 7.0f, 14.0f);
  const float stars_at = stars_x >= 0.0f ? stars_x : (c.x + d.x) * 0.5f;
  const float stars_edge =
      c.x - d.x > 1.0f ? d.y + (c.y - d.y) * std::clamp((stars_at - d.x) / (c.x - d.x), 0.0f, 1.0f) : c.y;
  const bool stars = m_prefs.ratings && game.rating.half_stars >= 0;
  if (stars && rating_alpha > 0.01f)
    DrawRating(dl, m_star, ImVec2(stars_at, stars_edge + 9.0f + star_r), star_r, game.rating.half_stars,
               Rgba(theme.text, rating_alpha * 0.30f), rating_alpha);
  // (01.05, the owner: "una marca debajo de las estrellas") Its file the standard version or not, under the stars (where
  // they would be, without them), centred as they are; its word too on the cover selected. Gone with the stars under
  // the keyboard.
  if (rating_alpha > 0.01f)
  {
    if (const int mark = StandardMark(game))
    {
      const float mr = std::max(6.0f, star_r * 0.75f);
      const float my = stars_edge + 9.0f + (stars ? star_r * 2.0f + 6.0f : 0.0f) + mr;
      const bool label = focus == 2;
      DrawStandardMark(dl, ImVec2(stars_at - StandardMarkWidth(mr, mark, label) * 0.5f, my), mr, mark, rating_alpha,
                       label);
    }
  }
  // RetroAchievements' mark (00.77): "RA" in the cover's bottom right corner for a game with achievements (gold when
  // completed), as big as the cover (30 pixels tall for the selected one).
  if (m_ra_marks && game.ra.state == Achievements::LibraryInfo::State::Known)
  {
    const float mark_h = std::clamp(h * 0.075f, 12.0f, 30.0f);
    const float inset = std::max(4.0f, h * 0.025f);
    DrawAchievementMark(dl, m_bold, ImVec2(c.x - inset, c.y - inset), mark_h, game.ra, alpha);
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
  constexpr float MARK = 30.0f;  // (00.88) the globe after the name of a console that plays online
  for (int i = 0; i < TAB_COUNT; i++)
    tab_w[i] = TextWidth(m_bold, TAB_FONT, TabName(tabs[i])) + PAD * 2 + (PlaysOnline(tabs[i]) ? MARK + 8 : 0);
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
    if (PlaysOnline(tabs[i]))
      DrawOnlineMark(dl, ImVec2(mid + tab_w[i] * 0.5f - PAD - MARK * 0.5f, BAR_Y + BAR_H * 0.5f), MARK,
                     (sel ? 1.0f : 0.72f) * fade);
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
  case Category::SaturnGames:
  case Category::PceGames:
  case Category::A2600Games:
  case Category::A7800Games:
  case Category::JaguarGames:
#ifdef EMUSUNI_OPENBOR
  case Category::OpenborGames:
#endif
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
  if (RaFilterOn() && (any || m_ra_emptied[c]))
    sub = RaFilterHiddenLine(c) + "  -  " + SortName();  // (00.89: was "With achievements: 438 of 793")
  else if (any)
    sub = std::to_string(count) + (count == 1 ? " game" : " games") + "  -  " + SortName();
  else if (m_all_covered[c])
    sub = "0 games  -  " + SortName();
  else if (!m_search.empty())
    sub = "0 games";
  else
    sub = favorites ? "No favorites yet" : m_reading_shown >= 0 ? "Reading your games..." : "No games yet";
  // ((01.18) Order and Filters' genre or players: first, with how many they hide.)
  if (ViewFilterOn() && (any || m_view_emptied[c]))
    sub = ViewFilterLine(c) + "  -  " + (m_view_emptied[c] ? "0 games  -  " + SortName() : sub);
  if (!m_search.empty())
    sub += "  -  Search: " + m_search;
  if (m_dl_thread.joinable() && !m_dl_done)
  {
    sub += "  -  Downloading covers " + std::to_string(m_dl_progress.load()) + " / " +
           std::to_string(m_dl_total.load());
  }
  if (m_prefs.ratings)
  {
    const std::string ratings = RatingsDownloadText();  // (LaunchBox's ratings: "Downloading ratings 45 / 109 MB")
    if (!ratings.empty())
      sub += "  -  " + ratings;
  }
  const int reading = m_reading_shown;  // (00.85: a read of the games going on: its count, UpdateReadingItems')
  if (any && reading >= 0)
    sub += "  -  Reading your games... " + std::to_string(reading) + " found";
  DrawPageTitle(dl, static_cast<Category>(c), page_title, sub, alpha,
                alpha * (m_sort_shown > 0.0f ? 1.0f : 0.85f));
  if (!any)
  {
    std::string line1;
    std::vector<std::string> lines;  // smaller, under it
    if (reading >= 0 && m_search.empty() && !m_all_covered[c] && !m_ra_emptied[c] && !m_view_emptied[c] && !favorites)
    {
      line1 = "Reading your games...";
      if (reading > 0)
        lines = {std::to_string(reading) + " found so far"};
    }
    else if (m_view_emptied[c])
    {
      // (01.18) Order and Filters' genre or players left none here.
      line1 = "No games of that genre or players here";
      lines = {"Options shows every game again"};
    }
    else if (m_all_covered[c])
    {
      line1 = m_search.empty() ? "Every game has a cover" : "Every game found has a cover";
      lines = {"Options shows every game again"};
    }
    else if (m_ra_emptied[c])
    {
      // (00.80) The RetroAchievements filter left none (yet: the files still being checked show as they're found).
      line1 = m_prefs.ra_filter == "progress" ? "No achievements in progress here" : "No games with achievements here";
      lines = {m_ra_count[c].unchecked > 0 ? "Still checking the files: they show here as they're found" :
                                             "Touchpad shows every game again"};
    }
    else if (!m_search.empty())
    {
      line1 = "No games match \"" + m_search + "\"";
      lines = {"Circle clears the search. R2 changes it."};
    }
    else if (favorites)
    {
      line1 = "No favorites yet";
      lines = {"Press L2 on a game to add it (again to take it out)."};
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
    if (favorites && m_search.empty() && !m_all_covered[c] && !m_ra_emptied[c])
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
  // (00.85) Menu Size: the covers and the room between them smaller (BASE_H, and so spread), on the same floor; the row
  // as much longer as they are smaller, its farthest covers fading out as before (more of them in view).
  const float menu_scale = s_menu_scale;
  const float BASE_H = (420.0f - 190.0f * k) * menu_scale, FLOOR_Y = 720.0f - 230.0f * k;
  const float spread = BASE_H / 420.0f;
  const float reach = 6.0f / menu_scale;  // (the farthest covers drawn, fading out over the last one and a half)
  std::vector<std::pair<float, int>> order;
  for (int i = 0; i < count; i++)
  {
    const float d = static_cast<float>(i) - pos;
    if (std::abs(d) < reach + 0.5f)
      order.push_back({std::abs(d), i});
  }
  std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  float selected_top = FLOOR_Y - BASE_H;
  // Each cover's place first (its stars go under the part of it in view: the nearer covers, drawn after it, hide its
  // inner side), then the covers, the farthest first.
  struct Placed
  {
    int index, game;
    float d, ad;
    ImVec2 a, b, cc, dd;
    float alpha;
  };
  std::vector<Placed> placed;
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
    const float fade = std::clamp((reach - ad) / 1.5f, 0.0f, 1.0f);
    const float dim = 1.0f - 0.22f * near;
    placed.push_back({i, game, d, ad, a, b, cc, dd, alpha * fade * dim});
    if (i == selected)
      selected_top = a.y;
  }
  for (const Placed& p : placed)
  {
    float lo = std::min(p.a.x, p.dd.x), hi = std::max(p.b.x, p.cc.x);
    for (const Placed& q : placed)
    {
      if (q.ad >= p.ad)
        continue;
      if (p.d < 0.0f)
        hi = std::min(hi, std::min(q.a.x, q.dd.x));
      else
        lo = std::max(lo, std::max(q.b.x, q.cc.x));
    }
    lo = std::max(lo, 0.0f);
    hi = std::min(hi, W);
    DrawCover(dl, p.game, p.a, p.b, p.cc, p.dd, p.alpha, p.index == selected && p.ad < 0.5f ? 2 : 1,
              hi > lo ? (lo + hi) * 0.5f : -1.0f);
  }
  const float info_alpha = alpha * (1.0f - k);
  // The console's box at the bottom left, over the covers' reflections (it fades out under the keyboard).
  const float box_right =
      list_system >= 0 ? DrawConsoleInfo(dl, static_cast<SystemId>(list_system), info_alpha) : 0.0f;
  const float pulse = 0.65f + 0.35f * std::sin(m_time * 3.0f);
  const float gw = PS4Glyphs::Width("Triangle", 40);
  if (info_alpha > 0.01f)
    PS4Glyphs::Draw(dl, m_bold, "Triangle", W * 0.5f - gw * 0.5f, selected_top - 44, 40, info_alpha * pulse);
  // The 3DS's tip at the bottom right (00.79, the owner: "si no les carga un juego a la primera, pueden apretar L3 y R3
  // y reiniciar el juego hasta que cargue"): a 3DS game that doesn't start the first time does after the in-game menu's
  // Reset Game, so the box says how, with the player's own In-Game Menu buttons (a touchpad's choice is L3 + R3 on the
  // 3DS, Controls::MenuButtonsFor). Over the reflections as the console's box on the other side, with its look: below
  // the covers, their stars (about 756 px down at most) and their RA marks (in the covers' corners) - its top is at
  // 849 px with its three lines (every label, English and Spanish), 819 px with four -, above the hints' line (972 px);
  // it fades out under the search keyboard and a side panel.
  float tip_left = W;
  if (list_system == static_cast<int>(SystemId::THREEDS) && info_alpha > 0.01f)
  {
    constexpr float RIGHT = W - 64.0f, BOTTOM = 962.0f, BOX_W = 400.0f, PAD = 14.0f, HEAD = 26.0f, SIZE = 25.0f,
                    LINE = 30.0f;
    std::string choice = Settings::Get("menu_buttons");
    if (Controls::MenuButtonsFor(choice, SystemId::THREEDS).mask == (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3))
      choice = "l3r3";
    const Settings::Setting* setting = Settings::Find("menu_buttons");
    const std::string keys = setting ? PS4Lang::T(Settings::LabelOf(*setting, choice)) : std::string("L3 + R3");
    // (es.ini: "Open the menu with {} and choose Reset Game until it loads." - the buttons as the setting names them.)
    auto lines_of = [this](ImFont* font, float size, const std::string& text) {
      std::vector<std::string> lines;
      std::stringstream wrapped(WrapText(font, size, PS4Lang::T(text), BOX_W - PAD * 2));
      for (std::string line; std::getline(wrapped, line);)
        lines.push_back(line);
      return lines;
    };
    const auto head = lines_of(m_bold, HEAD, "A game doesn't load the first time?");
    const auto body = lines_of(m_font, SIZE, "Open the menu with " + keys + " and choose Reset Game until it loads.");
    const float top = BOTTOM - PAD * 2 - static_cast<float>(head.size() + body.size()) * LINE + (LINE - SIZE);
    const bool light = theme.text != 0xFFFFFF;
    const float tip_alpha = info_alpha * (1.0f - m_panel.slide);  // (under a side panel too, as the counter)
    tip_left = RIGHT - BOX_W;
    dl->AddRectFilled(ImVec2(tip_left, top), ImVec2(RIGHT, BOTTOM),
                      light ? Rgba(0xFFFFFF, 0.6f * tip_alpha) : Rgba(0x000000, 0.4f * tip_alpha), 14.0f);
    dl->AddRect(ImVec2(tip_left, top), ImVec2(RIGHT, BOTTOM), Rgba(theme.wave, 0.45f * tip_alpha), 14.0f, 0, 1.5f);
    float y = top + PAD;
    for (const std::string& line : head)
    {
      dl->AddText(m_bold, HEAD, ImVec2(tip_left + PAD, y - 1.0f), Rgba(theme.wave, tip_alpha), line.c_str());
      y += LINE;
    }
    for (const std::string& line : body)
    {
      dl->AddText(m_font, SIZE, ImVec2(tip_left + PAD, y), Rgba(theme.text, 0.92f * tip_alpha), line.c_str());
      y += LINE;
    }
  }
  // The title stays in view above the keyboard: the game found. (Two sizes only: every font size
  // drawn is one more set of glyphs in the font atlas.) Centred, unless that would put it over the
  // console's box: then just right of it (and the line under it too) - and left of the 3DS's tip (00.79).
  const float left = box_right > 0.0f && k < 0.5f ? box_right + 24.0f : 80.0f;
  const float right = tip_left < W && k < 0.5f ? tip_left - 24.0f : W - 80.0f;
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
  if (TextWidth(m_font, 26, info) > right - left)
    info = Fit(m_font, 26, info, right - left);  // (only a line too long for the place between the boxes)
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

// The keyboard (00.80, src/TextKeyboard.h): the search's - a field with what is typed and how many games match - or a
// text asked for - its title and length, the field (a password as dots), the line under it -, then the keys. It slides
// up from the bottom (both menu styles); a text asked for dims what is behind it.
void Menu::Impl::DrawKeyboard(ImDrawList* dl, float dt)
{
  Approach(m_keyboard_slide, m_keyboard ? 1.0f : 0.0f, 14.0f, dt);
  if (m_keyboard_slide < 0.01f)
    return;
  const Theme& theme = CurrentTheme();
  const bool light = theme.text != 0xFFFFFF;
  const float a = std::min(1.0f, m_keyboard_slide * 1.5f);
  const bool search = m_board.GetMode() == TextKeyboard::Mode::Search;
  constexpr float KEY_W = 96.0f, KEY_H = 50.0f, GAP = 8.0f, PAD = 20.0f, FIELD_H = 60.0f, TITLE_H = 46.0f,
                  HINT_H = 36.0f;
  const int rows = m_board.Rows();
  const float panel_w = TextKeyboard::COLUMNS * KEY_W + (TextKeyboard::COLUMNS - 1) * GAP + PAD * 2;
  const float panel_h = PAD + (search ? 0.0f : TITLE_H) + FIELD_H + (search ? 0.0f : HINT_H) + 14.0f +
                        static_cast<float>(rows) * KEY_H + static_cast<float>(rows - 1) * GAP + PAD;
  const float bottom = H - 112.0f;  // above the hints
  const float x0 = W * 0.5f - panel_w * 0.5f;
  const float y0 = H + (bottom - panel_h - H) * m_keyboard_slide;
  if (!search)
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(0x000000, 0.45f * a));
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + panel_w, y0 + panel_h),
                    light ? Rgba(0xFFFFFF, 0.94f * a) : Rgba(0x0B0A14, 0.90f * a), 22.0f);
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + panel_w, y0 + panel_h), Rgba(theme.wave, 0.9f * a), 22.0f, 0, 2.5f);

  // A text asked for: its title, and its length of how many characters at most.
  float top = y0 + PAD;
  if (!search)
  {
    Text(dl, m_bold, 30, ImVec2(x0 + PAD + 6, top + 2), Rgba(theme.wave, a), m_ask_title, false);
    char length[32];
    snprintf(length, sizeof(length), "%zu / %zu", m_board.Length(), m_board.Max());
    const float lw = m_font->CalcTextSizeA(26, FLT_MAX, 0.0f, length).x;
    dl->AddText(m_font, 26, ImVec2(x0 + panel_w - PAD - 6 - lw, top + 6), Rgba(theme.text, 0.6f * a), length);
    top += TITLE_H;
  }

  // The field: the search's magnifying glass, the text (as typed, never translated; its end in view when it's longer
  // than the field) and a blinking cursor; the search's games found at its right.
  const ImVec2 f0(x0 + PAD, top), f1(x0 + panel_w - PAD, top + FIELD_H);
  dl->AddRectFilled(f0, f1, light ? Rgba(0x000000, 0.08f * a) : Rgba(0x000000, 0.45f * a), 12.0f);
  dl->AddRect(f0, f1, Rgba(theme.wave, 0.6f * a), 12.0f, 0, 1.5f);
  float tx = f0.x + 22.0f;
  std::string found;
  if (search)
  {
    const ImVec2 mc(f0.x + 34, f0.y + FIELD_H * 0.5f - 4);
    dl->AddCircle(mc, 12.0f, Rgba(theme.text, 0.85f * a), 20, 3.0f);
    dl->AddLine(ImVec2(mc.x + 9, mc.y + 9), ImVec2(mc.x + 19, mc.y + 19), Rgba(theme.text, 0.85f * a), 4.0f);
    tx = f0.x + 66;
    if (!m_search.empty())
    {
      const auto& items = m_items[IsGameList(m_category) ? m_category : static_cast<int>(Category::Games)];
      const int count = !items.empty() && items[0].game >= 0 ? static_cast<int>(items.size()) : 0;
      found = std::to_string(count) + (count == 1 ? " game" : " games");
    }
  }
  auto width = [this](const std::string& text) { return m_bold->CalcTextSizeA(36, FLT_MAX, 0.0f, text.c_str()).x; };
  std::string typed = search ? m_search : m_board.Display();
  const float room = f1.x - 24.0f - tx - (found.empty() ? 0.0f : TextWidth(m_bold, 28, found) + 24.0f);
  const float dots_w = width("...");
  bool cut = false;
  while (!typed.empty() && width(typed) > room - (cut ? dots_w : 0.0f))
  {
    typed.erase(0, std::min(typed.size(), TextKeyboard::CharBytes(static_cast<unsigned char>(typed[0]))));
    cut = true;
  }
  float cursor_x = tx;
  if (typed.empty() && search)
  {
    Text(dl, m_font, 30, ImVec2(tx, f0.y + 13), Rgba(theme.text, 0.45f * a), "Type a game's name", false);
  }
  else if (!typed.empty())
  {
    if (cut)
    {
      dl->AddText(m_bold, 36, ImVec2(tx, f0.y + 10), Rgba(theme.text, 0.5f * a), "...");
      cursor_x += dots_w;
    }
    dl->AddText(m_bold, 36, ImVec2(cursor_x, f0.y + 10), Rgba(theme.text, a), typed.c_str());
    cursor_x += width(typed);
  }
  if (std::fmod(m_time, 1.0f) < 0.6f)
    dl->AddRectFilled(ImVec2(cursor_x + 3, f0.y + 13), ImVec2(cursor_x + 6, f1.y - 13), Rgba(theme.wave, a));
  if (!found.empty())
  {
    const float fw = TextWidth(m_bold, 28, found);
    Text(dl, m_bold, 28, ImVec2(f1.x - 24 - fw, f0.y + 15), Rgba(theme.wave, a), found, false);
  }
  // A text asked for: the line under the field (what it's for).
  float ky = f1.y + 14.0f;
  if (!search)
  {
    if (!m_ask_hint.empty())
    {
      const std::string hint = Fit(m_font, 25, PS4Lang::T(m_ask_hint), panel_w - PAD * 2 - 12);
      dl->AddText(m_font, 25, ImVec2(f0.x + 6, f1.y + 6), Rgba(theme.text, 0.72f * a), hint.c_str());
    }
    ky += HINT_H;
  }

  // The keys: the characters as Cross types them now (never translated); the wide ones with the button that does the
  // same, Shift with its state (an arrow: hollow off, filled for the next letter, with a line under it for every one).
  auto key = [&](float kx, float kw, const std::string& label, bool translate, const char* glyph, bool selected,
                 float icon_w) {
    const ImVec2 k0(kx, ky), k1(kx + kw, ky + KEY_H);
    dl->AddRectFilled(k0, k1, selected ? Rgba(theme.wave, 0.9f * a) : Rgba(theme.text, 0.10f * a), 10.0f);
    if (selected)
      dl->AddRect(k0, k1, Rgba(0xFFFFFF, 0.9f * a), 10.0f, 0, 2.5f);
    const std::string shown = translate ? PS4Lang::T(label) : label;
    const float lw = m_bold->CalcTextSizeA(28, FLT_MAX, 0.0f, shown.c_str()).x;
    const float gw = glyph ? PS4Glyphs::Width(glyph, 30) + 10.0f : 0.0f;
    const float lx = kx + kw * 0.5f - (icon_w + lw + gw) * 0.5f + icon_w;
    dl->AddText(m_bold, 28, ImVec2(lx, ky + 10), selected ? Rgba(0xFFFFFF, a) : Rgba(theme.text, 0.9f * a),
                shown.c_str());
    if (glyph)
      PS4Glyphs::Draw(dl, m_bold, glyph, lx + lw + 10.0f, ky + KEY_H * 0.5f, 30, a);
    return lx;
  };
  for (int row = 0; row < rows; row++, ky += KEY_H + GAP)
  {
    if (row < TextKeyboard::CHAR_ROWS)
    {
      for (int col = 0; col < TextKeyboard::COLUMNS; col++)
        key(x0 + PAD + col * (KEY_W + GAP), KEY_W, m_board.Char(row, col), false, nullptr,
            m_board.IsSelected(row, col), 0.0f);
      continue;
    }
    int count = 0;
    const TextKeyboard::WideKey* keys = m_board.WideKeys(row, &count);
    for (int i = 0; i < count; i++)
    {
      const TextKeyboard::WideKey& wide = keys[i];
      const bool selected = m_board.IsSelected(row, wide.first);
      const bool shift = wide.action == TextKeyboard::Action::Shift;
      const float lx = key(x0 + PAD + wide.first * (KEY_W + GAP), wide.span * KEY_W + (wide.span - 1) * GAP,
                           m_board.Label(wide), wide.action != TextKeyboard::Action::Page, wide.glyph, selected,
                           shift ? 28.0f : 0.0f);
      if (!shift)
        continue;
      const TextKeyboard::Caps caps = m_board.GetCaps();
      const ImU32 color = selected                           ? Rgba(0xFFFFFF, a) :
                          caps != TextKeyboard::Caps::Off ? Rgba(theme.wave, a) :
                                                              Rgba(theme.text, 0.9f * a);
      const float cx = lx - 18.0f, at = ky + 11.0f;
      if (caps == TextKeyboard::Caps::Off)
      {
        const ImVec2 arrow[7] = {ImVec2(cx, at),          ImVec2(cx + 10, at + 11), ImVec2(cx + 5, at + 11),
                                 ImVec2(cx + 5, at + 21), ImVec2(cx - 5, at + 21),  ImVec2(cx - 5, at + 11),
                                 ImVec2(cx - 10, at + 11)};
        dl->AddPolyline(arrow, 7, color, ImDrawFlags_Closed, 2.0f);
      }
      else
      {
        dl->AddTriangleFilled(ImVec2(cx, at), ImVec2(cx + 11, at + 12), ImVec2(cx - 11, at + 12), color);
        dl->AddRectFilled(ImVec2(cx - 5, at + 11), ImVec2(cx + 5, at + 21), color);
        if (caps == TextKeyboard::Caps::Lock)
          dl->AddRectFilled(ImVec2(cx - 7, at + 24), ImVec2(cx + 7, at + 27), color);
      }
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
    float ratio = 8.0f / 7.0f;
    const ImTextureID picture = StatePicture(save.id, slot, &ratio);
    if (picture != ImTextureID{})
    {
      // (00.89) The picture in its own proportion, centred in its cell (not stretched to 8:7).
      float w = cell_w, h = cell_w / std::max(ratio, 0.1f);
      if (h > cell_h)
      {
        h = cell_h;
        w = cell_h * ratio;
      }
      const ImVec2 q0(p0.x + (cell_w - w) * 0.5f, p0.y + (cell_h - h) * 0.5f);
      dl->AddRectFilled(p0, p1, Rgba(0x000000, alpha * 0.35f), 6.0f);
      dl->AddImage(ImTextureRef(picture), q0, ImVec2(q0.x + w, q0.y + h), ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)));
    }
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
  const bool online = c == static_cast<int>(Category::Online);
  if (online)
  {
    // (00.85) The rooms' table under the title (the players online and the rooms beside the globe).
    DrawPageTitle(dl, Category::Online, "Online", OnlineSummary(), alpha, alpha * 0.85f);
    DrawOnlineTable(dl, alpha, 90.0f, W - 90.0f, 258.0f, 960.0f, false);
    return;
  }
  else
    DrawPageTitle(dl, themes ? Category::Themes : Category::Settings, themes ? "Themes" : "Settings",
                  themes ? "Colours and wallpapers (PNG in " + UserDir() + "/themes)" :
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
    // (00.80) The keyboard's buttons (src/TextKeyboard.h): the search's, or a text asked for.
    for (const TextKeyboard::Hint& h : TextKeyboard::Hints(m_board.GetMode()))
    {
      if (h.right)
        right(h.glyph, h.label);
      else if (std::string(h.glyph) == "L1/R1")
      {
        x += PS4Glyphs::Draw(dl, m_bold, "L1", x, y, 36, 0.9f) + 8;
        left("R1", h.label);
      }
      else
        left(h.glyph, h.label);
    }
    return;
  }
  if (m_category == static_cast<int>(Category::Online))
  {
    // (00.85) The Online tab's table. (00.86: Cross on our own room goes back to it.)
    const bool mine = m_online_index >= 0 && m_online_index < static_cast<int>(m_online_rows.size()) &&
                      m_online_rows[static_cast<size_t>(m_online_index)].mine;
    left("D-Pad", "Browse");
    // ((01.08) Its pages: L2 / R2, (01.09) the only ones - the Library's L1 / R1 are its tabs.)
    x += PS4Glyphs::Draw(dl, m_bold, "L2", x, y, 36, 0.9f) + 8;
    left("R2", "Page");
    // ((01.05) the games most played online: Cross makes a room for the selected one, when it is ours)
    if (m_online_page == PAGE_TOP)
    {
      left("Cross", "Create Room");
      right("Options", "Online Settings");
      return;
    }
    if (m_online_page == PAGE_CHAT)
    {
      if (m_world_index >= 0)
        left("Cross", "Mute or Report");
      right("Options", "Online Settings");
      right("Triangle", "Write");
      right("Square", "Quick Phrase");
      return;
    }
    if (m_online_page == PAGE_PLAYERS)
    {
      right("Options", "Online Settings");
      right("Triangle", m_players_days ? "Since Ever" : "Last 7 Days");  // ((01.07) the ranking's period)
      return;
    }
    left("Cross", mine ? "Go to Your Room" : OnlineRowWatch() ? "Watch" : "Join");
    if (!mine && OnlineRowCanWatch() && !OnlineRowWatch())
      left("Touchpad", "Watch");  // ((01.15) a spectator's, a player's place left or not)
    right("Options", "Online Settings");
    right("Triangle", "Join with Code");
    right("Square", "Create Room");
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
    right("Options", "Order and Filters");  // ((01.18) until 01.17: Sort, the next order at once)
    // (00.80) RetroAchievements' filter: with the marks on (not beside a search's Clear Search: no room).
    if (m_ra_marks && m_search.empty())
      right("Touchpad", "RA Filter");
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
  g_start_step = "the menu: its pictures, fonts and sounds";
  LoadAssets();
  m_audio.Start();
  m_input.Open(user);
  g_start_step = "the menu: the saves";
  LoadSaves();
  g_start_step = "the menu: its lists";
  ApplyPreferences();
  BuildItems();
  g_start_step = "the menu: the rest before its first frame";
  m_launch = -1;
  m_launch_time = 0.0f;
  m_fade = 1.0f;
  m_panel = {};
  m_controls_open = false;
  m_keyboard = false;
  m_keyboard_slide = 0.0f;
  m_toast_time = 0.0f;
  // (00.85) A read of the games a game's start stopped (HoldRead): again now, the look at the places with it.
  if (m_read_resume)
  {
    m_read_resume = false;
    AskRead(true, true);
  }
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
  m_rating_matcher.Pause(false);
  Achievements::PauseLibrary(false);  // (00.77)
  m_rating_download.Pause(false);
  // Star ratings: LaunchBox's, downloaded in the background when there are none or they are 60 days old (once a
  // session; Settings -> Star Ratings Off: never), paused while a game runs.
  if (m_prefs.ratings && !m_rating_download_tried && !m_safe_start && Ratings::CopyOld(m_rating_copy))
  {
    Log(std::string("ratings: downloading LaunchBox's ratings in the background (") +
        (!m_rating_copy.there ? std::string("none yet") :
         !m_rating_copy.meta  ? "the copy of " + m_rating_copy.date + " has no genres and players: before 01.18" :
                                "the copy of " + m_rating_copy.date + " is 60 days old") + ")");
    StartRatingsDownload();
  }
  // (00.85) The read waits while a game runs (Library::ScanProgress::pause), and goes on now; the missing covers are
  // downloaded once the start's read has the games and the covers (StartCoversOnce, else in the frames' loop).
  m_running = true;
  m_read_progress.pause = false;
  StartCoversOnce();
  // Game Bezels (00.80): nothing comes while a game runs; a download a cut stopped (the app closed, the connection
  // lost) goes on in the background when the menu first opens.
  m_bezel_download.Pause(false);
  NetplayLibrary::Pause(false);  // (00.23: the games' files hashed again, if any is left)
  if (!m_bezel_resume_tried && !m_safe_start)  // ((01.11) a safe start's next start goes on with it)
  {
    m_bezel_resume_tried = true;
    const std::vector<std::string> pending = BezelProject::Downloader::Pending(UserDir());
    if (!pending.empty() && !m_bezel_download.Busy())
      StartBezelDownload(pending, true);
  }
  // Updates (00.73): what update.ini remembers, and the day's check of GitHub when one is due (its thread), when the
  // menu first opens (not before a default game).
  if (!m_update_started)
  {
    m_update_started = true;
    Update::Start(m_data_dir, App::VERSION);
    // (00.23: off in Settings -> Anonymous Statistics) (00.89) Once the notice is answered, when there is one.
    m_stats_notice = StatsNoticeWanted();
    m_stats_waiting = m_stats_notice;
    if (!m_stats_waiting)
      UsageStats::Start(m_data_dir, App::VERSION);
    // (00.91) EmuSuNi closed by itself last time (crash.log grew, or the start watchdog restarted it): ask to send it.
    // (01.11: or the start-up watchdog closed it - a safe start now: SafeStart)
    m_report_ask = LogReport::CrashToAsk(m_data_dir, StartGuard::Restarted() || m_safe_start);
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
    m_panel.title = m_pending_title.empty() ? std::string("The game couldn't start") : m_pending_title;
    m_panel.lines = {""};
    for (size_t start = 0; start <= m_pending_error.size();)
    {
      const size_t end = std::min(m_pending_error.find('\n', start), m_pending_error.size());
      m_panel.lines.push_back(m_pending_error.substr(start, end - start));
      start = end + 1;
    }
    m_pending_error.clear();
  }
  // (01.22) Back from a game its in-game menu closed to install or remove something: that game's panel on it (its own
  // confirmation), Start the Game at its top.
  {
    GameExtras::AfterGame after;
    if (GameExtras::TakeAfterGame(&after) && !m_panel.open)
      OpenAfterGame(after);
  }
  // (00.23) Back from a Choose Together match: its room again, the games both have to pick another. (00.86) Back from
  // a game played alone while our room stayed open: the room again only when someone is in it or asks to join (the
  // in-game menu's Accept Player ends the game for that) - else the menu as it was.
  {
    const Netplay::View v = Netplay::GetView();
    bool other = false;
    for (const Netplay::PlayerView& p : v.players)
      other = other || !p.me;
    // ((01.23) Back from a match of any room: its room again, the host's alone too - Back to the Room.)
    const bool back = m_online_back;
    m_online_back = false;
    if (!m_panel.open && v.phase == Netplay::Phase::Room && (!v.host || other || back))
    {
      OpenOnlineRoom();
      if (v.host && v.asking < 0)
        m_online_prefer = "start";
    }
  }

  g_start_step = "the menu: its first frames";
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
    TakeRead();  // (00.85: the read's covers and games, "Reading your games..." as it goes)
    UpdateReadingItems();
    StartCoversOnce();
    UpdateNoCoverItem();
    UpdateShownConsoles();
    // While the tour plays, the controller is left out (a press would change what it shows).
    const bool touring = Tour::Active();
    if (m_intro)
    {
      if (m_input.Poll(dt) != 0 && !touring)
        m_intro_time = std::max(m_intro_time, 2.1f);
    }
    else if (!m_capture)
    {
      const uint32_t pad = m_input.Poll(dt);
      m_repeated = touring ? 0u : m_input.Repeated();  // (the tour's presses are fresh ones)
      HandleInput((touring ? 0u : pad) | MenuButtons(tour.buttons));
    }
    // (00.76) A console tab reached with L1 / R1 (held through the others): the drives' look once the tabs settle.
    if (m_places_waiting && TabsSettled())
    {
      m_places_waiting = false;
      if (IsGameList(m_category))
        CheckPlaces(false);
    }
    UpdateCovers();
    UpdateRatings();
    UpdateCoverDownload();
    UpdateBezelDownload();  // (Game Bezels, 00.80)
    // (01.11) The bezels there, counted from their records once the menu has shown itself (BezelsDetail; a safe start:
    // when their page opens).
    if (!m_bezels_counted && !m_safe_start && !m_bezel_download.Busy() &&
        g_display_frames.load(std::memory_order_relaxed) >= 3)
    {
      for (Item& item : m_items[static_cast<int>(Category::Settings)])
      {
        if (item.setting == SET_BEZELS)
          item.detail = BezelsDetail();
      }
    }
#ifdef EMUSUNI_3DS
    Update3dsInstall();  // ((01.09) a CIA's install as it goes)
#endif
    UpdatePatchedCopy();  // ((01.22) an Arcade game's patched romset's copy as it goes)
    UpdateCheatFinder();
    UpdateAchievements(dt);  // (RetroAchievements, 00.73: the login as it goes, rc_client's waiting work)
    // (00.23) Voice Chat: Test Microphone's progress and level meter while its panel shows; it stops when it closes.
    const bool voice_shown = m_panel.open && m_panel.setting == SET_VOICE;
    if (Voice::PanelUpdate(voice_shown) && voice_shown)
      BuildVoiceRows();
    UpdateFind(dt);
    UpdateUpdater();
    UpdatePortable();  // (00.87: the copy of the player's data, a drive's question, the warnings)
    UpdateOnline();  // (00.23: the room as it goes; the match's start)
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
      if (m_launch_online && Netplay::LinkRoom())
        SetGbLinkMatch();  // (00.95: the Game Boy link cable's two games, src/GbLink.h)
      TellInstallFolders(m_games[m_launch].rom.system);  // ((01.22) for its in-game menu's Updates, DLC and Mods)
      *result = m_games[m_launch].rom;
      picked = true;
      break;
    }
  }
  m_input.Close();
  Voice::PanelUpdate(false);  // (00.23: Test Microphone stops with the menu)
  m_audio.Stop();
  ForgetStatePictures();
  m_running = false;
  HoldRead();  // (00.85: no read of the games while the game runs)
  m_dl_pause = true;
  m_rating_matcher.Pause(true);  // (its games wait while a game runs, and the ratings' download)
  Achievements::PauseLibrary(true);  // (the RetroAchievements marks' hashing too, 00.77)
  m_rating_download.Pause(true);
  m_bezel_download.Pause(true);  // (Game Bezels, 00.80: the file coming stops, its part kept for afterwards)
  NetplayLibrary::Pause(true);  // (00.23: no game file hashed while a game runs)
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

bool Menu::FindGame(const std::string& path, Library::Game* game)
{
  return m_impl->FindGame(path, game);
}

void Menu::SafeStart()
{
  m_impl->SafeStart();
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
