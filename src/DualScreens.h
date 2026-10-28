// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Nintendo DS's and the Nintendo 3DS's two screens on the TV: one way for both consoles (00.80, the owner: "dejar
// para ambas consolas las mismas opciones de configuración del tamaño y lugar de las pantallas"). Each core gives its
// two screens side by side, always the same picture - melonDS DS 512x192 (the top screen on the left, the touch screen
// on the right), Azahar 720x240 times its resolution (the 400-wide top screen on the left, the 320-wide touch screen on
// the right) - so the touchpad's place on the touch screen never changes; the app places them as the settings say,
// the same settings on both consoles (Settings.cpp: <console>_layout, _swap, _small_size, _small_position, _gap).
// Header-only: the PC tests use it as it is.

#pragma once

#include <algorithm>
#include <string>

namespace DualScreens
{
// A rectangle: x, y, width, height.
struct Rect
{
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// The settings a game's screens are shown with (Emulator.cpp reads them).
struct Options
{
  std::string layout = "stack";           // Screen Layout: stack, side, large, top, bottom
  bool swapped = false;                   // Swap Screens (or R3's swap in the game): the touch screen first
  int small_size = 3;                     // Small Screen Size: a half, a third or a quarter of the big one (2, 3, 4)
  std::string small_position = "right";  // Small Screen Position: right, right_bottom, right_top, left, left_bottom,
                                          // left_top, below, above
  std::string gap = "none";               // Gap Between Screens: none, thin, small, medium, large
};

// Gap Between Screens in sixteenths of a screen's height: none 0, thin 1, small 2, medium 4, large 8 (about the DS's
// own hinge: half a screen).
inline int GapSixteenths(const std::string& gap)
{
  return gap == "thin" ? 1 : gap == "small" ? 2 : gap == "medium" ? 4 : gap == "large" ? 8 : 0;
}

// A screen shown: which one, and its place in the picture.
struct Shown
{
  int screen = 0;  // 0: the top screen, 1: the touch screen
  Rect at;         // in the picture's units (the console's pixels of the screen shown at full size)
};

struct Layout
{
  float width = 0.0f, height = 0.0f;  // the whole picture (the screens and the gaps between them)
  int count = 0;                      // the screens shown: 1 or 2
  Shown shown[2];
  bool same_scale = true;             // every screen at its own size: one picture for the GPU's filters
  int big = 0;                        // Large Screen + Small: the big one's index in shown (the small one: 1 - big)
};

// The screens of top_w x top_h and bottom_w x bottom_h pixels (the DS's 256x192 both, the 3DS's 400x240 and 320x240)
// as the settings place them. One Above the Other: the first (the top screen, the touch screen swapped) above, each
// centred; Side by Side: the first on the left; Large Screen + Small: the first big, the other a half, a third or a
// quarter of its own size beside it (in the middle of a side, in a corner) or below or above it; one screen: Top Screen
// Only the top one (swapped: the touch screen), Touch Screen Only the touch one (swapped: the top one). The gap is a
// share of the screens' height.
inline Layout Arrange(const Options& options, float top_w, float top_h, float bottom_w, float bottom_h)
{
  Layout layout;
  const float widths[2] = {top_w, bottom_w}, heights[2] = {top_h, bottom_h};
  const int first = options.swapped ? 1 : 0, second = 1 - first;
  const float gap = static_cast<float>(GapSixteenths(options.gap)) * std::max(top_h, bottom_h) / 16.0f;
  if (options.layout == "top" || options.layout == "bottom")
  {
    const int screen = (options.layout == "bottom") != options.swapped ? 1 : 0;
    layout.count = 1;
    layout.shown[0] = {screen, {0.0f, 0.0f, widths[screen], heights[screen]}};
    layout.width = widths[screen];
    layout.height = heights[screen];
    return layout;
  }
  layout.count = 2;
  const float w1 = widths[first], h1 = heights[first];
  if (options.layout == "side")
  {
    const float w2 = widths[second], h2 = heights[second];
    layout.height = std::max(h1, h2);
    layout.width = w1 + gap + w2;
    layout.shown[0] = {first, {0.0f, (layout.height - h1) * 0.5f, w1, h1}};
    layout.shown[1] = {second, {w1 + gap, (layout.height - h2) * 0.5f, w2, h2}};
    return layout;
  }
  if (options.layout == "large")
  {
    const float k = static_cast<float>(std::clamp(options.small_size, 2, 4));
    const float w2 = widths[second] / k, h2 = heights[second] / k;
    const std::string& at = options.small_position;
    layout.same_scale = false;
    layout.big = 0;
    if (at == "below" || at == "above")
    {
      layout.width = std::max(w1, w2);
      layout.height = h1 + gap + h2;
      const bool above = at == "above";
      layout.shown[0] = {first, {(layout.width - w1) * 0.5f, above ? h2 + gap : 0.0f, w1, h1}};
      layout.shown[1] = {second, {(layout.width - w2) * 0.5f, above ? 0.0f : h1 + gap, w2, h2}};
      return layout;
    }
    layout.width = w1 + gap + w2;
    layout.height = std::max(h1, h2);
    const bool left = at == "left" || at == "left_top" || at == "left_bottom";
    const bool top = at == "right_top" || at == "left_top", bottom = at == "right_bottom" || at == "left_bottom";
    const float small_y = top ? 0.0f : bottom ? layout.height - h2 : (layout.height - h2) * 0.5f;
    layout.shown[0] = {first, {left ? w2 + gap : 0.0f, (layout.height - h1) * 0.5f, w1, h1}};
    layout.shown[1] = {second, {left ? 0.0f : w1 + gap, small_y, w2, h2}};
    return layout;
  }
  // One Above the Other (also any value not known: the default).
  const float w2 = widths[second], h2 = heights[second];
  layout.width = std::max(w1, w2);
  layout.height = h1 + gap + h2;
  layout.shown[0] = {first, {(layout.width - w1) * 0.5f, 0.0f, w1, h1}};
  layout.shown[1] = {second, {(layout.width - w2) * 0.5f, h1 + gap, w2, h2}};
  return layout;
}

// A place in the picture on the TV: the picture drawn from x0, y0 to x1, y1.
inline Rect OnScreen(const Rect& at, const Layout& layout, float x0, float y0, float x1, float y1)
{
  const float sx = (x1 - x0) / std::max(1.0f, layout.width), sy = (y1 - y0) / std::max(1.0f, layout.height);
  return {x0 + at.x * sx, y0 + at.y * sy, at.w * sx, at.h * sy};
}

// Where the two screens are in the core's picture (0: the top screen, 1: the touch screen), in its pixels: the DS's
// 512x192, the 3DS's 720x240 at `factor` times (1 to 4). False for a picture of another size (none is drawn as two).
inline bool InCorePicture(bool threeds, unsigned width, unsigned height, int factor, Rect screens[2])
{
  if (!threeds)
  {
    if (width != 512 || height != 192)
      return false;
    screens[0] = {0.0f, 0.0f, 256.0f, 192.0f};
    screens[1] = {256.0f, 0.0f, 256.0f, 192.0f};
    return true;
  }
  const unsigned f = static_cast<unsigned>(std::clamp(factor, 1, 4));
  if (width != 720 * f || height != 240 * f)
    return false;
  const float k = static_cast<float>(f);
  screens[0] = {0.0f, 0.0f, 400.0f * k, 240.0f * k};
  screens[1] = {400.0f * k, 0.0f, 320.0f * k, 240.0f * k};
  return true;
}
}  // namespace DualScreens
