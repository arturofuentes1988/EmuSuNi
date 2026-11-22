// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include <fcntl.h>
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

// (01.67, the master: a game that hung wrote an emusuni-1.log of 18.6 GB on a player's PS4) This session's log stops at
// 64 MiB: its last line says so, and stdout and stderr go to /dev/null until EmuSuNi starts again - the cores' own
// lines too (they write to the same file: its size, the file's position, is looked at every 64 lines).
constexpr off_t LOG_MOST = static_cast<off_t>(64 * MIB);
std::atomic<bool> s_log_full{false};
unsigned s_log_lines = 0;  // (under LogLine's lock)

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
  if (!s_log_full.load())
  {
    fprintf(stderr, "%s\n", text.c_str());
    if (++s_log_lines % 64 == 0 && lseek(STDERR_FILENO, 0, SEEK_CUR) >= LOG_MOST)
    {
      fprintf(stderr, "log: this log reached %zu MB: nothing more is written to it until EmuSuNi starts again\n",
              static_cast<size_t>(LOG_MOST) / MIB);
      s_log_full = true;
      const int null = open("/dev/null", O_WRONLY);
      if (null >= 0)
      {
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        close(null);
      }
    }
  }
  ps4_boot_trace(text.c_str());  // (the trace's own limit: port/ps4_crashlog.cpp)
}

// For C code (port/ps4_n64.c).
extern "C" void emusuni_log_line(const char* text)
{
  LogLine(text);
}

void LogF(const char* format, ...)
{
  // (01.69) 2304: a core's line of up to 2048 (Emulator.cpp's CoreLog) with its name before it; at 1024 the ends of
  // the cores' long 600-frame lines were lost.
  char line[2304];
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

namespace
{
std::atomic<WriteRedirect> s_write_redirect{nullptr};

bool WriteDataFileAt(const std::string& path, const void* data, size_t size)
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
}  // namespace

void SetWriteRedirect(WriteRedirect redirect)
{
  s_write_redirect.store(redirect);
}

bool WriteDataFile(const std::string& path, const void* data, size_t size)
{
  if (WriteDataFileAt(path, data, size))
    return true;
  // (00.87) A write that failed: the redirect's other place, when it has one (src/Portable.h: the player's data on a
  // USB drive that was taken out goes to /data/EmuSuNi/portable-fallback/ instead).
  const WriteRedirect redirect = s_write_redirect.load();
  const std::string other = redirect ? redirect(path) : std::string();
  return !other.empty() && WriteDataFileAt(other, data, size);
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

std::vector<std::string> CheckDataDir(const std::string& data_dir, DataDirCheck* found)
{
  std::vector<std::string> lines;
  const auto error = [](const char* step) { return std::string(step) + ": " + std::strerror(errno); };
  struct stat st = {};
  if (stat(data_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
  {
    const std::string why = mkdir(data_dir.c_str(), 0777) == 0 ? std::string("made now") : error("mkdir");
    lines.push_back(data_dir + " wasn't there (" + why + ")");
  }
  // Where /data and the folder lead when they are links (a PS5's /data may be another place than a PS4's).
  for (const std::string& path : {std::string("/data"), data_dir})
  {
    char target[256] = {};
    const ssize_t got = readlink(path.c_str(), target, sizeof(target) - 1);
    if (got > 0)
      lines.push_back(path + " leads to " + std::string(target, static_cast<size_t>(got)));
  }
  // A file made, written, renamed, read and deleted, as the saves and the menu's settings are written.
  const std::string test = data_dir + "/.write-check.new", done = data_dir + "/.write-check";
  std::string write = "writable (a file made, written, renamed, read and deleted)";
  FILE* file = std::fopen(test.c_str(), "wb");
  if (!file)
    write = error("can't make a file");
  else
  {
    const bool wrote = std::fwrite("ok\n", 1, 3, file) == 3 && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    const std::string why = wrote ? std::string() : error("can't write a file");
    std::fclose(file);
    if (!wrote)
      write = why;
    else if (std::rename(test.c_str(), done.c_str()) != 0)
      write = error("can't rename a file");
    else if (ReadTextFile(done) != "ok\n")
      write = "a file written reads back otherwise";
    if (std::remove(done.c_str()) != 0 && std::remove(test.c_str()) != 0 && write.compare(0, 8, "writable") == 0)
      write = error("can't delete a file");
  }
  lines.push_back(data_dir + ": " + write);
  // The starts' count: one more each start, kept in the folder.
  const std::string check = data_dir + "/data-check.ini", text = ReadTextFile(check);
  long starts = 0;
  std::string since;
  for (size_t at = 0; at < text.size();)
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    if (line.compare(0, 7, "starts=") == 0)
      starts = std::max(0L, std::atol(line.c_str() + 7));
    else if (line.compare(0, 6, "since=") == 0)
      since = line.substr(6, 10);
    at = end + 1;
  }
  if (since.empty())
  {
    const std::time_t now = std::time(nullptr);
    char day[16] = {};
    std::strftime(day, sizeof(day), "%Y-%m-%d", std::gmtime(&now));
    since = day;
  }
  lines.push_back(starts > 0 ? data_dir + " has seen " + std::to_string(starts) + " start(s) before this one (since " +
                                   since + ")" :
                               "no start before this one recorded in " + data_dir + " (a first start, the first of " +
                                   "01.25, or the folder was emptied: again at the next start - it doesn't keep files)");
  const std::string kept = "# EmuSuNi: this folder's starts (the log's \"data:\" lines)\nstarts=" +
                           std::to_string(starts + 1) + "\nsince=" + since + "\n";
  if (!WriteTextFile(check, kept))
    lines.push_back(error("data-check.ini can't be written"));
  if (found)
  {
    found->writable = write.compare(0, 8, "writable") == 0;
    found->starts_before = starts;
  }
  return lines;
}
