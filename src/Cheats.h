// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Cheat codes (Game Genie, Pro Action Replay, raw): data_dir/cheats/<game key>.cht, in RetroArch's
// .cht format (cheats = N, cheat0_desc, cheat0_code, cheat0_enable). The ON/OFF choices are
// written back to the file. Shown in the menu (Triangle on a game -> Cheats) and the in-game menu.

#pragma once

#include <string>
#include <vector>

#include "Cores.h"

namespace Cheats
{
struct Entry
{
  std::string name;
  std::string code;
  bool enabled = false;
};

class List
{
public:
  void Load(const std::string& data_dir, const std::string& game_id);
  const std::vector<Entry>& Entries() const { return m_entries; }
  void Toggle(size_t index);
  void SetAll(bool on);
  const std::string& Path() const { return m_path; }
  // Into the running core (retro_cheat_reset / retro_cheat_set).
  void ApplyToCore(const CoreApi& core) const;

private:
  void Save() const;
  std::string m_path;
  std::vector<Entry> m_entries;
};
}  // namespace Cheats
