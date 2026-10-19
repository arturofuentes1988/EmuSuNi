// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Cheat codes (Game Genie, Pro Action Replay, raw): data_dir/cheats/<game key>.cht, in RetroArch's
// .cht format (cheats = N, cheat0_desc, cheat0_code, cheat0_enable). The ON/OFF choices are
// written back to the file. Shown in the menu (Triangle on a game -> Cheats) and the in-game menu;
// both find more in libretro's collection (CheatDownload.h).

#pragma once

#include <string>
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

// The codes of a .cht file's text (those without a code left out).
std::vector<Entry> Parse(const std::string& text);

class List
{
public:
  void Load(const std::string& data_dir, const std::string& game_id);
  // Codes found online join the list (off; one it has already is left out). How many joined.
  int Add(const std::vector<Entry>& entries);
  const std::vector<Entry>& Entries() const { return m_entries; }
  void Toggle(size_t index);
  void SetAll(bool on);
  const std::string& Path() const { return m_path; }
  // Into the running core (retro_cheat_reset / retro_cheat_set), as long as the console's core
  // takes a code.
  void ApplyToCore(const CoreApi& core, SystemId system) const;

private:
  void Save() const;
  std::string m_path;
  std::vector<Entry> m_entries;
};
}  // namespace Cheats
