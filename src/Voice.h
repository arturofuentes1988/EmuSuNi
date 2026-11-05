// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Voice chat for the online matches (00.23): the PS4 user's microphone (a headset on the DualShock 4's jack, a USB
// microphone or the camera's, whichever the PS4 gives that user: sceAudioIn), a noise gate (or push to talk), Opus
// (16 kHz mono, 20 ms packets, ~20 kbps, in-band FEC, DTX: silence sends nothing) on a thread of its own; the
// friends' packets through a jitter buffer per friend (40 to 200 ms, after the network's jitter; FEC and Opus' own
// concealment for a lost packet) into the app's one sound port (GameAudio.h's AudioPort): the menu's and the games'
// sound threads add the voices to what they play (Mix), so they are heard in a game, its in-game menu and the menus.
// The packets themselves are netplay's: it takes ours (TakePacket) and hands over the friends' (GivePacket).
// Settings -> Voice Chat (settings.ini voice_*): on/off, open mic or push to talk, the microphone's and the voices'
// volumes, and Test Microphone (5 s recorded, then played back through the same path as a friend's voice).
// Every step is logged with "voice: " (the open's tries and their codes, the first inputs, the levels, the packets).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Voice
{
// --- Netplay's side --------------------------------------------------------------------------------------------------
// Opens that PS4 user's microphone and the encoder (again after Stop); false when voice chat is off in Settings or
// there is no microphone (the reason logged). Packets from friends are played either way (while voice chat is on).
bool Start(int32_t user_id);
void Stop();
// The next 20 ms Opus packet to send (oldest first; about half a second is kept for a caller that falls behind), with
// its sequence number (one per 20 ms of microphone sound, sent or not: a gap is silence or a lost packet).
bool TakePacket(std::vector<uint8_t>* packet, uint16_t* sequence);
// A friend's packet, as it came: any order, duplicates and late ones are fine. `peer` >= 0 (netplay's numbering).
void GivePacket(int peer, uint16_t sequence, const uint8_t* data, size_t size);
// The friend left: their voice stops and their buffer is let go (a new friend may get that number).
void ForgetPeer(int peer);
// Speaking now (the last 0.3 s), for a "speaking" mark: a friend, or -1 for the player (sending).
bool Talking(int peer);

// The in-game menu's mute (netplay's toggle): nothing is sent while muted; friends are still heard.
void SetMicMuted(bool muted);
bool MicMuted();
// Safety with strangers (public rooms). A muted friend's packets are dropped as they come (nothing of them decoded or
// heard, Talking false); their voice starts clean when unmuted. Kept for that number until ForgetPeer.
void SetPeerMuted(int peer, bool muted);
// Netplay's switch for sending at all (off in a public room until the player opts in): while off nothing captured is
// encoded or sent, and what was waiting for TakePacket is dropped. On by default; Start and Stop leave it as it is.
void SetSending(bool on);
// The voice_* settings read again (Settings -> Voice Chat reads them itself; for a caller that changed them).
void ReloadSettings();

// --- The sound threads (AudioPort's: the menu's, the games') ---------------------------------------------------------
// The voices added into a period of interleaved 48 kHz stereo (`frames` frames, at most 960: AudioPort's 256), at the
// Voices Volume. Never blocks: a busy lock leaves the voices out of that period.
void Mix(int16_t* stereo, uint32_t frames);

// --- Settings -> Voice Chat (drawn by Menu.cpp) ----------------------------------------------------------------------
struct Row
{
  std::string label;  // English (the menu translates it)
  std::string value;
  std::string key;    // "" for a note (a line of text, not a setting)
  std::string help;
};
std::vector<Row> PanelRows();
// Left (-1), Right or Cross (+1) on a row: the setting changed (saved), Test Microphone started or stopped. `user_id`:
// the PS4 user whose microphone the test opens. True when the rows changed.
bool PanelChange(const std::string& key, int direction, int32_t user_id);
// Every frame. `shown`: the panel is open (the test stops when it closes). True when its rows should be made again
// (the test's progress and its level meter, about 15 times a second).
bool PanelUpdate(bool shown);
// The Settings list's line for Voice Chat ("On - Open Mic").
std::string Detail();
}  // namespace Voice
