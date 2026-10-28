// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The menus' moves through a list (00.76, the owner: "que todos los menús se puedan pasar del primero al último
// apretando flecha para arriba"): Up on the first item is the last one, Down on the last the first - on a fresh
// press. A button held repeats (HeldRepeat), and a repeat stops at the end, once: holding the arrow never goes round
// and round by itself; the next press goes on round. Used by the main menu (src/Menu.cpp: its lists, panels, games,
// tabs, the search keyboard) and the in-game menu (src/IngameMenu.cpp); the unit tests run them as they are.

#pragma once

#include <algorithm>
#include <cstdint>

namespace MenuNav
{
// The row `direction` (-1 up, +1 down) from `from` in a list of `count` rows, past the rows `selectable` refuses
// (headers). Past an end: with `wrap` (a fresh press) round to the other end, else `from` stays (a held repeat
// stops at the end). 0: `from` itself, or the nearest selectable row below it, else above it. `from` may be out of
// the list (it grew shorter): the move starts from its end.
template <typename Selectable>
int Step(int from, int direction, int count, bool wrap, Selectable selectable)
{
  if (count <= 0)
    return from;
  if (direction == 0)
  {
    const int start = std::clamp(from, 0, count - 1);
    for (int row = start; row < count; row++)
    {
      if (selectable(row))
        return row;
    }
    for (int row = start - 1; row >= 0; row--)
    {
      if (selectable(row))
        return row;
    }
    return from;
  }
  const int step = direction < 0 ? -1 : 1;
  int row = std::clamp(from, -1, count) + step;
  for (int seen = 0; seen < count; seen++)
  {
    if (row < 0 || row >= count)
    {
      if (!wrap)
        return from >= 0 && from < count ? from : Step(from, 0, count, false, selectable);
      row = step > 0 ? 0 : count - 1;
    }
    if (selectable(row))
      return row;
    row += step;
  }
  return from;
}

inline int Step(int from, int direction, int count, bool wrap)
{
  return Step(from, direction, count, wrap, [](int) { return true; });
}

// A jump of `delta` rows (a page: the XMB's L1 / R1, the Library's Up / Down in a row of games): no further than an
// end; from the end itself, with `wrap` (a fresh press), the other end.
inline int Jump(int from, int delta, int count, bool wrap)
{
  if (count <= 0 || delta == 0)
    return from;
  const int last = count - 1;
  if (wrap && delta > 0 && from >= last)
    return 0;
  if (wrap && delta < 0 && from <= 0)
    return last;
  return std::clamp(from + delta, 0, last);
}

// Buttons held repeat: the same ones held since the pass before count again after `delay`, then every `period`
// (the first press is the caller's: held and not held before). Update returns the buttons repeated this pass.
class HeldRepeat
{
public:
  HeldRepeat(uint32_t mask, float delay, float period) : m_mask(mask), m_delay(delay), m_period(period), m_left(delay)
  {
  }
  uint32_t Update(uint32_t held, uint32_t before, float dt)
  {
    const uint32_t now = held & m_mask;
    if (now != 0 && now == (before & m_mask))
    {
      m_left -= dt;
      if (m_left <= 0.0f)
      {
        m_left = m_period;
        return now;
      }
      return 0;
    }
    m_left = m_delay;
    return 0;
  }

private:
  uint32_t m_mask;
  float m_delay, m_period, m_left;
};

// The main menu's: the directions (D-pad or left stick) after 0.35 s every 0.075 s, as before 00.76; L1 / R1 (the
// Library's console tabs, the XMB's five items, the search keyboard's games) after 0.4 s every 0.18 s - about five
// tabs a second, each name readable as it passes.
constexpr float DIRECTION_DELAY = 0.35f, DIRECTION_PERIOD = 0.075f;
constexpr float SHOULDER_DELAY = 0.40f, SHOULDER_PERIOD = 0.18f;
// After a console tab (or an XMB column) changes, its covers and a look at the drives wait until no other change has
// come for this long: holding L1 / R1 through the tabs loads nothing on the way.
constexpr float TAB_SETTLE = 0.22f;
}  // namespace MenuNav
