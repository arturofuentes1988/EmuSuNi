// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The DualShock 4's touchpad as a console's touch screen (the Nintendo DS's and the 3DS's): src/Emulator.cpp keeps one
// Stylus per game and updates it every frame from player 1's pad, the same way for both consoles.
//   Trackpad (DS / 3DS Touch Screen: Trackpad, the default): as a laptop's trackpad. Putting the finger down never moves
//   the stylus; moving it moves the stylus from where it is (the touchpad's height is about the screen's). A short tap -
//   the finger on the pad for at most a quarter of a second, moved less than about 2 mm - touches the screen where the
//   stylus is (down a few frames, then up). Held down, to drag: the Tap button (the touchpad's click, Controls) while it
//   is held, so click and move drags; or tap, then put the finger down again at once and move: down until it leaves.
//   Direct (the old Touch to Tap): the touchpad is the screen; the stylus goes where the finger lands and is down under it.
// The right stick moves the stylus too where the console has no use for it (the DS: Emulator.cpp).
#pragma once

#include <algorithm>
#include <cmath>

namespace Touchpad
{
// The DualShock 4's touchpad, in its own units (about 52 x 23 mm).
constexpr float PAD_W = 1920.0f, PAD_H = 942.0f;
// A tap: the finger on the pad at most TAP_FRAMES frames (60 a second: 250 ms) and never farther than TAP_MOVE units
// from where it came down (about 2 mm).
constexpr int TAP_FRAMES = 15;
constexpr float TAP_MOVE = 80.0f;
// A tap's touch: down CLICK_FRAMES frames (the games read the screen once a frame), then up at least GAP_FRAMES before
// the stylus goes down again (a double tap is two touches).
constexpr int CLICK_FRAMES = 4;
constexpr int GAP_FRAMES = 2;
// A tap, then the finger down again within DRAG_FRAMES frames: the stylus down while that finger stays (a drag).
constexpr int DRAG_FRAMES = 15;

struct Stylus
{
  float x = 0.0f, y = 0.0f;  // on the touch screen, in its pixels
  bool down = false;          // touching the screen this frame
  // The finger: on the pad last frame; where then and where it came down (pad units); frames since; moved farther than a
  // tap since; the Tap button pressed during this touch (its click is no tap too); this touch is a drag (after a tap).
  bool finger = false;
  float last_x = 0.0f, last_y = 0.0f, start_x = 0.0f, start_y = 0.0f;
  int frames = 0;
  bool moved = false, button = false, drag = false;
  int click = 0;          // frames a tap's touch still holds the stylus down
  int gap = 0;            // frames it stays up before it can go down again
  int since_tap = 1000;   // frames since the last tap
  int taps = 0;           // taps (the log's count)
};

// One frame. `finger`: on the pad, at fx, fy (pad units); `button`: the Tap button down; the touch screen's size in
// pixels; `direct`: DS / 3DS Touch Screen: Direct.
inline void Update(Stylus& s, bool finger, float fx, float fy, bool button, float screen_w, float screen_h, bool direct)
{
  const float max_x = screen_w - 1.0f, max_y = screen_h - 1.0f;
  if (direct)
  {
    if (finger)
    {
      s.x = std::clamp(fx / (PAD_W - 1.0f), 0.0f, 1.0f) * max_x;
      s.y = std::clamp(fy / (PAD_H - 1.0f), 0.0f, 1.0f) * max_y;
    }
    s.finger = finger;
    s.click = s.gap = 0;
    s.drag = false;
    s.down = button || finger;
    return;
  }
  if (s.since_tap < 1000)
    s.since_tap++;
  if (finger && !s.finger)
  {
    // Down: the stylus stays where it is.
    s.start_x = s.last_x = fx;
    s.start_y = s.last_y = fy;
    s.frames = 0;
    s.moved = false;
    s.button = button;
    s.drag = s.since_tap <= DRAG_FRAMES;
  }
  else if (finger)
  {
    // Moving: the stylus from where it is, the same distance across as down.
    const float gain = screen_h / PAD_H;
    s.x = std::clamp(s.x + (fx - s.last_x) * gain, 0.0f, max_x);
    s.y = std::clamp(s.y + (fy - s.last_y) * gain, 0.0f, max_y);
    s.last_x = fx;
    s.last_y = fy;
    s.frames++;
    s.button = s.button || button;
    if (std::hypot(fx - s.start_x, fy - s.start_y) > TAP_MOVE)
      s.moved = true;
  }
  else if (s.finger)
  {
    // Up: a tap when it was short and still, and neither the touchpad's click nor the end of a drag.
    if (!s.moved && !s.button && !s.drag && s.frames <= TAP_FRAMES)
    {
      s.click = CLICK_FRAMES;
      s.since_tap = 0;
      s.taps++;
    }
    s.drag = false;
  }
  s.finger = finger;
  // Down: the Tap button; a tap's touch; a drag's finger (once the tap's touch is over and the stylus was up a moment).
  bool touch = false;
  if (s.click > 0)
  {
    touch = true;
    if (--s.click == 0)
      s.gap = GAP_FRAMES;
  }
  else if (s.gap > 0)
  {
    s.gap--;
  }
  else if (s.drag && finger)
  {
    touch = true;
  }
  s.down = button || touch;
}

// The in-game menu opens: the finger and any touch in progress forgotten (a finger lifted while the menu was open is
// no tap after it), the stylus kept where it is.
inline void Forget(Stylus& s)
{
  s.finger = s.drag = s.down = false;
  s.click = s.gap = 0;
  s.since_tap = 1000;
}
}  // namespace Touchpad
