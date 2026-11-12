// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameDelete.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "Log.h"

namespace GameDelete
{
Files DeleteFiles(const Library::Game& game)
{
  Files out;
  for (const std::string& file : Library::GameFiles(game))
  {
    std::error_code ec;
    if (std::filesystem::remove(file, ec))
    {
      LogLine("menu: deleted " + file);
      out.deleted.push_back(file);
      continue;
    }
    if (!ec)
    {
      LogLine("menu: " + file + ": already gone");
      out.deleted.push_back(file);
      continue;
    }
    out.failed.push_back(file);
    if (out.error.empty())
      out.error = ec.message();
    LogLine("menu: couldn't delete " + file + ": " + ec.message());
  }
  out.own_gone = true;
  for (const std::string& own : game.discs.size() > 1 ? game.discs : std::vector<std::string>{game.path})
  {
    std::error_code ec;
    out.own_gone = out.own_gone && !std::filesystem::exists(own, ec) && !ec;
  }
  return out;
}

bool RemoveEmptyFolder(const Library::Game& game)
{
  // (Named after its folder: its id isn't its file's name - Library::Scan gives such a game its folder's name.)
  const std::filesystem::path file(game.path);
  if (game.id.empty() || game.id == file.stem().string() || !file.has_parent_path())
    return false;
  std::error_code ec;
  const std::filesystem::path folder = file.parent_path();
  if (!std::filesystem::is_directory(folder, ec) || !std::filesystem::is_empty(folder, ec) || ec)
    return false;
  if (!std::filesystem::remove(folder, ec))
    return false;
  LogLine("menu: deleted its folder (empty now) " + folder.string());
  return true;
}

std::set<std::string> OwnPictures(const std::vector<std::string>& names, const std::map<std::string, std::string>& index,
                                  const std::unordered_set<std::string>& kept)
{
  std::set<std::string> pictures;
  for (const std::string& name : names)
  {
    const auto it = index.find(name);
    if (it != index.end() && kept.count(it->second) == 0)
      pictures.insert(it->second);
  }
  return pictures;
}

bool DropPlayed(const std::string& file, const std::set<std::string>& paths,
                const std::function<std::string(const std::string&)>& from_stored)
{
  std::ifstream in(file);
  if (!in)
    return false;
  std::string text, line;
  bool dropped = false;
  while (std::getline(in, line))
  {
    const size_t space = line.find(' ');
    if (space != std::string::npos && space > 0 && paths.count(from_stored(line.substr(space + 1))) > 0)
    {
      dropped = true;
      continue;
    }
    text += line + "\n";
  }
  in.close();
  return dropped && WriteTextFile(file, text);
}
}  // namespace GameDelete
