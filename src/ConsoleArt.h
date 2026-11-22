// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.26, a player: "de lejos no se distinguen"; the owner: "qué se ve: nada, la consola, o el mando") Settings ->
// Console Icons: the drawings src/Menu.cpp's DrawCategoryIcon had not - the consoles it drew as their controllers
// (Super Nintendo, NES, Nintendo 64, PlayStation, the Saturn's logo, the Atari's sticks and pad, the Atomiswave's sign)
// drawn as the consoles, and the ones it drew as consoles (Dreamcast, SG-1000, Master System, Mega Drive, PC Engine,
// the arcade cabinets) as their controllers. Our own drawings, plain shapes in one colour: no logo, no name. Like
// DrawCategoryIcon's, the pieces in `color` never overlap (it is see-through when the icon isn't selected); the
// details over them are a darker shade. DrawConsoleArt: true when it drew one, false - DrawCategoryIcon's own then
// (a handheld is its own controller; OpenBOR's fist is both).

#pragma once

#include <cmath>

#include <imgui.h>

#include "Systems.h"

inline bool DrawConsoleArt(ImDrawList* dl, SystemId id, bool controller, ImVec2 c, float s, ImU32 color)
{
  const float r = s * 0.5f;
  const ImU32 hole = IM_COL32(0, 0, 0, 130);
  const auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  const auto box = [&](float x0, float y0, float x1, float y1, ImU32 col, float round = 0.0f,
                       ImDrawFlags flags = 0) { dl->AddRectFilled(P(x0, y0), P(x1, y1), col, round * r, flags); };
  const auto dot = [&](float x, float y, float rad, ImU32 col) { dl->AddCircleFilled(P(x, y), rad * r, col, 20); };
  const auto cross = [&](float x, float y, float arm, float thick) {
    box(x - arm, y - thick, x + arm, y + thick, hole);  // (the arms above and below apart: holes never overlap)
    box(x - thick, y - arm, x + thick, y - thick, hole);
    box(x - thick, y + thick, x + thick, y + arm, hole);
  };
  const auto quad = [&](ImVec2 a, ImVec2 b, ImVec2 cc, ImVec2 d, ImU32 col) { dl->AddQuadFilled(a, b, cc, d, col); };
  // An arcade control panel seen from above: the panel, a stick (its ball, its shaft) at x, the buttons as holes.
  const auto stick = [&](float x) {
    dot(x, -0.42f, 0.2f, color);
    box(x - 0.05f, -0.22f, x + 0.05f, 0.1f, color);
  };
  if (!controller)
  {
    switch (id)
    {
    case SystemId::SNES:
      // The Super Nintendo: its rounded body, the cartridge slot on top between the power and reset switches, the
      // eject button, the line round its middle and the two controller ports in front.
      box(-0.95f, -0.4f, 0.95f, 0.45f, color, 0.28f);
      box(-0.38f, -0.32f, 0.38f, -0.16f, hole, 0.04f);
      box(-0.82f, -0.28f, -0.56f, -0.16f, hole, 0.04f);
      box(0.56f, -0.28f, 0.82f, -0.16f, hole, 0.04f);
      box(-0.12f, -0.08f, 0.12f, 0.02f, hole, 0.03f);
      box(-0.95f, 0.08f, 0.95f, 0.12f, hole);
      box(-0.75f, 0.24f, -0.55f, 0.36f, hole, 0.04f);
      box(-0.45f, 0.24f, -0.25f, 0.36f, hole, 0.04f);
      return true;
    case SystemId::NES:
      // The NES: the box that loads from the front, the cartridge's door, the line under it, the power and reset
      // buttons and the two ports.
      box(-0.95f, -0.45f, 0.95f, 0.5f, color, 0.06f);
      box(-0.5f, -0.33f, 0.82f, 0.02f, hole, 0.03f);
      box(-0.95f, 0.12f, 0.95f, 0.16f, hole);
      box(-0.85f, 0.26f, -0.65f, 0.4f, hole, 0.02f);
      box(-0.58f, 0.26f, -0.38f, 0.4f, hole, 0.02f);
      box(0.22f, 0.26f, 0.42f, 0.42f, hole, 0.02f);
      box(0.52f, 0.26f, 0.72f, 0.42f, hole, 0.02f);
      return true;
    case SystemId::N64:
      // The Nintendo 64: the wide body, the raised slot in its middle, the power and reset sliders, four ports.
      box(-0.36f, -0.42f, 0.36f, -0.06f, color, 0.08f, ImDrawFlags_RoundCornersTop);
      box(-0.95f, -0.06f, 0.95f, 0.45f, color, 0.22f);
      box(-0.22f, -0.33f, 0.22f, -0.22f, hole, 0.03f);
      box(-0.78f, 0.02f, -0.56f, 0.1f, hole, 0.03f);
      box(0.56f, 0.02f, 0.78f, 0.1f, hole, 0.03f);
      for (const float x : {-0.54f, -0.24f, 0.06f, 0.36f})
        box(x, 0.24f, x + 0.18f, 0.38f, hole, 0.04f);
      return true;
    case SystemId::PS1:
      // The PlayStation: the grey box, its round lid, the open, power and reset buttons, the two ports in front.
      box(-0.95f, -0.42f, 0.95f, 0.45f, color, 0.1f);
      dl->AddCircle(P(-0.25f, -0.02f), 0.36f * r, hole, 32, 0.08f * r);
      dot(-0.25f, -0.02f, 0.08f, hole);
      dot(0.45f, -0.22f, 0.08f, hole);
      dot(0.72f, -0.22f, 0.08f, hole);
      dot(0.6f, 0.12f, 0.13f, hole);
      box(-0.82f, 0.34f, -0.58f, 0.42f, hole, 0.02f);
      box(-0.5f, 0.34f, -0.26f, 0.42f, hole, 0.02f);
      return true;
    case SystemId::SATURN:
      // The Saturn: the low body, the wide oval lid, the power, reset and open buttons, the ports in front.
      box(-0.95f, -0.32f, 0.95f, 0.45f, color, 0.2f);
      dl->AddRect(P(-0.7f, -0.22f), P(0.3f, 0.22f), hole, 0.22f * r, 0, 0.08f * r);
      dot(0.55f, -0.1f, 0.08f, hole);
      dot(0.78f, -0.1f, 0.08f, hole);
      dot(0.66f, 0.18f, 0.12f, hole);
      box(-0.75f, 0.33f, -0.52f, 0.42f, hole, 0.02f);
      box(-0.42f, 0.33f, -0.19f, 0.42f, hole, 0.02f);
      return true;
    case SystemId::A2600:
      // The Atari 2600: the ribbed deck sloping down to the front, the cartridge slot, the switches, the wood panel.
      quad(P(-0.78f, -0.42f), P(0.78f, -0.42f), P(0.95f, -0.12f), P(-0.95f, -0.12f), color);
      box(-0.95f, -0.12f, 0.95f, 0.45f, color, 0.04f, ImDrawFlags_RoundCornersBottom);
      box(-0.3f, -0.38f, 0.3f, -0.28f, hole, 0.02f);
      for (const float x : {-0.66f, -0.56f, 0.46f, 0.56f})
        box(x, -0.36f, x + 0.04f, -0.16f, hole);
      for (const float x : {-0.8f, -0.55f, 0.35f, 0.6f})
        box(x, -0.04f, x + 0.16f, 0.12f, hole, 0.03f);
      box(-0.95f, 0.22f, 0.95f, 0.45f, hole, 0.04f, ImDrawFlags_RoundCornersBottom);
      return true;
    case SystemId::A7800:
      // The Atari 7800: a low wedge, the slot on top, two buttons, the line along its front and the two ports.
      quad(P(-0.85f, -0.28f), P(0.85f, -0.28f), P(0.95f, 0.0f), P(-0.95f, 0.0f), color);
      box(-0.95f, 0.0f, 0.95f, 0.4f, color, 0.06f, ImDrawFlags_RoundCornersBottom);
      box(-0.32f, -0.24f, 0.32f, -0.14f, hole, 0.02f);
      box(0.48f, -0.22f, 0.62f, -0.12f, hole, 0.02f);
      box(0.68f, -0.22f, 0.82f, -0.12f, hole, 0.02f);
      box(-0.95f, 0.12f, 0.95f, 0.16f, hole);
      box(-0.72f, 0.24f, -0.52f, 0.34f, hole, 0.02f);
      box(-0.45f, 0.24f, -0.25f, 0.34f, hole, 0.02f);
      return true;
    case SystemId::JAGUAR:
      // The Jaguar: its dome over the body, the slot on top of the dome, the power button, the ports in front.
      dl->PathEllipticalArcTo(P(0.0f, -0.08f), ImVec2(0.82f * r, 0.36f * r), 0.0f, IM_PI, 2.0f * IM_PI, 32);
      dl->PathFillConvex(color);
      box(-0.95f, -0.08f, 0.95f, 0.42f, color, 0.14f, ImDrawFlags_RoundCornersBottom);
      box(-0.24f, -0.38f, 0.24f, -0.28f, hole, 0.02f);
      dot(0.62f, 0.08f, 0.09f, hole);
      box(-0.72f, 0.26f, -0.5f, 0.36f, hole, 0.02f);
      box(-0.42f, 0.26f, -0.2f, 0.36f, hole, 0.02f);
      return true;
    case SystemId::ATOMISWAVE:
      // An Atomiswave cartridge (its games are cartridges in the board's slot): the wide case, its label, the edge
      // with its contacts below.
      box(-0.95f, -0.55f, 0.95f, 0.48f, color, 0.08f);
      box(-0.75f, -0.4f, 0.75f, 0.2f, hole, 0.05f);
      box(-0.7f, 0.48f, 0.7f, 0.68f, color, 0.02f, ImDrawFlags_RoundCornersBottom);
      for (int i = 0; i < 9; i++)
      {
        const float x = -0.62f + i * 0.14f;
        box(x, 0.52f, x + 0.06f, 0.66f, hole);
      }
      return true;
    default:
      return false;
    }
  }
  switch (id)
  {
  case SystemId::DC:
  {
    // The Dreamcast's pad: its wide top, the two grips, the memory card's slot in the middle, the stick, the D-pad
    // and the four buttons.
    box(-0.95f, -0.48f, 0.95f, 0.14f, color, 0.32f, ImDrawFlags_RoundCornersTop);
    box(-0.88f, 0.14f, -0.42f, 0.62f, color, 0.2f, ImDrawFlags_RoundCornersBottom);
    box(0.42f, 0.14f, 0.88f, 0.62f, color, 0.2f, ImDrawFlags_RoundCornersBottom);
    box(-0.26f, -0.44f, 0.26f, -0.02f, hole, 0.04f);
    dot(-0.62f, -0.24f, 0.15f, hole);
    cross(-0.62f, 0.12f, 0.16f, 0.05f);
    dot(0.62f, -0.34f, 0.08f, hole);
    dot(0.62f, -0.02f, 0.08f, hole);
    dot(0.46f, -0.18f, 0.08f, hole);
    dot(0.78f, -0.18f, 0.08f, hole);
    return true;
  }
  case SystemId::SG1000:
    // The SG-1000's joystick: its small box with two buttons, the stick with its ball on top.
    dot(0.0f, -0.62f, 0.2f, color);
    box(-0.06f, -0.42f, 0.06f, 0.0f, color);
    box(-0.62f, 0.0f, 0.62f, 0.52f, color, 0.08f);
    dot(-0.32f, 0.26f, 0.1f, hole);
    dot(0.32f, 0.26f, 0.1f, hole);
    return true;
  case SystemId::SMS:
    // The Master System's pad: the flat box, the square D-pad with its little stick, buttons 1 and 2.
    box(-0.95f, -0.36f, 0.95f, 0.36f, color, 0.08f);
    box(-0.74f, -0.22f, -0.3f, 0.22f, hole, 0.06f);
    dot(-0.52f, 0.0f, 0.06f, hole);
    dot(0.38f, 0.06f, 0.13f, hole);
    dot(0.72f, 0.06f, 0.13f, hole);
    return true;
  case SystemId::MD:
    // The Mega Drive's pad: its round body, the round D-pad, Start, and A, B, C rising to the right.
    box(-0.95f, -0.42f, 0.95f, 0.42f, color, 0.42f);
    dl->AddCircle(P(-0.5f, 0.0f), 0.24f * r, hole, 24, 0.06f * r);
    cross(-0.5f, 0.0f, 0.16f, 0.05f);
    box(-0.08f, -0.24f, 0.1f, -0.16f, hole, 0.03f);
    dot(0.28f, 0.16f, 0.11f, hole);
    dot(0.52f, 0.04f, 0.11f, hole);
    dot(0.76f, -0.08f, 0.11f, hole);
    return true;
  case SystemId::PCE:
    // The PC Engine's pad: the small box, the D-pad, Select and Run, the two turbo switches over buttons II and I.
    box(-0.95f, -0.32f, 0.95f, 0.36f, color, 0.12f);
    cross(-0.55f, 0.04f, 0.2f, 0.06f);
    box(-0.16f, 0.12f, -0.02f, 0.2f, hole, 0.02f);
    box(0.06f, 0.12f, 0.2f, 0.2f, hole, 0.02f);
    box(0.36f, -0.22f, 0.5f, -0.14f, hole);
    box(0.62f, -0.22f, 0.76f, -0.14f, hole);
    dot(0.43f, 0.12f, 0.12f, hole);
    dot(0.69f, 0.12f, 0.12f, hole);
    return true;
  case SystemId::SATURN:
    // The Saturn's pad: its round body, the D-pad, Start, and the six buttons in two rows.
    box(-0.95f, -0.36f, 0.95f, 0.36f, color, 0.34f);
    cross(-0.55f, 0.0f, 0.18f, 0.055f);
    box(-0.1f, 0.08f, 0.08f, 0.16f, hole, 0.03f);
    for (int row = 0; row < 2; row++)
    {
      for (int i = 0; i < 3; i++)
        dot(0.28f + i * 0.22f, -0.1f + row * 0.24f + i * -0.03f, 0.08f, hole);
    }
    return true;
  case SystemId::ARCADE:
    // An arcade control panel: the stick and six buttons.
    stick(-0.55f);
    box(-0.95f, 0.1f, 0.95f, 0.5f, color, 0.08f);
    for (int row = 0; row < 2; row++)
    {
      for (int i = 0; i < 3; i++)
        dot(0.05f + i * 0.28f, 0.2f + row * 0.18f, 0.07f, hole);
    }
    return true;
  case SystemId::NAOMI:
    // A NAOMI cabinet's panel: two players' sticks and buttons.
    stick(-0.62f);
    stick(0.2f);
    box(-0.95f, 0.1f, 0.95f, 0.5f, color, 0.08f);
    for (const float x0 : {-0.4f, 0.42f})
    {
      for (int i = 0; i < 3; i++)
        dot(x0 + (i % 2) * 0.16f, 0.2f + (i / 2) * 0.18f + (i % 2) * 0.02f, 0.06f, hole);
    }
    return true;
  case SystemId::ATOMISWAVE:
    // An Atomiswave cabinet's panel: the stick, four buttons in a square.
    stick(-0.5f);
    box(-0.95f, 0.1f, 0.95f, 0.5f, color, 0.08f);
    for (int i = 0; i < 4; i++)
      dot(0.18f + (i % 2) * 0.3f, 0.2f + (i / 2) * 0.18f, 0.08f, hole);
    return true;
  default:
    return false;
  }
}
