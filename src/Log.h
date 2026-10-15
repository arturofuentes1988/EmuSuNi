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
// app) and the flexible memory still free, with where it was measured.
void LogMemory(const char* where);
void LogF(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Small file helpers shared by the menus.
std::string ReadTextFile(const std::string& path);
bool WriteTextFile(const std::string& path, const std::string& text);  // through a temporary file
bool FileExists(const std::string& path);
void MakeDirs(const std::string& path);
