// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DualShock 4 control icons drawn with ImGui primitives (no image assets): face buttons in their
// PlayStation colours, shoulder buttons, sticks, D-pad, Options and touch pad. Used by the XMB's
// Controls lists.

#pragma once

#include <string>

#include <imgui.h>

namespace PS4Glyphs
{
// Width of the icon for a PS4Pad input name ("Cross", "L2 Click", "Pad N", "Left X-") or stick
// group ("Left Stick", "Right Stick", "D-Pad") at height h; 0 when it has no icon (show text).
float Width(const std::string& name, float h);
// Draws it with its left edge at x, centred vertically on cy. Returns the width drawn.
float Draw(ImDrawList* dl, ImFont* font, const std::string& name, float x, float cy, float h,
           float alpha);
}  // namespace PS4Glyphs
