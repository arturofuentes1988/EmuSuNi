// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The DualShock 4's touchpad as a console's touch screen (the Nintendo DS's and the 3DS's): src/Emulator.cpp keeps one
// Stylus per game and updates it every frame from player 1's pad, the same way for both consoles.
//   Trackpad (DS / 3DS Touch Screen: Trackpad, the default): as a laptop's trackpad. Putting the finger down never moves
//   the stylus; moving it moves the stylus from where it is (the touchpad's height is about the screen's). A short tap -
//   the finger on the pad for at most a quarter of a second, moved less than about 2 mm - touches the screen where the
//   stylus is (down a few frames, then up). Held down, to drag: the Tap button (the touchpad's click, Controls) while it
//   is held, so click and move drags; or tap, then put the finger down again at once and move: down until it leaves.
//   Direct (the old Touch to Tap): the touchpad is the screen; the stylus goes where the finger lands and is down under it.
// The finger is one of the pad's touches, followed by its number (Frame, Pick): a touch resting on the pad is none.
// The right stick moves the stylus too where the console has no use for it (the DS; the 3DS with 3DS Right Stick:
// Stylus: Emulator.cpp).
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
// (01.15) A touch resting on the pad is no finger until it moves farther than TAP_MOVE or leaves: one there when the
// game started or the in-game menu closed, or one still for REST_FRAMES (10 s) - a thumb resting there, a holder's
// clip, a worn pad. In the owner's 01.04, 01.05 and 01.14 a touch stayed on the pad all the time and the stylus never
// moved: only the pad's first touch was read, never the finger beside it.
constexpr int REST_FRAMES = 600;

// One of the pad's touches (up to two), as the controller gives it: where (pad units) and its number, the same while
// that finger stays on the pad.
struct Touch
{
  float x = 0.0f, y = 0.0f;
  int id = -1;
};

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
  // The touches on the pad last frame: their numbers, where each last moved (pad units), frames since, resting.
  struct Seen
  {
    int id = -1;
    float x = 0.0f, y = 0.0f;
    int still = 0;
    bool resting = false;
  };
  Seen seen[2];
  bool fresh = true;  // no frame yet since the game started or the in-game menu closed (Forget)
  int id = -1;        // the touch followed as the finger
  int rested = 0;     // frames with a touch left alone as resting (the log's count), and where the last one was
  float rest_x = 0.0f, rest_y = 0.0f;
};

// The finger among this frame's touches (`count`, at most two): the one followed, else one that isn't resting; nullptr
// when there is none.
inline const Touch* Pick(Stylus& s, const Touch* touches, int count)
{
  count = std::clamp(count, 0, 2);
  Stylus::Seen now[2];
  const Touch* pick = nullptr;
  bool rested = false;
  for (int i = 0; i < count; i++)
  {
    const Touch& t = touches[i];
    Stylus::Seen& n = now[i];
    n = {t.id, t.x, t.y, 0, s.fresh};
    for (const Stylus::Seen& was : s.seen)
    {
      if (was.id == t.id)
      {
        n = was;
        n.still++;
        break;
      }
    }
    if (std::hypot(t.x - n.x, t.y - n.y) > TAP_MOVE)
    {
      n.x = t.x;
      n.y = t.y;
      n.still = 0;
      n.resting = false;
    }
    else if (n.still >= REST_FRAMES)
    {
      n.resting = true;
    }
    if (n.resting)
    {
      rested = true;
      s.rest_x = t.x;
      s.rest_y = t.y;
    }
    else if (!pick || t.id == s.id)
    {
      pick = &t;
    }
  }
  s.seen[0] = now[0];
  s.seen[1] = now[1];
  s.fresh = false;
  if (rested)
    s.rested++;
  return pick;
}

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

// One frame from the pad's touches: the finger (Pick), then the stylus (Update). True: a finger on the pad.
inline bool Frame(Stylus& s, const Touch* touches, int count, bool button, float screen_w, float screen_h, bool direct)
{
  const Touch* finger = Pick(s, touches, count);
  // Another finger took over while one was down: put down where it is (no tap of the one that left, no jump).
  if (finger && s.finger && finger->id != s.id)
    s.finger = false;
  s.id = finger ? finger->id : -1;
  Update(s, finger != nullptr, finger ? finger->x : 0.0f, finger ? finger->y : 0.0f, button, screen_w, screen_h,
         direct);
  return finger != nullptr;
}

// The in-game menu opens: the finger and any touch in progress forgotten (a finger lifted while the menu was open is
// no tap after it; one on the pad as it closes is resting until it moves), the stylus kept where it is.
inline void Forget(Stylus& s)
{
  s.finger = s.drag = s.down = false;
  s.click = s.gap = 0;
  s.since_tap = 1000;
  s.seen[0] = s.seen[1] = {};
  s.fresh = true;
  s.id = -1;
}
}  // namespace Touchpad
