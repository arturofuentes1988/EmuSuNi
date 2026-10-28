// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The game's sound: the core's samples (Snes9x 32 kHz, the N64's, PSP's and Dreamcast's 44.1 kHz, the DS's and 3DS's
// 32.7 kHz, the Game Boys' 131 kHz...) resampled to the PS4's 48 kHz at the core's own rate - its own pitch - into a
// ring buffer that a thread feeds to AudioOut. Two things keep the buffer near its target, neither of which changes
// the pitch by more than RetroArch's rate control does:
//  - rate control, as RetroArch's: the resampling up to 0.5% faster or slower, for the small difference between the
//    game's clock and the PS4's (a 60.1 FPS game shown at 59.94 Hz sends 0.26% less sound than is played);
//  - a stretcher for the rest: a game below full speed (its core can't keep up) or above it (the frames owed after a
//    stall). One waveform period of the sound (2.5 to 25 ms) is played twice, or left out, where the sound matches
//    itself best, crossfaded over 6 ms: the sound lasts longer or shorter at the same pitch (its tempo follows the
//    game's). The rate control stays at 1 while it works (the pitch exact). A stall longer than it bridges is a short
//    silence, faded out and in.
// (00.39 to 00.65 played a slow game's sound that much slower instead: its pitch down by the game's speed, 2 to 11
// semitones on the Dreamcast, the PSP and the 3DS - the "deep" sound.)

#pragma once

#include <atomic>
#include <chrono>
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
  // Interleaved stereo samples at the core's own rate (Hz): never one scaled by the game's speed (that is the pitch).
  void Push(const int16_t* samples, size_t frames, double input_rate);
  void SetVolume(int percent) { m_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
  void SetMuted(bool muted) { m_muted = muted; }
  // Empties the buffer (the in-game menu closed): the sound thread does it at its next period.
  void Clear() { m_clear = true; }
  // Buffered frames as a fraction of the buffer (0..1).
  float Fill() const;
  // One log line about the sound since the last call: what the core sent against what the PS4 played (the rates),
  // the pitch (the rate control's correction) and what the stretcher did; how much was played, how much was silence
  // and how much was lost to AudioOut's errors; the times the buffer ran dry, late outputs and the errors. A
  // `seconds` of 0 or less starts a new window without a line (the in-game menu closed).
  void LogHealth(float seconds);

private:
  static constexpr uint32_t RATE = AudioPort::RATE;
  static constexpr uint32_t PERIOD = AudioPort::PERIOD;
  static constexpr int LEVELS = 8;  // periods the low-water mark looks back over (43 ms)
  void Loop();
  // The next period (PERIOD frames) from the buffer through the stretcher and the fades, at the volume; returns the
  // frames of sound in it (the rest is silence).
  uint32_t MakePeriod(int16_t* out);
  // One splice if the buffer's level calls for one (the sound thread's; none while one is being played), from the
  // read position `read` (moved past what it staged), with `ring` frames there and `low` the low-water mark: returns
  // the frames it added (> 0) or took out (< 0); `cut` says it was a cut back to the target (not a stretch).
  long Stretch(size_t& read, size_t ring, size_t low, bool* cut);
  // The offset in [0, range] at which the FADE frames from `cand_at` + offset match the FADE frames from `ref_at` best
  // (normalized cross-correlation of both channels); of near equals, `prefer` > 0 takes the higher offset, < 0 the
  // lower. Ring positions (taken modulo its capacity).
  uint32_t BestMatch(size_t ref_at, size_t cand_at, uint32_t range, int prefer);
  size_t Buffered() const;

  int m_port = -1;  // the shared port while this game's sound runs (never closed here)
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_volume{100};
  std::atomic<bool> m_muted{false};
  std::atomic<bool> m_clear{false};
  std::vector<int16_t> m_ring;  // interleaved stereo, 48 kHz
  size_t m_capacity = 0;        // frames
  size_t m_target = 0;          // frames the buffer is kept at
  std::atomic<size_t> m_read{0}, m_write{0};
  std::atomic<size_t> m_staged{0};  // frames of a splice still to play (the sound thread's, read by the producer)
  // Resampler state (producer side).
  double m_phase = 0.0;
  int16_t m_last[2] = {};
  // Sound made faster than 1.2x the output's (the Game Boys' 131 kHz): two one-pole low-pass
  // stages per channel before it is resampled, so its high notes don't fold back into it.
  float m_low[2][2] = {};
  // The stretcher and the fades (the sound thread's): a splice's frames, played before the ring's; the buffer's level
  // at the last periods; how much it may still add or take out (at most half the sound, 50 ms of it ahead); the share
  // of the sound it changed lately (about 2 s), which holds the rate control at 1 above 1%.
  std::vector<int16_t> m_stage;
  size_t m_stage_at = 0, m_stage_len = 0;
  std::vector<float> m_match;    // BestMatch's frames
  std::vector<double> m_energy;  // and their running sums of squares
  size_t m_levels[LEVELS] = {};
  int m_level_at = 0;
  double m_allowance = 0.0;
  double m_activity = 0.0;
  bool m_hold = false;
  std::atomic<bool> m_hold_rate{false};
  bool m_playing = false;  // false: silence until half the target is buffered (the start, after running dry)
  uint32_t m_fade_in = 0;  // frames of the fade-in still to come
  float m_gain = 0.0f;     // the volume reached (a change is ramped over a period)
  // Health (the log): counts since Start (the producer's and the sound thread's) and LogHealth's
  // last reading of them.
  struct Counts
  {
    uint64_t in = 0, dropped = 0, played = 0, silent = 0, lost = 0, outputs = 0, added = 0, removed = 0, held = 0;
    int64_t rate_ppm = 0;  // the rate control's correction (millionths) times the frames it was given to
    uint32_t dry = 0, late = 0, errors = 0, inserts = 0, deletes = 0, cuts = 0;
  };
  std::atomic<uint64_t> m_in{0}, m_dropped{0}, m_played{0}, m_silent{0}, m_lost{0}, m_outputs{0};
  std::atomic<uint64_t> m_added{0}, m_removed{0}, m_held{0};
  std::atomic<int64_t> m_rate_ppm{0};
  std::atomic<int> m_rate_min{0}, m_rate_max{0};  // the correction's extremes since the last line (millionths)
  std::atomic<uint32_t> m_dry{0}, m_late{0}, m_errors{0}, m_max_gap_us{0}, m_inserts{0}, m_deletes{0}, m_cuts{0};
  std::atomic<size_t> m_lowest{0};  // the lowest level while playing since the last line (frames)
  std::atomic<int> m_last_error{0}, m_input_rate{0};
  Counts m_logged;
  std::chrono::steady_clock::time_point m_logged_at{};
};
