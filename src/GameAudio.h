// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game's sound: Snes9x's 32 kHz stereo resampled to the PS4's 48 kHz into a ring buffer that a
// thread feeds to AudioOut. The resampling ratio follows the buffer's fill level (dynamic rate
// control, as RetroArch does), so a game running one frame per TV refresh never runs dry or over.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

class GameAudio
{
public:
  bool Start(int buffer_ms);
  void Stop();
  // Interleaved stereo samples at input_rate (Hz).
  void Push(const int16_t* samples, size_t frames, double input_rate);
  void SetVolume(int percent) { m_volume = percent; }
  void SetMuted(bool muted) { m_muted = muted; }
  void Clear();
  // Buffered frames as a fraction of the buffer (0..1).
  float Fill() const;

private:
  static constexpr uint32_t RATE = 48000;
  static constexpr uint32_t PERIOD = 256;  // frames per sceAudioOutOutput
  void Loop();
  size_t Buffered() const;

  int m_port = -1;
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_volume{100};
  std::atomic<bool> m_muted{false};
  std::vector<int16_t> m_ring;  // interleaved stereo
  size_t m_capacity = 0;        // frames
  size_t m_target = 0;          // frames the rate control aims for
  std::atomic<size_t> m_read{0}, m_write{0};
  // Resampler state (producer side).
  double m_phase = 0.0;
  int16_t m_last[2] = {};
};
