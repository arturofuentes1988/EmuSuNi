// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "AchievementsUi.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "Achievements.h"
#include "Display.h"
#include "Image.h"
#include "Log.h"
#include "PS4Lang.h"

namespace AchievementsUi
{
namespace
{
struct Texture
{
  ImTextureID id{};
  uint64_t used = 0;  // the frame it was last drawn
};
std::map<std::string, Texture> s_textures;  // by address
std::set<std::string> s_broken;             // files that aren't pictures
uint64_t s_frame = 0;
int s_made_this_frame = 0;
constexpr size_t MOST_TEXTURES = 192;  // (64x64: 16 KiB each)

struct Shown
{
  Achievements::Notice notice;
  float time = 0.0f;  // shown so far
  float wait = 0.0f;  // waited for its picture
};
std::deque<Achievements::Notice> s_queue;
bool s_showing = false;
Shown s_current;
int s_progress_serial = 0;
float s_progress_time = 0.0f;

// The text cut to fit the width ("..." at its end): the longest start of it that fits, found by halves (a long
// description is measured a few times each frame the popup shows, not once per letter).
std::string Fit(ImFont* font, float size, const std::string& text, float width)
{
  if (!font || font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x <= width)
    return text;
  auto whole = [&text](size_t n) {  // (not in the middle of a UTF-8 letter)
    while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
      n--;
    return n;
  };
  size_t fits = 0, too_long = text.size();
  while (too_long - fits > 1)
  {
    const size_t mid = (fits + too_long) / 2;
    if (font->CalcTextSizeA(size, FLT_MAX, 0, (text.substr(0, whole(mid)) + "...").c_str()).x <= width)
      fits = mid;
    else
      too_long = mid;
  }
  return text.substr(0, whole(fits)) + "...";
}

ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255));
}

// The popup's first line: what happened (00.78: Casual only - no mode said, a game "Completed").
std::string Label(const Achievements::Notice& notice)
{
  using Kind = Achievements::Notice::Kind;
  switch (notice.kind)
  {
  case Kind::Unlocked:
    return PS4Lang::T(std::string("Achievement Unlocked")) + "  -  " +
           PS4Lang::T(notice.points == 1 ? std::string("1 point") : std::to_string(notice.points) + " points");
  case Kind::Mastered:
    return PS4Lang::T(std::string("Game Completed"));
  case Kind::LeaderboardStarted:
    return PS4Lang::T(std::string("Leaderboard Started"));
  case Kind::LeaderboardFailed:
    return PS4Lang::T(std::string("Leaderboard Failed"));
  case Kind::LeaderboardSubmitted:
    return PS4Lang::T(std::string("Leaderboard Submitted"));
  case Kind::Disconnected:
  case Kind::Reconnected:
  case Kind::Message:
  case Kind::Placard:
    return PS4Lang::T(std::string("RetroAchievements"));
  }
  return {};
}

// Its second and third lines.
void Lines(const Achievements::Notice& notice, const Achievements::Summary& summary, std::string* title,
           std::string* text)
{
  using Kind = Achievements::Notice::Kind;
  *title = notice.title;
  *text = notice.text;
  switch (notice.kind)
  {
  case Kind::Placard:
    if (notice.text.empty())
    {
      // The game's achievements loaded: how many the player has.
      *text = PS4Lang::T(std::to_string(summary.unlocked) + " of " + std::to_string(summary.total) + " achievements") +
              "  -  " +
              PS4Lang::T(std::to_string(summary.points) + " of " + std::to_string(summary.total_points) + " points");
    }
    else
    {
      // Why there are none (the app's own words).
      *title = PS4Lang::T(notice.text);
      text->clear();
    }
    break;
  case Kind::Mastered:
    *text = PS4Lang::T(std::string("Every achievement of the game"));
    break;
  case Kind::Disconnected:
    *title = PS4Lang::T(std::string("No connection to RetroAchievements"));
    *text = PS4Lang::T(std::string("The unlocks are sent when it comes back"));
    break;
  case Kind::Reconnected:
    *title = PS4Lang::T(std::string("Connected to RetroAchievements again"));
    *text = PS4Lang::T(std::string("The unlocks that waited were sent"));
    break;
  case Kind::Message:
    // (The app's own notes in the language chosen; other messages stay as they come.)
    *title = PS4Lang::T(notice.title);
    *text = PS4Lang::T(notice.text);
    break;
  default:
    break;
  }
}

void DrawPopup(ImDrawList* dl, Display& display, ImFont* regular, ImFont* bold, const Shown& shown,
               const Achievements::Summary& summary)
{
  const float W = static_cast<float>(Display::WIDTH);
  // Slides in from the right and out again.
  const float in = std::min(1.0f, shown.time / 0.25f);
  const float out = std::min(1.0f, std::max(0.0f, shown.notice.seconds - shown.time) / 0.3f);
  const float a = std::min(in, out);
  const float width = 660.0f, height = 118.0f;
  const float x = W - 40.0f - width * (0.3f + 0.7f * a) - (1.0f - a) * 40.0f;
  const float y = 40.0f;
  using Kind = Achievements::Notice::Kind;
  const bool gold = shown.notice.kind == Kind::Mastered;
  const bool warn = shown.notice.kind == Kind::Disconnected;
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), Rgba(26, 12, 52, 0.92f * a), 16.0f);
  dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
              gold ? Rgba(255, 200, 80, a) : warn ? Rgba(255, 150, 110, a) : Rgba(196, 170, 255, 0.9f * a), 16.0f, 0,
              2.0f);
  float text_x = x + 24.0f;
  const ImTextureID picture = shown.notice.badge.empty() ? ImTextureID{} : Picture(display, shown.notice.badge);
  if (picture != ImTextureID{})
  {
    dl->AddImage(ImTextureRef(picture), ImVec2(x + 18, y + 19), ImVec2(x + 98, y + 99), ImVec2(0, 0), ImVec2(1, 1),
                 Rgba(255, 255, 255, a));
    text_x = x + 116.0f;
  }
  const float text_w = x + width - 20.0f - text_x;
  std::string title, text;
  Lines(shown.notice, summary, &title, &text);
  const std::string label = Label(shown.notice);
  dl->AddText(regular, 24, ImVec2(text_x, y + 12), gold ? Rgba(255, 210, 110, a) : Rgba(196, 170, 255, a),
              Fit(regular, 24, label, text_w).c_str());
  dl->AddText(bold, 32, ImVec2(text_x, y + 40), Rgba(255, 255, 255, a), Fit(bold, 32, title, text_w).c_str());
  if (!text.empty())
    dl->AddText(regular, 23, ImVec2(text_x, y + 80), Rgba(255, 255, 255, 0.72f * a), Fit(regular, 23, text, text_w).c_str());
}

void DrawOverlay(ImDrawList* dl, Display& display, ImFont* regular, ImFont* bold, float dt)
{
  const Achievements::Overlay overlay = Achievements::GetOverlay();
  const float W = static_cast<float>(Display::WIDTH), H = static_cast<float>(Display::HEIGHT);
  // The challenge indicators: the pictures of the achievements one move away, bottom left.
  float x = 40.0f;
  const float icon = 60.0f, bottom = H - 40.0f;
  for (const std::string& badge : overlay.challenges)
  {
    const ImTextureID picture = Picture(display, badge);
    dl->AddRectFilled(ImVec2(x - 4, bottom - icon - 4), ImVec2(x + icon + 4, bottom + 4), Rgba(0, 0, 0, 0.55f), 8.0f);
    if (picture != ImTextureID{})
      dl->AddImage(ImTextureRef(picture), ImVec2(x, bottom - icon), ImVec2(x + icon, bottom), ImVec2(0, 0), ImVec2(1, 1),
                   Rgba(255, 255, 255, 1.0f));
    x += icon + 14.0f;
  }
  // The leaderboards' trackers, above them.
  float y = bottom - (overlay.challenges.empty() ? 0.0f : icon + 20.0f);
  for (const std::string& value : overlay.trackers)
  {
    const float w = bold->CalcTextSizeA(30, FLT_MAX, 0, value.c_str()).x + 32.0f;
    dl->AddRectFilled(ImVec2(40, y - 50), ImVec2(40 + w, y), Rgba(26, 12, 52, 0.85f), 10.0f);
    dl->AddText(bold, 30, ImVec2(56, y - 43), Rgba(255, 255, 255, 1.0f), value.c_str());
    y -= 60.0f;
  }
  // A measured achievement's progress ("5/10"): for a moment at each change, under the popups.
  if (overlay.progress_serial != s_progress_serial)
  {
    s_progress_serial = overlay.progress_serial;
    s_progress_time = overlay.progress.empty() ? 0.0f : 2.5f;
  }
  if (s_progress_time > 0.0f && !overlay.progress.empty())
  {
    s_progress_time -= dt;
    const float a = std::min(1.0f, s_progress_time / 0.3f);
    const float w = bold->CalcTextSizeA(28, FLT_MAX, 0, overlay.progress.c_str()).x + 96.0f;
    const float px = W - 40.0f - w, py = 176.0f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + w, py + 60), Rgba(26, 12, 52, 0.88f * a), 12.0f);
    const ImTextureID picture = Picture(display, overlay.progress_badge);
    if (picture != ImTextureID{})
      dl->AddImage(ImTextureRef(picture), ImVec2(px + 10, py + 8), ImVec2(px + 54, py + 52), ImVec2(0, 0), ImVec2(1, 1),
                   Rgba(255, 255, 255, a));
    dl->AddText(bold, 28, ImVec2(px + 66, py + 14), Rgba(255, 255, 255, a), overlay.progress.c_str());
  }
  (void)regular;
}
}  // namespace

ImTextureID Picture(Display& display, const std::string& address)
{
  // A new frame (the main menu's too, which has no Draw: 00.75's history): a few more pictures may be made.
  static int imgui_frame = -1;
  if (ImGui::GetFrameCount() != imgui_frame)
  {
    imgui_frame = ImGui::GetFrameCount();
    s_frame++;
    s_made_this_frame = 0;
  }
  if (address.empty())
    return ImTextureID{};
  if (const auto it = s_textures.find(address); it != s_textures.end())
  {
    it->second.used = s_frame;
    return it->second.id;
  }
  if (s_broken.count(address) || s_made_this_frame >= 3)
    return ImTextureID{};
  const std::string file = Achievements::PictureFile(address);
  if (file.empty())
    return ImTextureID{};  // (downloading)
  ImageRGBA image;
  if (!LoadPng(file, &image, 128, 128) || image.empty())
  {
    LogF("ra: picture %s isn't a picture: left out", file.c_str());
    s_broken.insert(address);
    std::remove(file.c_str());
    return ImTextureID{};
  }
  // At most MOST_TEXTURES: the one drawn longest ago goes first.
  if (s_textures.size() >= MOST_TEXTURES)
  {
    auto oldest = s_textures.begin();
    for (auto it = s_textures.begin(); it != s_textures.end(); ++it)
    {
      if (it->second.used < oldest->second.used)
        oldest = it;
    }
    display.DestroyTexture(oldest->second.id);
    s_textures.erase(oldest);
  }
  s_made_this_frame++;
  Texture texture;
  texture.id = display.CreateTexture(image.pixels.data(), image.width, image.height);
  texture.used = s_frame;
  if (texture.id == ImTextureID{})
  {
    s_broken.insert(address);
    return ImTextureID{};
  }
  s_textures[address] = texture;
  return texture.id;
}

void Draw(ImDrawList* dl, Display& display, ImFont* regular, ImFont* bold, float dt)
{
  s_frame++;
  s_made_this_frame = 0;
  for (Achievements::Notice& notice : Achievements::TakeNotices())
  {
    // (A placard after another one replaces it: the latest is what's true.)
    if (notice.kind == Achievements::Notice::Kind::Placard)
    {
      s_queue.erase(std::remove_if(s_queue.begin(), s_queue.end(),
                                   [](const Achievements::Notice& n) {
                                     return n.kind == Achievements::Notice::Kind::Placard;
                                   }),
                    s_queue.end());
    }
    s_queue.push_back(std::move(notice));
  }
  if (!s_showing && !s_queue.empty())
  {
    s_current = {};
    s_current.notice = std::move(s_queue.front());
    s_queue.pop_front();
    s_showing = true;
  }
  const Achievements::Summary summary =
      s_showing || !s_queue.empty() ? Achievements::GetSummary() : Achievements::Summary{};
  if (s_showing)
  {
    // (Its picture first, for up to a second and a half: an unlock's download starts with it.)
    const bool picture = s_current.notice.badge.empty() || Picture(display, s_current.notice.badge) != ImTextureID{};
    if (!picture && s_current.wait < 1.5f)
    {
      s_current.wait += dt;
    }
    else
    {
      s_current.time += dt;
      // (Many waiting: each a little shorter.)
      const float seconds = s_queue.size() > 3 ? std::min(s_current.notice.seconds, 2.5f) : s_current.notice.seconds;
      s_current.notice.seconds = seconds;
      DrawPopup(dl, display, regular, bold, s_current, summary);
      if (s_current.time >= seconds)
        s_showing = false;
    }
  }
  DrawOverlay(dl, display, regular, bold, dt);
}

void Forget(Display& display)
{
  for (auto& [address, texture] : s_textures)
    display.DestroyTexture(texture.id);
  s_textures.clear();
  s_broken.clear();
  s_queue.clear();
  s_showing = false;
  s_current = {};
  s_progress_time = 0.0f;
}
}  // namespace AchievementsUi
