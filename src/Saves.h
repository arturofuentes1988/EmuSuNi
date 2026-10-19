// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Saved data: each game's battery save (the cartridge's SRAM, data_dir/saves/<key>.srm) and save
// states (data_dir/states/<key>.state<N>, with a picture <key>.state<N>.png); the key of another
// console's game puts them in its subfolder (saves/nes/...). Slot 0 is the state "Resume Where You
// Left Off" keeps. The menu's Saved Data tab lists them (Dolphin for PS4 has Memory Cards there).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Saves
{
constexpr int SLOTS = 8;  // slots 1..8 in the menus; 0 = resume state

void Init(const std::string& data_dir);
std::string SramPath(const std::string& game_id);
// The cartridge's clock (Game Boy games with one: Pokémon Gold, Silver, Crystal), beside the save.
std::string RtcPath(const std::string& game_id);
// The disc a game on several discs was played with last (its number from 0, as text), beside the save.
std::string DiscPath(const std::string& game_id);
std::string StatePath(const std::string& game_id, int slot);
std::string StatePicturePath(const std::string& game_id, int slot);
// Unix time of a file's last change, 0 when it doesn't exist.
int64_t Modified(const std::string& path);
uint64_t Size(const std::string& path);

struct GameSaves
{
  std::string id;  // the game's key
  int64_t sram_time = 0;
  uint64_t sram_size = 0;
  std::vector<int> slots;  // slots with a state (0 = resume)
  int64_t latest = 0;
};
// Every game with saved data, the latest first.
std::vector<GameSaves> List();
bool DeleteSram(const std::string& game_id);
bool DeleteState(const std::string& game_id, int slot);
}  // namespace Saves
