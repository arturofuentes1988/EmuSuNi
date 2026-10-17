// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game's sound: the core's samples (Snes9x 32 kHz, FCEUmm 48 kHz, Mupen64Plus 44.1 kHz)
// resampled to the PS4's 48 kHz into a ring buffer that a thread feeds to AudioOut. The resampling
// ratio follows the buffer's fill level (dynamic rate control, as RetroArch does), so a game running
// one frame per TV refresh never runs dry or over.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// The app's one AudioOut port (48 kHz, stereo, 256 frames per output), shared by the menu's sounds
// and the games' sound, which never play at the same time (each one's thread is joined before the
// other starts). It is opened the first time it is needed and kept until the app closes: the PS4
// gives an app 8 main ports, and the ones closed after each game and each menu visit never came
// back (00.15, 00.21, 00.24: the 9th open failed with 0x80260005, PORT_FULL; after the fourth game
// there was no sound at all, in the menu or in any game, N64 included).
namespace AudioPort
{
constexpr uint32_t RATE = 48000;
constexpr uint32_t PERIOD = 256;  // frames per Output (5.3 ms)
// The port (opened on first use and kept), < 0 when there is none (logged).
int Get();
// Plays PERIOD interleaved stereo frames: blocks until the period before has been handed to the
// hardware. Returns sceAudioOutOutput's result; after an error it has waited a period (a sound
// thread never spins), and after half a second of errors the port is made again (at most 3 times).
int Output(const int16_t* frames);
// Waits until the last period has been played: a sound thread's last call, so the port is clean
// for the next one.
void Drain();
// The calling sound thread above every game thread (it needs microseconds every 5.3 ms) and off the
// cores of the N64's emulation (2) and ParaLLEl-RDP's command thread (3). Logged with `who`.
void SetSoundThread(const char* who);
}  // namespace AudioPort

class GameAudio
{
public:
  bool Start(int buffer_ms);
  void Stop();
  // Interleaved stereo samples at input_rate (Hz).
  void Push(const int16_t* samples, size_t frames, double input_rate);
  void SetVolume(int percent) { m_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
  void SetMuted(bool muted) { m_muted = muted; }
  void Clear();
  // Buffered frames as a fraction of the buffer (0..1).
  float Fill() const;
  // One log line about the sound since the last call: what the core sent; how much was played, how
  // much was silence and how much was lost to AudioOut's errors; the times the buffer ran dry, late
  // outputs and the errors. A `seconds` of 0 or less starts a new window without a line (the
  // in-game menu closed).
  void LogHealth(float seconds);

private:
  static constexpr uint32_t RATE = AudioPort::RATE;
  static constexpr uint32_t PERIOD = AudioPort::PERIOD;
  void Loop();
  size_t Buffered() const;

  int m_port = -1;  // the shared port while this game's sound runs (never closed here)
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
  // Sound made faster than 1.2x the output's (the Game Boys' 131 kHz): two one-pole low-pass
  // stages per channel before it is resampled, so its high notes don't fold back into it.
  float m_low[2][2] = {};
  // Health (the log): counts since Start (the producer's and the sound thread's) and LogHealth's
  // last reading of them.
  struct Counts
  {
    uint64_t in = 0, dropped = 0, played = 0, silent = 0, lost = 0, outputs = 0;
    uint32_t dry = 0, late = 0, errors = 0;
  };
  std::atomic<uint64_t> m_in{0}, m_dropped{0}, m_played{0}, m_silent{0}, m_lost{0}, m_outputs{0};
  std::atomic<uint32_t> m_dry{0}, m_late{0}, m_errors{0}, m_max_gap_us{0};
  std::atomic<int> m_last_error{0}, m_input_rate{0};
  Counts m_logged;
};
