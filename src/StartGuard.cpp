// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "StartGuard.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include "Log.h"

#ifdef __ORBIS__
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
extern "C" int startguard_set_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");
#else
// (The PC tests define it: they look at what would have been restarted.)
extern "C" int sceSystemServiceLoadExec(const char* path, const char* args[]);
#endif
extern "C" void ps4_boot_trace(const char* line);  // port/ps4_crashlog.cpp

namespace StartGuard
{
namespace
{
using Clock = std::chrono::steady_clock;

enum Phase : int
{
  IDLE = 0,
  STARTING = 1,  // from Begin to the first picture shown
  SETTLING = 2,  // the first seconds after it (a stop or a crash there is the start's too)
  ENDING = 3,    // the game's end path
};
constexpr float SETTLE_SECONDS = 15.0f;

int64_t NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

double Seconds(int64_t ns)
{
  return static_cast<double>(ns) / 1e9;
}

void SleepUs(unsigned us)
{
#ifdef __ORBIS__
  sceKernelUsleep(us);
#else
  usleep(us);
#endif
}

// ---- Shared with the watchdog's thread and the crash handler (atomics) ----
std::atomic<int> s_phase{IDLE};
std::atomic<int> s_attempt{1};
std::atomic<int64_t> s_begin_ns{0}, s_beat_ns{0};
std::atomic<const char*> s_step{""};
std::atomic<float> s_limit{45.0f};  // the seconds the current step may take without a heartbeat
std::atomic<int64_t> s_pictures{0};
std::atomic<int> s_crash{0};  // the signal of a crash while a game starts or ends
std::atomic<bool> s_thread{false};  // the watchdog's thread runs
// (Read in the crash handler: initial-exec, as port/ps4_runtime.cpp's - no lookup that could allocate.)
__attribute__((tls_model("initial-exec"))) thread_local bool t_watchdog = false;
// The game under way (the watchdog's thread reads them under the lock; only the main thread writes them).
std::mutex s_mutex;
std::string s_path, s_title, s_console;

// ---- The main thread's ----
std::string s_dir;  // data_dir ("": no files: a program that didn't call AppStarted)
Limits s_limits;
int64_t s_pictures_at_beat = 0;
int64_t s_last_pass_ns = 0;
double s_run = 0.0;   // seconds the game ran (the in-game menu's time left out) without a picture
double s_note = 10.0;  // the next "no picture yet" line
double s_settle = 0.0;
bool s_first_frame = false;
std::string s_toast, s_fail;
bool s_skip_continue = false;
// A start again waiting for its game (after Retry, or EmuSuNi restarted for it).
struct Pending
{
  std::string path, toast;
  bool skip_continue = false;
};
Pending s_pending;

std::string MarkerPath()
{
  return s_dir + "/start.txt";
}

std::string AgainPath()
{
  return s_dir + "/start-again.txt";
}

// A file of "key=value" lines (none: empty).
std::map<std::string, std::string> ReadKeys(const std::string& path)
{
  std::map<std::string, std::string> keys;
  const std::string text = ReadTextFile(path);
  for (size_t at = 0; at < text.size();)
  {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(at, end - at);
    const size_t equal = line.find('=');
    if (equal != std::string::npos)
      keys[line.substr(0, equal)] = line.substr(equal + 1);
    at = end + 1;
  }
  return keys;
}

std::string Key(const std::map<std::string, std::string>& keys, const char* key)
{
  const auto it = keys.find(key);
  return it == keys.end() ? std::string() : it->second;
}

// No line breaks in a file's value.
std::string OneLine(std::string text)
{
  for (char& c : text)
  {
    if (c == '\n' || c == '\r')
      c = ' ';
  }
  return text;
}

std::string LocalTime()
{
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_r(&now, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  return text;
}

// The watchdog's own lines: written straight to the log and the trace (not through LogLine's lock, which the stopped
// or crashed thread may hold).
void RawLog(const char* text)
{
  const size_t length = std::strlen(text);
  if (write(STDERR_FILENO, text, length) < 0 || write(STDERR_FILENO, "\n", 1) < 0)
  {
  }
  ps4_boot_trace(text);
}

// A small file, written and synced to the disk (open/write/fsync: no heap).
void WriteNow(const std::string& path, const std::string& text)
{
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return;
  if (write(fd, text.data(), text.size()) < 0)
  {
  }
  fsync(fd);
  close(fd);
}

const char* const MESSAGE_TAIL = "\n\nThe game's file may be damaged,\nor the emulator can't start it.";

// The watchdog's way out: start-again.txt for the next process, then EmuSuNi restarted. Doesn't come back.
[[noreturn]] void Restart(int phase, int crash, double stopped)
{
  std::string path, title, console;
  for (int i = 0; i < 100; i++)
  {
    // (The stopped thread may hold the lock - only for a few copies: tried for a second.)
    if (s_mutex.try_lock())
    {
      path = s_path;
      title = s_title;
      console = s_console;
      s_mutex.unlock();
      break;
    }
    SleepUs(10000);
  }
  const char* step = s_step.load();
  const int attempt = s_attempt.load();
  const std::string why = crash != 0 ? "crash" : "stuck";
  char what[256];
  if (crash != 0)
    std::snprintf(what, sizeof(what), "a crash (signal %d) while \"%s\"", crash, step);
  else
    std::snprintf(what, sizeof(what), "the emulation thread stopped %.0f s in \"%s\"", stopped, step);
  char line[1024];
  std::string again = "step=" + OneLine(step) + "\nseconds=" + std::to_string(static_cast<int>(stopped)) + "\n";
  if (phase == ENDING)
  {
    std::snprintf(line, sizeof(line), "start watchdog: %s as %s ended: EmuSuNi restarts into the menu", what,
                  title.c_str());
    again += "why=end-" + why + "\ntitle=" + OneLine(title) + "\n";
  }
  else if (attempt <= 1)
  {
    std::snprintf(line, sizeof(line),
                  "start watchdog: %s while %s started: EmuSuNi restarts and starts it again at once (attempt 2)",
                  what, title.c_str());
    again += "why=" + why + "\ngame=" + OneLine(path) + "\ntitle=" + OneLine(title) + "\nconsole=" + console + "\n";
  }
  else
  {
    std::snprintf(line, sizeof(line),
                  "start watchdog: %s while %s started, a second time: EmuSuNi restarts into the menu, which says so",
                  what, title.c_str());
    again += "why=" + why + "-again\ntitle=" + OneLine(title) + "\n";
  }
  RawLog(line);
  if (!s_dir.empty())
    WriteNow(AgainPath(), again);
  LogSync();
  const int result = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
  std::snprintf(line, sizeof(line), "start watchdog: restart asked (%#x)", static_cast<unsigned>(result));
  RawLog(line);
  LogSync();
  for (int i = 0; i < 50; i++)
    SleepUs(100000);
  // Still here after 5 s: the restart didn't happen. Closed instead (start-again.txt waits for the next start).
  RawLog("start watchdog: EmuSuNi wasn't restarted: it closes (its next start reads start-again.txt)");
  LogSync();
  sceSystemServiceLoadExec("exit", nullptr);
  _exit(1);
}

void* WatchdogMain(void*)
{
  t_watchdog = true;
#ifdef __ORBIS__
  // Above every thread of the app (it sleeps nearly all the time), on cores 0, 1, 4 and 5: never in the way of the
  // emulation thread (core 2) or Flycast's and the DS's (core 3).
  const int priority = scePthreadSetprio(scePthreadSelf(), 350);
  const int affinity = startguard_set_affinity(scePthreadSelf(), 0x33);
  LogF("start watchdog: ready (priority 350 %#x, cores 0x33 %#x)", static_cast<unsigned>(priority),
       static_cast<unsigned>(affinity));
#else
  LogLine("start watchdog: ready");
#endif
  int64_t last_sync = 0;
  for (;;)
  {
    SleepUs(250000);
    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == IDLE)
      continue;
    const int64_t now = NowNs();
    // The log through to the disk every 2 s while a game starts (until its first picture) or ends. (Not once it plays:
    // a write of the game's threads to the log could wait for the sync.)
    if (phase != SETTLING && now - last_sync >= 2000000000LL)
    {
      LogSync();
      last_sync = now;
    }
    const int crash = s_crash.load();
    const double stopped = Seconds(now - s_beat_ns.load());
    if (crash != 0 || stopped > s_limit.load())
      Restart(phase, crash, stopped);
  }
}

void StartThread()
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_t thread;
  const int result = pthread_create(&thread, &attr, WatchdogMain, nullptr);
  pthread_attr_destroy(&attr);
  if (result == 0)
  {
    pthread_detach(thread);
    s_thread = true;
  }
  else
  {
    LogF("start watchdog: its thread couldn't start (%d)", result);
  }
}
}  // namespace

void AppStarted(const std::string& data_dir, std::string* game_path, std::string* message)
{
  s_dir = data_dir;
  game_path->clear();
  message->clear();
  const auto marker = ReadKeys(MarkerPath());
  const auto again = ReadKeys(AgainPath());
  std::remove(MarkerPath().c_str());
  std::remove(AgainPath().c_str());
  const std::string why = Key(again, "why");
  if (!marker.empty())
  {
    LogF("start: the last start didn't finish: %s (%s, %s), attempt %s, begun %s with EmuSuNi %s - %s (its log: "
         "emusuni-1.log)",
         Key(marker, "title").c_str(), Key(marker, "console").c_str(), Key(marker, "game").c_str(),
         Key(marker, "attempt").c_str(), Key(marker, "began").c_str(), Key(marker, "version").c_str(),
         again.empty() ? "the app was closed or stopped during it" : "the start watchdog restarted EmuSuNi");
  }
  if (!again.empty())
  {
    LogF("start: EmuSuNi was restarted by the start watchdog: %s (%s, %s s in \"%s\")", Key(again, "title").c_str(),
         why.c_str(), Key(again, "seconds").c_str(), Key(again, "step").c_str());
    const std::string game = Key(again, "game");
    if (!game.empty() && (why == "stuck" || why == "crash"))
    {
      *game_path = game;
      s_pending.path = game;
      s_pending.skip_continue = Key(again, "step") == CONTINUE_STEP;
      s_pending.toast = s_pending.skip_continue ?
                            "The game stopped while loading where you left off: EmuSuNi restarted and started it from "
                            "the beginning" :
                        why == "crash" ? "The game crashed while starting: EmuSuNi restarted and started it again" :
                                         "The game stopped while starting: EmuSuNi restarted and started it again";
      LogF("start: %s starts again at once (attempt 2%s)", Key(again, "title").c_str(),
           s_pending.skip_continue ? ", without Continue's state" : "");
    }
    else if (why == "stuck-again" || why == "crash-again")
    {
      *message = Key(again, "title") + "\nThe emulator stopped while it started,\ntwice in a row: EmuSuNi restarted." +
                 MESSAGE_TAIL;
    }
  }
  else if (!marker.empty() && std::atoi(Key(marker, "attempt").c_str()) >= 2)
  {
    // The second attempt's start ended the app too (a crash the watchdog couldn't catch, the PS4 closing it).
    *message = Key(marker, "title") + "\nEmuSuNi closed while it started,\ntwice in a row." + MESSAGE_TAIL;
  }
  if (!message->empty())
    LogLine("start: the menu says the game couldn't start");
  StartThread();
}

void Forget()
{
  s_pending = {};
}

void Begin(const std::string& path, const std::string& title, const std::string& console, const Limits& limits)
{
  const int64_t now = NowNs();
  int attempt = 1;
  s_toast.clear();
  s_skip_continue = false;
  if (!s_pending.path.empty() && s_pending.path == path)
  {
    attempt = 2;
    s_toast = s_pending.toast;
    s_skip_continue = s_pending.skip_continue;
  }
  s_pending = {};
  {
    std::lock_guard lock(s_mutex);
    s_path = path;
    s_title = title;
    s_console = console;
  }
  s_limits = limits;
  s_pictures_at_beat = 0;
  s_last_pass_ns = 0;
  s_run = 0.0;
  s_note = 10.0;
  s_first_frame = false;
  s_fail.clear();
  s_attempt = attempt;
  s_step = "the start begins";
  s_limit = limits.stuck;
  s_crash = 0;
  s_pictures = 0;
  s_begin_ns = now;
  s_beat_ns = now;
  if (!s_dir.empty())
  {
    WriteTextFile(MarkerPath(), "game=" + OneLine(path) + "\ntitle=" + OneLine(title) + "\nconsole=" + console +
                                    "\nattempt=" + std::to_string(attempt) + "\nbegan=" + LocalTime() + "\nversion=" +
                                    App::VERSION + "\n");
  }
  s_phase.store(STARTING, std::memory_order_release);
  LogF("start: %s: attempt %d; no picture allowed for %.0f s of running, a call into the core for %.0f s (the game's "
       "load %.0f s)%s",
       title.c_str(), attempt, limits.no_picture, limits.stuck, limits.loading,
       s_thread.load() ? "" : "; no watchdog thread");
}

int Attempt()
{
  return s_attempt.load();
}

std::string RetryToast()
{
  return s_toast;
}

bool SkipContinue()
{
  return s_skip_continue;
}

void Step(const char* what, bool loading)
{
  if (s_phase.load(std::memory_order_relaxed) == IDLE)
    return;
  const int64_t now = NowNs();
  s_beat_ns.store(now);
  s_step.store(what);
  s_limit.store(loading ? s_limits.loading : s_limits.stuck);
  LogF("start: %s (%.2f s)", what, Seconds(now - s_begin_ns.load()));
}

void Beat()
{
  if (s_phase.load(std::memory_order_relaxed) == IDLE)
    return;
  s_beat_ns.store(NowNs(), std::memory_order_relaxed);
  s_pictures_at_beat = s_pictures.load(std::memory_order_relaxed);
}

void Picture()
{
  if (s_phase.load(std::memory_order_relaxed) == STARTING)
    s_pictures.fetch_add(1, std::memory_order_relaxed);
}

Verdict Pass(int64_t frames_run, bool menu_open)
{
  const int phase = s_phase.load(std::memory_order_relaxed);
  if (phase != STARTING && phase != SETTLING)
    return Verdict::Go;
  const int64_t now = NowNs();
  const double pass = s_last_pass_ns != 0 ? Seconds(now - s_last_pass_ns) : 0.0;
  s_last_pass_ns = now;
  const double since = Seconds(now - s_begin_ns.load());
  if (!s_first_frame && frames_run > 0)
  {
    s_first_frame = true;
    LogF("start: the core's first frame ran (%.2f s)", since);
  }
  if (phase == SETTLING)
  {
    // The watch ends once the game has run a while after its first picture, or the in-game menu opens (its pages
    // may take their time: a download).
    s_settle -= menu_open ? s_settle : pass;
    if (s_settle <= 0.0)
      s_phase.store(IDLE, std::memory_order_release);
    return Verdict::Go;
  }
  if (s_pictures_at_beat > 0)
  {
    // This pass drew the core's picture (it had come before the pass began).
    LogF("start: the first picture shown (%.2f s; the game ran %.1f s for it, %lld frames of the core)", since, s_run,
         static_cast<long long>(frames_run));
    if (!s_dir.empty())
      std::remove(MarkerPath().c_str());
    s_settle = SETTLE_SECONDS;
    s_phase.store(SETTLING, std::memory_order_release);
    return Verdict::Go;
  }
  if (menu_open)
    return Verdict::Go;
  s_run += pass;
  if (s_run >= s_note)
  {
    LogF("start: no picture yet: the game ran %.0f s (%lld frames of the core), the limit %.0f s", s_run,
         static_cast<long long>(frames_run), s_limits.no_picture);
    s_note += 10.0;
  }
  if (s_run < s_limits.no_picture)
    return Verdict::Go;
  if (s_attempt.load() <= 1)
  {
    LogF("start: no picture: the game ran %.0f s (%lld frames of the core): it is stopped and started again (attempt 2)",
         s_run, static_cast<long long>(frames_run));
    s_pending.path = s_path;
    s_pending.toast = "The game showed no picture: it was started again";
    s_pending.skip_continue = false;
    return Verdict::Retry;
  }
  LogF("start: no picture again: the game ran %.0f s (%lld frames of the core), attempt 2: it ends, the menu says so",
       s_run, static_cast<long long>(frames_run));
  s_fail = "No picture came in " + std::to_string(static_cast<int>(s_limits.no_picture + 0.5f)) +
           " seconds,\ntwice in a row." + MESSAGE_TAIL;
  return Verdict::Fail;
}

std::string FailMessage()
{
  return s_fail;
}

void Ending()
{
  const int phase = s_phase.load(std::memory_order_relaxed);
  const int64_t now = NowNs();
  if (phase == STARTING)
    LogF("start: the game ends before its first picture (%.2f s)", Seconds(now - s_begin_ns.load()));
  s_beat_ns.store(now);
  s_step.store("the game ends");
  s_limit.store(s_limits.stuck);
  s_phase.store(ENDING, std::memory_order_release);
}

void End()
{
  s_phase.store(IDLE, std::memory_order_release);
  if (!s_dir.empty())
    std::remove(MarkerPath().c_str());
}
}  // namespace StartGuard

extern "C" void emusuni_start_crashed(int sig)
{
  using namespace StartGuard;
  // (A signal handler: atomics and sleeps only.)
  if (t_watchdog || !s_thread.load() || s_phase.load() == IDLE)
    return;
  int none = 0;
  s_crash.compare_exchange_strong(none, sig);
  // The watchdog's thread looks every 250 ms; its restart ends this process meanwhile.
  for (int i = 0; i < 60; i++)
    SleepUs(100000);
}
