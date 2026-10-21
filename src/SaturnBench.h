// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Saturn speed test's measuring (00.64; src/SaturnTest.cpp is its screen on the PS4). YabaSanshiro without a
// renderer (scripts/build-saturn-core.sh) runs one game from the BIOS, frame after frame as fast as it can - no
// picture, no sound out, no pad: the game plays its own demo -, and each frame is measured: the emulation thread's
// CPU time (the console's two SH-2s, the VDPs' emulation, the CD, the rest of retro_run: what the PS4 has to give
// one core), the SCSP thread's (the 68000 and the sound chip, on another core) and the wall time. The same code on
// the PS4 and on the PC (the PC check builds it with the core for the PC: scripts/build-saturn-core.sh's host
// mode), so their numbers compare frame for frame: the PS4's factor to the PC, and from it the verdict.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CoreApi;

namespace SaturnBench
{
struct Options
{
  std::string bios;          // the BIOS file (any name: scripts/build-saturn-core.sh's EMUSUNI_SATURN_BIOS)
  std::string system_dir;    // the core's system folder
  std::string save_dir;      // the core's save folder (its backup RAM: <save_dir>/yabasanshiro/backup.bin)
  int frames = 4800;         // frames to run from the BIOS (80 s of the console's time; FramesFor: a known game's)
  double max_seconds = 400;  // at most this long (wall clock)
  int window = 600;          // a log line every this many frames (10 s of the console's time; the PC's windows too)
  int scsp_sleep_us = 30;    // the SCSP thread's sleep when the SH-2s have no new cycles for it
  // After each frame, outside its measuring (the PC check loads and saves states there): the frames done so far.
  std::function<void(const CoreApi& core, int frame)> after_frame;
};

struct Result
{
  std::string path, name;  // the game's file, its name (the file's, without the extension)
  bool started = false;
  std::string error;       // why it didn't start
  std::string stopped;     // why it stopped before its frames ("" = all of them)
  double fps = 60.0;       // the core's frame rate (its frame's budget: 1000 / fps ms)
  double load_ms = 0;      // load_game (the console's start)
  double seconds = 0;      // wall time of the frames run
  // Per frame: the emulation thread's CPU and wall time of retro_run, the SCSP thread's CPU time, its sleeps and
  // their wall time, the sound frames the core gave.
  std::vector<float> cpu_ms, wall_ms, scsp_ms, sleep_ms;
  std::vector<uint32_t> sleeps, audio;
};

// The numbers of frames [first, last).
struct Stats
{
  int first = 0, last = 0;
  double cpu = 0, cpu_p95 = 0, cpu_max = 0;  // emulation thread CPU ms per frame
  double wall = 0, wait = 0;                 // retro_run's wall ms per frame; of it not on the CPU (the SCSP's waits)
  double scsp = 0;                           // SCSP thread CPU ms per frame
  double sleeps = 0, sleep_us = 0;           // SCSP sleeps per frame, the mean sleep's length
  double audio = 0;                          // sound frames per frame
  double speed = 0;                          // % of full speed (the frame's budget / wall), at most 100
};
Stats Measure(const Result& result, int first, int last);

// Which clock gives a thread's CPU time on this system, checked once (a sleep mustn't count, a busy loop must):
// false when none works (the wall clock is used then). *report: the log line's text.
bool CheckCpuClock(std::string* report);

// Runs a game from the BIOS: the frames of options, unless progress (called about every half second, between two
// frames: the PS4 draws its screen there) says to stop (false) or the time runs out. Logs a line for each window
// of frames and the result.
using Progress = std::function<bool(const Result& result, int frame)>;
Result Run(const CoreApi& core, const std::string& path, const Options& options, const Progress& progress);

// The frames to run for a game the PC measured (to the end of its own demo race: Daytona USA 4200, Sonic R 6600), by
// its file's name; otherwise for any other.
int FramesFor(const std::string& name, int otherwise);

// The lines that say what the results mean (the verdict), logged; returned for the screen too.
std::vector<std::string> Verdict(const std::vector<Result>& results, const Options& options);
}  // namespace SaturnBench
