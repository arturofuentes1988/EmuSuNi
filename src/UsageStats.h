// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Anonymous statistics (00.23): how many use EmuSuNi, from which countries, with which versions, what they play and
// how many have it open right now - for the owner's report (server/emusuni-netd-stats.h) and Online Play's "N players
// online now". Settings -> Anonymous Statistics: on unless the player turns it off; off, nothing at all is sent.
//
// What is sent: a small UDP heartbeat to online play's main server (src/Netplay.h finds it) when the app starts, every
// 5 minutes while it is open, and a minute after an update check: a random install id this app made once and keeps in
// /data/EmuSuNi/stats.ini (never the PS4's, a user's or PSN's), the app's version, the PS4's language, what the app is
// at (the menus / playing a console - its key, "snes" - / online play), the game being played (SetGame: a checksum,
// an arcade set's or a disc's code - never a file's name), the update checks since the last heartbeat, and once after
// an update the version it ran before. Never sent: names, the library, files, saves, any id of the PS4, its users or
// PSN. The server keeps counts and the country of the address (looked up when the packet comes in), never the
// address; the public page shows a game's title (from those lists) only once 3 installs or more played it.
//
// The heartbeats are a thread's, fire and forget: nothing waits for them, and one not answered isn't sent again
// before the next (a Hello and a Stats, the server's cookie first, as every request to it). Nothing here throws.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UsageStats
{
// At the start (the menu's first): stats.ini read - made with a new install id the first time -, the thread started.
void Start(const std::string& data_dir, const std::string& version);
// Settings -> Anonymous Statistics (remembered in stats.ini; on by default).
bool Enabled();
void SetEnabled(bool on);
// What the app is at: a game of a console (its key; nullptr: back in the menus), online or not; online play's room.
void SetPlaying(const char* console_key, bool online);
// The game being played (src/Emulator.cpp, once it loaded), as No-Intro's, Redump's and FinalBurn Neo's lists know
// it - never its file's name: a cartridge's CRC32 (its bytes as the core got them, 64 MiB at most; a Super Nintendo
// copier header left out, a Nintendo 64 image in the lists' byte order), an arcade, NAOMI or Atomiswave set's name, a
// PSP disc's ID (UMD_DATA.BIN, read on the statistics' thread). Nothing while the statistics are off.
void SetGame(const char* console_key, const std::string& path, const std::vector<uint8_t>& rom);
// A PlayStation disc's code once the core read it ("SCUS-94426": SwanStation's log line).
void SetDiscCode(const std::string& code);
void SetRoom(bool in_room);
// src/Update.cpp: a check of GitHub for a newer version started.
void UpdateChecked();
// A heartbeat soon (Online Play's screen: its players online now), a minute after the last one at the soonest.
void Refresh();
// The players with EmuSuNi open now, as the server last said; -1: not known (statistics off, no answer yet).
int OnlineNow();

// ---- The opt-in online profile (the Online tab; off unless the player turns it on) ----
// On: the server keeps this install's history - minutes, sessions and online matches by game and console - and gives
// it a code (shown in the Online tab) that opens it on EmuSuNi's website; public, it is in the rankings with the
// nickname. Turning it on turns the statistics on (it needs the heartbeat); turning them off turns it off. Off, the
// server keeps what it had (until Delete My Profile); delete: it is deleted on the server (asked until it answers,
// even with the statistics off) and its code forgotten.
struct Profile
{
  bool on = false, is_public = false;
  std::string code;       // "" until the server gave one
  bool deleting = false;  // the deletion asked, not answered yet
};
Profile GetProfile();
void SetProfile(bool on);
void SetPublicProfile(bool on);
void DeleteProfile();
}  // namespace UsageStats
