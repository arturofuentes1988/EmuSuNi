// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// App-wide constants and the log: lines go to stderr (data_dir/emusuni.log) and the boot trace
// (port/ps4_crashlog.cpp, data_dir/xmb-trace.log), which survives a crash.

#pragma once

#include <string>
#include <vector>

namespace App
{
inline constexpr const char* NAME = "EmuSuNi";
inline constexpr const char* DATA_DIR = "/data/EmuSuNi";
inline constexpr const char* ASSETS_DIR = "/app0";
inline constexpr const char* VERSION = EMUSUNI_VERSION;
}  // namespace App

void LogLine(const std::string& text);
// libkernel's internal memory (threads, mutexes and memory mappings use it; running out kills the
// app) and the flexible memory still free, with where it was measured; then the heap's use and the
// threads alive by stack size (port/ps4_runtime.cpp). "menu" starts FlexibleMostMiB's readings.
void LogMemory(const char* where);
void LogF(const char* format, ...) __attribute__((format(printf, 1, 2)));
// At the start: the stack and guard a thread gets when its creator doesn't say (libthr's default).
void LogThreadDefaults();
// The log and the trace written through to the disk (fsync): a hang, a forced close or the power going off keeps
// them (src/StartGuard.cpp, every 2 s while a game starts). Any thread.
void LogSync();
// The logs of the sessions before (00.70): <base>.log becomes <base>-1.log, <base>-1.log becomes <base>-2.log, ...
// up to <base>-<keep>.log (the oldest goes). Before <base>.log is opened.
void RotateLogs(const std::string& base, int keep);
// (01.25, the owner: "más compatible con ps5" - players on a PS5 saw /data/EmuSuNi never made, or its folders lost at
// every start) The data folder at the start, as the log's "data:" lines: whether it is there (else mkdir's error),
// whether a file can be made, written, renamed and deleted in it (else the step and its error), where /data and it
// lead when they are links, and its starts' count (data-check.ini, one more each start: none before this one again and
// again - the folder emptied between starts - shows here). Nothing of the player's.
// (01.27) *found: whether the file was written, and the count before this start - the start's choice of where the
// player's data lives (src/Portable.h's Local).
struct DataDirCheck
{
  bool writable = false;
  long starts_before = 0;  // (0: none recorded)
};
std::vector<std::string> CheckDataDir(const std::string& data_dir, DataDirCheck* found = nullptr);

// The regular flexible memory free now, in MiB (a reading, as LogMemory's: see FlexibleMostMiB).
int FlexibleFreeMiB();
// The most flexible memory free at a reading since the menu's first one, in MiB (0 before it). Thread
// stacks are never given back, so most of what that has above the memory free now is stacks of threads
// that ended, kept for the next ones (src/MemoryGuard.h).
int FlexibleMostMiB();
// A game's memory, from its memory check before it starts to its end: LogMemorySession says how much
// flexible memory it took (what was free before it, at its lowest reading, at its end).
void MemorySessionStart();
void LogMemorySession();

// Small file helpers shared by the menus.
std::string ReadTextFile(const std::string& path);
bool WriteTextFile(const std::string& path, const std::string& text);  // through a temporary file
bool WriteDataFile(const std::string& path, const void* data, size_t size);  // the same, from memory
// (00.87) Where a write of WriteTextFile / WriteDataFile that failed is made instead ("" none): set once at the start
// (main.cpp: src/Portable.h's Redirect, the player's data on a USB drive taken out). None: a failed write fails.
using WriteRedirect = std::string (*)(const std::string& failed_path);
void SetWriteRedirect(WriteRedirect redirect);
bool FileExists(const std::string& path);
void MakeDirs(const std::string& path);
