// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include <sys/stat.h>
#include <unistd.h>

extern "C" void ps4_boot_trace(const char* line);  // port/ps4_crashlog.cpp
extern "C" int sceKernelInternalMemoryGetAvailableSize(size_t* size);
extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
// port/ps4_runtime.cpp: the heap's use, the threads by stack size, libthr's default stack. (Weak: the PC
// harnesses that build this file have none of them.)
extern "C" __attribute__((weak)) void ps4rt_heap_usage(size_t* in_use, size_t* most, size_t* size);
extern "C" __attribute__((weak)) int ps4rt_thread_counts(char* text, size_t size);
extern "C" __attribute__((weak)) int ps4rt_thread_defaults(size_t* stack, size_t* guard);
extern "C" __attribute__((weak)) unsigned ps4rt_threads_made(void);
extern "C" __attribute__((weak)) void ps4_trace_sync(void);  // port/ps4_crashlog.cpp

namespace
{
constexpr size_t MIB = 1024 * 1024;

// The flexible memory readings (LogMemory, FlexibleFreeMiB): the most since the menu's first one, and a game's
// lowest (MemorySessionStart to LogMemorySession). Read on any thread.
std::atomic<bool> s_menu_seen{false};
std::atomic<size_t> s_most{0};
std::atomic<bool> s_session{false};
std::atomic<size_t> s_session_before{0}, s_session_low{0};
std::atomic<unsigned> s_session_threads{0};  // threads started before the game

size_t ReadFlexible()
{
  size_t flexible = 0;
  sceKernelAvailableFlexibleMemorySize(&flexible);
  if (s_menu_seen.load())
  {
    size_t most = s_most.load();
    while (flexible > most && !s_most.compare_exchange_weak(most, flexible))
    {
    }
  }
  if (s_session.load())
  {
    size_t low = s_session_low.load();
    while (flexible < low && !s_session_low.compare_exchange_weak(low, flexible))
    {
    }
  }
  return flexible;
}
}  // namespace

void LogLine(const std::string& text)
{
  // One line at a time: stderr is unbuffered, so lines written by several threads at once (the
  // N64 core's threads start together) came out mixed into one another.
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  fprintf(stderr, "%s\n", text.c_str());
  ps4_boot_trace(text.c_str());
}

// For C code (port/ps4_n64.c).
extern "C" void emusuni_log_line(const char* text)
{
  LogLine(text);
}

void LogF(const char* format, ...)
{
  char line[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  LogLine(line);
}

void LogMemory(const char* where)
{
  size_t internal = 0;
  sceKernelInternalMemoryGetAvailableSize(&internal);
  if (std::strcmp(where, "menu") == 0)
    s_menu_seen = true;
  const size_t flexible = ReadFlexible();
  // The heap (the 512 MiB of system flexible memory: port/ps4_runtime.cpp): what the program holds of it now
  // and at most so far. The threads alive by stack size and the most at once of each: their stacks are what
  // takes the regular flexible memory after the start.
  char heap[96] = "";
  if (ps4rt_heap_usage)
  {
    size_t in_use = 0, most = 0, size = 0;
    ps4rt_heap_usage(&in_use, &most, &size);
    snprintf(heap, sizeof(heap), "; heap %zu MiB in use of %zu (most %zu)", in_use / MIB, size / MIB, most / MIB);
  }
  char threads[480] = "";
  if (ps4rt_thread_counts)
  {
    char counts[440] = "";
    if (ps4rt_thread_counts(counts, sizeof(counts)) > 0)
      snprintf(threads, sizeof(threads), "; %s", counts);
  }
  LogF("memory (%s): internal %zu KiB free, flexible %zu MiB free (%zu KiB)%s%s", where, internal / 1024,
       flexible / MIB, flexible / 1024, heap, threads);
}

void LogThreadDefaults()
{
  size_t stack = 0, guard = 0;
  if (ps4rt_thread_defaults && ps4rt_thread_defaults(&stack, &guard) == 0)
    LogF("threads: default stack %zu KiB, guard %zu KiB", stack / 1024, guard / 1024);
  else
    LogLine("threads: default stack unknown");
}

void LogSync()
{
  fsync(STDERR_FILENO);
  if (ps4_trace_sync)
    ps4_trace_sync();
}

void RotateLogs(const std::string& base, int keep)
{
  for (int i = keep - 1; i >= 1; i--)
    std::rename((base + "-" + std::to_string(i) + ".log").c_str(), (base + "-" + std::to_string(i + 1) + ".log").c_str());
  if (keep >= 1)
    std::rename((base + ".log").c_str(), (base + "-1.log").c_str());
}

int FlexibleFreeMiB()
{
  return static_cast<int>(ReadFlexible() / MIB);
}

int FlexibleMostMiB()
{
  return static_cast<int>(s_most.load() / MIB);
}

void MemorySessionStart()
{
  s_session = false;
  const size_t flexible = ReadFlexible();
  s_session_before = flexible;
  s_session_low = flexible;
  s_session_threads = ps4rt_threads_made ? ps4rt_threads_made() : 0;
  s_session = true;
}

void LogMemorySession()
{
  if (!s_session.load())
    return;
  const size_t now = ReadFlexible();
  s_session = false;
  const size_t before = s_session_before.load() / MIB, low = s_session_low.load() / MIB;
  // (And the threads it started: every one alive at once takes a stack that stays.)
  char threads[64] = "";
  if (ps4rt_threads_made)
    snprintf(threads, sizeof(threads), "; %u threads started", ps4rt_threads_made() - s_session_threads.load());
  LogF("memory: the game took %zu MiB of flexible memory (%zu MiB free before it, %zu at the lowest, %zu now)%s",
       before > low ? before - low : 0, before, low, now / MIB, threads);
}

std::string ReadTextFile(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::string& path, const std::string& text)
{
  return WriteDataFile(path, text.data(), text.size());
}

bool WriteDataFile(const std::string& path, const void* data, size_t size)
{
  const std::string temp = path + ".new";
  FILE* file = fopen(temp.c_str(), "wb");
  if (!file)
    return false;
  const bool written = fwrite(data, 1, size, file) == size;
  fflush(file);
  fsync(fileno(file));
  fclose(file);
  return written && std::rename(temp.c_str(), path.c_str()) == 0;
}

bool FileExists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void MakeDirs(const std::string& path)
{
  for (size_t at = 1; at <= path.size(); at++)
  {
    if (at == path.size() || path[at] == '/')
      mkdir(path.substr(0, at).c_str(), 0777);
  }
}
