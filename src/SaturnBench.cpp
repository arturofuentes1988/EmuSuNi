// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "SaturnBench.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <libretro.h>

#include "Cores.h"
#include "Log.h"

// Newer libretro.h than the app's (Snes9x's): the values are libretro's.
#ifndef RETRO_ENVIRONMENT_EXPERIMENTAL
#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
#endif
#ifndef RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS
#define RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS 44
#endif
#ifndef RETRO_ENVIRONMENT_GET_FASTFORWARDING
#define RETRO_ENVIRONMENT_GET_FASTFORWARDING (49 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif
#ifndef RETRO_ENVIRONMENT_GET_INPUT_BITMASKS
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif

#ifdef __PS4__
// libkernel's thread calls as they really are (OpenOrbis declares some without parameters).
extern "C" void* saturn_pthread_self(void) __asm__("scePthreadSelf");
extern "C" int saturn_set_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
extern "C" int saturn_set_priority(void* thread, int priority) __asm__("scePthreadSetprio");
extern "C" int saturn_rename_thread(void* thread, const char* name) __asm__("scePthreadRename");
extern "C" int sceKernelGetCurrentCpu(void);
#endif

namespace
{
// ---- Clocks ----
// The wall clock (the app's CLOCK_MONOTONIC: the CPU's time stamp counter on the PS4, port/ps4_runtime.cpp).
int64_t WallNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 1: CLOCK_THREAD_CPUTIME_ID gives the calling thread's CPU time (CheckCpuClock); 0: it doesn't, the wall clock.
std::atomic<int> s_cpu_clock{0};

int64_t ThreadCpuNs()
{
  timespec ts;
  if (s_cpu_clock.load(std::memory_order_relaxed) == 1 && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
  return WallNs();
}

// ---- What the SCSP thread reports (the core's calls, scripts/build-saturn-core.sh) ----
std::atomic<int64_t> s_scsp_cpu_ns{0};    // the SCSP thread's CPU time at its last frame's end
std::atomic<int64_t> s_scsp_start_ns{0};  // its CPU clock when it started
std::atomic<uint32_t> s_sleeps{0};        // its sleeps (the SH-2s had no new cycles for it)
std::atomic<int64_t> s_sleep_ns{0};       // their wall time
std::atomic<uint64_t> s_audio_frames{0};  // the sound frames the core gave (from the SCSP thread)
std::atomic<int> s_sleep_us{30};

// ---- The core's frontend: no picture, no pad, no sound out ----
const std::map<std::string, std::string> VARIABLES = {
    // As the PC measurement's (scratchpad research-saturn.md): the real BIOS, no frame skip, the 4M cartridge, the
    // original resolution (no renderer: these change nothing here but are the measured ones).
    {"yabasanshiro_force_hle_bios", "disabled"},
    {"yabasanshiro_frameskip", "disabled"},
    {"yabasanshiro_addon_cart", "4M_extended_ram"},
    {"yabasanshiro_system_language", "english"},
    {"yabasanshiro_multitap_port1", "disabled"},
    {"yabasanshiro_multitap_port2", "disabled"},
    {"yabasanshiro_polygon_mode", "perspective_correction"},
    {"yabasanshiro_resolution_mode", "original"},
    {"yabasanshiro_rbg_resolution_mode", "original"},
    {"yabasanshiro_rbg_use_compute_shader", "disabled"},
};
std::string s_system_dir, s_save_dir, s_core_error;
int s_core_lines = 0;
uint32_t s_pictures = 0;

void CoreLog(enum retro_log_level level, const char* format, ...)
{
  char text[512];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  size_t length = std::strlen(text);
  while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
    text[--length] = 0;
  if (level >= RETRO_LOG_ERROR)
    s_core_error = text;
  // (At most 40 lines a game: the core's messages, not every frame's.)
  if (length > 0 && s_core_lines < 40)
  {
    s_core_lines++;
    LogF("saturn: core: %s", text);
  }
}

bool Environment(unsigned command, void* data)
{
  switch (command)
  {
  case RETRO_ENVIRONMENT_GET_VARIABLE:
  {
    auto* variable = static_cast<retro_variable*>(data);
    const auto it = variable && variable->key ? VARIABLES.find(variable->key) : VARIABLES.end();
    if (it == VARIABLES.end())
      return false;
    variable->value = it->second.c_str();
    return true;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *static_cast<bool*>(data) = false;
    return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *static_cast<const char**>(data) = s_system_dir.c_str();
    return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *static_cast<const char**>(data) = s_save_dir.c_str();
    return true;
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    static_cast<retro_log_callback*>(data)->log = CoreLog;
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    return true;  // (no pad: one call a port, nothing held)
  case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
    *static_cast<bool*>(data) = false;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
  case RETRO_ENVIRONMENT_SET_GEOMETRY:
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
  case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
  case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
  case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    return true;
  default:
    return false;  // (no GL context, no perf interface, no VFS: the core's own file access)
  }
}

void VideoRefresh(const void*, unsigned, unsigned, size_t)
{
  s_pictures++;  // (duplicates: no renderer)
}

size_t AudioBatch(const int16_t*, size_t frames)
{
  s_audio_frames.fetch_add(frames, std::memory_order_relaxed);
  return frames;
}

void AudioSample(int16_t, int16_t) {}

void InputPoll() {}

int16_t InputState(unsigned, unsigned, unsigned, unsigned)
{
  return 0;
}

float Percentile(std::vector<float> values, double p)
{
  if (values.empty())
    return 0.0f;
  const size_t at = std::min(values.size() - 1, static_cast<size_t>(p * static_cast<double>(values.size() - 1) + 0.5));
  std::nth_element(values.begin(), values.begin() + static_cast<long>(at), values.end());
  return values[at];
}

std::string Fixed(double value, int decimals)
{
  char text[32];
  snprintf(text, sizeof(text), "%.*f", decimals, value);
  return text;
}

void LogWindow(const SaturnBench::Result& result, const SaturnBench::Stats& s)
{
  LogF("saturn test: %s frames %d-%d: emulation thread %.2f ms per frame (p95 %.2f, max %.1f), waiting %.2f; SCSP "
       "thread %.2f ms (%.0f sleeps of %.0f us); wall %.2f ms = %.0f%% of full speed; sound %.0f a frame",
       result.name.c_str(), s.first, s.last - 1, s.cpu, s.cpu_p95, s.cpu_max, s.wait, s.scsp, s.sleeps, s.sleep_us,
       s.wall, s.speed, s.audio);
}

// The PC's numbers for the owner's games (the PC check of 00.64, scratchpad saturn-0064-progress.md): this core built
// for the PC (scripts/build-saturn-core.sh's host mode), the same frames from the BIOS with no pad, the same
// measuring, median of 3 runs on an idle Intel i7-11700F (the emulation thread on one core, the SCSP thread on
// another): the emulation thread's CPU ms per frame in each 600-frame window from the BIOS. And on the same frames of
// the game's own demo race, how much more the OpenGL renderer's work on that thread makes it: YabaSanshiro's GL build
// with the same fast paths, Mesa doing nothing with the GL calls (GALLIUM_NOOP) - the study's way (research-saturn.md),
// whose numbers it reproduces (its stock build: Daytona USA's demo race 3.29 ms, Sonic R's 4.23).
struct Reference
{
  const char* name;            // a part of the game's file name
  int demo_first, demo_last;   // its own demo race (from the BIOS, no pad): frames [first, last)
  double pc_ms[11];            // windows 0-10: frames 0-599, 600-1199, ... 6000-6599
  double pc_demo, pc_demo_gl;  // the PC, its demo race: this core; the GL build (with the fast paths)
};
const Reference REFERENCES[] = {
    // The title over its demo race from frame ~1750, the high scores over it from ~3000, the logo at ~4500.
    {"Daytona USA", 1800, 4200, {1.008, 1.366, 1.756, 2.113, 2.101, 2.173, 2.108, 2.057, 1.966, 1.640, 2.054},
     2.133, 2.804},
    // Its menus up to ~4430, then its demo race (Resort Island).
    {"Sonic R", 4800, 6600, {0.982, 1.510, 2.165, 2.330, 2.379, 2.331, 2.346, 2.310, 2.513, 2.356, 2.431}, 2.430,
     4.082},
};

const Reference* FindReference(const std::string& name)
{
  for (const Reference& reference : REFERENCES)
  {
    if (name.find(reference.name) != std::string::npos)
      return &reference;
  }
  return nullptr;
}
}  // namespace

// ---- The core's calls (scripts/build-saturn-core.sh: its forced header declares them) ----
extern "C" void emusuni_saturn_thread(const char* name)
{
  const bool scsp = std::strcmp(name, "scsp") == 0;
  const bool vdp1 = std::strcmp(name, "vdp1") == 0, vdp2 = std::strcmp(name, "vdp2") == 0;
  const bool vdp2b = std::strcmp(name, "vdp2b") == 0, vdp1b = std::strcmp(name, "vdp1b") == 0;
  if (scsp)
    s_scsp_start_ns.store(ThreadCpuNs());
#ifdef __PS4__
  // The SCSP thread beside the emulation thread (core 2, src/SaturnTest.cpp): on core 3 (the same CPU module), at
  // its priority (480): the emulation waits for it at every frame's end. The software renderer's threads (the VDP1's
  // drawing in two bands of rows with its helper, 00.69; the VDP2's screens and picture, in two bands of lines with its
  // helper: scripts/build-saturn-core.sh) on cores 4 and 0, 5 and 1, at the same priority: the emulation waits for
  // them when they fall behind (the app's drawing beside the emulation then on cores 0 and 6, src/Emulator.cpp: the
  // VDP1's helper is idle most of the time - its drawings come every 2 or 3 frames, 4 ms each on the PS4 by the PC's
  // estimate). Others (none) below.
  void* self = saturn_pthread_self();
  const uint64_t mask = scsp  ? 1ull << 3 :
                        vdp1  ? 1ull << 4 :
                        vdp2  ? 1ull << 5 :
                        vdp2b ? 1ull << 1 :
                        vdp1b ? 1ull << 0 :
                                (1ull << 1) | (1ull << 4) | (1ull << 5);
  const int priority = scsp || vdp1 || vdp2 || vdp2b || vdp1b ? 480 : 700;
  const int affinity = saturn_set_affinity(self, mask);
  const int prio = saturn_set_priority(self, priority);
  char thread_name[32];
  snprintf(thread_name, sizeof(thread_name), "saturn %s", name);
  saturn_rename_thread(self, thread_name);
  LogF("saturn: thread \"%s\": cores %#llx (%#x), priority %d (%#x), running on CPU %d, flexible %d MiB free", name,
       static_cast<unsigned long long>(mask), static_cast<unsigned>(affinity), priority, static_cast<unsigned>(prio),
       sceKernelGetCurrentCpu(), FlexibleFreeMiB());
#else
  // The PC check: SATBENCH_SCSP_CPU=n puts it on CPU n (the PC measurement: emulation on CPU 0, SCSP on CPU 3), and
  // SATBENCH_VDP1_CPU, SATBENCH_VDP1B_CPU, SATBENCH_VDP2_CPU, SATBENCH_VDP2B_CPU the renderer's.
  const char* cpu = std::getenv(scsp ? "SATBENCH_SCSP_CPU" : vdp1 ? "SATBENCH_VDP1_CPU" :
                                vdp1b ? "SATBENCH_VDP1B_CPU" :
                                vdp2 ? "SATBENCH_VDP2_CPU" : vdp2b ? "SATBENCH_VDP2B_CPU" : "SATBENCH_OTHER_CPU");
#ifdef __linux__
  if (cpu)
  {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(std::atoi(cpu), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
#endif
  LogF("saturn: thread \"%s\" started%s%s", name, cpu ? " on CPU " : "", cpu ? cpu : "");
#endif
}

extern "C" void emusuni_saturn_stats(const char* text)
{
  LogF("saturn: %s", text);
}

extern "C" void emusuni_saturn_scsp_idle(void)
{
  const int64_t start = WallNs();
  const int us = s_sleep_us.load(std::memory_order_relaxed);
  if (us > 0)
    usleep(static_cast<useconds_t>(us));
  else
    sched_yield();
  s_sleep_ns.fetch_add(WallNs() - start, std::memory_order_relaxed);
  s_sleeps.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void emusuni_saturn_scsp_frame(void)
{
  s_scsp_cpu_ns.store(ThreadCpuNs() - s_scsp_start_ns.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

namespace SaturnBench
{
bool CheckCpuClock(std::string* report)
{
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
  {
    s_cpu_clock = 0;
    *report = "the threads' CPU clock (" + std::to_string(CLOCK_THREAD_CPUTIME_ID) +
              ") is refused: wall times instead (they count the waits too)";
    return false;
  }
  auto cpu = [] {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<int64_t>(t.tv_sec) * 1000000000ll + t.tv_nsec;
  };
  const int64_t c0 = cpu(), w0 = WallNs();
  usleep(20000);
  const int64_t c1 = cpu(), w1 = WallNs();
  volatile uint64_t spins = 0;
  while (WallNs() - w1 < 5000000)
    spins = spins + 1;
  const int64_t c2 = cpu(), w2 = WallNs();
  // How long the SCSP thread's sleep really is here (the system's timer).
  const int us = std::max(1, s_sleep_us.load());
  const int64_t s0 = WallNs();
  for (int i = 0; i < 50; i++)
    usleep(static_cast<useconds_t>(us));
  const double slept_us = (WallNs() - s0) / 1e3 / 50.0;
  const double sleep_cpu = (c1 - c0) / 1e6, sleep_wall = (w1 - w0) / 1e6;
  const double busy_cpu = (c2 - c1) / 1e6, busy_wall = (w2 - w1) / 1e6;
  const bool ok = sleep_cpu < 0.25 * sleep_wall && busy_cpu > 0.6 * busy_wall && busy_cpu < 1.5 * busy_wall;
  s_cpu_clock = ok ? 1 : 0;
  char text[300];
  snprintf(text, sizeof(text),
           "the threads' CPU clock (%d) %s: a %.1f ms sleep counted %.2f ms of CPU, %.1f ms busy counted %.2f; "
           "usleep(%d) sleeps %.0f us",
           CLOCK_THREAD_CPUTIME_ID, ok ? "works" : "doesn't (wall times instead: they count the waits too)",
           sleep_wall, sleep_cpu, busy_wall, busy_cpu, us, slept_us);
  *report = text;
  return ok;
}

Stats Measure(const Result& result, int first, int last)
{
  Stats s;
  first = std::max(0, first);
  last = std::min(last, static_cast<int>(result.cpu_ms.size()));
  s.first = first;
  s.last = last;
  if (last <= first)
    return s;
  const double n = last - first;
  double sleep_ms = 0, sleeps = 0;
  for (int i = first; i < last; i++)
  {
    s.cpu += result.cpu_ms[i];
    s.cpu_max = std::max(s.cpu_max, static_cast<double>(result.cpu_ms[i]));
    s.wall += result.wall_ms[i];
    s.scsp += result.scsp_ms[i];
    sleep_ms += result.sleep_ms[i];
    sleeps += result.sleeps[i];
    s.audio += result.audio[i];
  }
  s.cpu /= n;
  s.wall /= n;
  s.scsp /= n;
  s.audio /= n;
  s.wait = std::max(0.0, s.wall - s.cpu);
  s.sleeps = sleeps / n;
  s.sleep_us = sleeps > 0 ? sleep_ms * 1000.0 / sleeps : 0.0;
  s.cpu_p95 = Percentile(std::vector<float>(result.cpu_ms.begin() + first, result.cpu_ms.begin() + last), 0.95);
  const double budget = 1000.0 / (result.fps > 0 ? result.fps : 60.0);
  s.speed = s.wall > 0 ? std::min(100.0, 100.0 * budget / s.wall) : 0.0;
  return s;
}

Result Run(const CoreApi& core, const std::string& path, const Options& options, const Progress& progress)
{
  Result result;
  result.path = path;
  const size_t slash = path.find_last_of('/');
  result.name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = result.name.rfind('.');
  if (dot != std::string::npos)
    result.name.resize(dot);
  s_system_dir = options.system_dir;
  s_save_dir = options.save_dir;
  s_core_error.clear();
  s_core_lines = 0;
  s_pictures = 0;
  s_scsp_cpu_ns = 0;
  s_scsp_start_ns = ThreadCpuNs();
  s_sleeps = 0;
  s_sleep_ns = 0;
  s_audio_frames = 0;
  s_sleep_us = options.scsp_sleep_us;
  setenv("EMUSUNI_SATURN_BIOS", options.bios.c_str(), 1);
  LogF("saturn test: %s: %s, BIOS %s, %d frames at most (%.0f s at most)", result.name.c_str(), path.c_str(),
       options.bios.c_str(), options.frames, options.max_seconds);

  core.set_environment(Environment);
  core.set_video_refresh(VideoRefresh);
  core.set_audio_sample(AudioSample);
  core.set_audio_sample_batch(AudioBatch);
  core.set_input_poll(InputPoll);
  core.set_input_state(InputState);
  core.init();
  retro_game_info info = {};
  info.path = path.c_str();
  const int64_t load_start = WallNs();
  const bool loaded = core.load_game(&info);
  result.load_ms = (WallNs() - load_start) / 1e6;
  if (!loaded)
  {
    result.error = s_core_error.empty() ? std::string("the emulator didn't start it") : s_core_error;
    LogF("saturn test: %s didn't start (%.0f ms): %s", result.name.c_str(), result.load_ms, result.error.c_str());
    core.deinit();
    unsetenv("EMUSUNI_SATURN_BIOS");
    return result;
  }
  result.started = true;
  retro_system_av_info av = {};
  core.get_system_av_info(&av);
  if (av.timing.fps > 0)
    result.fps = av.timing.fps;
  LogF("saturn test: %s started in %.0f ms: %.2f frames a second (%.2f ms a frame for full speed), %.0f Hz sound",
       result.name.c_str(), result.load_ms, result.fps, 1000.0 / result.fps, av.timing.sample_rate);
  LogMemory("saturn test: game started");
  result.cpu_ms.reserve(options.frames);
  result.wall_ms.reserve(options.frames);
  result.scsp_ms.reserve(options.frames);
  result.sleep_ms.reserve(options.frames);
  result.sleeps.reserve(options.frames);
  result.audio.reserve(options.frames);

  const int64_t start = WallNs();
  int64_t next_progress = start;
  for (int frame = 0; frame < options.frames; frame++)
  {
    const int64_t scsp0 = s_scsp_cpu_ns.load(), sleep0 = s_sleep_ns.load();
    const uint32_t sleeps0 = s_sleeps.load();
    const uint64_t audio0 = s_audio_frames.load();
    const int64_t cpu0 = ThreadCpuNs(), wall0 = WallNs();
    core.run();
    const int64_t cpu1 = ThreadCpuNs(), wall1 = WallNs();
    result.cpu_ms.push_back(static_cast<float>((cpu1 - cpu0) / 1e6));
    result.wall_ms.push_back(static_cast<float>((wall1 - wall0) / 1e6));
    result.scsp_ms.push_back(static_cast<float>(std::max<int64_t>(0, s_scsp_cpu_ns.load() - scsp0) / 1e6));
    result.sleep_ms.push_back(static_cast<float>((s_sleep_ns.load() - sleep0) / 1e6));
    result.sleeps.push_back(s_sleeps.load() - sleeps0);
    result.audio.push_back(static_cast<uint32_t>(s_audio_frames.load() - audio0));
    const int done = frame + 1;
    if (options.after_frame)
      options.after_frame(core, done);
    if (done % options.window == 0)
      LogWindow(result, Measure(result, done - options.window, done));
    result.seconds = (wall1 - start) / 1e9;
    if (result.seconds > options.max_seconds)
    {
      result.stopped = "the time limit";
      break;
    }
    if (wall1 >= next_progress)
    {
      next_progress = wall1 + 500000000ll;
      if (progress && !progress(result, done))
      {
        result.stopped = "stopped with Circle";
        break;
      }
    }
  }
  const int frames = static_cast<int>(result.cpu_ms.size());
  if (frames % options.window != 0)
    LogWindow(result, Measure(result, frames - frames % options.window, frames));
  core.unload_game();
  core.deinit();
  unsetenv("EMUSUNI_SATURN_BIOS");
  // The game's numbers after its first window (the BIOS's start: the disc read, the logo).
  const Stats all = Measure(result, options.window, frames);
  LogF("saturn test: %s: %d frames in %.0f s%s%s; frames %d-%d: emulation thread %.2f ms per frame (p95 %.2f), "
       "SCSP thread %.2f, wall %.2f = %.0f%% of full speed; %u pictures (duplicates), sound %.0f frames a frame",
       result.name.c_str(), frames, result.seconds, result.stopped.empty() ? "" : ", stopped: ", result.stopped.c_str(),
       all.first, all.last - 1, all.cpu, all.cpu_p95, all.scsp, all.wall, all.speed, s_pictures, all.audio);
  LogMemory("saturn test: game ended");
  return result;
}

int FramesFor(const std::string& name, int otherwise)
{
  const Reference* reference = FindReference(name);
  return reference ? reference->demo_last : otherwise;
}

std::vector<std::string> Verdict(const std::vector<Result>& results, const Options& options)
{
  // Each game: one line in the log with everything, a short one for the screen (returned).
  std::vector<std::string> screen;
  bool daytona = false;
  for (const Result& result : results)
  {
    if (!result.started)
    {
      LogLine("saturn test: " + result.name + " didn't start: " + result.error);
      screen.push_back(result.name + ": didn't start (" + result.error + ")");
      continue;
    }
    const int frames = static_cast<int>(result.cpu_ms.size());
    const double budget = 1000.0 / result.fps;
    const Stats all = Measure(result, options.window, frames);
    const std::string range = std::to_string(all.first) + "-" + std::to_string(all.last - 1);
    const Reference* reference = FindReference(result.name);
    if (!reference)
    {
      LogLine("saturn test: " + result.name + ": frames " + range + ": emulation thread " + Fixed(all.cpu, 2) +
              " ms a frame (p95 " + Fixed(all.cpu_p95, 2) + ") without the renderer, SCSP thread " + Fixed(all.scsp, 2) +
              ", " + Fixed(all.speed, 0) + "% of full speed (" + Fixed(budget, 2) + " ms a frame); the PC didn't "
              "measure this game");
      screen.push_back(result.name + ": " + Fixed(all.cpu, 1) + " ms a frame without the renderer, " +
                       Fixed(all.speed, 0) + "% of full speed");
      continue;
    }
    // The PS4's factor to the PC: the same 600-frame windows of both (after the BIOS's first), this core on both.
    double ps4 = 0, pc = 0;
    int windows = 0;
    for (int w = 1; w < 11 && (w + 1) * 600 <= frames; w++, windows++)
    {
      ps4 += Measure(result, w * 600, (w + 1) * 600).cpu;
      pc += reference->pc_ms[w];
    }
    const double factor = pc > 0 ? ps4 / pc : 0.0;
    // Its demo race as measured here (when the run got to it; else from the factor and the PC's race), and with the GL
    // renderer's work on the emulation thread as the PC measured it on the same frames.
    const Stats race = Measure(result, reference->demo_first, std::min(frames, reference->demo_last));
    const bool raced = race.last - race.first >= 600;
    const double without = raced ? race.cpu : factor * reference->pc_demo;
    const double renderer = reference->pc_demo_gl / reference->pc_demo;
    const double with = without * renderer;
    const double speed = with > 0 ? std::min(100.0, 100.0 * budget / with) : 0.0;
    LogLine("saturn test: " + result.name + ": its demo race " +
            (raced ? "(frames " + std::to_string(race.first) + "-" + std::to_string(race.last - 1) + ")" :
                     std::string("(not reached: from the PS4's factor and the PC's race)")) +
            ": emulation thread " + Fixed(without, 2) + " ms a frame" +
            (raced ? " (p95 " + Fixed(race.cpu_p95, 2) + ")" : std::string()) + " without the renderer; with the GL "
            "renderer's work on it (x" + Fixed(renderer, 3) + ", the PC on the same frames) about " + Fixed(with, 2) +
            " ms = " + Fixed(speed, 0) + "% of full speed (" + Fixed(budget, 2) + " ms a frame; Zink's own cost not "
            "counted, about +0.3-1.5 ms); SCSP thread " + Fixed(raced ? race.scsp : all.scsp, 2) +
            " ms a frame on its own core");
    LogLine("saturn test: " + result.name + ": the PS4 is " + Fixed(factor, 2) + "x the PC (" + std::to_string(windows) +
            " windows of 600 frames: " + Fixed(windows ? ps4 / windows : 0.0, 2) + " ms a frame vs " +
            Fixed(windows ? pc / windows : 0.0, 2) + "); the PC's demo race: " + Fixed(reference->pc_demo, 2) +
            " ms without the renderer, " + Fixed(reference->pc_demo_gl, 2) + " with the GL renderer");
    screen.push_back(result.name + ": " + Fixed(with, 1) + " ms a frame (" + Fixed(speed, 0) +
                     "% of full speed); the PS4 is " + Fixed(factor, 1) + "x the PC");
    if (std::string(reference->name) == "Daytona USA" && !daytona)
    {
      daytona = true;
      const char* what = with <= 20.0 ? "20 ms or less: go on to the full Saturn (Zink and the port)" :
                         with <= 30.0 ? "between 20 and 30 ms: a Saturn for light games only" :
                                        "above 30 ms: the Saturn closes";
      const std::string verdict = "VERDICT: Daytona USA " + Fixed(with, 1) + " ms a frame: " + what;
      LogLine("saturn test: " + verdict);
      screen.push_back(verdict);
    }
  }
  if (!daytona)
  {
    LogLine("saturn test: VERDICT: none (Daytona USA wasn't measured)");
    screen.push_back("VERDICT: none (Daytona USA wasn't measured)");
  }
  return screen;
}
}  // namespace SaturnBench
