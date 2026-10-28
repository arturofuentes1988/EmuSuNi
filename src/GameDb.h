// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The cores' own lists of games and the settings each needs (00.81, the owner: "revisa en los proyectos originales de
// cada emulador si existe una base de datos ... y aplícalos"), as the app sees them: what the list gives the game
// starting, in the log ("gamedb: <core> <game>: <setting> = <value> (<source>)"), and the app's corrections to a list
// from its project's newer entries. Each core applies its own list itself (scratchpad gamedb-0081\informe.md: which
// list each one has and how it is read):
//   - Mupen64Plus-Next (the N64): mupen64plus.ini, built into the core, written to system/Mupen64plus/ when it starts
//     and read at each game's start (by the ROM's MD5, else the header's CRCs): save type, CPU timing (CountPerOp),
//     the 8 MB expansion off, SI/AI DMA timing, which controller paks the game takes, fix patches.
//   - PPSSPP (the PSP): compat.ini in the package (its flags logged by the core: scripts/build-psp-core.sh).
//   - SwanStation (the PlayStation), Flycast (Dreamcast, NAOMI, Atomiswave), Azahar (3DS), Snes9x, FCEUmm, mGBA,
//     Genesis Plus GX, Beetle PCE Fast, melonDS DS, FinalBurn Neo: compiled into the core.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace GameDb
{
// One line of the log: "gamedb: <core> <game>: <setting> = <value> (<source>)".
void Log(const char* core, const std::string& game, const std::string& setting, const std::string& value,
         const std::string& source);

// ---- The N64: mupen64plus.ini ----
// The game's entry in the core's list, found by the cartridge header's CRCs (0x10 and 0x14, big-endian: the list's
// "CRC=" - the core looks the ROM's MD5 up first, the same entry for a good dump), the entry it refers to
// (RefMD5) filling in what it doesn't say. `settings`: the ones the core applies, in the list's words and order.
struct N64Entry
{
  std::string md5, good_name;
  std::vector<std::pair<std::string, std::string>> settings;
};
// `text` is the list's contents; false if the cartridge isn't in it. Of several entries with those CRCs, the good
// dump's ("[!]"), else the first.
bool FindN64(const std::string& text, uint32_t crc1, uint32_t crc2, N64Entry* entry);

// The app's correction of the core's list for a cartridge (its project's newer entry; the core's copy is older): the
// setting as the core's option, its value and why. Null when the list is right for it.
struct N64Correction
{
  const char* name;
  uint32_t crc1, crc2;
  const char* option;  // mupen64plus-<option>
  const char* value;
  const char* source;
};
const N64Correction* FindN64Correction(uint32_t crc1, uint32_t crc2);

// The list's settings for the game in the log, read from `ini_path` (the core's own copy); `header_name` names it
// when it isn't in the list.
void LogN64(const std::string& ini_path, uint32_t crc1, uint32_t crc2, const std::string& header_name);
}  // namespace GameDb
