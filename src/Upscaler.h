// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Resolution (Settings): the game's picture made bigger before it is fitted to the TV, with
// Snes9x's xBRZ or HQx filters (rounded, "HD" edges) or plain bigger pixels. The work is split
// into slices of rows that run on a few worker threads at once.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Upscaler
{
enum class Filter
{
  XBRZ,
  HQx,
  Pixels,
};

// The worker threads start with the first picture made bigger; a game's end stops them.
void Stop();

// xBRZ builds a 64 MiB table the first time it is used (about a second on the PS4), on a thread
// of its own: until then the picture stays at its own resolution. True when `filter` can run now.
bool Ready(Filter filter);

// `src` (RGB565, `pitch` bytes per row) `factor` times bigger into `out` (RGB565, width * factor
// pixels per row, height * factor rows). HQx goes up to 4x, the others to 6x. Returns the factor
// used (1: nothing was done).
int Scale(Filter filter, int factor, const uint16_t* src, size_t pitch, int width, int height,
          std::vector<uint16_t>* out);
}  // namespace Upscaler
