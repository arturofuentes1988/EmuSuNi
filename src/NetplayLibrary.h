// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The library as online play's Choose Together rooms see it (00.23, src/Netplay.h): each game file's CRC32 and size -
// hashed on a thread of its own, never the menu's, and kept in data_dir/cache/netplay-crcs.txt by path, size and
// modification time (a file is read once, again only when it changes) - and how many players each game takes at once.
//
// The players: assets/xmb/netplay/players.bin (scripts/make-netplay-players.py): libretro-database's metadat/maxusers
// (CC BY-SA 4.0: the No-Intro names' and the ROMs' CRC32s with their players) for the cartridge consoles, and
// FinalBurn Neo's own drivers' players for the Arcade - only those facts, compressed. A game is found by its file's
// CRC32 (a file as the DAT's: unzipped, no header), else by its name (TitleKey of the file's own name), else by its
// arcade set; 0: not known.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NetplayLibrary
{
struct File
{
  std::string console;  // its console's key ("snes")
  std::string path;
  std::string stem;     // its file's name without the extension (the No-Intro name; an arcade set's)
};
// The files' hashing (those not in the copy yet, or changed), on its thread; called again with the library as it is
// now, it goes on with the files it doesn't know.
void Start(const std::string& data_dir, const std::vector<File>& files);
struct Progress
{
  int done = 0, total = 0;
  bool finished = false;  // every file of the last Start hashed (or found unreadable)
};
Progress GetProgress();
// A file's CRC32 and size, once hashed (false: not yet, or it couldn't be read).
bool Lookup(const std::string& path, uint32_t* crc, uint64_t* size);
// No file read while a game runs (its USB reads are the game's); on again in the menu.
void Pause(bool paused);
// The players the game takes at once (2 or more: a game to play together), 1 (alone), 0 (not known).
int Players(const std::string& console, const std::string& stem, uint32_t crc);
// A name's key for the players' list: before its first "(" or "[", lower case letters and digits only, the word "the"
// left out ("Legend of Zelda, The - A Link to the Past (USA)" and "The Legend of Zelda - A Link to the Past" are the
// same). scripts/make-netplay-players.py makes the same keys.
std::string TitleKey(const std::string& name);
}  // namespace NetplayLibrary
