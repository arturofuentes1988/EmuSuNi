// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameAudio.h"

#include <algorithm>
#include <cstring>

#include <orbis/AudioOut.h>

#include "Log.h"

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
  sceAudioOutInit();
  m_port = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, PERIOD, RATE,
                           ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
  if (m_port < 0)
  {
    LogF("audio: sceAudioOutOpen failed (%#x)", static_cast<unsigned>(m_port));
    return false;
  }
  int32_t levels[2] = {32768, 32768};
  sceAudioOutSetVolume(m_port, 0x3, levels);
  try
  {
    m_thread = std::thread([this] { Loop(); });
  }
  catch (...)
  {
    LogLine("audio: the sound thread couldn't be made (no sound)");
    sceAudioOutClose(m_port);
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
  if (m_port >= 0)
    sceAudioOutClose(m_port);
  m_port = -1;
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
  if (m_port < 0 || frames == 0 || input_rate <= 0.0)
    return;
  // Rate control: up to 0.5% faster or slower output to keep the buffer near its target.
  const double fill = static_cast<double>(Buffered()) / static_cast<double>(m_target);
  const double adjust = 1.0 + 0.005 * (fill - 1.0);
  const double step = input_rate / RATE * std::clamp(adjust, 0.99, 1.01);
  size_t write = m_write.load(std::memory_order_relaxed);
  const size_t read = m_read.load(std::memory_order_acquire);
  size_t space = (read + m_capacity - write - 1) % m_capacity;
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
      m_phase -= 1.0;
    else
      m_phase = 0.0;  // dropped
    m_last[0] = l;
    m_last[1] = r;
  }
  m_write.store(write, std::memory_order_release);
}

void GameAudio::Loop()
{
  std::vector<int16_t> out(PERIOD * 2);
  bool started = false;
  while (!m_stop)
  {
    // Start playing once half the target is there (no crackle at the start).
    const size_t available = Buffered();
    if (!started && available >= m_target / 2)
      started = true;
    const int volume = m_muted ? 0 : m_volume.load();
    size_t read = m_read.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < PERIOD; i++)
    {
      int16_t l = 0, r = 0;
      if (started && read != m_write.load(std::memory_order_acquire))
      {
        l = m_ring[read * 2];
        r = m_ring[read * 2 + 1];
        read = (read + 1) % m_capacity;
      }
      out[i * 2] = static_cast<int16_t>(l * volume / 100);
      out[i * 2 + 1] = static_cast<int16_t>(r * volume / 100);
    }
    m_read.store(read, std::memory_order_release);
    if (started && Buffered() == 0)
      started = false;  // ran dry: wait for some sound again
    sceAudioOutOutput(m_port, out.data());
  }
}
