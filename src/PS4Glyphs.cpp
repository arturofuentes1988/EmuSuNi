// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "PS4Glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PS4Glyphs
{
namespace
{
enum class Kind
{
  None,
  Cross,
  Circle,
  Square,
  Triangle,
  Shoulder,  // L1 / R1
  Trigger,   // L2 / R2 (click or analog)
  StickPress,  // L3 / R3
  StickDir,    // a stick pushed one way
  Stick,       // a whole stick (stick groups)
  DPadDir,
  DPad,
  Options,
  Touchpad,
};

struct Info
{
  Kind kind = Kind::None;
  const char* label = "";  // L1, R2, L, R...
  int dir = -1;            // 0 up, 1 down, 2 left, 3 right
  bool analog = false;     // L2 / R2 as an axis
};

Info Lookup(const std::string& name)
{
  struct Entry
  {
    const char* name;
    Info info;
  };
  static const Entry ENTRIES[] = {
      {"Cross", {Kind::Cross}},
      {"Circle", {Kind::Circle}},
      {"Square", {Kind::Square}},
      {"Triangle", {Kind::Triangle}},
      {"L1", {Kind::Shoulder, "L1"}},
      {"R1", {Kind::Shoulder, "R1"}},
      {"L2 Click", {Kind::Trigger, "L2"}},
      {"R2 Click", {Kind::Trigger, "R2"}},
      {"L2", {Kind::Trigger, "L2", -1, true}},
      {"R2", {Kind::Trigger, "R2", -1, true}},
      {"L3", {Kind::StickPress, "L"}},
      {"R3", {Kind::StickPress, "R"}},
      {"Options", {Kind::Options}},
      {"Touchpad", {Kind::Touchpad}},
      {"Pad N", {Kind::DPadDir, "", 0}},
      {"Pad S", {Kind::DPadDir, "", 1}},
      {"Pad W", {Kind::DPadDir, "", 2}},
      {"Pad E", {Kind::DPadDir, "", 3}},
      {"Left Y-", {Kind::StickDir, "L", 0}},
      {"Left Y+", {Kind::StickDir, "L", 1}},
      {"Left X-", {Kind::StickDir, "L", 2}},
      {"Left X+", {Kind::StickDir, "L", 3}},
      {"Right Y-", {Kind::StickDir, "R", 0}},
      {"Right Y+", {Kind::StickDir, "R", 1}},
      {"Right X-", {Kind::StickDir, "R", 2}},
      {"Right X+", {Kind::StickDir, "R", 3}},
      {"Left Stick", {Kind::Stick, "L"}},
      {"Right Stick", {Kind::Stick, "R"}},
      {"D-Pad", {Kind::DPad}},
  };
  std::string stripped;
  for (char c : name)
  {
    if (c != '`')
      stripped += c;
  }
  for (const Entry& entry : ENTRIES)
  {
    if (stripped == entry.name)
      return entry.info;
  }
  return {};
}

ImU32 Colour(int r, int g, int b, float alpha)
{
  return IM_COL32(r, g, b, static_cast<int>(255 * alpha));
}

void CentredText(ImDrawList* dl, ImFont* font, float size, ImVec2 centre, ImU32 colour,
                 const char* text)
{
  if (!font || !*text)
    return;
  const ImVec2 extent = font->CalcTextSizeA(size, 10000.0f, 0.0f, text);
  dl->AddText(font, size, ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), colour,
              text);
}

// A filled triangle pointing in direction dir (0 up, 1 down, 2 left, 3 right).
void Arrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 colour)
{
  const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
  const ImVec2 tip(c.x + dx[dir] * s, c.y + dy[dir] * s);
  const ImVec2 base(c.x - dx[dir] * s * 0.4f, c.y - dy[dir] * s * 0.4f);
  const ImVec2 side(dy[dir] * s * 0.8f, dx[dir] * s * 0.8f);
  dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y),
                        ImVec2(base.x - side.x, base.y - side.y), colour);
}

void DPadShape(ImDrawList* dl, ImVec2 c, float r, int lit_dir, bool all_lit, float alpha)
{
  const float arm = r * 0.42f;
  const ImU32 dim = Colour(255, 255, 255, 0.28f * alpha);
  const ImU32 lit = Colour(255, 255, 255, alpha);
  const ImVec2 mins[] = {{c.x - arm * 0.5f, c.y - r},
                         {c.x - arm * 0.5f, c.y + arm * 0.5f},
                         {c.x - r, c.y - arm * 0.5f},
                         {c.x + arm * 0.5f, c.y - arm * 0.5f}};
  const ImVec2 maxs[] = {{c.x + arm * 0.5f, c.y - arm * 0.5f},
                         {c.x + arm * 0.5f, c.y + r},
                         {c.x - arm * 0.5f, c.y + arm * 0.5f},
                         {c.x + r, c.y + arm * 0.5f}};
  for (int i = 0; i < 4; i++)
    dl->AddRectFilled(mins[i], maxs[i], all_lit || i == lit_dir ? lit : dim, r * 0.12f);
  dl->AddRectFilled(ImVec2(c.x - arm * 0.5f, c.y - arm * 0.5f),
                    ImVec2(c.x + arm * 0.5f, c.y + arm * 0.5f), all_lit ? lit : dim);
}
}  // namespace

float Width(const std::string& name, float h)
{
  switch (Lookup(name).kind)
  {
  case Kind::None:
    return 0.0f;
  case Kind::Shoulder:
  case Kind::Trigger:
    return h * 1.35f;
  case Kind::Options:
    return h * 1.15f;
  case Kind::Touchpad:
    return h * 1.7f;
  default:
    return h;
  }
}

float Draw(ImDrawList* dl, ImFont* font, const std::string& name, float x, float cy, float h,
           float alpha)
{
  const Info info = Lookup(name);
  const float w = Width(name, h);
  if (info.kind == Kind::None)
    return 0.0f;
  const float r = h * 0.5f;
  const ImVec2 c(x + w * 0.5f, cy);
  const ImU32 white = Colour(255, 255, 255, alpha);
  const ImU32 disc = Colour(20, 24, 34, 0.85f * alpha);
  const ImU32 rim = Colour(255, 255, 255, 0.35f * alpha);
  const float line = std::max(1.5f, h * 0.085f);

  switch (info.kind)
  {
  case Kind::Cross:
  case Kind::Circle:
  case Kind::Square:
  case Kind::Triangle:
  {
    dl->AddCircleFilled(c, r, disc, 32);
    dl->AddCircle(c, r, rim, 32, 1.5f);
    const float s = r * 0.48f;
    if (info.kind == Kind::Cross)
    {
      const ImU32 blue = Colour(124, 178, 232, alpha);
      dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), blue, line);
      dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), blue, line);
    }
    else if (info.kind == Kind::Circle)
    {
      dl->AddCircle(c, s, Colour(255, 102, 102, alpha), 28, line);
    }
    else if (info.kind == Kind::Square)
    {
      dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f),
                  Colour(216, 143, 201, alpha), 0.0f, 0, line);
    }
    else
    {
      const ImU32 green = Colour(64, 224, 176, alpha);
      dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x - s * 1.05f, c.y + s * 0.75f),
                      ImVec2(c.x + s * 1.05f, c.y + s * 0.75f), green, line);
    }
    break;
  }
  case Kind::Shoulder:
  {
    // A flat bumper.
    const ImVec2 a(x, cy - r * 0.62f), b(x + w, cy + r * 0.62f);
    dl->AddRectFilled(a, b, disc, r * 0.5f);
    dl->AddRect(a, b, rim, r * 0.5f, 0, 1.5f);
    CentredText(dl, font, h * 0.55f, c, white, info.label);
    break;
  }
  case Kind::Trigger:
  {
    // A trigger: rounded on top, square at the bottom; analog ones get a pressure bar.
    const ImVec2 a(x, cy - r), b(x + w, cy + r);
    dl->AddRectFilled(a, b, disc, r * 0.7f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, rim, r * 0.7f, ImDrawFlags_RoundCornersTop, 1.5f);
    CentredText(dl, font, h * 0.55f, ImVec2(c.x, c.y - (info.analog ? r * 0.12f : 0.0f)), white,
                info.label);
    if (info.analog)
    {
      const float bar_y = cy + r * 0.62f;
      dl->AddRectFilledMultiColor(ImVec2(x + w * 0.2f, bar_y - line * 0.5f),
                                  ImVec2(x + w * 0.8f, bar_y + line * 0.5f),
                                  Colour(255, 255, 255, 0.15f * alpha), white, white,
                                  Colour(255, 255, 255, 0.15f * alpha));
    }
    break;
  }
  case Kind::StickPress:
  case Kind::StickDir:
  case Kind::Stick:
  {
    dl->AddCircleFilled(c, r, disc, 32);
    dl->AddCircle(c, r, rim, 32, 1.5f);
    dl->AddCircle(c, r * 0.68f, Colour(255, 255, 255, 0.5f * alpha), 28, 1.5f);
    if (info.kind == Kind::StickPress)
    {
      // L3 / R3: the stick top pushed in.
      CentredText(dl, font, h * 0.42f, ImVec2(c.x, c.y - r * 0.05f), white,
                  info.label[0] == 'L' ? "L3" : "R3");
    }
    else
    {
      CentredText(dl, font, h * 0.42f, c, white, info.label);
      if (info.kind == Kind::StickDir)
      {
        const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
        Arrow(dl, ImVec2(c.x + dx[info.dir] * r * 0.84f, c.y + dy[info.dir] * r * 0.84f),
              r * 0.26f, info.dir, Colour(255, 214, 102, alpha));
      }
      else
      {
        for (int d = 0; d < 4; d++)
        {
          const float dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
          Arrow(dl, ImVec2(c.x + dx[d] * r * 0.84f, c.y + dy[d] * r * 0.84f), r * 0.18f, d,
                Colour(255, 255, 255, 0.7f * alpha));
        }
      }
    }
    break;
  }
  case Kind::DPadDir:
  case Kind::DPad:
    DPadShape(dl, c, r * 0.95f, info.dir, info.kind == Kind::DPad, alpha);
    break;
  case Kind::Options:
  {
    // A small pill with the three-line OPTIONS mark.
    const ImVec2 a(x + w * 0.18f, cy - r * 0.8f), b(x + w * 0.82f, cy + r * 0.8f);
    dl->AddRectFilled(a, b, disc, r * 0.3f);
    dl->AddRect(a, b, rim, r * 0.3f, 0, 1.5f);
    for (int i = -1; i <= 1; i++)
    {
      const float y = cy + i * r * 0.32f;
      dl->AddLine(ImVec2(c.x - r * 0.3f, y), ImVec2(c.x + r * 0.3f, y), white, line * 0.8f);
    }
    break;
  }
  case Kind::Touchpad:
  {
    const ImVec2 a(x, cy - r * 0.7f), b(x + w, cy + r * 0.7f);
    dl->AddRectFilled(a, b, disc, r * 0.25f);
    dl->AddRect(a, b, white, r * 0.25f, 0, line * 0.8f);
    dl->AddCircleFilled(ImVec2(c.x, cy), r * 0.14f, white, 12);
    break;
  }
  case Kind::None:
    break;
  }
  return w;
}
}  // namespace PS4Glyphs
