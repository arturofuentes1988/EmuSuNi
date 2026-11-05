// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Voice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

#include <opus.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
#include <pthread.h>

#include "Log.h"
#include "Settings.h"

// libSceAudioIn and the pad's state, declared here: the OpenOrbis headers give AudioIn no parameters. The types and
// formats are the SDK's (SCE_AUDIO_IN_TYPE_VOICE_CHAT 0, _GENERAL 1; SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO 0,
// _S16_STEREO 2), the parameters and error codes as shadPS4's libSceAudioIn has them (audioin.h, audioin_error.h):
// sceAudioInOpen(user, type, index, samples per input, 16000 or 48000 Hz, format) gives a handle;
// sceAudioInInput(handle, buffer) waits for the next samples.
extern "C" {
int sceAudioInOpen(int32_t user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioInHqOpen(int32_t user_id, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioInInput(int32_t handle, void* dest);
int sceAudioInClose(int32_t handle);
int sceAudioInGetSilentState(int32_t handle);
int scePadGetHandle(int32_t user_id, uint32_t type, uint32_t index);
int scePadReadState(int32_t handle, void* data);
}
// libkernel's affinity call as it really is (as GameAudio.cpp's): bit n of the mask is core n.
extern "C" int emusuni_voice_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");

namespace Voice
{
namespace
{
using Clock = std::chrono::steady_clock;

// The sound: 16 kHz mono from the microphone into the encoder (wideband speech), 20 ms a packet; the friends' voices
// decoded at 48 kHz, AudioPort's rate (Opus makes it itself: no resampler of ours).
constexpr int IN_RATE = 16000;
constexpr int FRAME = 320;  // 20 ms at 16 kHz: one packet
constexpr int OUT_FRAME = 960;  // 20 ms at 48 kHz
constexpr uint32_t GRAIN = 256;  // the microphone's samples per sceAudioInInput (16 ms at 16 kHz)
// Opus (fixed-point, scripts/build-opus.sh): speech (VOIP), 20 kbps, in-band FEC for 10% loss, DTX. Complexity 5: on
// the PC (btver2 code) 87 us an encode and 10 a decode; 10 would be 167 for no audible gain at this rate. DTX is Opus'
// own voice detector after the gate: a frame it takes for noise (a fan, a hum, even a steady tone) or its comfort noise
// is not sent either (OPUS_GET_IN_DTX); settings.ini voice_dtx=off sends everything the gate lets through.
constexpr int BITRATE = 20000;
constexpr int COMPLEXITY = 5;
constexpr int LOSS_PERCENT = 10;
constexpr int MAX_PACKET = 400;  // bytes (20 kbps makes about 50)
constexpr size_t SEND_QUEUE = 25;  // packets waiting for netplay: half a second, the oldest dropped after it
// The friends: up to 8, and Test Microphone's playback (the last place), which goes the same way as a friend's voice.
constexpr int MAX_PEERS = 8;
constexpr int TEST_PEER = -2;
constexpr int PLACES = MAX_PEERS + 1;
constexpr int SLOTS = 32;  // a friend's jitter buffer: 0.64 s of packets
// The buffer's delay in packets (playing starts with that many there, or once the first waited that long): 40 to 200
// ms, from the jitter of the last 100 packets (the 95th percentile of their transit times' spread); 60 ms until then.
constexpr int MIN_TARGET = 2;
constexpr int START_TARGET = 3;
constexpr int MAX_TARGET = 10;
constexpr int TRANSITS = 100;
constexpr int OVERFULL = 4;  // packets over the target: the oldest let go (the friend's clock faster, a stall)
constexpr int TAIL = 2;  // packets concealed after a friend's last one, before silence (the end of a talk)
constexpr int NEW_TALK = 10;  // a gap of more packets than this before the next one there: a new talk, not a loss
constexpr int64_t TALKING_MS = 300;
constexpr float TALKING_DB = -45.0f;  // a decoded packet louder than this is "speaking"
// The noise gate (Open Mic): open while a 20 ms frame is 10 dB over the room's noise and over -48 dBFS (RMS); closed
// 4 dB under that, 400 ms after the last loud frame (the words' ends and short pauses go with them). Push to talk:
// open while the button is held, and 200 ms after.
constexpr float GATE_MIN_DB = -48.0f;
constexpr float GATE_ABOVE_NOISE = 10.0f;
constexpr float GATE_CLOSE_DB = 4.0f;
constexpr int GATE_HANG = 20;
constexpr int PTT_HANG = 10;
constexpr int FADE_IN = 80;  // samples (5 ms) at the gate's opening
// Test Microphone: 5 s recorded, then played back (and half a second for its end).
constexpr int TEST_FRAMES = 250;
constexpr int TEST_TAIL = 25;
// The capture thread: above the games' threads (480), below the sound threads (400); off the cores of the emulation
// (2) and ParaLLEl-RDP's command thread (3), as AudioPort's.
constexpr int CAPTURE_PRIORITY = 450;
constexpr uint64_t CAPTURE_CORES = 0x73;

// sceAudioIn's types and formats (the SDK's values).
constexpr uint32_t TYPE_VOICE_CHAT = 0;
constexpr uint32_t TYPE_GENERAL = 1;
constexpr uint32_t FORMAT_MONO = 0;
constexpr uint32_t FORMAT_STEREO = 2;
// The microphone's opens tried in turn, each logged: the voice chat port at 16 kHz (what the PS4's voice chat uses), a
// general one, at 48 kHz (taken to 16 kHz here), and the high-quality open.
struct Config
{
  const char* name;
  bool hq;
  uint32_t type;
  uint32_t rate;
  uint32_t format;
};
constexpr Config CONFIGS[] = {
    {"voice chat, 16000 Hz, mono", false, TYPE_VOICE_CHAT, 16000, FORMAT_MONO},
    {"general, 16000 Hz, mono", false, TYPE_GENERAL, 16000, FORMAT_MONO},
    {"general, 48000 Hz, mono", false, TYPE_GENERAL, 48000, FORMAT_MONO},
    {"general, 48000 Hz, stereo (high quality)", true, TYPE_GENERAL, 48000, FORMAT_STEREO},
};

// The pad's buttons (ORBIS_PAD_BUTTON_*) push to talk can be on.
constexpr uint32_t PAD_L3 = 0x0002;
constexpr uint32_t PAD_R3 = 0x0004;
constexpr uint32_t PAD_L2 = 0x0100;

// Settings -> Voice Chat -> Microphone (settings.ini voice_mic). Push to talk on a button no console's pad has, the
// stick's press: L3 by default (the DS blows into its microphone with it, the 3DS's SELECT, the Arcade's button 10),
// R3 (the DS's and the 3DS's screen swap), or L2 (a game that uses it - the N64's Z, the PlayStation's L2 - gets it
// too).
struct Mode
{
  const char* value;
  const char* label;
  const char* button;
  uint32_t mask;
  const char* help;
};
const Mode MODES[] = {
    {"open", "Open Mic", "", 0,
     "Your voice is sent whenever you talk; the room's noise is left out (a noise gate)."},
    {"l3", "Push to Talk: L3", "L3", PAD_L3,
     "Hold L3 (press the left stick) while you talk. No console's pad has it; on the DS it also blows into the DS's "
     "microphone."},
    {"r3", "Push to Talk: R3", "R3", PAD_R3,
     "Hold R3 (press the right stick) while you talk. On the DS and the 3DS it also swaps the screens."},
    {"l2", "Push to Talk: L2", "L2", PAD_L2,
     "Hold L2 while you talk. A game that uses L2 (the N64's Z, the PlayStation's L2) gets it too."},
};
constexpr int MODE_COUNT = static_cast<int>(sizeof(MODES) / sizeof(MODES[0]));
constexpr int MIC_VOLUMES[] = {25, 50, 75, 100, 150, 200, 300, 400};
constexpr int VOICE_VOLUMES[] = {0, 25, 50, 75, 100, 125, 150, 200};

int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

float Db(double mean_square)
{
  return static_cast<float>(10.0 * std::log10(mean_square / (32768.0 * 32768.0) + 1e-10));
}

const char* AudioInError(int code)
{
  if (code >= 0)
    return "ok";
  switch (static_cast<uint32_t>(code))
  {
  case 0x80260101: return "invalid handle";
  case 0x80260102: return "invalid size";
  case 0x80260103: return "invalid frequency";
  case 0x80260104: return "invalid type";
  case 0x80260105: return "invalid pointer";
  case 0x80260106: return "invalid parameter";
  case 0x80260107: return "port full: every microphone port is in use";
  case 0x80260108: return "out of memory";
  case 0x80260109: return "not opened: no microphone for this user?";
  case 0x8026010A: return "busy";
  case 0x8026010B: return "system memory";
  case 0x8026010C: return "system IPC";
  default: return "unknown error";
  }
}

// sceAudioInGetSilentState's bits (shadPS4's names: 1 no device, 2 priority low, 8 unable format; 4 is taken for the
// user's mute, its value missing there).
std::string SilentText(int state)
{
  if (state < 0)
    return AudioInError(state);
  if (state == 0)
    return "sound coming";
  std::string text;
  const auto add = [&](const char* what) { text += (text.empty() ? "" : ", ") + std::string(what); };
  if (state & 1)
    add("no microphone connected");
  if (state & 2)
    add("another app has the microphone");
  if (state & 4)
    add("muted by the user?");
  if (state & 8)
    add("format not supported");
  if (state & ~0xF)
    add("other bits");
  return text;
}

// --- The settings -----------------------------------------------------------------------------------------------
std::atomic<bool> s_enabled{true};
std::atomic<int> s_mode{0};
std::atomic<int> s_mic_volume{100};
std::atomic<int> s_voices_volume{100};
std::atomic<bool> s_mic_muted{false};
std::atomic<bool> s_sending{true};  // netplay's (SetSending)

int Index(const int* values, int count, int value)
{
  for (int i = 0; i < count; i++)
  {
    if (values[i] == value)
      return i;
  }
  return -1;
}

// --- The friends' voices (the jitter buffers: s_peers_mutex's; the decoders and what they made: the sound thread's) --
struct Packet
{
  uint16_t sequence = 0;
  uint16_t size = 0;  // 0: empty
  int64_t arrived = 0;
  uint8_t data[MAX_PACKET];
};

enum class Play
{
  Idle,       // nothing to play: silence (between two talks)
  Buffering,  // a talk's first packets: waiting for the delay
  Playing,
};

enum class Action
{
  None,
  Decode,
  Fec,  // the next packet's copy of a lost one (in-band FEC)
  Plc,  // Opus' concealment
};

struct Peer
{
  // s_peers_mutex's.
  int id = 0;
  bool used = false;
  uint32_t generation = 0;  // a new friend in this place: the sound thread starts over (decoder, what it holds)
  std::array<Packet, SLOTS> slots;
  bool any = false;
  uint16_t newest = 0;
  int64_t newest_count = 0;  // the newest sequence, unwrapped (for the transit times)
  int64_t transit[TRANSITS] = {};
  int transit_count = 0, transit_at = 0;
  Play play = Play::Idle;
  uint16_t next = 0;  // the next sequence to play
  int target = START_TARGET;
  int missing = 0;
  int64_t buffering_since = 0;
  bool late_lately = false;  // a packet came after its time: one more packet of delay at the next talk
  uint32_t played_generation = 0;  // (the sound thread's, written under the lock)
  // Counts since the last log line (the lock's).
  uint32_t received = 0, late = 0, duplicates = 0, played = 0, lost = 0, fec = 0, plc = 0, tail = 0, skipped = 0,
           talks = 0, restarts = 0;
  // The sound thread's alone.
  OpusDecoder* decoder = nullptr;
  int16_t pcm[OUT_FRAME * 2] = {};
  int pcm_at = 0, pcm_len = 0;
  // Read anywhere.
  std::atomic<int> public_id{INT32_MIN};
  std::atomic<int64_t> talking_until{0};
  std::atomic<float> level{-100.0f};  // the last packet played (Test Microphone's meter)
  std::atomic<uint32_t> decode_us{0}, decode_max_us{0}, decodes{0};
};

std::mutex s_peers_mutex;
std::array<std::atomic<Peer*>, PLACES> s_peers{};
std::atomic<bool> s_any_peer{false};
std::vector<int> s_muted;  // the friends muted (SetPeerMuted; the lock's)
int64_t s_peers_logged = 0;  // (the lock's)
uint32_t s_bad_packets = 0;

Packet* Slot(Peer& p, uint16_t sequence)
{
  Packet& slot = p.slots[sequence % SLOTS];
  return slot.size != 0 && slot.sequence == sequence ? &slot : nullptr;
}

void ClearSlots(Peer& p)
{
  for (Packet& slot : p.slots)
    slot.size = 0;
}

// A friend's place (the lock held): theirs, else a free one (made the first time, with its decoder: never let go, so
// the sound thread never decodes with a freed one). nullptr when there is no room (or no memory).
Peer* FindPeerLocked(int id, bool make)
{
  const int first = id == TEST_PEER ? MAX_PEERS : 0, last = id == TEST_PEER ? PLACES : MAX_PEERS;
  for (int i = first; i < last; i++)
  {
    Peer* p = s_peers[i].load(std::memory_order_relaxed);
    if (p && p->used && p->id == id)
      return p;
  }
  if (!make)
    return nullptr;
  for (int i = first; i < last; i++)
  {
    Peer* p = s_peers[i].load(std::memory_order_relaxed);
    if (p && p->used)
      continue;
    if (!p)
    {
      p = new (std::nothrow) Peer;
      int error = 0;
      OpusDecoder* decoder = p ? opus_decoder_create(48000, 1, &error) : nullptr;
      if (!decoder)
      {
        LogF("voice: no memory for a friend's voice (decoder %d)", error);
        delete p;
        return nullptr;
      }
      p->decoder = decoder;
      s_peers[i].store(p, std::memory_order_release);
    }
    p->used = true;
    p->id = id;
    p->generation++;
    p->any = false;
    p->play = Play::Idle;
    p->target = START_TARGET;
    p->transit_count = p->transit_at = 0;
    p->late_lately = false;
    ClearSlots(*p);
    p->public_id.store(id);
    p->talking_until.store(0);
    return p;
  }
  return nullptr;
}

// The delay for a talk that starts (the lock held): the jitter of the last packets (their transit times' spread up to
// the 95th percentile) and 10 ms, in packets, one more for the decoding's place in the sound's periods.
int TargetLocked(Peer& p)
{
  int target = START_TARGET;
  if (p.transit_count >= 20)
  {
    int64_t sorted[TRANSITS];
    std::copy(p.transit, p.transit + p.transit_count, sorted);
    std::sort(sorted, sorted + p.transit_count);
    const int64_t spread = sorted[(p.transit_count - 1) * 95 / 100] - sorted[0];
    target = static_cast<int>((spread + 10 + 19) / 20) + 1;
  }
  if (p.late_lately)
    target = std::max(target, p.target + 1);
  p.late_lately = false;
  return std::clamp(target, MIN_TARGET, MAX_TARGET);
}

// What the friend's next 20 ms are made from (the lock held): a packet, the next one's FEC copy of a lost one, Opus'
// concealment, or nothing (between talks). `data` gets the packet's bytes.
Action TickLocked(Peer& p, int64_t now, uint8_t* data, int* size)
{
  if (p.play != Play::Playing)
  {
    // Between talks, or a talk's first packets: its oldest packet there (from `next` on: a packet played or let go
    // leaves its slot empty; one older than that came too late). Reordered ones may still come before it plays.
    if (!p.any)
      return Action::None;
    const Packet* oldest = nullptr;
    for (int back = SLOTS - 1; back >= 0 && !oldest; back--)
    {
      const uint16_t sequence = static_cast<uint16_t>(p.newest - back);
      if (static_cast<int16_t>(sequence - p.next) >= 0)
        oldest = Slot(p, sequence);
    }
    if (!oldest)
      return Action::None;
    if (p.play == Play::Idle)
    {
      p.play = Play::Buffering;
      p.target = TargetLocked(p);
      p.buffering_since = oldest->arrived;
      p.talks++;
    }
    // Playing once the delay's packets are there, or the first one waited as long.
    const int depth = static_cast<int16_t>(p.newest - oldest->sequence) + 1;
    if (depth < p.target && now - p.buffering_since < p.target * 20)
      return Action::None;
    p.play = Play::Playing;
    p.next = oldest->sequence;
    p.missing = 0;
  }
  // Nothing for the next 200 ms and more, then packets: a new talk came while the last one's end was concealed. It
  // starts with its own delay (as after silence), not after 200 ms of concealment.
  if (!Slot(p, p.next))
  {
    int ahead = 1;
    while (ahead < SLOTS && static_cast<int16_t>(p.newest - p.next) >= ahead &&
           !Slot(p, static_cast<uint16_t>(p.next + ahead)))
      ahead++;
    if (ahead > NEW_TALK && ahead < SLOTS && Slot(p, static_cast<uint16_t>(p.next + ahead)))
    {
      p.play = Play::Idle;
      return TickLocked(p, now, data, size);
    }
  }
  // Far more buffered than the delay (the sound threads stopped a while - a game starting -, or the friend's clock
  // runs faster): the oldest let go, back to the delay.
  const int depth = static_cast<int16_t>(p.newest - p.next) + 1;
  if (depth > p.target + OVERFULL)
  {
    const int drop = depth - p.target;
    for (int i = 0; i < drop; i++)
    {
      if (Packet* old = Slot(p, static_cast<uint16_t>(p.next + i)))
        old->size = 0;
    }
    p.next = static_cast<uint16_t>(p.next + drop);
    p.skipped += drop;
  }
  Action action;
  if (Packet* packet = Slot(p, p.next))
  {
    std::memcpy(data, packet->data, packet->size);
    *size = packet->size;
    packet->size = 0;
    p.missing = 0;
    action = Action::Decode;
  }
  else if (static_cast<int16_t>(p.newest - p.next) > 0)
  {
    // Lost (a later one is there): the next packet's FEC copy when it came, else concealed.
    p.lost++;
    if (Packet* after = Slot(p, static_cast<uint16_t>(p.next + 1)))
    {
      std::memcpy(data, after->data, after->size);
      *size = after->size;
      p.fec++;
      action = Action::Fec;
    }
    else
    {
      p.plc++;
      action = Action::Plc;
    }
  }
  else
  {
    // Nothing newer: the talk ended (or the rest is late). A little concealment, then silence.
    if (++p.missing > TAIL)
    {
      p.play = Play::Idle;
      return Action::None;
    }
    p.tail++;
    action = Action::Plc;
  }
  p.next++;
  p.played++;
  return action;
}

// A packet into a friend's jitter buffer (any thread but a sound thread's).
void Give(int id, uint16_t sequence, const uint8_t* data, size_t size)
{
  const int64_t now = NowMs();
  std::string lines;
  {
    std::lock_guard lock(s_peers_mutex);
    if (!data || size == 0 || size > MAX_PACKET)
    {
      s_bad_packets++;
      return;
    }
    if (std::find(s_muted.begin(), s_muted.end(), id) != s_muted.end())
      return;  // (muted: nothing of theirs kept)
    Peer* p = FindPeerLocked(id, true);
    if (!p)
      return;
    p->received++;
    if (!p->any)
    {
      // A new friend: the packets just before this one may still come (reordered).
      p->any = true;
      p->newest = sequence;
      p->next = static_cast<uint16_t>(sequence - (SLOTS - 1));
      p->newest_count = 0;
    }
    else
    {
      const int ahead = static_cast<int16_t>(sequence - p->newest);
      const int from_next = static_cast<int16_t>(sequence - p->next);
      if (from_next < 0 && from_next >= -SLOTS)
      {
        // Its time to play has passed (or its talk ended without it): dropped, and the next talk waits longer.
        p->late++;
        p->late_lately = true;
        return;
      }
      if (from_next < -SLOTS || (ahead >= SLOTS && p->play != Play::Idle))
      {
        // Far behind (the friend started over: a new session) or far ahead of what plays (the sound stopped for
        // a long while): start again from this one.
        ClearSlots(*p);
        p->play = Play::Idle;
        p->newest = sequence;
        p->next = static_cast<uint16_t>(sequence - (SLOTS - 1));
        p->transit_count = p->transit_at = 0;
        p->restarts++;
      }
    }
    Packet& slot = p->slots[sequence % SLOTS];
    if (slot.size != 0 && slot.sequence == sequence)
    {
      p->duplicates++;
      return;
    }
    const int ahead = static_cast<int16_t>(sequence - p->newest);
    const int64_t count = p->newest_count + ahead;
    if (ahead > 0)
    {
      p->newest = sequence;
      p->newest_count = count;
    }
    slot.sequence = sequence;
    slot.size = static_cast<uint16_t>(size);
    slot.arrived = now;
    std::memcpy(slot.data, data, size);
    // The transit time (relative): the arrival against the sequence's 20 ms.
    p->transit[p->transit_at] = now - count * 20;
    p->transit_at = (p->transit_at + 1) % TRANSITS;
    p->transit_count = std::min(p->transit_count + 1, TRANSITS);
    s_any_peer.store(true, std::memory_order_release);
    // Every 10 s while voices come: a line per friend (written after the lock).
    if (id != TEST_PEER && now - s_peers_logged >= 10000)
    {
      const double seconds = s_peers_logged ? (now - s_peers_logged) / 1000.0 : 0.0;
      s_peers_logged = now;
      for (const auto& place : s_peers)
      {
        Peer* q = place.load(std::memory_order_relaxed);
        if (!q || !q->used || q->id == TEST_PEER || seconds <= 0.0)
          continue;
        const uint32_t decodes = q->decodes.exchange(0);
        const uint32_t decode_us = q->decode_us.exchange(0), decode_max = q->decode_max_us.exchange(0);
        char line[512];
        snprintf(line, sizeof(line),
                 "voice: friend %d, %.0f s: %u packets came (%u late, %u twice), %u played (%u lost: %u from FEC, %u "
                 "concealed), %u concealed at talks' ends, %u let go, %u talks, %u restarts; delay %d packets (%d "
                 "ms); decoding %.0f us average, %u at most%s\n",
                 q->id, seconds, q->received, q->late, q->duplicates, q->played, q->lost, q->fec, q->plc, q->tail,
                 q->skipped, q->talks, q->restarts, q->target, q->target * 20,
                 decodes ? static_cast<double>(decode_us) / decodes : 0.0, decode_max,
                 s_bad_packets ? "; bad packets came" : "");
        lines += line;
        q->received = q->late = q->duplicates = q->played = q->lost = q->fec = q->plc = q->tail = q->skipped =
            q->talks = q->restarts = 0;
      }
    }
  }
  for (size_t start = 0; start < lines.size();)
  {
    const size_t end = lines.find('\n', start);
    LogLine(lines.substr(start, end - start));
    start = end + 1;
  }
}

// What a friend's place holds let go (the lock held): the sound thread starts it over (its decoder reset, what it had
// decoded dropped) at its next period.
void ResetPeerLocked(Peer& p)
{
  p.any = false;
  p.play = Play::Idle;
  p.generation++;
  ClearSlots(p);
  p.talking_until.store(0);
  p.level.store(-100.0f);
}

void Forget(int id)
{
  std::lock_guard lock(s_peers_mutex);
  s_muted.erase(std::remove(s_muted.begin(), s_muted.end(), id), s_muted.end());
  Peer* p = FindPeerLocked(id, false);
  if (!p)
    return;
  if (id != TEST_PEER)
    LogF("voice: friend %d gone (since the last line: %u packets came, %u played, %u lost, %u late)", id, p->received,
         p->played, p->lost, p->late);
  p->received = p->late = p->duplicates = p->played = p->lost = p->fec = p->plc = p->tail = p->skipped = p->talks =
      p->restarts = 0;
  p->used = false;
  ResetPeerLocked(*p);
  p->public_id.store(INT32_MIN);
}

// --- The microphone ----------------------------------------------------------------------------------------------
std::mutex s_control;  // Start, Stop and the test: the microphone opened and closed
bool s_session = false;
bool s_module_tried = false;
uint32_t s_module = 0;
int s_handle = -1;
Config s_config{};
int32_t s_user = -1;
pthread_t s_thread{};
bool s_thread_made = false;
std::atomic<bool> s_capturing{false};
std::atomic<bool> s_thread_done{true};
std::atomic<uint32_t> s_capture_generation{0};
OpusEncoder* s_encoder = nullptr;
std::atomic<float> s_level_db{-100.0f};  // the last frame's level (after Mic Volume)
std::atomic<bool> s_gate{false};
std::atomic<int64_t> s_talk_until{0};
std::atomic<int> s_silent_state{0};

// Our packets for netplay.
struct Outgoing
{
  uint16_t sequence;
  uint8_t size;
  uint8_t data[255];
};
std::mutex s_send_mutex;
std::deque<Outgoing> s_send;
std::atomic<uint32_t> s_not_taken{0};

// Test Microphone (s_test_mutex's; the capture thread records and plays it back, the menu reads it).
enum class TestState
{
  Off,
  Recording,
  Playing,
  Done,
  Failed,
};
struct Test
{
  TestState state = TestState::Off;
  int frames = 0;  // recorded, then played back
  std::vector<std::pair<int, std::vector<uint8_t>>> packets;  // (frame, packet)
  std::vector<uint16_t> sequences;
  int sent = 0;  // frames the gate let through
  size_t bytes = 0;
  float loudest = -100.0f, peak = -100.0f, noise = -100.0f;
  double level_sum = 0.0;
  bool finished = false;  // its end seen (PanelUpdate: the microphone closed when no session has it)
  std::string result;
};
std::mutex s_test_mutex;
Test s_test;
std::atomic<int> s_test_state{0};  // TestState, for a quick look

void SetTestState(TestState state)
{
  s_test.state = state;
  s_test_state.store(static_cast<int>(state));
}

std::string Hex(int value)
{
  char text[16];
  snprintf(text, sizeof(text), "%#x", static_cast<unsigned>(value));
  return text;
}

// 48 kHz to 16 kHz: a 31-tap low-pass at 7 kHz (Blackman-windowed sinc), then every third sample.
struct Decimator
{
  static constexpr int TAPS = 31;
  float taps[TAPS];
  float history[TAPS] = {};
  int at = 0, phase = 0;
  Decimator()
  {
    constexpr double PI = 3.14159265358979323846;
    const double cut = 7000.0 / 48000.0;
    double sum = 0.0;
    for (int n = 0; n < TAPS; n++)
    {
      const double m = n - (TAPS - 1) / 2.0;
      const double sinc = m == 0.0 ? 2.0 * cut : std::sin(2.0 * PI * cut * m) / (PI * m);
      const double window =
          0.42 - 0.5 * std::cos(2.0 * PI * n / (TAPS - 1)) + 0.08 * std::cos(4.0 * PI * n / (TAPS - 1));
      taps[n] = static_cast<float>(sinc * window);
      sum += taps[n];
    }
    for (float& tap : taps)
      tap = static_cast<float>(tap / sum);
  }
  bool Push(float x, float* out)
  {
    history[at] = x;
    at = (at + 1) % TAPS;
    if (++phase < 3)
      return false;
    phase = 0;
    float y = 0.0f;
    for (int k = 0; k < TAPS; k++)
      y += taps[k] * history[(at + k) % TAPS];
    *out = y;
    return true;
  }
};

// The capture thread's own state.
struct Capture
{
  uint32_t generation = 0;
  float floor_db = -60.0f;  // the room's noise
  bool open = false;  // the gate
  int hang = 0;
  uint16_t sequence = 0;
  int pad = -1;
  int64_t pad_retry = 0;
  int pad_logged_mode = -1;
  // Counts for the log (every 10 s, and at the end).
  int64_t logged_at = 0;
  uint32_t inputs = 0, errors = 0, waits = 0, frames = 0, gate_frames = 0, sent = 0, dtx = 0, encode_errors = 0;
  uint64_t bytes = 0;
  int last_error = 0;
  double level_sum = 0.0;
  float loudest = -100.0f;
  uint64_t encode_us = 0;
  uint32_t encode_max_us = 0;
};

// Push to talk: the button held on the user's controller (its state read here: the menu's and the game's reading of
// the pad is left as it is).
bool PadHeld(Capture& c, int mode)
{
  const int64_t now = NowMs();
  if (c.pad < 0 && now >= c.pad_retry)
  {
    c.pad = scePadGetHandle(s_user, 0, 0);
    c.pad_retry = now + 1000;
    if (c.pad_logged_mode != mode)
    {
      c.pad_logged_mode = mode;
      LogF("voice: push to talk on %s: the controller of user %#x is %#x%s", MODES[mode].button,
           static_cast<unsigned>(s_user), static_cast<unsigned>(c.pad),
           c.pad < 0 ? " (none yet: tried every second)" : "");
    }
  }
  if (c.pad < 0)
    return false;
  // (ScePadData: the buttons first; the rest of it fits.)
  uint32_t state[64] = {};
  if (scePadReadState(c.pad, state) < 0)
  {
    c.pad = -1;
    return false;
  }
  return (state[0] & MODES[mode].mask) != 0;
}

// Our sound to the friends (or to the test's recording).
// (The send switches are read under the queue's lock, which SetSending and SetMicMuted take to empty it: no packet
// gets in after they turn sending off.)
void Deliver(uint16_t sequence, const uint8_t* data, int size)
{
  {
    std::lock_guard lock(s_test_mutex);
    if (s_test.state == TestState::Recording)
    {
      s_test.packets.emplace_back(s_test.frames, std::vector<uint8_t>(data, data + size));
      s_test.sequences.push_back(sequence);
      s_test.bytes += size;
      return;
    }
  }
  std::lock_guard lock(s_send_mutex);
  if (!s_enabled.load() || !s_sending.load() || s_mic_muted.load() || size > 255)
    return;
  s_talk_until.store(NowMs() + TALKING_MS);
  if (s_send.size() >= SEND_QUEUE)
  {
    s_send.pop_front();
    s_not_taken++;
  }
  Outgoing out;
  out.sequence = sequence;
  out.size = static_cast<uint8_t>(size);
  std::memcpy(out.data, data, size);
  s_send.push_back(out);
}

// Test Microphone, each 20 ms of the microphone: the recording's counts, its end (the result), the playback (one packet
// handed to the test's "friend" each 20 ms, at the microphone's pace) and its end.
void TestFrame(float level, bool sent)
{
  std::lock_guard lock(s_test_mutex);
  if (s_test.state == TestState::Recording)
  {
    s_test.frames++;
    s_test.level_sum += level;
    s_test.loudest = std::max(s_test.loudest, level);
    s_test.sent += sent ? 1 : 0;
    if (s_test.frames < TEST_FRAMES)
      return;
    const float average = static_cast<float>(s_test.level_sum / s_test.frames);
    const int silent = s_silent_state.load();
    const int mode = s_mode.load();
    char text[256];
    if (s_test.loudest < -75.0f && (silent & 1))
      snprintf(text, sizeof(text), "No microphone found: plug a headset into the controller, or connect a USB "
               "microphone or the PlayStation Camera.");
    else if (s_test.loudest < -75.0f)
      snprintf(text, sizeof(text), "Only silence came from the microphone (loudest %.0f dB): is it plugged in, and not "
               "muted on the PS4 (Settings -> Devices -> Audio Devices)?", s_test.loudest);
    else if (s_test.sent == 0 && mode != 0)
      snprintf(text, sizeof(text), "The microphone works (loudest %.0f dB), but nothing was sent: hold %s while you "
               "talk.", s_test.loudest, MODES[mode].button);
    else if (s_test.sent == 0)
      snprintf(text, sizeof(text), "Sound came (loudest %.0f dB), but not loud enough to be sent: raise Mic Volume, or "
               "talk closer to the microphone.", s_test.loudest);
    else if (s_test.peak > -1.0f)
      snprintf(text, sizeof(text), "Your voice was sent for %.1f s of 5 (loudest %.0f dB), but it clipped: lower Mic "
               "Volume. Played back as your friends will hear it.", s_test.sent * 0.02, s_test.loudest);
    else if (s_test.loudest < -35.0f)
      snprintf(text, sizeof(text), "Your voice was sent for %.1f s of 5 (loudest %.0f dB), but quietly: raise Mic "
               "Volume. Played back as your friends will hear it.", s_test.sent * 0.02, s_test.loudest);
    else
      snprintf(text, sizeof(text), "Your voice was sent for %.1f s of 5 (loudest %.0f dB). Played back as your friends "
               "will hear it.", s_test.sent * 0.02, s_test.loudest);
    s_test.result = text;
    LogF("voice: test: recorded 5 s: %d packets (%.1f s of sound sent, %zu bytes), level %.0f dB average, %.0f dB "
         "loudest, peak %.0f dBFS, room noise %.0f dB; silent state %#x (%s); %s",
         static_cast<int>(s_test.packets.size()), s_test.sent * 0.02, s_test.bytes, average, s_test.loudest,
         s_test.peak, s_test.noise, static_cast<unsigned>(silent), SilentText(silent).c_str(), text);
    s_test.frames = 0;
    SetTestState(TestState::Playing);
    return;
  }
  if (s_test.state != TestState::Playing)
    return;
  const int frame = s_test.frames++;
  for (size_t i = 0; i < s_test.packets.size(); i++)
  {
    if (s_test.packets[i].first == frame)
    {
      const auto& packet = s_test.packets[i].second;
      Give(TEST_PEER, s_test.sequences[i], packet.data(), packet.size());
    }
  }
  if (s_test.frames >= TEST_FRAMES + TEST_TAIL)
  {
    LogF("voice: test: played back (%d packets)", static_cast<int>(s_test.packets.size()));
    SetTestState(TestState::Done);
  }
}

// One 20 ms frame of the microphone (16 kHz mono): Mic Volume, the level, the gate (or push to talk), the encoder.
void ProcessFrame(Capture& c, int16_t* frame)
{
  // Mic Volume, then a soft limit above -2.5 dBFS (no harsh clipping at 300 or 400%).
  const float gain = static_cast<float>(s_mic_volume.load()) / 100.0f;
  double sum = 0.0;
  float peak = 0.0f;
  for (int i = 0; i < FRAME; i++)
  {
    float x = frame[i] * gain;
    const float a = std::fabs(x);
    if (a > 24576.0f)
      x = std::copysign(24576.0f + 8191.0f * std::tanh((a - 24576.0f) / 8191.0f), x);
    frame[i] = static_cast<int16_t>(std::lround(x));
    sum += static_cast<double>(x) * x;
    peak = std::max(peak, a);
  }
  const float level = Db(sum / FRAME);
  s_level_db.store(level);
  // The room's noise: a quieter level at once (30% a frame), a louder one 1 dB a second (never over -30 dB).
  if (level < c.floor_db)
    c.floor_db += (level - c.floor_db) * 0.3f;
  else
    c.floor_db = std::min(c.floor_db + 0.02f, -30.0f);
  const float threshold = std::max(GATE_MIN_DB, c.floor_db + GATE_ABOVE_NOISE);
  const int mode = std::clamp(s_mode.load(), 0, MODE_COUNT - 1);
  const int test = s_test_state.load();
  const bool testing = test == static_cast<int>(TestState::Recording);
  bool want;
  if (mode == 0)
  {
    const bool loud = level > (c.open ? threshold - GATE_CLOSE_DB : threshold);
    if (loud)
      c.hang = GATE_HANG;
    else if (c.hang > 0)
      c.hang--;
    want = loud || c.hang > 0;
  }
  else
  {
    const bool held = PadHeld(c, mode);
    if (held)
      c.hang = PTT_HANG;
    else if (c.hang > 0)
      c.hang--;
    want = held || c.hang > 0;
  }
  // Muted (the in-game menu), sending off (netplay's: a public room), voice chat off, or the test playing back: nothing
  // encoded or sent (the test records either way: it is never sent).
  if (!testing && (s_mic_muted.load() || !s_sending.load() || !s_enabled.load() ||
                   test == static_cast<int>(TestState::Playing)))
    want = false;
  const bool closing = c.open && !want;
  const bool opening = want && !c.open;
  c.open = want;
  s_gate.store(want);
  const uint16_t sequence = c.sequence++;
  c.frames++;
  c.level_sum += level;
  c.loudest = std::max(c.loudest, level);
  if (testing)
  {
    std::lock_guard lock(s_test_mutex);
    s_test.peak = std::max(s_test.peak, Db(static_cast<double>(peak) * peak));
    s_test.noise = c.floor_db;
  }
  if (want || closing)
  {
    c.gate_frames++;
    // The gate's edges faded: in over 5 ms, out over the talk's last frame.
    if (opening)
    {
      for (int i = 0; i < FADE_IN; i++)
        frame[i] = static_cast<int16_t>(frame[i] * (i + 1) / (FADE_IN + 1));
    }
    if (closing)
    {
      for (int i = 0; i < FRAME; i++)
        frame[i] = static_cast<int16_t>(frame[i] * (FRAME - i) / (FRAME + 1));
    }
    uint8_t packet[MAX_PACKET];
    opus_int32 in_dtx = 0;
    const auto start = Clock::now();
    const int bytes = opus_encode(s_encoder, frame, FRAME, packet, MAX_PACKET);
    const auto us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    c.encode_us += us;
    c.encode_max_us = std::max(c.encode_max_us, us);
    if (bytes < 0)
    {
      if (c.encode_errors++ == 0)
        LogF("voice: opus_encode failed (%d: %s)", bytes, opus_strerror(bytes));
    }
    else if (bytes <= 2 || (opus_encoder_ctl(s_encoder, OPUS_GET_IN_DTX(&in_dtx)) == OPUS_OK && in_dtx))
    {
      c.dtx++;  // DTX: no voice to Opus either (or its comfort noise): nothing to send
    }
    else
    {
      c.sent++;
      c.bytes += bytes;
      Deliver(sequence, packet, bytes);
    }
  }
  TestFrame(level, want);
}

void LogCapture(Capture& c, double seconds, const char* why)
{
  const int silent = s_silent_state.load();
  LogF("voice: microphone, %.1f s%s: %u inputs (%.1f a second, %.1f expected), %u errors (last %#x: %s), %u waits; "
       "level %.0f dB average, %.0f loudest, room noise %.0f dB; gate open %.0f%%; %u packets sent (%.0f bytes "
       "average, %.1f kbps while sending), %u DTX, %u not taken by netplay; encoding %.0f us average, %u at most; "
       "silent state %#x (%s); %s%s%s",
       seconds, why, c.inputs, seconds > 0 ? c.inputs / seconds : 0.0,
       static_cast<double>(s_config.rate) / GRAIN, c.errors, static_cast<unsigned>(c.last_error),
       AudioInError(c.last_error), c.waits, c.frames ? c.level_sum / c.frames : -100.0, c.loudest, c.floor_db,
       c.frames ? 100.0 * c.gate_frames / c.frames : 0.0, c.sent, c.sent ? static_cast<double>(c.bytes) / c.sent : 0.0,
       c.sent ? c.bytes * 8.0 / (c.sent * 0.02) / 1000.0 : 0.0, c.dtx, s_not_taken.exchange(0),
       c.gate_frames ? static_cast<double>(c.encode_us) / c.gate_frames : 0.0, c.encode_max_us,
       static_cast<unsigned>(silent), SilentText(silent).c_str(),
       MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)].label,
       s_mic_muted.load() ? ", muted" : "", s_sending.load() ? "" : ", sending off (netplay)");
  const uint32_t generation = c.generation;
  const uint16_t sequence = c.sequence;
  const float floor_db = c.floor_db;
  const bool open = c.open;
  const int hang = c.hang, pad = c.pad, pad_logged_mode = c.pad_logged_mode;
  const int64_t pad_retry = c.pad_retry;
  c = Capture{};
  c.generation = generation;
  c.sequence = sequence;
  c.floor_db = floor_db;
  c.open = open;
  c.hang = hang;
  c.pad = pad;
  c.pad_retry = pad_retry;
  c.pad_logged_mode = pad_logged_mode;
  c.logged_at = NowMs();
}

void SetCaptureThread()
{
  const OrbisPthread self = scePthreadSelf();
  int before = 0;
  scePthreadGetprio(self, &before);
  const int priority = scePthreadSetprio(self, CAPTURE_PRIORITY);
  const int cores = emusuni_voice_affinity(self, CAPTURE_CORES);
  LogF("voice: capture thread: priority %d -> %d (%#x), cores %#llx (%#x), running on CPU %d; %s, %u samples an "
       "input", before, CAPTURE_PRIORITY, static_cast<unsigned>(priority),
       static_cast<unsigned long long>(CAPTURE_CORES), static_cast<unsigned>(cores), sceKernelGetCurrentCpu(),
       s_config.name, GRAIN);
}

void* CaptureMain(void*)
{
  Capture c;
  c.generation = s_capture_generation.load();
  c.logged_at = NowMs();
  SetCaptureThread();
  const int channels = s_config.format == FORMAT_STEREO ? 2 : 1;
  const bool decimate = s_config.rate != IN_RATE;
  int16_t grain[GRAIN * 2];
  int16_t frame[FRAME];
  int fill = 0;
  Decimator decimator;
  const auto opened = Clock::now();
  uint64_t samples = 0;
  int64_t silent_checked = 0;
  int errors_in_a_row = 0;
  while (s_capturing.load() && s_capture_generation.load() == c.generation)
  {
    const auto before = Clock::now();
    const int result = sceAudioInInput(s_handle, grain);
    const auto after = Clock::now();
    c.inputs++;
    if (s_capture_generation.load() != c.generation)
      break;
    if (result < 0)
    {
      c.errors++;
      c.last_error = result;
      errors_in_a_row++;
      if (c.errors <= 3 || errors_in_a_row % 300 == 0)
        LogF("voice: sceAudioInInput failed: %#x (%s), %d in a row", static_cast<unsigned>(result),
             AudioInError(result), errors_in_a_row);
      if (errors_in_a_row == 60)
      {
        // A second of errors: the test says so (a session's log says it every 5 s meanwhile).
        std::lock_guard lock(s_test_mutex);
        if (s_test.state == TestState::Recording)
        {
          s_test.result = "The microphone stopped answering (error " + Hex(result) + ").";
          SetTestState(TestState::Failed);
        }
      }
      sceKernelUsleep(GRAIN * 1000000u / s_config.rate);  // (never spinning)
      continue;
    }
    errors_in_a_row = 0;
    // (Its result: the samples, or 0 for a whole input - the first ones logged to tell.)
    const uint32_t count = result > 0 && static_cast<uint32_t>(result) < GRAIN ? static_cast<uint32_t>(result) : GRAIN;
    if (c.inputs <= 3)
    {
      int peak = 0;
      for (uint32_t i = 0; i < count * channels; i++)
        peak = std::max(peak, std::abs(static_cast<int>(grain[i])));
      LogF("voice: input %u: %d (%#x) after %.1f ms, %u samples, peak %.0f dBFS", c.inputs, result,
           static_cast<unsigned>(result), std::chrono::duration<double, std::milli>(after - before).count(), count,
           Db(static_cast<double>(peak) * peak));
    }
    // An input that didn't wait for its samples (sooner than the microphone makes them): the rest of the wait here.
    samples += count;
    const double elapsed = std::chrono::duration<double>(after - opened).count();
    if (static_cast<double>(samples) > elapsed * s_config.rate + 4.0 * GRAIN)
    {
      c.waits++;
      sceKernelUsleep(GRAIN * 1000000u / s_config.rate);
    }
    for (uint32_t i = 0; i < count; i++)
    {
      float x = channels == 2 ? (grain[i * 2] + grain[i * 2 + 1]) * 0.5f : grain[i];
      if (decimate && !decimator.Push(x, &x))
        continue;
      frame[fill++] = static_cast<int16_t>(std::clamp(std::lround(x), -32768L, 32767L));
      if (fill == FRAME)
      {
        fill = 0;
        ProcessFrame(c, frame);
      }
    }
    const int64_t now = NowMs();
    if (now - silent_checked >= 2000)
    {
      silent_checked = now;
      const int silent = sceAudioInGetSilentState(s_handle);
      if (silent != s_silent_state.exchange(silent))
        LogF("voice: the microphone's silent state is now %#x (%s)", static_cast<unsigned>(silent),
             SilentText(silent).c_str());
    }
    if (now - c.logged_at >= 10000)
      LogCapture(c, (now - c.logged_at) / 1000.0, "");
  }
  if (c.frames > 0)
    LogCapture(c, (NowMs() - c.logged_at) / 1000.0, ", to its close");
  s_thread_done.store(true);
  return nullptr;
}

// The microphone of `user` and the capture thread (s_control held). False with the reason in `error`.
bool OpenMicLocked(int32_t user, const char* why, std::string* error)
{
  if (!s_module_tried)
  {
    s_module_tried = true;
    // (No sceAudioInInit: games open without it - shadPS4's note.)
    s_module = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOIN);
    LogF("voice: libSceAudioIn loaded (%#x); %s", s_module, opus_get_version_string());
  }
  if (s_module != 0)
  {
    *error = "The PS4's microphone library couldn't be loaded (" + Hex(static_cast<int>(s_module)) + ").";
    return false;
  }
  LogF("voice: opening the microphone of user %#x (%s)", static_cast<unsigned>(user), why);
  int handle = -1, first_error = 0;
  for (const Config& config : CONFIGS)
  {
    const int result = config.hq ? sceAudioInHqOpen(user, config.type, 0, GRAIN, config.rate, config.format) :
                                   sceAudioInOpen(user, config.type, 0, GRAIN, config.rate, config.format);
    LogF("voice: sceAudioIn%sOpen(user %#x, %s, %u samples) = %#x (%s)", config.hq ? "Hq" : "",
         static_cast<unsigned>(user), config.name, GRAIN, static_cast<unsigned>(result), AudioInError(result));
    if (result >= 0)
    {
      handle = result;
      s_config = config;
      break;
    }
    handle = result;
    first_error = first_error ? first_error : result;
  }
  if (handle < 0)
  {
    *error = "No microphone could be opened (error " + Hex(first_error) +
             "). Plug a headset into the controller, or connect a USB microphone or the PlayStation Camera.";
    return false;
  }
  const int silent = sceAudioInGetSilentState(handle);
  s_silent_state.store(silent);
  LogF("voice: microphone open: handle %#x, %s; silent state %#x (%s)", static_cast<unsigned>(handle), s_config.name,
       static_cast<unsigned>(silent), SilentText(silent).c_str());
  if (!s_encoder)
  {
    int code = 0;
    s_encoder = opus_encoder_create(IN_RATE, 1, OPUS_APPLICATION_VOIP, &code);
    if (!s_encoder)
    {
      LogF("voice: opus_encoder_create failed (%d: %s)", code, opus_strerror(code));
      sceAudioInClose(handle);
      *error = "No memory for the voice's encoder.";
      return false;
    }
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(BITRATE));
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(COMPLEXITY));
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_PACKET_LOSS_PERC(LOSS_PERCENT));
    LogF("voice: Opus encoder: 16 kHz mono, %d bps, complexity %d, FEC for %d%% loss, 20 ms packets", BITRATE,
         COMPLEXITY, LOSS_PERCENT);
  }
  else
  {
    opus_encoder_ctl(s_encoder, OPUS_RESET_STATE);
  }
  const bool dtx = Settings::Get("voice_dtx") != "off";
  opus_encoder_ctl(s_encoder, OPUS_SET_DTX(dtx ? 1 : 0));
  if (!dtx)
    LogLine("voice: DTX off (settings.ini voice_dtx=off): everything the gate lets through is sent");
  s_handle = handle;
  s_user = user;
  s_level_db.store(-100.0f);
  s_capturing.store(true);
  s_thread_done.store(false);
  s_capture_generation.fetch_add(1);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);  // (Opus' encoder needs a few dozen KiB; libthr keeps one this size)
  s_thread_made = pthread_create(&s_thread, &attr, CaptureMain, nullptr) == 0;
  pthread_attr_destroy(&attr);
  if (!s_thread_made)
  {
    LogLine("voice: the capture thread couldn't be made");
    s_capturing.store(false);
    s_thread_done.store(true);
    sceAudioInClose(handle);
    s_handle = -1;
    *error = "The microphone's thread couldn't start.";
    return false;
  }
  return true;
}

// The capture thread ended and the microphone closed (s_control held). The thread waits at most an input (16 ms) in
// sceAudioInInput; one that doesn't come back in 0.3 s is left behind (its handle closed under it, logged).
void CloseMicLocked()
{
  if (s_handle < 0)
    return;
  s_capturing.store(false);
  bool ended = false;
  for (int i = 0; i < 150 && !(ended = s_thread_done.load()); i++)
    sceKernelUsleep(2000);
  int closed = 0;
  if (ended)
  {
    if (s_thread_made)
      pthread_join(s_thread, nullptr);
    closed = sceAudioInClose(s_handle);
  }
  else
  {
    s_capture_generation.fetch_add(1);  // (it does nothing more once it comes back)
    closed = sceAudioInClose(s_handle);
    LogF("voice: the capture thread didn't come back from sceAudioInInput in 0.3 s: left behind");
    if (s_thread_made)
      pthread_detach(s_thread);
  }
  s_thread_made = false;
  LogF("voice: microphone closed (%#x)", static_cast<unsigned>(closed));
  s_handle = -1;
  s_gate.store(false);
  s_talk_until.store(0);
  s_level_db.store(-100.0f);
}

bool TestActive()
{
  const int state = s_test_state.load();
  return state == static_cast<int>(TestState::Recording) || state == static_cast<int>(TestState::Playing);
}

void TestStart(int32_t user)
{
  ReloadSettings();
  Forget(TEST_PEER);
  std::lock_guard control(s_control);
  {
    std::lock_guard lock(s_test_mutex);
    s_test = Test{};
    SetTestState(TestState::Recording);
  }
  LogF("voice: test: recording 5 s from user %#x (%s, Mic Volume %d%%)%s", static_cast<unsigned>(user),
       MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)].label, s_mic_volume.load(),
       s_handle >= 0 ? ", the session's microphone" : "");
  if (s_handle >= 0)
    return;
  std::string error;
  if (!OpenMicLocked(user, "Test Microphone", &error))
  {
    LogF("voice: test: %s", error.c_str());
    std::lock_guard lock(s_test_mutex);
    s_test.result = error;
    SetTestState(TestState::Failed);
  }
}

void TestStop()
{
  Forget(TEST_PEER);
  std::lock_guard control(s_control);
  {
    std::lock_guard lock(s_test_mutex);
    if (s_test.state == TestState::Recording || s_test.state == TestState::Playing)
      LogLine("voice: test: stopped");
    SetTestState(TestState::Off);
    s_test.packets.clear();
  }
  if (!s_session)
    CloseMicLocked();
}

// The test ended: the microphone closed if only the test had it.
void TestFinished()
{
  std::lock_guard control(s_control);
  if (!s_session && s_handle >= 0)
    CloseMicLocked();
}

std::string Percent(int value)
{
  return std::to_string(value) + "%";
}

int64_t s_panel_drawn = 0;
std::string s_panel_shown;
}  // namespace

// --- Netplay's side ------------------------------------------------------------------------------------------------

bool Start(int32_t user_id)
{
  ReloadSettings();
  std::lock_guard control(s_control);
  {
    std::lock_guard lock(s_send_mutex);
    s_send.clear();
  }
  if (!s_enabled.load())
  {
    LogLine("voice: start: voice chat is off (Settings -> Voice Chat)");
    s_session = false;
    return false;
  }
  if (s_handle >= 0 && s_user == user_id)
  {
    LogLine("voice: start: the microphone is open already (Test Microphone)");
    s_session = true;
    return true;
  }
  if (s_handle >= 0)
    CloseMicLocked();
  std::string error;
  s_session = OpenMicLocked(user_id, "voice chat", &error);
  LogF("voice: start: %s (%s, Mic Volume %d%%, Voices Volume %d%%)%s%s",
       s_session ? "the microphone open" : error.c_str(), MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)].label,
       s_mic_volume.load(), s_voices_volume.load(), s_sending.load() ? "" : "; sending off (netplay)",
       s_mic_muted.load() ? "; muted" : "");
  return s_session;
}

void Stop()
{
  std::lock_guard control(s_control);
  if (!s_session)
    return;
  s_session = false;
  LogLine("voice: stop");
  if (!TestActive())
    CloseMicLocked();
  std::lock_guard lock(s_send_mutex);
  s_send.clear();
}

bool TakePacket(std::vector<uint8_t>* packet, uint16_t* sequence)
{
  std::lock_guard lock(s_send_mutex);
  if (s_send.empty())
    return false;
  const Outgoing& out = s_send.front();
  packet->assign(out.data, out.data + out.size);
  *sequence = out.sequence;
  s_send.pop_front();
  return true;
}

void GivePacket(int peer, uint16_t sequence, const uint8_t* data, size_t size)
{
  if (peer < 0 || !s_enabled.load(std::memory_order_relaxed))
    return;
  Give(peer, sequence, data, size);
}

void ForgetPeer(int peer)
{
  if (peer >= 0)
    Forget(peer);
}

bool Talking(int peer)
{
  const int64_t now = NowMs();
  if (peer == -1)
    return now < s_talk_until.load();
  for (const auto& place : s_peers)
  {
    const Peer* p = place.load(std::memory_order_acquire);
    if (p && p->public_id.load() == peer)
      return now < p->talking_until.load();
  }
  return false;
}

void SetMicMuted(bool muted)
{
  {
    std::lock_guard lock(s_send_mutex);
    if (s_mic_muted.exchange(muted) == muted)
      return;
    if (muted)
    {
      s_send.clear();
      s_talk_until.store(0);
    }
  }
  LogF("voice: microphone %s", muted ? "muted" : "on again");
}

bool MicMuted()
{
  return s_mic_muted.load();
}

void SetPeerMuted(int peer, bool muted)
{
  if (peer < 0)
    return;
  {
    std::lock_guard lock(s_peers_mutex);
    const auto at = std::find(s_muted.begin(), s_muted.end(), peer);
    if (muted == (at != s_muted.end()))
      return;
    if (muted)
      s_muted.push_back(peer);
    else
      s_muted.erase(at);
    // Either way their voice starts over: silent at once when muted, clean when unmuted.
    if (Peer* p = FindPeerLocked(peer, false))
      ResetPeerLocked(*p);
  }
  LogF("voice: friend %d %s", peer, muted ? "muted" : "unmuted");
}

void SetSending(bool on)
{
  {
    std::lock_guard lock(s_send_mutex);
    if (s_sending.exchange(on) == on)
      return;
    if (!on)
    {
      s_send.clear();
      s_talk_until.store(0);
    }
  }
  LogF("voice: sending %s (netplay)", on ? "on" : "off: nothing captured is sent");
}

void ReloadSettings()
{
  s_enabled.store(Settings::Get("voice_chat") != "off");
  const std::string mode = Settings::Get("voice_mic");
  int index = 0;
  for (int i = 0; i < MODE_COUNT; i++)
  {
    if (mode == MODES[i].value)
      index = i;
  }
  s_mode.store(index);
  const std::string mic = Settings::Get("voice_mic_volume"), voices = Settings::Get("voice_volume");
  const int mic_value = mic.empty() ? 100 : std::atoi(mic.c_str());
  const int voices_value = voices.empty() ? 100 : std::atoi(voices.c_str());
  s_mic_volume.store(Index(MIC_VOLUMES, 8, mic_value) >= 0 ? mic_value : 100);
  s_voices_volume.store(Index(VOICE_VOLUMES, 8, voices_value) >= 0 ? voices_value : 100);
}

// --- The sound threads -----------------------------------------------------------------------------------------------

void Mix(int16_t* stereo, uint32_t frames)
{
  if (!s_any_peer.load(std::memory_order_acquire))
    return;
  // Under the lock (if it's free: a packet being put in holds it a moment): what each friend's next 20 ms are made of,
  // for those whose decoded sound runs out in this period; the decoding after it.
  struct Job
  {
    Peer* peer = nullptr;
    bool reset = false;
    Action action = Action::None;
    int size = 0;
    uint8_t data[MAX_PACKET];
  };
  Job jobs[PLACES];
  int count = 0;
  const int64_t now = NowMs();
  if (s_peers_mutex.try_lock())
  {
    for (const auto& place : s_peers)
    {
      Peer* p = place.load(std::memory_order_relaxed);
      if (!p)
        continue;
      Job& job = jobs[count];
      job = Job{};
      job.peer = p;
      if (p->played_generation != p->generation)
      {
        p->played_generation = p->generation;
        job.reset = true;
      }
      if (p->used && (job.reset ? 0 : p->pcm_len - p->pcm_at) < static_cast<int>(frames))
        job.action = TickLocked(*p, now, job.data, &job.size);
      if (job.reset || job.action != Action::None)
        count++;
    }
    s_peers_mutex.unlock();
  }
  for (int i = 0; i < count; i++)
  {
    Job& job = jobs[i];
    Peer& p = *job.peer;
    if (job.reset)
    {
      opus_decoder_ctl(p.decoder, OPUS_RESET_STATE);
      p.pcm_at = p.pcm_len = 0;
    }
    if (job.action == Action::None)
      continue;
    // What is left of the last packet to the front, the new one after it.
    if (p.pcm_at > 0)
    {
      std::memmove(p.pcm, p.pcm + p.pcm_at, (p.pcm_len - p.pcm_at) * sizeof(int16_t));
      p.pcm_len -= p.pcm_at;
      p.pcm_at = 0;
    }
    int16_t* out = p.pcm + p.pcm_len;
    const auto start = Clock::now();
    int made = 0;
    if (job.action == Action::Decode)
      made = opus_decode(p.decoder, job.data, job.size, out, OUT_FRAME, 0);
    else if (job.action == Action::Fec)
      made = opus_decode(p.decoder, job.data, job.size, out, OUT_FRAME, 1);
    else
      made = opus_decode(p.decoder, nullptr, 0, out, OUT_FRAME, 0);
    const auto us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    p.decode_us.fetch_add(us, std::memory_order_relaxed);
    p.decodes.fetch_add(1, std::memory_order_relaxed);
    if (us > p.decode_max_us.load(std::memory_order_relaxed))
      p.decode_max_us.store(us, std::memory_order_relaxed);
    if (made != OUT_FRAME)
    {
      std::memset(out, 0, OUT_FRAME * sizeof(int16_t));  // (an error: 20 ms of silence)
      made = OUT_FRAME;
    }
    p.pcm_len += made;
    double sum = 0.0;
    for (int k = 0; k < made; k++)
      sum += static_cast<double>(out[k]) * out[k];
    const float level = Db(sum / made);
    p.level.store(level, std::memory_order_relaxed);
    if (job.action != Action::Plc && level > TALKING_DB)
      p.talking_until.store(now + TALKING_MS, std::memory_order_relaxed);
  }
  // Each voice into the period, at the Voices Volume (Test Microphone's at 100%).
  const int volume = s_voices_volume.load(std::memory_order_relaxed);
  for (const auto& place : s_peers)
  {
    Peer* p = place.load(std::memory_order_relaxed);
    if (!p || p->pcm_at >= p->pcm_len)
      continue;
    const int gain = p->public_id.load(std::memory_order_relaxed) == TEST_PEER ? 100 : volume;
    const int n = std::min(static_cast<int>(frames), p->pcm_len - p->pcm_at);
    const int16_t* in = p->pcm + p->pcm_at;
    for (int k = 0; k < n; k++)
    {
      const int v = in[k] * gain / 100;
      stereo[k * 2] = static_cast<int16_t>(std::clamp(stereo[k * 2] + v, -32768, 32767));
      stereo[k * 2 + 1] = static_cast<int16_t>(std::clamp(stereo[k * 2 + 1] + v, -32768, 32767));
    }
    p->pcm_at += n;
  }
}

// --- Settings -> Voice Chat ---------------------------------------------------------------------------------------

std::vector<Row> PanelRows()
{
  ReloadSettings();
  const Mode& mode = MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)];
  std::vector<Row> rows;
  rows.push_back({"Voice Chat", s_enabled.load() ? "On" : "Off", "vc:on",
                  "Talk with your friends in online games: your microphone (a headset on the controller, a USB "
                  "microphone or the PlayStation Camera) and their voices."});
  rows.push_back({"Microphone", mode.label, "vc:mode", mode.help});
  rows.push_back({"Mic Volume", Percent(s_mic_volume.load()), "vc:mic",
                  "How loud your voice is sent. Test Microphone tells you whether to raise or lower it."});
  rows.push_back({"Voices Volume", Percent(s_voices_volume.load()), "vc:voices",
                  "How loud your friends' voices are, over the game's sound."});
  const int state = s_test_state.load();
  const bool active = state == static_cast<int>(TestState::Recording) || state == static_cast<int>(TestState::Playing);
  rows.push_back({"Test Microphone", active ? "Stop" : "", "vc:test",
                  "Records 5 seconds and plays them back as your friends will hear them, with a level meter: try it "
                  "before playing online."});
  // The test's progress and its result under the rows (notes).
  rows.push_back({"", "", "", ""});
  std::string status;
  bool meter = false;
  {
    std::lock_guard lock(s_test_mutex);
    const int left = std::max(0, (TEST_FRAMES - s_test.frames + 49) / 50);
    switch (s_test.state)
    {
    case TestState::Off:
      status = "Test Microphone: 5 seconds recorded, then played back.";
      break;
    case TestState::Recording:
      status = mode.mask ?
                   "Recording: hold " + std::string(mode.button) + " and talk (" + std::to_string(left) + " s)" :
                   "Recording: talk now (" + std::to_string(left) + " s)";
      meter = true;
      break;
    case TestState::Playing:
      status = "Playing back what your friends will hear (" + std::to_string(left) + " s)";
      meter = true;
      break;
    case TestState::Done:
    case TestState::Failed:
      status = s_test.result;
      break;
    }
  }
  rows.push_back({status, "", "", ""});
  if (meter)
  {
    // The level: 24 marks from -60 to 0 dB; "Voice" while the gate sends (recording).
    float level = -100.0f;
    bool voice = false;
    if (state == static_cast<int>(TestState::Recording))
    {
      level = s_level_db.load();
      voice = s_gate.load();
    }
    else
    {
      const Peer* p = s_peers[MAX_PEERS].load();
      level = p ? p->level.load() : -100.0f;
      voice = p && NowMs() < p->talking_until.load();
    }
    const int marks = std::clamp(static_cast<int>((level + 60.0f) / 60.0f * 24.0f + 0.5f), 0, 24);
    char value[32];
    snprintf(value, sizeof(value), "%.0f dB", std::max(level, -99.0f));
    rows.push_back({std::string(marks, '|') + std::string(24 - marks, '.'),
                    voice ? "Voice: " + std::string(value) : std::string(value), "", ""});
  }
  return rows;
}

bool PanelChange(const std::string& key, int direction, int32_t user_id)
{
  ReloadSettings();
  const auto step = [direction](const int* values, int count, int now) {
    const int at = std::max(0, Index(values, count, now));
    return values[std::clamp(at + (direction < 0 ? -1 : 1), 0, count - 1)];
  };
  if (key == "vc:on")
  {
    Settings::Set("voice_chat", s_enabled.load() ? "off" : "on");
  }
  else if (key == "vc:mode")
  {
    const int now = std::clamp(s_mode.load(), 0, MODE_COUNT - 1);
    Settings::Set("voice_mic", MODES[(now + (direction < 0 ? MODE_COUNT - 1 : 1)) % MODE_COUNT].value);
  }
  else if (key == "vc:mic")
  {
    Settings::Set("voice_mic_volume", std::to_string(step(MIC_VOLUMES, 8, s_mic_volume.load())));
  }
  else if (key == "vc:voices")
  {
    Settings::Set("voice_volume", std::to_string(step(VOICE_VOLUMES, 8, s_voices_volume.load())));
  }
  else if (key == "vc:test")
  {
    if (direction < 0)
      return false;
    if (TestActive())
      TestStop();
    else
      TestStart(user_id);
    return true;
  }
  else
  {
    return false;
  }
  ReloadSettings();
  LogF("voice: settings: %s, %s, Mic Volume %d%%, Voices Volume %d%%", s_enabled.load() ? "on" : "off",
       MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)].label, s_mic_volume.load(), s_voices_volume.load());
  return true;
}

bool PanelUpdate(bool shown)
{
  const int state = s_test_state.load();
  if (!shown)
  {
    if (state != static_cast<int>(TestState::Off))
      TestStop();
    s_panel_shown.clear();
    return false;
  }
  if (state == static_cast<int>(TestState::Done) || state == static_cast<int>(TestState::Failed))
  {
    bool first = false;
    {
      std::lock_guard lock(s_test_mutex);
      first = !s_test.finished;
      s_test.finished = true;
    }
    if (first)
      TestFinished();
  }
  // About 15 times a second while the test runs; else when its state changed.
  const int64_t now = NowMs();
  const std::string key = std::to_string(state);
  const bool active = TestActive();
  if (key == s_panel_shown && !(active && now - s_panel_drawn >= 66))
    return false;
  s_panel_shown = key;
  s_panel_drawn = now;
  return true;
}

std::string Detail()
{
  ReloadSettings();
  return s_enabled.load() ? MODES[std::clamp(s_mode.load(), 0, MODE_COUNT - 1)].label : "Off";
}
}  // namespace Voice
