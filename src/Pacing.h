// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Frame pacing with V-Sync (Emulator.cpp, N64): which emulated frames each TV refresh's flip
// shows. Once per pass of its loop the emulator gives the vblank that pass's flip will be shown at
// (VideoOut's count of vblanks + the flips already queued + 1) and runs the frames due by then:
//   - a game slower than the TV (PAL's 50 frames on 59.94 Hz) gets an even cadence: five frames,
//     then one shown again;
//   - a heavy frame whose time the queued flips covered is not "caught up" with two frames in the
//     next refresh (a jump on screen): 00.27's clock-based schedule did that after every frame
//     longer than 20 ms, and credited a short pass with half a refresh it didn't take (Mario
//     Kart 64 ran at 104-108% of its speed in the heavy scenes);
//   - far behind (a stall longer than the queued flips cover), the time is let go: the game waits
//     a moment instead of running 3+ frames in one refresh.
// Header-only (no PS4 calls): tested on a PC.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

class FramePacer
{
public:
  // Starts again at the next call (the game's start, after the in-game menu, a new speed).
  void Restart() { m_base = -1; }

  // The frames to run now for the flip VideoOut will show at vblank `land`. frames_per_refresh:
  // the game's frame rate (times its speed) over the TV's refresh rate (0.834 for PAL on 59.94 Hz,
  // 1 for NTSC). *let_go counts the frames skipped when far behind.
  int Frames(int64_t land, double frames_per_refresh, int64_t* let_go)
  {
    if (m_base < 0 || land < m_base || frames_per_refresh != m_ratio)
    {
      m_base = land;  // this refresh shows the next frame
      m_ratio = frames_per_refresh;
      m_done = 0;
    }
    // The frames that should exist by then (+0.5: the repeats fall in the middle of the cadence).
    const int64_t want = static_cast<int64_t>(std::floor(static_cast<double>(land - m_base) * m_ratio + 0.5)) + 1;
    int64_t owed = want - m_done;
    if (owed > MAX_PER_REFRESH)
    {
      if (let_go)
        *let_go += owed - 1;
      m_base = land;
      m_done = 0;
      owed = 1;
    }
    owed = std::max<int64_t>(owed, 0);
    m_done += owed;
    return static_cast<int>(owed);
  }

private:
  static constexpr int64_t MAX_PER_REFRESH = 2;
  int64_t m_base = -1;   // the vblank whose flip showed the first frame of this run
  double m_ratio = 0.0;  // frames per refresh of this run
  int64_t m_done = 0;    // frames run since m_base
};

// VideoOut's count of vblanks against the clock, once a second: a count that doesn't follow it
// (a VideoOut that lays its status out otherwise than shadPS4 and fpPS4 found) turns the pacing off
// for the rest of the game.
class VblankCheck
{
public:
  // vblanks: VideoOut's count now (-1: none); seconds: the clock now; hz: the TV's refresh rate.
  // False: don't pace by the count. *failed is set once, when the count stops being trusted.
  bool Ok(int64_t vblanks, double seconds, double hz, bool* failed = nullptr)
  {
    if (m_off || vblanks < 0)
      return false;
    if (m_vblanks < 0 || hz != m_hz || vblanks < m_vblanks)
    {
      m_vblanks = vblanks;
      m_seconds = seconds;
      m_hz = hz;
      return true;
    }
    const double elapsed = seconds - m_seconds;
    if (elapsed < 1.0)
      return true;
    m_counted = static_cast<double>(vblanks - m_vblanks);
    m_expected = elapsed * hz;
    if (std::abs(m_counted - m_expected) > 3.0)
    {
      m_off = true;
      if (failed)
        *failed = true;
      return false;
    }
    m_vblanks = vblanks;
    m_seconds = seconds;
    return true;
  }
  // The last check's numbers (for the log line when it failed).
  double Counted() const { return m_counted; }
  double Expected() const { return m_expected; }

private:
  bool m_off = false;
  int64_t m_vblanks = -1;
  double m_seconds = 0.0, m_hz = 0.0, m_counted = 0.0, m_expected = 0.0;
};
