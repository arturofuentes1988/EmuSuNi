// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.27, a player's report from a PS5 11.40 with v00.24: "sometimes, after leaving the app, it won't open again until
// the console restarts") The session's own file, emusuni-session.ini next to the log: when it started, what it was
// doing and when last seen (the menu, a game; a heartbeat every minute), whether the PS4's own screen was over it, and
// whether it closed itself - EmuSuNi's own closes (Close EmuSuNi for an update, its restarts, the start watchdog) say
// so before they go (Closing). The PS4 closes an app by ending its process (PS button -> Close Application): nothing
// can write that one, so the next start's "session:" lines say how the session before was left - closed by itself,
// or not: last seen doing what, in front or behind the PS4's own screen - and when the console started (after it: the
// console restarted since). With the log's "system:" lines (main.cpp: the PS4's own screen over EmuSuNi and back), the
// end of the session before shows in emusuni-1.log. Nothing of the player's: times, the version, a game's key.
// Header only (the build's list of files stays); the words are made apart (Before), so the PC's test checks them.

#pragma once

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "Log.h"

namespace AppSession
{
namespace detail
{
struct Data
{
  std::mutex mutex;
  std::string path;  // "": not begun (nothing written)
  int64_t started = 0, seen = 0;
  std::string doing = "starting";
  bool front = true;
  std::string closed;
};
inline Data& Get()
{
  static Data data;
  return data;
}
// The file's path for the signal handler (no allocation there).
inline char g_signal_path[512] = {};

inline std::string Clock(int64_t time)
{
  if (time <= 0)
    return "?";
  const std::time_t t = static_cast<std::time_t>(time);
  std::tm local = {};
  localtime_r(&t, &local);
  char text[32] = {};
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  return text;
}

// The last "key=value" line's value ("" none).
inline std::string Value(const std::string& text, const std::string& key)
{
  std::string found;
  for (size_t at = 0; at < text.size();)
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
      found = line.substr(key.size() + 1);
  }
  return found;
}

// Under the lock.
inline void Write(Data& d)
{
  if (d.path.empty())
    return;
  std::string text = "# EmuSuNi: this session, for the next start's \"session:\" lines (src/AppSession.h)\nversion=" +
                     std::string(App::VERSION) + "\nstarted=" + std::to_string(static_cast<long long>(d.started)) +
                     "\nseen=" + std::to_string(static_cast<long long>(d.seen)) + "\ndoing=" + d.doing +
                     "\nfront=" + (d.front ? "yes" : "no") + "\n";
  if (!d.closed.empty())
    text += "closed=" + d.closed + "\n";
  WriteTextFile(d.path, text);
}

inline int64_t Now()
{
  return static_cast<int64_t>(std::time(nullptr));
}

// A signal the system may send before it ends the process: said in the log and the file, then its default (the end).
// Only what can be done in a signal handler (write, open, fsync).
inline void OnCloseSignal(int sig)
{
  char line[96] = "session: the system asked EmuSuNi to close (signal ";
  char number[8] = {};
  int n = sig, digits = 0;
  do
  {
    number[digits++] = static_cast<char>('0' + n % 10);
    n /= 10;
  } while (n > 0 && digits < 6);
  size_t at = std::strlen(line);
  while (digits > 0 && at < sizeof(line) - 4)
    line[at++] = number[--digits];
  line[at++] = ')';
  line[at++] = '\n';
  ssize_t ignored = write(STDERR_FILENO, line, at);
  fsync(STDERR_FILENO);
  if (g_signal_path[0])
  {
    const int fd = open(g_signal_path, O_WRONLY | O_APPEND);
    if (fd >= 0)
    {
      ignored = write(fd, "closed=a signal from the system\n", 32);
      fsync(fd);
      close(fd);
    }
  }
  (void)ignored;
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
}  // namespace detail

// The "session:" lines for the file the session before left (its text, "" none), with the console's start (Unix time;
// 0: not known).
inline std::vector<std::string> Before(const std::string& text, int64_t boot_time)
{
  using namespace detail;
  if (Value(text, "started").empty())
    return {"session: none before recorded here (a first start, the first of 01.27, or its folder lost it)"};
  const std::string version = Value(text, "version"), closed = Value(text, "closed"), doing = Value(text, "doing");
  const int64_t started = std::atoll(Value(text, "started").c_str()), seen = std::atoll(Value(text, "seen").c_str());
  const std::string head = "session: the one before (" + (version.empty() ? std::string("?") : version) +
                           ", started " + Clock(started);
  if (!closed.empty())
    return {head + ") closed itself: " + closed};
  std::vector<std::string> lines;
  lines.push_back(head + ", last seen " + Clock(seen) + ": " + (doing.empty() ? std::string("?") : doing) +
                  (Value(text, "front") == "no" ? ", behind the PS4's own screen" : "") +
                  ") didn't close itself: the PS4 closed it (PS button -> Close Application), or it crashed or stopped, "
                  "or the power went off");
  if (boot_time > 0)
  {
    lines.push_back("session: the console started " + Clock(boot_time) +
                    (boot_time > seen ? ": after the session before - the console restarted since (or rest mode)" :
                                        ": before the session before ended - the same power-on"));
  }
  return lines;
}

// At the start, once the log is where it stays: the session before's file read and said ("session:" lines), this
// one's written (path: emusuni-session.ini next to the log). The system's close signals caught for a line.
inline void Begin(const std::string& path, int64_t boot_time)
{
  for (const std::string& line : Before(ReadTextFile(path), boot_time))
    LogLine(line);
  detail::Data& d = detail::Get();
  {
    std::lock_guard lock(d.mutex);
    d.path = path;
    d.started = d.seen = detail::Now();
    d.doing = "starting";
    d.front = true;
    d.closed.clear();
    detail::Write(d);
  }
  if (path.size() < sizeof(detail::g_signal_path))
    std::memcpy(detail::g_signal_path, path.c_str(), path.size() + 1);
  // (Only a signal left as it comes - its default, the end: one the system set to be ignored stays so.)
  for (const int sig : {SIGTERM, SIGHUP, SIGINT})
  {
    struct sigaction old = {};
    if (sigaction(sig, nullptr, &old) == 0 && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL)
      std::signal(sig, detail::OnCloseSignal);
  }
}

// What the session does now: "the menu", "a game: snes/Super Mario World". Kept (a change only).
inline void Doing(const std::string& what)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  if (d.doing == what)
    return;
  d.doing = what;
  d.seen = detail::Now();
  detail::Write(d);
}

inline std::string DoingNow()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  return d.doing;
}

// The PS4's own screen over EmuSuNi (false) or EmuSuNi in front again (true).
inline void Front(bool front)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  d.front = front;
  d.seen = detail::Now();
  detail::Write(d);
}

// The heartbeat (every minute): the time last seen.
inline void Seen()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  d.seen = detail::Now();
  detail::Write(d);
}

// The same without allocating or locking (the start watchdog's restart after a crash: src/StartGuard.cpp): the line
// and the file's "closed=" appended. how: a literal.
inline void ClosingRaw(const char* how)
{
  const auto put = [](int fd, const char* a, const char* b, const char* c) {
    for (const char* part : {a, b, c})
    {
      ssize_t ignored = write(fd, part, std::strlen(part));
      (void)ignored;
    }
  };
  put(STDERR_FILENO, "session: EmuSuNi closes itself (", how, "): the session ends well\n");
  if (detail::g_signal_path[0])
  {
    const int fd = open(detail::g_signal_path, O_WRONLY | O_APPEND);
    if (fd >= 0)
    {
      put(fd, "closed=", how, "\n");
      fsync(fd);
      close(fd);
    }
  }
}

// EmuSuNi closes itself (how: "Close EmuSuNi for an update", "a restart for ..."): said, kept for the next start, the
// log through to the disk. Before sceSystemServiceLoadExec.
inline void Closing(const std::string& how)
{
  LogLine("session: EmuSuNi closes itself (" + how + "): the session ends well");
  {
    detail::Data& d = detail::Get();
    std::lock_guard lock(d.mutex);
    d.closed = how;
    d.seen = detail::Now();
    detail::Write(d);
  }
  LogSync();
}
}  // namespace AppSession
