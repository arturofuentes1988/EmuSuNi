// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameAudio.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include <orbis/AudioOut.h>
#include <orbis/libkernel.h>

#include "Log.h"

// libkernel's affinity call as it really is (OpenOrbis declares it without parameters): bit n of
// the mask is core n.
extern "C" int emusuni_sound_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");

namespace AudioPort
{
namespace
{
// Above the games' threads (the emulation and ParaLLEl-RDP's commands run at 480): the sound thread
// blocks in sceAudioOutOutput nearly all the time and must never miss its 5.3 ms (DolphinPS4's sound
// thread runs at 400 too). Lower is higher; 700 is the default.
constexpr int PRIORITY = 400;
// Every core the app has (0x7f) but 2 (the emulation thread, an N64 game's) and 3 (ParaLLEl-RDP's
// command thread).
constexpr uint64_t CORES = 0x73;
constexpr int ERRORS_BEFORE_REOPEN = 94;  // half a second of failed outputs
constexpr int MAX_REOPENS = 3;            // each may keep one of the 8 ports
constexpr uint32_t ALREADY_INIT = 0x8026000E;  // ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT

std::mutex s_mutex;  // opening the port
std::atomic<int> s_port{-1};
bool s_initialized = false;
int s_reopens = 0;
int s_errors_in_a_row = 0;  // the playing thread's (one plays at a time)

int OpenLocked()
{
  if (!s_initialized)
  {
    const int init = sceAudioOutInit();
    if (init != 0 && static_cast<uint32_t>(init) != ALREADY_INIT)
      LogF("audio: sceAudioOutInit: %#x", static_cast<unsigned>(init));
    s_initialized = true;
  }
  const int port = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, PERIOD, RATE,
                                   ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
  if (port < 0)
  {
    LogF("audio: sceAudioOutOpen failed (%#x)", static_cast<unsigned>(port));
    return port;
  }
  // 0 dB: Game Volume and the menu's volume scale the samples.
  int32_t levels[2] = {32768, 32768};
  const int volume = sceAudioOutSetVolume(port, 0x3, levels);
  LogF("audio: port %#x opened (volume %#x), kept for the menu and every game", static_cast<unsigned>(port),
       static_cast<unsigned>(volume));
  return port;
}

// Half a second of failed outputs: a new port (the old one closed; its result in the log).
void Reopen(int error)
{
  std::lock_guard lock(s_mutex);
  if (s_reopens >= MAX_REOPENS)
    return;
  s_reopens++;
  const int old = s_port.exchange(-1);
  const int closed = old >= 0 ? sceAudioOutClose(old) : 0;
  const int port = OpenLocked();
  s_port = port;
  LogF("audio: port %#x failed for 0.5 s (%#x): closed (%#x), new port %#x (%d of %d)", static_cast<unsigned>(old),
       static_cast<unsigned>(error), static_cast<unsigned>(closed), static_cast<unsigned>(port), s_reopens,
       MAX_REOPENS);
}
}  // namespace

int Get()
{
  std::lock_guard lock(s_mutex);
  int port = s_port.load();
  if (port < 0)
  {
    port = OpenLocked();
    s_port = port;
  }
  return port;
}

int Output(const int16_t* frames)
{
  const int port = s_port.load();
  const int result = port >= 0 ? sceAudioOutOutput(port, frames) : port;
  if (result >= 0)
  {
    s_errors_in_a_row = 0;
    return result;
  }
  // Never spin at the sound thread's priority: wait as long as a period plays.
  sceKernelUsleep(PERIOD * 1000000u / RATE);
  if (++s_errors_in_a_row >= ERRORS_BEFORE_REOPEN)
  {
    s_errors_in_a_row = 0;
    Reopen(result);
  }
  return result;
}

void Drain()
{
  const int port = s_port.load();
  if (port >= 0)
    sceAudioOutOutput(port, nullptr);  // null: waits for the last period to be played
}

void SetSoundThread(const char* who)
{
  const OrbisPthread self = scePthreadSelf();
  int before = 0;
  scePthreadGetprio(self, &before);
  const int priority = scePthreadSetprio(self, PRIORITY);
  const int cores = emusuni_sound_affinity(self, CORES);
  LogF("audio: %s sound thread: priority %d -> %d (%#x), cores %#llx (%#x), running on CPU %d", who, before,
       PRIORITY, static_cast<unsigned>(priority), static_cast<unsigned long long>(CORES),
       static_cast<unsigned>(cores), sceKernelGetCurrentCpu());
}
}  // namespace AudioPort

bool GameAudio::Start(int buffer_ms)
{
  buffer_ms = std::clamp(buffer_ms, 32, 256);
  m_target = RATE * static_cast<size_t>(buffer_ms) / 1000;
  m_capacity = m_target * 3;
  m_ring.assign(m_capacity * 2, 0);
  m_read = 0;
  m_write = 0;
  m_phase = 0.0;
  m_last[0] = m_last[1] = 0;
  m_stop = false;
  m_in = 0;
  m_dropped = 0;
  m_played = 0;
  m_silent = 0;
  m_lost = 0;
  m_outputs = 0;
  m_dry = 0;
  m_late = 0;
  m_errors = 0;
  m_max_gap_us = 0;
  m_last_error = 0;
  m_input_rate = 0;
  m_logged = {};
  // The app's one port (AudioPort): opened by the menu or the first game, never closed.
  m_port = AudioPort::Get();
  if (m_port < 0)
  {
    LogLine("audio: no sound port: this game plays without sound");
    return false;
  }
  try
  {
    m_thread = std::thread([this] { Loop(); });
  }
  catch (...)
  {
    LogLine("audio: the sound thread couldn't be made (no sound)");
    m_port = -1;
    return false;
  }
  LogF("audio: started, %d ms buffer", buffer_ms);
  return true;
}

void GameAudio::Stop()
{
  m_stop = true;
  if (m_thread.joinable())
    m_thread.join();
  m_port = -1;  // the port stays open for the menu and the next game
}

size_t GameAudio::Buffered() const
{
  const size_t write = m_write.load(std::memory_order_acquire);
  const size_t read = m_read.load(std::memory_order_acquire);
  return (write + m_capacity - read) % m_capacity;
}

float GameAudio::Fill() const
{
  return m_capacity ? static_cast<float>(Buffered()) / static_cast<float>(m_capacity) : 0.0f;
}

void GameAudio::Clear()
{
  m_read.store(m_write.load());
}

void GameAudio::Push(const int16_t* samples, size_t frames, double input_rate)
{
  m_in.fetch_add(frames, std::memory_order_relaxed);
  m_input_rate.store(static_cast<int>(input_rate), std::memory_order_relaxed);
  if (m_port < 0 || frames == 0 || input_rate <= 0.0)
    return;
  // Rate control: up to 0.5% faster or slower output to keep the buffer near its target.
  const double fill = static_cast<double>(Buffered()) / static_cast<double>(m_target);
  const double adjust = 1.0 + 0.005 * (fill - 1.0);
  const double step = input_rate / RATE * std::clamp(adjust, 0.99, 1.01);
  size_t write = m_write.load(std::memory_order_relaxed);
  const size_t read = m_read.load(std::memory_order_acquire);
  size_t space = (read + m_capacity - write - 1) % m_capacity;
  uint64_t dropped = 0;
  for (size_t i = 0; i < frames; i++)
  {
    const int16_t l = samples[i * 2], r = samples[i * 2 + 1];
    while (m_phase < 1.0)
    {
      if (space == 0)
        break;  // full (fast forward): the rest is dropped
      m_ring[write * 2] = static_cast<int16_t>(m_last[0] + (l - m_last[0]) * m_phase);
      m_ring[write * 2 + 1] = static_cast<int16_t>(m_last[1] + (r - m_last[1]) * m_phase);
      write = (write + 1) % m_capacity;
      space--;
      m_phase += step;
    }
    if (m_phase >= 1.0)
    {
      m_phase -= 1.0;
    }
    else
    {
      m_phase = 0.0;  // dropped
      dropped++;
    }
    m_last[0] = l;
    m_last[1] = r;
  }
  m_write.store(write, std::memory_order_release);
  if (dropped > 0)
    m_dropped.fetch_add(dropped, std::memory_order_relaxed);
}

void GameAudio::Loop()
{
  AudioPort::SetSoundThread("game");
  using Clock = std::chrono::steady_clock;
  constexpr uint32_t PERIOD_US = PERIOD * 1000000u / RATE;  // 5333
  std::vector<int16_t> out(PERIOD * 2);
  bool started = false;
  auto last = Clock::now();
  while (!m_stop)
  {
    // Start playing once half the target is there (no crackle at the start).
    const size_t available = Buffered();
    if (!started && available >= m_target / 2)
      started = true;
    const int volume = m_muted ? 0 : m_volume.load();
    size_t read = m_read.load(std::memory_order_relaxed);
    uint32_t played = 0;
    for (uint32_t i = 0; i < PERIOD; i++)
    {
      int16_t l = 0, r = 0;
      if (started && read != m_write.load(std::memory_order_acquire))
      {
        l = m_ring[read * 2];
        r = m_ring[read * 2 + 1];
        read = (read + 1) % m_capacity;
        played++;
      }
      out[i * 2] = static_cast<int16_t>(l * volume / 100);
      out[i * 2 + 1] = static_cast<int16_t>(r * volume / 100);
    }
    m_read.store(read, std::memory_order_release);
    if (started && Buffered() == 0)
    {
      started = false;  // ran dry: wait for some sound again
      m_dry.fetch_add(1, std::memory_order_relaxed);
    }
    const int result = AudioPort::Output(out.data());
    m_outputs.fetch_add(1, std::memory_order_relaxed);
    if (result >= 0)
    {
      m_played.fetch_add(played, std::memory_order_relaxed);
      m_silent.fetch_add(PERIOD - played, std::memory_order_relaxed);
    }
    else
    {
      m_lost.fetch_add(PERIOD, std::memory_order_relaxed);  // never heard
      m_errors.fetch_add(1, std::memory_order_relaxed);
      m_last_error.store(result, std::memory_order_relaxed);
    }
    // Since the last output came back: above two periods the hardware had nothing to play (a click).
    const auto now = Clock::now();
    const auto gap = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
    last = now;
    if (gap > 2 * PERIOD_US)
      m_late.fetch_add(1, std::memory_order_relaxed);
    if (gap > m_max_gap_us.load(std::memory_order_relaxed))
      m_max_gap_us.store(gap, std::memory_order_relaxed);
  }
  AudioPort::Drain();
}

void GameAudio::LogHealth(float seconds)
{
  Counts now;
  now.in = m_in.load();
  now.dropped = m_dropped.load();
  now.played = m_played.load();
  now.silent = m_silent.load();
  now.lost = m_lost.load();
  now.outputs = m_outputs.load();
  now.dry = m_dry.load();
  now.late = m_late.load();
  now.errors = m_errors.load();
  const uint32_t max_gap = m_max_gap_us.exchange(0);
  const Counts was = m_logged;
  m_logged = now;
  if (seconds <= 0.0f)
    return;
  const uint64_t played = now.played - was.played, silent = now.silent - was.silent, lost = now.lost - was.lost;
  const double total = static_cast<double>(played + silent + lost);
  const auto percent = [total](uint64_t frames) {
    return total > 0.0 ? 100.0 * static_cast<double>(frames) / total : 0.0;
  };
  LogF("audio: %.1f s: the core sent %.0f Hz (of %d); %.1f%% played, %.1f%% silence, %.1f%% lost, dry %u times, "
       "%llu frames dropped; %.0f ms buffered; %llu outputs, at most %.1f ms apart, %u late; "
       "%u errors (last %#x)%s",
       seconds, static_cast<double>(now.in - was.in) / seconds, m_input_rate.load(), percent(played),
       percent(silent), percent(lost), now.dry - was.dry, static_cast<unsigned long long>(now.dropped - was.dropped),
       m_capacity ? static_cast<double>(Buffered()) * 1000.0 / RATE : 0.0,
       static_cast<unsigned long long>(now.outputs - was.outputs), max_gap / 1000.0, now.late - was.late,
       now.errors - was.errors, static_cast<unsigned>(m_last_error.load()),
       m_port < 0 ? "; no sound port" : "");
}
