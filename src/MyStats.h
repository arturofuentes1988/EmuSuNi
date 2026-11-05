// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// My Stats (00.23): each player's own numbers, on the PS4 only - nothing of it is sent (the opt-in online profile is
// the server's, src/UsageStats.h). Every game played: its time (from its start to its end, a day at most a session),
// its sessions, when it was last played and its online matches, kept in /data/EmuSuNi/mystats.ini ("path\tconsole\t
// seconds\tsessions\tlast\tonline" a line). Settings -> My Stats shows them with the RetroAchievements numbers the
// app already has. Nothing here throws; every call is the menu's or the game's thread (one at a time).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace MyStats
{
struct Game
{
  std::string path;     // the game's file (the library's key for it)
  std::string console;  // its console's key ("snes")
  int64_t seconds = 0;  // played in EmuSuNi
  int sessions = 0;
  int64_t last = 0;     // when it was last played (Unix time)
  int online = 0;       // online matches
};

void Load(const std::string& data_dir);
// A game starts (src/Emulator.cpp: its Run), online or not; ends (its time added, the file saved).
void GameStarted(const std::string& path, const std::string& console, bool online);
void GameEnded();
// Every game played, the most played first.
std::vector<Game> Games();
}  // namespace MyStats
