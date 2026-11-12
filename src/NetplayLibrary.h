// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The library as online play's Choose Together rooms see it (00.23, src/Netplay.h): each game file's CRC32 and size -
// (01.05) its content's, ContentId - hashed on a thread of its own, never the menu's, and kept in
// data_dir/cache/netplay-ids.txt by path, size and modification time (a file is read once, again only when it
// changes; 01.04's netplay-crcs.txt held the whole files' CRC32s) - and how many players each game takes at once.
// (01.06) The whole file's CRC32 too (FileId: how v00.23's rooms know a game), in the same copy - read again only for
// the files whose content isn't the file itself (a header, a zip), and never an Arcade set's.
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
// (A file's CRC32 and size below: its content's - ContentId.)
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
// (01.06) Its whole file's (FileId's), once hashed - an Arcade set's never (a room finds one by its name).
bool LookupFile(const std::string& path, uint32_t* crc, uint64_t* size);
// No file read while a game runs (its USB reads are the game's); on again in the menu.
void Pause(bool paused);
// The players the game takes at once (2 or more: a game to play together), 1 (alone), 0 (not known).
int Players(const std::string& console, const std::string& stem, uint32_t crc);
// A name's key for the players' list: before its first "(" or "[", lower case letters and digits only, the word "the"
// left out ("Legend of Zelda, The - A Link to the Past (USA)" and "The Legend of Zelda - A Link to the Past" are the
// same). scripts/make-netplay-players.py makes the same keys.
std::string TitleKey(const std::string& name);

// ---- (01.05, the owner: "para que el online encuentre más partidas") A game's identity as online play compares it ----
// The CRC32 and size of its content as No-Intro's DATs have it - unzipped, without a copier's or an emulator's header -
// so two files of one dump with other headers, or zipped otherwise, are the same game (Netplay::GameCrc):
//  - a .zip: its game's entry (a cartridge's extension: the largest), read from the zip; an Arcade set's (console
//    "arcade", or a zip without such an entry): its entries' CRC32s, sorted, as one CRC32 - their sizes summed - so a
//    set packed again is the same set;
//  - NES: the iNES / NES 2.0 header (16 bytes) and its trainer (512); FDS: fwNES's header (16);
//  - SNES: a copier's 512 bytes (a size 512 past a multiple of 1 KiB); PC Engine: 512 past 8 KiB; Master System,
//    Game Gear, SG-1000: 512 past 16 KiB;
//  - Mega Drive: an SMD file (a copier's 512 bytes, then 16 KiB blocks interleaved) as the plain ROM;
//  - Atari 7800: the a78 header (128); Lynx: LNX's (64);
//  - N64: a .v64's or .n64's bytes in the .z64's order.
// Read a piece at a time (never the whole game in memory). A file over 64 MiB (a disc image): the CRC32 of its first
// and last 8 MiB, with its size, as ever (no header there). False: it can't be read.
bool ContentId(const std::string& path, const std::string& console, uint32_t* crc, uint64_t* size);
// (01.06, online play between versions) A game's identity as v00.23 (and 01.04) knew it: the CRC32 and size of its
// file as it is - a header, a zip and all (over 64 MiB: of its first and last 8 MiB). Such an app's rooms say their
// game so, and the server gives such an app ours so (NetProto::GameInfo::file_crc).
bool FileId(const std::string& path, uint32_t* crc, uint64_t* size);

// ---- (01.05, the owner: "Revisar mis juegos") The standard versions: assets/xmb/netplay/standard.bin ----
// (scripts/make-standard-games.py) No-Intro's names of the cartridges of the consoles that play online (libretro-
// database's DATs: the headerless entries) and FinalBurn Neo v1.0.0.03's Arcade sets (its "Arcade only" DAT: each set's
// ContentId, split and whole) - only the CRC32, the size and the name of each, compressed (about 34 000 entries; ~2
// MiB in memory once read, at the first question).
// The standard name of a content (ContentId's) of that console ("" none: not a standard version; size 0: by its CRC32
// alone - the server's list of the games most played); Covered: whether the list has that console's games (else
// nothing can be said).
std::string StandardName(const std::string& console, uint32_t crc, uint64_t size);
bool Covered(const std::string& console);
// What the list is ("No-Intro" for a cartridge console's, "FinalBurn Neo 1.0.0.03" for the Arcade's; "" none).
std::string StandardSource(const std::string& console);
}  // namespace NetplayLibrary
