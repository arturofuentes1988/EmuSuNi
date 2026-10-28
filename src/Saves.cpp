// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Saves.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>

#include <sys/stat.h>

#include "Log.h"
#include "Systems.h"

namespace Saves
{
namespace
{
std::string s_data_dir = App::DATA_DIR;
}

void Init(const std::string& data_dir)
{
  s_data_dir = data_dir;
  for (const System& system : Systems())
  {
    MakeDirs(data_dir + "/saves/" + system.prefix);
    MakeDirs(data_dir + "/states/" + system.prefix);
  }
}

std::string SramPath(const std::string& game_id)
{
  return s_data_dir + "/saves/" + game_id + ".srm";
}

std::string RtcPath(const std::string& game_id)
{
  return s_data_dir + "/saves/" + game_id + ".rtc";
}

std::string DiscPath(const std::string& game_id)
{
  return s_data_dir + "/saves/" + game_id + ".disc";
}

std::string StatePath(const std::string& game_id, int slot)
{
  return s_data_dir + "/states/" + game_id + ".state" + std::to_string(slot);
}

std::string StatePicturePath(const std::string& game_id, int slot)
{
  return StatePath(game_id, slot) + ".png";
}

int64_t Modified(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) : 0;
}

uint64_t Size(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

std::vector<GameSaves> List()
{
  std::map<std::string, GameSaves> games;
  std::error_code ec;
  // Each console's folder (its games' keys start with its prefix).
  for (const System& system : Systems())
  {
    const std::string prefix = system.prefix;
    for (const auto& entry : std::filesystem::directory_iterator(s_data_dir + "/saves/" + prefix, ec))
    {
      const std::filesystem::path path = entry.path();
      std::error_code type_error;
      if (path.extension() != ".srm" || entry.is_directory(type_error))
        continue;
      const std::string key = prefix + path.stem().string();
      GameSaves& game = games[key];
      game.id = key;
      game.sram_time = Modified(path.string());
      game.sram_size = Size(path.string());
      game.latest = std::max(game.latest, game.sram_time);
    }
    for (const auto& entry : std::filesystem::directory_iterator(s_data_dir + "/states/" + prefix, ec))
    {
      const std::string name = entry.path().filename().string();
      const size_t at = name.rfind(".state");
      if (at == std::string::npos || name.size() > at + 8 || name.size() == at + 6)
        continue;  // pictures (.png), folders and other files
      const std::string digits = name.substr(at + 6);
      if (digits.find_first_not_of("0123456789") != std::string::npos)
        continue;
      const std::string key = prefix + name.substr(0, at);
      GameSaves& game = games[key];
      game.id = key;
      game.slots.push_back(std::atoi(digits.c_str()));
      game.latest = std::max(game.latest, Modified(entry.path().string()));
    }
  }
  std::vector<GameSaves> list;
  for (auto& [id, game] : games)
  {
    std::sort(game.slots.begin(), game.slots.end());
    list.push_back(std::move(game));
  }
  std::sort(list.begin(), list.end(),
            [](const GameSaves& a, const GameSaves& b) { return a.latest > b.latest; });
  return list;
}

bool DeleteSram(const std::string& game_id)
{
  std::remove(RtcPath(game_id).c_str());
  return std::remove(SramPath(game_id).c_str()) == 0;
}

bool DeleteState(const std::string& game_id, int slot)
{
  std::remove(StatePicturePath(game_id, slot).c_str());
  std::remove((StatePath(game_id, slot) + ".disc").c_str());  // (a Dreamcast state's disc)
  std::remove((StatePath(game_id, slot) + ".ra").c_str());    // (its RetroAchievements progress, 00.73)
  return std::remove(StatePath(game_id, slot).c_str()) == 0;
}
}  // namespace Saves
