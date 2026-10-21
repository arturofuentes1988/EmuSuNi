// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// App-wide constants and the log: lines go to stderr (data_dir/emusuni.log) and the boot trace
// (port/ps4_crashlog.cpp, data_dir/xmb-trace.log), which survives a crash.

#pragma once

#include <string>

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
bool FileExists(const std::string& path);
void MakeDirs(const std::string& path);
