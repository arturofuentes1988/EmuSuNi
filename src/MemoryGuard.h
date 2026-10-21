// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The memory check before a game starts (Emulator::Run). When the PS4's regular flexible memory runs out, the
// PS4 closes the app without a word (the blue error screen, no crash report: 00.58's God of War, started with 41
// MiB free). After the app's start only thread stacks take that memory: libthr (libkernel) maps a stack for
// every thread alive at once (2 MiB + a 16 KiB guard by default) and never gives one back, but keeps it for the
// next thread of exactly the same size. So a start takes its core's "cold" cost (its threads' stacks, measured
// on the PS4 from a fresh app) less the stacks earlier games left behind to reuse: the most memory free at a
// reading since the menu's first one, less what is free now, less 4 MiB of 1 MiB stacks the default-size
// threads can't use. With 16 MiB left to the system:
//
//   need = 16 + max(0, cold - reusable) + the 1 MiB stacks a core takes at its first start (the DS's 3)
//
// cold has 12 MiB more the first time the CPU upscaler runs (its threads have the default size: earlier games'
// stacks serve them as well). A start that needs more than is free is refused: the menu asks the player to close
// EmuSuNi and open it again (Message). Replayed on the 64 starts of the PS4 logs of 00.20-00.62: every start that
// ran still starts, and the one that died of it (that God of War, need 76) is refused.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>

namespace MemoryGuard
{
constexpr int MARGIN_MIB = 16;        // left to the system (and to the measurements' error)
constexpr int SMALL_STACKS_MIB = 4;   // 1 MiB stacks among the reusable ones (the DS's, the drawing thread's)
constexpr int UPSCALER_MIB = 12;      // the CPU upscaler's threads (src/Upscaler.cpp), the first time it runs
constexpr int REFUSE_ALL = 999;       // in data_dir/memory-guard: every start is refused (to see the message)

// The regular flexible memory a game's start takes from a fresh app, in MiB, by the console's key (System::key),
// as measured in the PS4 logs: the PSP 63 (God of War: 15 PPSSPP threads at its load, then a thread for each
// shader compile while it plays), the 3DS 18 (Hey! Pikmin from a fresh start, about 9 threads), the N64 12, the
// Dreamcast and its arcade boards 9, the PlayStation 8, the DS and the Game Boys 6, the others 3.
inline int ColdMiB(const std::string& console)
{
  if (console == "psp")
    return 63;
  if (console == "3ds")
    return 18;
  if (console == "n64")
    return 12;
  if (console == "dc" || console == "naomi" || console == "atomiswave")
    return 9;
  if (console == "ps1")
    return 8;
  if (console == "nds" || console == "gb" || console == "gbc" || console == "gba")
    return 6;
#ifdef EMUSUNI_SATURN
  // The Saturn's speed test (src/SaturnTest.cpp): YabaSanshiro without a renderer starts one thread, its SCSP's
  // (2 MiB + 16 KiB, kept for the next one): 3, as the lightest consoles.
  if (console == "saturn")
    return 3;
#endif
  return 3;
}

// The 1 MiB stacks a core's threads take at its first start: libthr hands them none of the 2 MiB ones the other
// games left (the DS's: Mario Kart DS 24 -> 21 MiB free with about 60 MiB of 2 MiB stacks kept).
inline int OwnStacksMiB(const std::string& console)
{
  return console == "nds" ? 3 : 0;
}

// The text of data_dir/memory-guard: a number added to every need (999: every start refused; a negative one lets
// more through). No file, or no number at its start: 0.
inline int ExtraFromText(const std::string& text)
{
  const char* start = text.c_str();
  char* end = nullptr;
  const long value = std::strtol(start, &end, 10);
  if (end == start)
    return 0;
  return static_cast<int>(std::clamp(value, -static_cast<long>(REFUSE_ALL), static_cast<long>(REFUSE_ALL)));
}

struct Decision
{
  int free_mib = 0;      // free now
  int most_mib = 0;      // the most free at a reading since the menu's first one (free now when that is more)
  int reusable_mib = 0;  // stacks of threads that ended, kept for the next threads
  int need_mib = 0;
  bool start = false;
};

// free: the flexible memory free now; most: the most free at a reading since the menu's first one (0: no reading
// yet); cold: the start's own cost (ColdMiB, and UPSCALER_MIB the first time the CPU upscaler runs); own:
// OwnStacksMiB at the core's first start, else 0; extra: ExtraFromText. All in MiB.
inline Decision Decide(int free_mib, int most_mib, int cold_mib, int own_mib, int extra_mib)
{
  Decision decision;
  decision.free_mib = free_mib;
  decision.most_mib = std::max(most_mib, free_mib);
  decision.reusable_mib = std::max(0, decision.most_mib - free_mib - SMALL_STACKS_MIB);
  decision.need_mib = MARGIN_MIB + std::max(0, cold_mib - decision.reusable_mib) + own_mib + extra_mib;
  decision.start = free_mib >= decision.need_mib;
  return decision;
}

// What the menu's "The game couldn't start" panel says then, one line each (translated line by line,
// assets/xmb/lang: every line fits the panel's 550 px in English and in Spanish).
inline std::string Message()
{
  return "EmuSuNi is running low on memory.\n"
         "Close EmuSuNi and open it again,\n"
         "then start this game again.\n"
         "\n"
         "To close it: PS button, OPTIONS,\n"
         "Close Application.";
}
}  // namespace MemoryGuard
