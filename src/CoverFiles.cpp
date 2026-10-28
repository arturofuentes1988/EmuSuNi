// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "CoverFiles.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

#include "Log.h"

namespace CoverFiles
{
namespace
{
constexpr const char* MOVED_MARK = ".moved-from-covers";  // in covers/snes/: MoveOldSnesCovers was done

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// The folders in `place` named `name` in any case, the exact name first (a case-sensitive storage, the PS4's
// /data, can have covers/snes and covers/SNES side by side).
std::vector<std::string> FoldersNamed(const std::string& place, const std::string& name)
{
  std::vector<std::string> found;
  std::error_code ec;
  // (Not a range-for: its ++ throws when a USB drive goes away meanwhile; increment(ec) just stops.)
  for (std::filesystem::directory_iterator it(place, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string entry = it->path().filename().string();
    std::error_code type;
    if (Lower(entry) == name && it->is_directory(type))
    {
      if (entry == name)
        found.insert(found.begin(), entry);
      else
        found.push_back(entry);
    }
  }
  return found;
}

// A console's folder in `place` as it is there (another case), else its key.
std::string FolderIn(const std::string& place, const System& system)
{
  const std::vector<std::string> folders = FoldersNamed(place, system.key);
  return folders.empty() ? std::string(system.key) : folders.front();
}

int64_t Modified(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_mtime) : 0;
}

void Log(const std::string& text)
{
  LogLine("covers: " + text);
}
}  // namespace

bool IsPicture(const std::string& file_name)
{
  // (A name may begin with a dot - ".hack__Link (Japan).png" -, but not a Mac's ._Name.png, nor Find Cover's
  // .preview<N>.png.)
  if (file_name.rfind("._", 0) == 0 || file_name.rfind(".preview", 0) == 0)
    return false;
  const size_t dot = file_name.rfind('.');
  if (dot == std::string::npos || dot == 0)
    return false;
  const std::string extension = Lower(file_name.substr(dot));
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

std::vector<std::string> Names(const Library::Game& game)
{
  const std::string prefix = GetSystem(game.system).prefix;
  std::vector<std::string> names = {Lower(game.key), prefix + Lower(game.title)};
  for (const std::string& name : game.cover_names)
    names.push_back(prefix + Lower(name));
  return names;
}

std::string NewFile(const std::string& place, const std::string& game_key)
{
  const System& system = SystemOfKey(game_key);
  const std::string name = game_key.substr(std::strlen(system.prefix));
  return place + "/" + FolderIn(place, system) + "/" + name + ".png";
}

void List(const std::string& place, Index* index)
{
  // The place's own folders, once (by lower-case name): the consoles' are among them.
  std::map<std::string, std::vector<std::string>> folders;
  {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(place, ec), end; !ec && it != end; it.increment(ec))
    {
      std::error_code type;
      if (it->is_directory(type))
      {
        const std::string entry = it->path().filename().string();
        folders[Lower(entry)].push_back(entry);
      }
    }
  }
  struct Found
  {
    std::string path;
    int rank;  // 0: the Super Nintendo's old place (covers/ itself), 1: the console's folder
    int64_t time = -1;  // modified (read when two pictures have one name)
  };
  for (const System& system : Systems())
  {
    std::vector<std::pair<std::string, int>> places;
    if (system.id == SystemId::SNES)
      places.emplace_back(place, 0);
    if (const auto it = folders.find(system.key); it != folders.end())
    {
      for (const std::string& folder : it->second)
        places.emplace_back(place + "/" + folder, 1);
    }
    const std::string prefix = system.prefix;
    std::map<std::string, Found> found;
    for (const auto& [folder, rank] : places)
    {
      std::error_code ec;
      for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
      {
        const std::string file = it->path().filename().string();
        if (!IsPicture(file))
          continue;
        const std::string name = prefix + Lower(file.substr(0, file.rfind('.')));
        Found candidate{it->path().string(), rank};
        auto [at, fresh] = found.try_emplace(name, candidate);
        if (fresh)
          continue;
        Found& kept = at->second;
        if (candidate.rank != kept.rank)
        {
          if (candidate.rank > kept.rank)
            kept = candidate;
          continue;
        }
        // One folder (or two cases of one): the newest picture; the same second, the first name.
        if (kept.time < 0)
          kept.time = Modified(kept.path);
        candidate.time = Modified(candidate.path);
        if (candidate.time > kept.time || (candidate.time == kept.time && candidate.path < kept.path))
          kept = candidate;
      }
    }
    for (const auto& [name, picture] : found)
      (*index)[name] = picture.path;
  }
}

void MoveOldSnesCovers(const std::string& place)
{
  const std::string folder = place + "/" + FolderIn(place, GetSystem(SystemId::SNES));
  const std::string mark = folder + "/" + MOVED_MARK;
  if (FileExists(mark))
    return;
  std::error_code ec;
  if (!std::filesystem::is_directory(place, ec))
    return;  // (no covers there yet: nothing to move)
  MakeDirs(folder);
  // Each name keeps the picture List() shows for it: a name covers/snes/ has already (any case of it, any extension)
  // leaves its pictures here (they lose to that one); of a name only here, its newest picture moves (the others stay
  // here, and lose to it).
  std::set<std::string> there;
  for (const std::string& name : FoldersNamed(place, GetSystem(SystemId::SNES).key))
  {
    std::error_code list;
    for (std::filesystem::directory_iterator it(place + "/" + name, list), end; !list && it != end;
         it.increment(list))
    {
      const std::string file = it->path().filename().string();
      if (IsPicture(file))
        there.insert(Lower(file.substr(0, file.rfind('.'))));
    }
  }
  std::map<std::string, std::pair<std::string, int64_t>> newest;  // name -> its file here, modified
  int moved = 0, kept = 0, failed = 0;
  for (std::filesystem::directory_iterator it(place, ec), end; !ec && it != end; it.increment(ec))
  {
    std::error_code type;
    const std::string file = it->path().filename().string();
    if (!IsPicture(file) || !it->is_regular_file(type))
      continue;
    const std::string name = Lower(file.substr(0, file.rfind('.')));
    if (there.count(name))
    {
      kept++;
      continue;
    }
    const int64_t time = Modified(place + "/" + file);
    auto [at, fresh] = newest.try_emplace(name, file, time);
    if (fresh)
      continue;
    kept++;
    if (time > at->second.second || (time == at->second.second && file < at->second.first))
      at->second = {file, time};
  }
  if (ec)
  {
    Log(place + " can't be read (" + ec.message() + "): its Super Nintendo covers stay there for now");
    return;
  }
  for (const auto& [name, picture] : newest)
  {
    const std::string from = place + "/" + picture.first, to = folder + "/" + picture.first;
    if (FileExists(to))
      kept++;  // (a storage that ignores case found it in another case: that one is shown)
    else if (std::rename(from.c_str(), to.c_str()) == 0)
      moved++;
    else
      failed++;
  }
  if (moved > 0 || kept > 0 || failed > 0)
  {
    Log("Super Nintendo covers moved from " + place + " into " + folder + ": " + std::to_string(moved) +
        (kept > 0 ? ", " + std::to_string(kept) + " left there (a picture of that name wins over them)" :
                    std::string()) +
        (failed > 0 ? ", " + std::to_string(failed) + " couldn't be moved (tried again next time)" : std::string()));
  }
  if (failed > 0)
    return;
  if (!WriteTextFile(mark, "The Super Nintendo's covers that were right in covers/ were moved here (EmuSuNi 00.81).\n"))
    Log(mark + " can't be written: the move is looked at again next time");
}
}  // namespace CoverFiles
