// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameAudio.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
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

namespace
{
// Rate control, as RetroArch's (audio_rate_control_delta 0.005): the resampling at most 0.5% faster or slower - all of
// it once the buffer is 40% of its target above or below it.
constexpr double RATE_CONTROL = 0.005;
constexpr double RATE_SPAN = 0.4;
// The stretcher. A splice is a waveform period (2.5 to 25 ms: 400 Hz down to a 40 Hz bass, or several periods of a
// higher sound) played twice or left out, crossfaded over FADE frames where the sound matches itself best.
constexpr uint32_t SPLICE_MIN = 120;
constexpr uint32_t SPLICE_MAX = 1200;
constexpr uint32_t FADE = 288;  // 6 ms (even: BestMatch's coarse pass takes every other frame)
// When: the buffer's lowest level of the last 43 ms under 45% of its target (the game slower than the sound: a
// period played twice), or over 150% (faster: one left out). Over 250% (the frames owed after a stall, fast
// forward) it is cut back to the target at once, one crossfade.
constexpr double INSERT_BELOW = 0.45;
constexpr double DELETE_ABOVE = 1.5;
constexpr double CUT_ABOVE = 2.5;
// At most half of the sound added or taken out (a game down to half speed keeps all its sound, slower; a stall is
// bridged for a moment and then faded out), two of the longest splices of that ahead; a splice is made only with one
// of the longest allowed (the best match is looked for over every period length).
constexpr double MAX_STRETCH = 0.5;
constexpr double ALLOWANCE_MAX = 2.0 * SPLICE_MAX;
// The share of the sound the stretcher changes, over about 2 s: above 1% it does all of the buffer's work and the
// rate control stays at 1 (the pitch exact); under 0.3% the rate control is back.
constexpr double ACTIVITY_PERIODS = 375.0;
constexpr double HOLD_ON = 0.01;
constexpr double HOLD_OFF = 0.003;
// The fades into and out of silence (the start, running dry): 2 ms.
constexpr uint32_t RAMP = 96;

// The sum of a[i] * b[i] (in any order: the compiler may use its vector units).
float Dot(const float* a, const float* b, uint32_t n)
{
#pragma clang fp reassociate(on)
  float sum = 0.0f;
  for (uint32_t i = 0; i < n; i++)
    sum += a[i] * b[i];
  return sum;
}
}  // namespace

bool GameAudio::Start(int buffer_ms)
{
  buffer_ms = std::clamp(buffer_ms, 32, 256);
  m_target = RATE * static_cast<size_t>(buffer_ms) / 1000;
  m_capacity = m_target * 4;
  m_ring.assign(m_capacity * 2, 0);
  m_read = 0;
  m_write = 0;
  m_staged = 0;
  m_phase = 0.0;
  m_last[0] = m_last[1] = 0;
  std::memset(m_low, 0, sizeof(m_low));
  m_stage.assign((SPLICE_MAX + FADE) * 2, 0);
  m_stage_at = m_stage_len = 0;
  m_match.assign(6 * FADE + 3 * SPLICE_MAX + 2, 0.0f);
  m_energy.assign(SPLICE_MAX + FADE + 1 + (SPLICE_MAX + FADE) / 2 + 2, 0.0);
  std::fill(std::begin(m_levels), std::end(m_levels), 0);
  m_level_at = 0;
  m_allowance = 0.0;
  m_activity = 0.0;
  m_hold = false;
  m_hold_rate = false;
  m_playing = false;
  m_fade_in = 0;
  m_gain = 0.0f;
  m_stop = false;
  m_clear = false;
  m_in = 0;
  m_dropped = 0;
  m_played = 0;
  m_silent = 0;
  m_lost = 0;
  m_outputs = 0;
  m_added = 0;
  m_removed = 0;
  m_held = 0;
  m_rate_ppm = 0;
  m_rate_min = INT_MAX;
  m_rate_max = INT_MIN;
  m_dry = 0;
  m_late = 0;
  m_errors = 0;
  m_max_gap_us = 0;
  m_inserts = 0;
  m_deletes = 0;
  m_cuts = 0;
  m_lowest = SIZE_MAX;
  m_last_error = 0;
  m_input_rate = 0;
  m_logged = {};
  m_logged_at = std::chrono::steady_clock::now();
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
  LogF("audio: started, %d ms buffer; the sound at the core's own pitch (rate control +-0.5%%, the stretcher keeps "
       "the pitch when the game is slower or faster)",
       buffer_ms);
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

void GameAudio::Push(const int16_t* samples, size_t frames, double input_rate)
{
  m_in.fetch_add(frames, std::memory_order_relaxed);
  m_input_rate.store(static_cast<int>(std::lround(input_rate)), std::memory_order_relaxed);
  if (m_port < 0 || frames == 0 || input_rate <= 0.0)
    return;
  // Rate control (RetroArch's): the resampling up to 0.5% faster while the buffer is above its target, slower below
  // - unless the stretcher is doing the buffer's work (the pitch exact then).
  const bool hold = m_hold_rate.load(std::memory_order_relaxed);
  const double level = static_cast<double>(Buffered() + m_staged.load(std::memory_order_relaxed));
  const double target = static_cast<double>(m_target);
  const double direction = hold ? 0.0 : std::clamp((level - target) / (target * RATE_SPAN), -1.0, 1.0);
  const double step = input_rate / RATE * (1.0 + RATE_CONTROL * direction);
  const int ppm = static_cast<int>(std::lround(RATE_CONTROL * direction * 1e6));
  m_rate_ppm.fetch_add(static_cast<int64_t>(ppm) * static_cast<int64_t>(frames), std::memory_order_relaxed);
  if (ppm < m_rate_min.load(std::memory_order_relaxed))
    m_rate_min.store(ppm, std::memory_order_relaxed);
  if (ppm > m_rate_max.load(std::memory_order_relaxed))
    m_rate_max.store(ppm, std::memory_order_relaxed);
  if (hold)
    m_held.fetch_add(frames, std::memory_order_relaxed);
  size_t write = m_write.load(std::memory_order_relaxed);
  const size_t read = m_read.load(std::memory_order_acquire);
  // The free part of the ring but the longest splice's worth just behind the read position: the sound just played,
  // which the stretcher may play again.
  const size_t room = (read + m_capacity - write - 1) % m_capacity;
  size_t space = room > SPLICE_MAX ? room - SPLICE_MAX : 0;
  uint64_t dropped = 0;
  // Above 1.2x the output's rate: low-pass at 19 kHz first (std::exp: the stages' coefficient).
  const bool filter = input_rate > RATE * 1.2;
  const float alpha = filter ? static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 19000.0 / input_rate)) : 1.0f;
  for (size_t i = 0; i < frames; i++)
  {
    int16_t l = samples[i * 2], r = samples[i * 2 + 1];
    if (filter)
    {
      m_low[0][0] += alpha * (l - m_low[0][0]);
      m_low[0][1] += alpha * (m_low[0][0] - m_low[0][1]);
      m_low[1][0] += alpha * (r - m_low[1][0]);
      m_low[1][1] += alpha * (m_low[1][0] - m_low[1][1]);
      l = static_cast<int16_t>(std::clamp(m_low[0][1], -32768.0f, 32767.0f));
      r = static_cast<int16_t>(std::clamp(m_low[1][1], -32768.0f, 32767.0f));
    }
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

uint32_t GameAudio::BestMatch(size_t ref_at, size_t cand_at, uint32_t range, int prefer)
{
  // Stereo floats (frames interleaved): the reference, FADE frames from `ref_at`, and the candidates' frames, from
  // `cand_at` to `cand_at` + range + FADE (range at most SPLICE_MAX); every other frame of both too, for a coarse pass
  // over every other offset, then the best one's neighbours at full resolution. Running sums of their squares.
  const uint32_t count = range + FADE;
  const uint32_t count2 = (count + 1) / 2;
  float* ref = m_match.data();
  float* cand = ref + 2 * FADE;
  float* ref2 = cand + 2 * (SPLICE_MAX + FADE);
  float* cand2 = ref2 + FADE;
  double* energy = m_energy.data();
  double* energy2 = energy + (SPLICE_MAX + FADE + 1);
  for (uint32_t i = 0; i < FADE; i++)
  {
    const int16_t* f = &m_ring[((ref_at + i) % m_capacity) * 2];
    ref[i * 2] = f[0];
    ref[i * 2 + 1] = f[1];
  }
  energy[0] = 0.0;
  for (uint32_t i = 0; i < count; i++)
  {
    const int16_t* f = &m_ring[((cand_at + i) % m_capacity) * 2];
    cand[i * 2] = f[0];
    cand[i * 2 + 1] = f[1];
    energy[i + 1] = energy[i] + static_cast<double>(f[0]) * f[0] + static_cast<double>(f[1]) * f[1];
  }
  for (uint32_t i = 0; i < FADE / 2; i++)
  {
    ref2[i * 2] = ref[i * 4];
    ref2[i * 2 + 1] = ref[i * 4 + 1];
  }
  energy2[0] = 0.0;
  for (uint32_t i = 0; i < count2; i++)
  {
    cand2[i * 2] = cand[i * 4];
    cand2[i * 2 + 1] = cand[i * 4 + 1];
    energy2[i + 1] = energy2[i] + static_cast<double>(cand2[i * 2]) * cand2[i * 2] +
                     static_cast<double>(cand2[i * 2 + 1]) * cand2[i * 2 + 1];
  }
  const double ref_energy = Dot(ref, ref, 2 * FADE), ref_energy2 = Dot(ref2, ref2, FADE);
  if (ref_energy < 16.0 * FADE)
    return prefer > 0 ? range : 0;  // silence: any offset will do
  // Normalized cross-correlation; of near equals the offset `prefer` points to (the longer splice: fewer of them).
  const auto score = [&](double dot, double e_ref, double e_cand, uint32_t offset) {
    return dot / std::sqrt(e_ref * e_cand + 1.0) + 0.05 * prefer * (offset / (range + 1.0));
  };
  uint32_t best = 0;
  double best_score = -1e30;
  for (uint32_t q = 0; 2 * q <= range; q++)
  {
    const double s = score(Dot(ref2, cand2 + 2 * q, FADE), ref_energy2, energy2[q + FADE / 2] - energy2[q], 2 * q);
    if (s > best_score)
    {
      best_score = s;
      best = 2 * q;
    }
  }
  const uint32_t from = best >= 2 ? best - 2 : 0, to = std::min(best + 2, range);
  best_score = -1e30;
  for (uint32_t o = from; o <= to; o++)
  {
    const double s = score(Dot(ref, cand + 2 * o, 2 * FADE), ref_energy, energy[o + FADE] - energy[o], o);
    if (s > best_score)
    {
      best_score = s;
      best = o;
    }
  }
  return best;
}

long GameAudio::Stretch(size_t& read, size_t ring, size_t low, bool* cut)
{
  *cut = false;
  const auto frame = [this](size_t at) { return &m_ring[(at % m_capacity) * 2]; };
  const auto blend = [](const int16_t* from, const int16_t* to, uint32_t i, int16_t* out) {
    const float w = (static_cast<float>(i) + 0.5f) / FADE;
    for (int c = 0; c < 2; c++)
      out[c] = static_cast<int16_t>(std::lround(from[c] + (to[c] - from[c]) * w));
  };
  const double target = static_cast<double>(m_target);
  // Far too much buffered: back to the target at once, the cut where the sound matches itself best.
  if (low > target * CUT_ABOVE && ring > m_target + SPLICE_MAX + FADE)
  {
    const size_t excess = ring - m_target;
    const uint32_t lo = static_cast<uint32_t>(std::max<size_t>(excess - SPLICE_MAX / 2, SPLICE_MIN));
    const uint32_t range = static_cast<uint32_t>(std::min<size_t>(SPLICE_MAX, ring - FADE - lo));
    const uint32_t p = lo + BestMatch(read, read + lo, range, 0);
    for (uint32_t i = 0; i < FADE; i++)
      blend(frame(read + i), frame(read + p + i), i, &m_stage[i * 2]);
    m_stage_at = 0;
    m_stage_len = FADE;
    read = (read + p + FADE) % m_capacity;
    *cut = true;
    m_cuts.fetch_add(1, std::memory_order_relaxed);
    m_removed.fetch_add(p, std::memory_order_relaxed);
    return -static_cast<long>(p);
  }
  if (m_allowance < SPLICE_MAX)
    return 0;
  if (low < target * INSERT_BELOW && ring >= FADE)
  {
    // The game slower than the sound: the last period played (kept in the ring behind `read`: the producer never
    // writes over the longest splice's worth there) played again - the crossfade from the sound at `read` into the
    // sound a period before it, then on from there. Only the crossfade's frames are needed ahead.
    const uint32_t hi = SPLICE_MAX;
    const uint32_t p = hi - BestMatch(read, read + m_capacity - hi, hi - SPLICE_MIN, -1);
    const size_t back = read + m_capacity - p;
    for (uint32_t i = 0; i < FADE; i++)
      blend(frame(read + i), frame(back + i), i, &m_stage[i * 2]);
    // (A period shorter than the crossfade: the crossfade ends past `read`, where the ring goes on.)
    for (uint32_t i = FADE; i < p; i++)
      std::memcpy(&m_stage[i * 2], frame(back + i), 2 * sizeof(int16_t));
    m_stage_at = 0;
    m_stage_len = std::max(p, FADE);
    if (p < FADE)
      read = (read + FADE - p) % m_capacity;
    m_allowance -= p;
    m_inserts.fetch_add(1, std::memory_order_relaxed);
    m_added.fetch_add(p, std::memory_order_relaxed);
    return static_cast<long>(p);
  }
  if (low > target * DELETE_ABOVE && ring >= SPLICE_MIN + FADE)
  {
    // Faster: the period from `read` left out (the crossfade from the sound at `read` into the sound a period on).
    const uint32_t hi = static_cast<uint32_t>(std::min<size_t>(SPLICE_MAX, ring - FADE));
    const uint32_t p = SPLICE_MIN + BestMatch(read, read + SPLICE_MIN, hi - SPLICE_MIN, 1);
    for (uint32_t i = 0; i < FADE; i++)
      blend(frame(read + i), frame(read + p + i), i, &m_stage[i * 2]);
    m_stage_at = 0;
    m_stage_len = FADE;
    read = (read + p + FADE) % m_capacity;
    m_allowance -= p;
    m_deletes.fetch_add(1, std::memory_order_relaxed);
    m_removed.fetch_add(p, std::memory_order_relaxed);
    return -static_cast<long>(p);
  }
  return 0;
}

uint32_t GameAudio::MakePeriod(int16_t* out)
{
  size_t read = m_read.load(std::memory_order_relaxed);
  if (m_clear.exchange(false))
  {
    read = m_write.load(std::memory_order_acquire);
    m_stage_at = m_stage_len = 0;
    m_playing = false;
  }
  const size_t write = m_write.load(std::memory_order_acquire);
  size_t ring = (write + m_capacity - read) % m_capacity;
  size_t level = ring + (m_stage_len - m_stage_at);
  // Muted (fast forward, the in-game menu) and faded out: nothing is heard, so nothing is stretched, and what is
  // buffered past the target is let go at once (fast forward sends sound faster than it plays: after it, the delay is
  // the target's again, not a full ring's).
  const bool unheard = m_muted.load() && m_gain == 0.0f;
  if (unheard && m_playing && ring > m_target)
  {
    m_dropped.fetch_add(ring - m_target, std::memory_order_relaxed);
    read = (write + m_capacity - m_target) % m_capacity;
    m_stage_at = m_stage_len = 0;
    ring = level = m_target;
    std::fill(std::begin(m_levels), std::end(m_levels), level);
  }
  // The low-water mark: the lowest level of the last periods (the game's sound comes a frame at a time: the level
  // just before one comes is what runs dry).
  m_levels[m_level_at] = level;
  m_level_at = (m_level_at + 1) % LEVELS;
  if (!m_playing && level >= m_target / 2)
  {
    m_playing = true;  // half the target there: on, faded in
    m_fade_in = RAMP;
    std::fill(std::begin(m_levels), std::end(m_levels), level);
  }
  const size_t low = *std::min_element(std::begin(m_levels), std::end(m_levels));
  m_allowance = std::min(m_allowance + PERIOD * MAX_STRETCH, ALLOWANCE_MAX);
  long changed = 0;
  bool cut = false;
  if (m_playing && m_stage_at == m_stage_len && !unheard)
  {
    changed = Stretch(read, ring, low, &cut);
    if (changed != 0)
    {
      ring = (write + m_capacity - read) % m_capacity;
      level = ring + (m_stage_len - m_stage_at);
      for (size_t& l : m_levels)
        l = changed > 0 ? l + static_cast<size_t>(changed) : l - std::min(l, static_cast<size_t>(-changed));
    }
  }
  // (A cut is no rate: it doesn't count; nor does a time nothing is heard.)
  const double stretched = cut ? 0.0 : std::abs(static_cast<double>(changed));
  if (!unheard)
    m_activity += (stretched / PERIOD - m_activity) / ACTIVITY_PERIODS;
  if (!m_hold && m_activity > HOLD_ON)
    m_hold = true;
  else if (m_hold && m_activity < HOLD_OFF)
    m_hold = false;
  m_hold_rate.store(m_hold, std::memory_order_relaxed);
  if (m_playing && level < m_lowest.load(std::memory_order_relaxed))
    m_lowest.store(level, std::memory_order_relaxed);
  // The period: the staged splice first, then the ring. Running out (less than the period and its fade left), what
  // there is fades out (2 ms) and the rest is silence until half the target is there again.
  const uint32_t sound = m_playing ? static_cast<uint32_t>(std::min<size_t>(level, PERIOD)) : 0;
  const bool ends = m_playing && level < PERIOD + RAMP;
  const uint32_t fade_out = ends ? std::min(RAMP, sound) : 0;
  const float gain = m_muted ? 0.0f : static_cast<float>(m_volume.load()) / 100.0f;
  const float gain_step = (gain - m_gain) / PERIOD;  // a volume change (the mute) ramped over the period
  for (uint32_t i = 0; i < PERIOD; i++)
  {
    float l = 0.0f, r = 0.0f;
    if (i < sound)
    {
      const int16_t* f;
      if (m_stage_at < m_stage_len)
        f = &m_stage[m_stage_at++ * 2];
      else
      {
        f = &m_ring[read * 2];
        if (++read == m_capacity)
          read = 0;
      }
      float k = 1.0f;
      if (m_fade_in > 0)
        k = static_cast<float>(RAMP - m_fade_in-- + 1) / (RAMP + 1);
      if (i >= sound - fade_out)
        k *= static_cast<float>(sound - i) / (fade_out + 1);
      l = f[0] * k;
      r = f[1] * k;
    }
    const float g = m_gain + gain_step * static_cast<float>(i + 1);
    out[i * 2] = static_cast<int16_t>(std::clamp(std::lround(l * g), -32768L, 32767L));
    out[i * 2 + 1] = static_cast<int16_t>(std::clamp(std::lround(r * g), -32768L, 32767L));
  }
  m_gain = gain;
  if (ends)
  {
    m_playing = false;
    m_dry.fetch_add(1, std::memory_order_relaxed);
  }
  m_read.store(read, std::memory_order_release);
  m_staged.store(m_stage_len - m_stage_at, std::memory_order_relaxed);
  return sound;
}

void GameAudio::Loop()
{
  AudioPort::SetSoundThread("game");
  using Clock = std::chrono::steady_clock;
  constexpr uint32_t PERIOD_US = PERIOD * 1000000u / RATE;  // 5333
  std::vector<int16_t> out(PERIOD * 2);
  auto last = Clock::now();
  while (!m_stop)
  {
    const uint32_t sound = MakePeriod(out.data());
    const int result = AudioPort::Output(out.data());
    m_outputs.fetch_add(1, std::memory_order_relaxed);
    if (result >= 0)
    {
      m_played.fetch_add(sound, std::memory_order_relaxed);
      m_silent.fetch_add(PERIOD - sound, std::memory_order_relaxed);
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
  now.added = m_added.load();
  now.removed = m_removed.load();
  now.held = m_held.load();
  now.rate_ppm = m_rate_ppm.load();
  now.dry = m_dry.load();
  now.late = m_late.load();
  now.errors = m_errors.load();
  now.inserts = m_inserts.load();
  now.deletes = m_deletes.load();
  now.cuts = m_cuts.load();
  const uint32_t max_gap = m_max_gap_us.exchange(0);
  const int rate_min = m_rate_min.exchange(INT_MAX), rate_max = m_rate_max.exchange(INT_MIN);
  const size_t lowest = m_lowest.exchange(SIZE_MAX);
  const auto clock = std::chrono::steady_clock::now();
  const double real = std::chrono::duration<double>(clock - m_logged_at).count();
  const Counts was = m_logged;
  m_logged = now;
  m_logged_at = clock;
  if (seconds <= 0.0f)
    return;
  const uint64_t played = now.played - was.played, silent = now.silent - was.silent, lost = now.lost - was.lost;
  const double total = static_cast<double>(played + silent + lost);
  const auto percent = [total](uint64_t frames) {
    return total > 0.0 ? 100.0 * static_cast<double>(frames) / total : 0.0;
  };
  // The window by the PS4's sound clock (the periods it played): the core must send its rate per second of that.
  const uint64_t outputs = now.outputs - was.outputs;
  const double span = outputs > 0 ? static_cast<double>(outputs) * PERIOD / RATE : seconds;
  const uint64_t in = now.in - was.in;
  const int nominal = m_input_rate.load();
  const double sent = static_cast<double>(in) / span;
  const double of = nominal > 0 ? 100.0 * sent / nominal : 0.0;
  // The pitch: the rate control's correction, its mean over the frames it was given to and its extremes.
  const double mean = in > 0 ? static_cast<double>(now.rate_ppm - was.rate_ppm) / static_cast<double>(in) / 1e4 : 0.0;
  const double held = in > 0 ? 100.0 * static_cast<double>(now.held - was.held) / static_cast<double>(in) : 0.0;
  const double low_pct = rate_min == INT_MAX ? 0.0 : rate_min / 1e4, high_pct = rate_max == INT_MIN ? 0.0 : rate_max / 1e4;
  // The stretcher: the sound it added and took out against what was played.
  const double heard = static_cast<double>(std::max<uint64_t>(played, 1));
  const double added = 100.0 * static_cast<double>(now.added - was.added) / heard;
  const double removed = 100.0 * static_cast<double>(now.removed - was.removed) / heard;
  LogF("audio: %.1f s: the core sent %.0f Hz, %.1f%% of its %d Hz; pitch: the core's, rate control %+.2f%% "
       "(%+.2f..%+.2f%%, held at 0 for %.0f%%); stretched +%.1f%% (%u splices), shortened %.1f%% (%u, %u cuts); "
       "%.1f%% played, %.1f%% silence, %.1f%% lost, dry %u times, %llu frames dropped; %.0f ms buffered (lowest "
       "%.0f); the PS4 played %.0f Hz: %llu outputs, at most %.1f ms apart, %u late; %u errors (last %#x)%s",
       span, sent, of, nominal, mean, low_pct, high_pct, held, added, now.inserts - was.inserts, removed,
       now.deletes - was.deletes, now.cuts - was.cuts, percent(played), percent(silent), percent(lost),
       now.dry - was.dry, static_cast<unsigned long long>(now.dropped - was.dropped),
       m_capacity ? static_cast<double>(Buffered() + m_staged.load()) * 1000.0 / RATE : 0.0,
       lowest == SIZE_MAX ? 0.0 : static_cast<double>(lowest) * 1000.0 / RATE,
       real > 0.0 ? static_cast<double>(outputs) * PERIOD / real : 0.0, static_cast<unsigned long long>(outputs),
       max_gap / 1000.0, now.late - was.late, now.errors - was.errors, static_cast<unsigned>(m_last_error.load()),
       m_port < 0 ? "; no sound port" : "");
}
