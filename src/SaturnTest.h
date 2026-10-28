// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Saturn speed test (00.64; builds with EMUSUNI_SATURN only: scripts/build.sh with-saturn). Players see nothing
// of it: only with the file /data/EmuSuNi/saturn-test does Settings have a row "Saturn (speed test)". It runs every
// Saturn game in a drive's Roms/saturn folder (or /data/EmuSuNi/Roms/saturn) with the owner's BIOS, with no picture
// - YabaSanshiro is built without a renderer for it (scripts/build-saturn-core.sh) - and logs what the PS4's CPU takes
// for each frame (src/SaturnBench.cpp), with the verdict: whether a Saturn for the PS4 is worth building (Daytona USA
// at 20 ms a frame or less), only for light games (20-30 ms), or not (more). A "measuring" screen shows how far it
// is (Circle stops it), the results at the end; then the menu again.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Library.h"

class Display;
struct ImFont;

namespace SaturnTest
{
// The Saturn's BIOS (the test's and the console's, src/Emulator.cpp): a file of 512 KiB by a name the core knows
// (saturn_bios.bin, mpr-17933.bin, sega_101.bin) or any such .bin with "bios" in its name, in these folders; a USA or
// Europe one first. "" when there is none; crc: its CRC-32. The name of a known one by its CRC-32.
std::string FindSaturnBios(const std::vector<std::string>& folders, uint32_t* crc);
std::string SaturnBiosName(uint32_t crc);
// Does Settings show the test (the file /data/EmuSuNi/saturn-test is there)? Its lines may set the test as well:
// "frames=N" (frames from the BIOS, 4800), "seconds=N" (at most, each game: 300), "sleep_us=N" (the SCSP thread's
// sleep, 30).
bool Enabled();
// The menu's stand-in for it: main.cpp runs Run for it instead of a game.
Library::Game Entry();
bool IsEntry(const Library::Game& game);
// The test, then back to the menu.
void Run(Display& display, ImFont* regular, ImFont* bold);
}  // namespace SaturnTest
