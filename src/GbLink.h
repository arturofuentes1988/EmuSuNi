// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Game Boy link cable online (00.95): two PS4s play two Game Boy / Game Boy Color games joined by the cable -
// Pokemon's trades and battles above all (Red, Blue, Yellow; Gold, Silver, Crystal; a first generation's game with a
// second's through the Time Capsule).
//
// How: SameBoy's "2 Player Game Boy Link" (scripts/build-gblink-core.sh) runs both Game Boys, joined by its emulated
// cable, in one core, and EACH PS4 of the match runs both, kept in step by the online play's rollback (src/Netplay.h)
// like any game: the cable never waits for the network, a trade takes what it takes on two real Game Boys.
//  - Game Boy 1 is the host's game (player 1, the host's controller), Game Boy 2 the guest's (player 2). Each PS4
//    shows and plays the sound of its own Game Boy only (the core draws them side by side: VisibleHalf).
//  - Each PS4 needs both games' files, then (the same file: one is enough). A game's file is never sent: the
//    consoles compare their files' CRC32 and size, as for every online game.
//  - The battery saves: the guest's (its RAM and clock, PackSave's bytes) goes to the host before the game starts;
//    the host loads both and sends its state, as for every online game, which carries both. At the end each PS4
//    writes only its own Game Boy's save, into its own game's save (and once before, a copy of it: BackupSave).
//  - Alone, a Game Boy game stays mGBA's: its saves are mGBA's (<key>.srm, <key>.rtc: the clock's 48 bytes,
//    VBA-M's/BGB's format, which SameBoy reads and writes too).
//
// Netplay sets the match (Set) before Emulator::Run starts its game, and clears it after (Clear). Nothing here
// throws; Set/Clear/Get from the menu's or the game's thread, never both at once.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Systems.h"

namespace GbLink
{
// The libretro subsystem SameBoy (and TGB Dual) load two games with (retro_load_game_special).
constexpr unsigned GAME_TYPE_LINK_2P = 0x101;

struct Game
{
  std::string path;   // this console's file of it
  std::string key;    // its key in this console's library (its saves')
  std::string title;  // (the log)
  SystemId system = SystemId::GB;
  uint32_t crc = 0;
  uint64_t size = 0;
};

struct Match
{
  Game games[2];  // [0] the host's game (Game Boy 1), [1] the guest's (Game Boy 2), as this console has them
  int local = 0;  // this console's Game Boy: 0 the host's, 1 the guest's
  // The host: the guest's battery save as it sent it (PackSave's bytes; empty: none - a new game).
  std::vector<uint8_t> guest_save;
};

void Set(const Match& match);
void Clear();
bool Active();
const Match& Get();

// A game's battery save as this console keeps it, as one: <key>.srm, then <key>.rtc's 48 bytes (a cartridge with a
// clock) with the clock brought to `now` (Unix time: what mGBA does as it loads it). Empty: no save yet.
std::vector<uint8_t> ReadSave(const std::string& key, uint64_t now);
// The battery save of a Game Boy written back: its first `ram_size` bytes into <key>.srm, the rest (the clock's 48
// bytes) into <key>.rtc. `rtc_too` false: the clock isn't written (only the RAM changed).
bool WriteSave(const std::string& key, const uint8_t* data, size_t size, size_t ram_size, bool rtc_too);
// The copy of a game's save before a link match: <key>.srm.antes-link (and .rtc), the last match's replaced.
void BackupSave(const std::string& key);

// The clock's 48 bytes (mGBA's GBMBCRTCSaveBuffer: seconds, minutes, hours, days, days' high bits and flags, the same
// latched, a 64-bit Unix time; little-endian) brought to `now`: the time passed since added unless the clock is
// stopped (its halt flag), days past 511 setting the carry flag.
void AdvanceClock(uint8_t* rtc, uint64_t now);
constexpr size_t RTC_SIZE = 48;

// The battery save the guest sends to the host (a version byte, the save's bytes) and back. Max: a Game Boy
// cartridge's RAM (128 KiB, MBC5) and its clock.
std::vector<uint8_t> PackSave(const std::vector<uint8_t>& save);
bool UnpackSave(const uint8_t* data, size_t size, std::vector<uint8_t>* save);
constexpr size_t MAX_SAVE = 128 * 1024 + RTC_SIZE;

// The core draws the two screens side by side (SameBoy's "left-right" layout, 320x144): the column this console's
// Game Boy starts at and how many are its own, for a picture `width` wide (a picture of one screen: all of it).
void VisibleHalf(unsigned width, unsigned* first_column, unsigned* columns);
}  // namespace GbLink
