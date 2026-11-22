// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Cheat codes (Game Genie, Pro Action Replay, raw): data_dir/cheats/<game key>.cht, in RetroArch's
// .cht format (cheats = N, cheat0_desc, cheat0_code, cheat0_enable). The ON/OFF choices are
// written back to the file (in that form). (00.90) A PlayStation file of DuckStation's or PCSXR's
// ("[Name]" or "#Name", then lines of "80012345 0064") is read too. Shown in the menu (Triangle on a game -> Cheats) and the in-game menu;
// both find more in libretro's collection (CheatDownload.h).

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Cores.h"
#include "Systems.h"

namespace Cheats
{
struct Entry
{
  std::string name;
  std::string code;
  bool enabled = false;
};

// The codes of a .cht file's text (those without a code left out). (01.26) dreamcast: an entry of RetroArch's
// "advanced" form - an address in the Dreamcast's RAM, a value, its size; libretro's collection has those, with no
// code - becomes the GameShark pair Flycast's cheat engine takes ("0SAAAAAA VVVVVVVV", S 0 / 1 / 2: 8 / 16 / 32 bits);
// only "set to value" ones of those sizes (another kind is left out).
std::vector<Entry> Parse(const std::string& text, bool dreamcast = false);

class List
{
public:
  void Load(const std::string& data_dir, const std::string& game_id, bool dreamcast = false);
  // Codes found online join the list (off; one it has already is left out). How many joined.
  int Add(const std::vector<Entry>& entries);
  const std::vector<Entry>& Entries() const { return m_entries; }
  void Toggle(size_t index);
  void SetAll(bool on);
  // (01.36, a player who added some by mistake) One out of the list and its file for good; false: no such one.
  bool Remove(size_t index);
  const std::string& Path() const { return m_path; }
  // Into the running core (retro_cheat_reset / retro_cheat_set), as long as the console's core
  // takes a code.
  void ApplyToCore(const CoreApi& core, SystemId system) const;

private:
  void Save() const;
  std::string m_path;
  std::vector<Entry> m_entries;
};

// ---- (01.89, the owner: «prefiero la de importar trucos») Import Cheats: a RetroArch .cht file of the player's own ----
// (Triangle on a game -> Cheats, and the in-game menu's Cheats) chosen on a USB drive or in the app's storage, copied
// to the game's own file (List::Path: cheats/<console>/<file name>.cht, the SNES's in cheats/). The player's file is
// only read. Not for the Arcade (FinalBurn Neo's own files) nor the consoles whose cores take none (the NAOMI, the
// Atomiswave, the Atari 2600 and 7800, OpenBOR, the PS Vita). (01.89) The Dreamcast's .cht (libretro's: addresses and
// values, read as List::Load reads them); the 3DS's .cht of Gateway / Action Replay codes, or (01.90, the owner) an
// Azahar / Citra .txt (CTRPF's): its cheats made the game's list, all off - as Find Cheats Online's CTRPF file -, never
// Azahar's own file (saves/3ds/Azahar/cheats/<title ID>.txt), so that none is on twice.
bool Importable(SystemId system);
// A folder's subfolders and .cht files - (the 3DS) .txt too -, by name (hidden ones and the drives' system folders left
// out; 5000 at most).
struct Folder
{
  std::vector<std::string> folders;
  std::vector<std::pair<std::string, uintmax_t>> files;  // name, bytes
  bool readable = false, cut = false;
  std::string error;  // why it can't be opened
};
Folder ReadFolder(const std::string& dir, bool txt = false);
// What a file chosen is read as: a RetroArch .cht (the Dreamcast's addresses; the 3DS's Gateway / Action Replay codes),
// or an Azahar cheats file (a 3DS game's .txt: "[Name]", then lines of 8 + 8 hexadecimal digits) made a .cht.
enum class Kind
{
  Cht,
  ChtDreamcast,
  Cht3ds,
  Azahar,
};
Kind KindFor(SystemId system, const std::string& file);
enum class Import
{
  Done,
  Exists,      // the game has cheats already: asked first (replace: the one before kept as <its file>.bak)
  NotCht,      // not a RetroArch .cht ("cheats = N", then cheatN_...)
  NotAzahar,   // (01.89) not Azahar's form
  No3dsCodes,  // (01.89) a .cht for the 3DS without a code of Gateway / Action Replay lines
  Unreadable,
  TooBig,      // over 4 MB
  NotWritten,  // nothing changed
};
// from -> to (List::Path()); *codes: the codes it has (Parse's; the 3DS's: those of Gateway / Action Replay lines).
Import ImportFile(const std::string& from, const std::string& to, Kind kind, bool replace, int* codes);
// Why it wasn't imported, in plain words (English, for PS4Lang::T: the menus' toast); "" for Done and Exists.
const char* ImportProblem(Import result);
}  // namespace Cheats
