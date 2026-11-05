// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "NetplayUi.h"

#include <algorithm>
#include <cfloat>

#include "Netplay.h"
#include "PS4Lang.h"

namespace NetplayUi
{
namespace
{
ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(255 * std::clamp(a, 0.0f, 1.0f)));
}
}  // namespace

ImU32 QualityColor(int quality, float alpha)
{
  return quality < 0 ? Rgba(160, 160, 160, 0.9f * alpha) : quality == 0 ? Rgba(80, 210, 110, 0.95f * alpha) :
         quality == 1 ? Rgba(240, 190, 60, 0.95f * alpha) :
                        Rgba(235, 80, 70, 0.95f * alpha);
}

void DrawBars(ImDrawList* dl, ImVec2 at, float h, int quality, int ping_ms, float alpha)
{
  const int lit = quality < 0 ? 0 : quality == 0 ? 4 : quality == 1 ? 3 : ping_ms >= 250 ? 1 : 2;
  const float w = h * 0.22f, gap = h * 0.12f;
  for (int i = 0; i < 4; i++)
  {
    const float bh = h * (0.4f + 0.2f * static_cast<float>(i));
    const float x = at.x + static_cast<float>(i) * (w + gap);
    dl->AddRectFilled(ImVec2(x, at.y + h - bh), ImVec2(x + w, at.y + h),
                      i < lit ? QualityColor(quality, alpha) : Rgba(255, 255, 255, 0.22f * alpha), 1.5f);
  }
}

float DrawLatencyBar(ImDrawList* dl, ImFont* regular, ImFont* bold, const std::vector<Netplay::PlayerView>& players,
                     ImVec2 corner, int delay, float alpha)
{
  constexpr float SIZE = 22.0f, ROW = 34.0f, PAD = 14.0f, BARS = 28.0f;
  // The rows' texts: the nickname (mine marked), the ping and "relay" through a relay server.
  struct Line
  {
    std::string name, ping;
    const Netplay::PlayerView* p;
  };
  std::vector<Line> lines;
  float name_w = 0.0f, ping_w = 0.0f;
  for (const Netplay::PlayerView& p : players)
  {
    if (p.asking)
      continue;
    Line line{p.me ? PS4Lang::T(p.nick + " (you)") : p.nick,
              p.ping_ms >= 0 ? std::to_string(p.ping_ms) + " ms" : std::string("..."), &p};
    if (p.relay && !p.me)
      line.ping += " (" + std::string(PS4Lang::T("relay")) + ")";
    name_w = std::max(name_w, bold->CalcTextSizeA(SIZE, FLT_MAX, 0, line.name.c_str()).x);
    ping_w = std::max(ping_w, regular->CalcTextSizeA(SIZE, FLT_MAX, 0, line.ping.c_str()).x);
    lines.push_back(std::move(line));
  }
  if (lines.empty())
    return corner.y;
  const std::string delay_text =
      delay > 0 ? PS4Lang::T("Input delay: " + std::to_string(delay) + (delay == 1 ? " frame" : " frames")) :
                  std::string();
  const float delay_w = delay_text.empty() ? 0.0f : regular->CalcTextSizeA(18, FLT_MAX, 0, delay_text.c_str()).x;
  const float width = std::max(PAD + BARS + 10.0f + name_w + 24.0f + ping_w + PAD, delay_w + PAD * 2.0f);
  const float height = PAD * 2.0f - 8.0f + ROW * static_cast<float>(lines.size()) + (delay_text.empty() ? 0.0f : 26.0f);
  const ImVec2 p0(corner.x - width, corner.y), p1(corner.x, corner.y + height);
  dl->AddRectFilled(p0, p1, Rgba(0, 0, 0, 0.5f * alpha), 10.0f);
  float y = p0.y + PAD - 4.0f;
  for (const Line& line : lines)
  {
    const Netplay::PlayerView& p = *line.p;
    DrawBars(dl, ImVec2(p0.x + PAD, y + 5.0f), 20.0f, p.quality, p.ping_ms, alpha);
    // (Who speaks in the voice chat: the nickname in green.)
    dl->AddText(bold, SIZE, ImVec2(p0.x + PAD + BARS + 10.0f, y),
                p.talking ? Rgba(110, 230, 140, alpha) : Rgba(255, 255, 255, 0.92f * alpha), line.name.c_str());
    const float pw = regular->CalcTextSizeA(SIZE, FLT_MAX, 0, line.ping.c_str()).x;
    dl->AddText(regular, SIZE, ImVec2(p1.x - PAD - pw, y), Rgba(255, 255, 255, 0.85f * alpha), line.ping.c_str());
    y += ROW;
  }
  if (!delay_text.empty())
    dl->AddText(regular, 18, ImVec2(p0.x + PAD, y - 2.0f), Rgba(255, 255, 255, 0.6f * alpha), delay_text.c_str());
  return p1.y;
}

void DrawOverlay(ImDrawList* dl, ImFont* regular, ImFont* bold)
{
  const Netplay::Status status = Netplay::GetStatus();
  if (!status.active)
    return;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  // The corner (00.92): the latency bar - each player's ping and connection - unless hidden.
  float ty = 26;
  if (status.latency_bar)
    ty = DrawLatencyBar(dl, regular, bold, status.players, ImVec2(size.x - 24, 26), status.delay, 1.0f) + 12;
  // Who is speaking (the voice chat), with the bar hidden: their names in the corner, with a green dot.
  for (const Netplay::PlayerView& p : status.players)
  {
    if (!p.talking || status.latency_bar)
      continue;
    const float w = regular->CalcTextSizeA(24, FLT_MAX, 0, p.nick.c_str()).x;
    dl->AddRectFilled(ImVec2(size.x - w - 72, ty - 6), ImVec2(size.x - 24, ty + 32), Rgba(0, 0, 0, 0.45f), 10);
    dl->AddCircleFilled(ImVec2(size.x - w - 52, ty + 13), 6, Rgba(80, 210, 110, 0.95f));
    dl->AddText(regular, 24, ImVec2(size.x - w - 38, ty), Rgba(255, 255, 255, 0.9f), p.nick.c_str());
    ty += 42;
  }
  // What the match waits for, in the middle of the screen's top.
  if (!status.line.empty())
  {
    const char* text = PS4Lang::T(status.line).c_str();
    const float tw = bold->CalcTextSizeA(34, FLT_MAX, 0, text).x;
    const ImVec2 p((size.x - tw) * 0.5f, size.y * 0.16f);
    dl->AddRectFilled(ImVec2(p.x - 34, p.y - 20), ImVec2(p.x + tw + 34, p.y + 58), Rgba(26, 12, 52, 0.88f), 14);
    dl->AddText(bold, 34, p, Rgba(255, 255, 255, 1.0f), text);
  }
}

void DrawWaiting(ImDrawList* dl, ImFont* regular, ImFont* bold, const std::string& line)
{
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
  const char* title = PS4Lang::T("Online Play");
  const float w = bold->CalcTextSizeA(44, FLT_MAX, 0, title).x;
  dl->AddText(bold, 44, ImVec2((size.x - w) * 0.5f, size.y * 0.42f), IM_COL32(255, 255, 255, 255), title);
  const char* text = PS4Lang::T(line.empty() ? std::string("Connecting...") : line).c_str();
  const float tw = regular->CalcTextSizeA(32, FLT_MAX, 0, text).x;
  dl->AddText(regular, 32, ImVec2((size.x - tw) * 0.5f, size.y * 0.42f + 70), IM_COL32(220, 220, 230, 255), text);
}
}  // namespace NetplayUi
