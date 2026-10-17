// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>

#include <minizip/unzip.h>

#include "Log.h"

namespace Library
{
namespace
{
constexpr long MAX_ROM = 96L * 1024 * 1024;  // the biggest N64 cartridges are 64 MiB

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool HasExtension(const std::string& name, std::initializer_list<const char*> extensions)
{
  const std::string lower = Lower(name);
  for (const char* extension : extensions)
  {
    const size_t length = std::char_traits<char>::length(extension);
    if (lower.size() > length && lower.compare(lower.size() - length, length, extension) == 0)
      return true;
  }
  return false;
}

bool IsRomFile(const std::string& name)
{
  return SystemOfFile(name) != nullptr;
}

// The console of the first ROM inside a .zip (nullptr: none).
const System* SystemOfZip(const std::string& path)
{
  unzFile zip = unzOpen(path.c_str());
  if (!zip)
    return nullptr;
  const System* found = nullptr;
  for (int result = unzGoToFirstFile(zip); result == UNZ_OK && !found; result = unzGoToNextFile(zip))
  {
    char name[512];
    unz_file_info info;
    if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) == UNZ_OK)
      found = SystemOfFile(name);
  }
  unzClose(zip);
  return found;
}

// The Game Boy and the Game Boy Color: their games' files are alike (a game made for both, like
// Pokémon Yellow, is a .gb).
bool GameBoyFamily(const System* system)
{
  return system && (system->id == SystemId::GB || system->id == SystemId::GBC);
}

// Is the file one of the console's games (a ROM, or a .zip with one inside; without open_zips,
// any .zip)? Without open_zips (the console's folder is its own) a Game Boy game in the Game Boy
// Color's folder is the Game Boy Color's, and the other way round: the folder says where it goes.
bool IsGameOf(const std::filesystem::path& path, const System& system, bool open_zips = true)
{
  const std::string name = path.filename().string();
  if (HasExtension(name, {".zip"}))
    return !open_zips || SystemOfZip(path.string()) == &system;
  const System* of = SystemOfFile(name);
  return of == &system || (!open_zips && GameBoyFamily(of) && GameBoyFamily(&system));
}

const std::map<std::string, std::string>& Regions()
{
  static const std::map<std::string, std::string> regions = {
      {"u", "USA"},          {"usa", "USA"},         {"e", "Europe"},
      {"europe", "Europe"},  {"j", "Japan"},         {"japan", "Japan"},
      {"ue", "USA, Europe"}, {"usa, europe", "USA, Europe"},
      {"ju", "Japan, USA"},  {"japan, usa", "Japan, USA"},
      {"jue", "World"},      {"world", "World"},     {"w", "World"},
      {"f", "France"},
      {"france", "France"},  {"g", "Germany"},       {"germany", "Germany"},
      {"s", "Spain"},        {"spain", "Spain"},     {"i", "Italy"},
      {"italy", "Italy"},    {"b", "Brazil"},        {"brazil", "Brazil"},
      {"k", "Korea"},        {"korea", "Korea"},     {"a", "Australia"},
      {"australia", "Australia"}, {"c", "China"},    {"ch", "China"},
      {"china", "China"},    {"h", "Holland"},       {"nl", "Netherlands"},
      {"sw", "Sweden"},      {"canada", "Canada"},   {"asia", "Asia"},
  };
  return regions;
}

void ScanFolder(const std::filesystem::path& folder, int depth, const System& system, bool open_zips,
                std::vector<Game>* games)
{
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
  {
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.' || name == "System Volume Information" ||
        name == "$RECYCLE.BIN")
    {
      continue;
    }
    std::error_code type_error;
    if (entry.is_directory(type_error))
    {
      if (depth < 4)
        ScanFolder(entry.path(), depth + 1, system, open_zips, games);
      continue;
    }
    if (!IsGameOf(entry.path(), system, open_zips))
      continue;
    Game game;
    game.path = entry.path().string();
    game.id = entry.path().stem().string();
    game.system = system.id;
    game.key = system.prefix + game.id;
    std::error_code size_error;
    game.size = entry.file_size(size_error);
    ParseName(game.id, &game.title, &game.region, &game.tags);
    games->push_back(std::move(game));
  }
}
}  // namespace

bool IsRomName(const std::string& name)
{
  return IsRomFile(name) || HasExtension(name, {".zip"});
}

void ParseName(const std::string& stem, std::string* title, std::string* region, std::string* tags)
{
  // The title ends where the first "(" or "[" group starts.
  size_t end = stem.size();
  for (size_t i = 0; i < stem.size(); i++)
  {
    if ((stem[i] == '(' || stem[i] == '[') && i > 0)
    {
      end = i;
      break;
    }
  }
  std::string name = stem.substr(0, end);
  while (!name.empty() && (name.back() == ' ' || name.back() == '_'))
    name.pop_back();
  std::replace(name.begin(), name.end(), '_', ' ');
  // "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
  const size_t dash = name.find(" - ");
  std::string head = name.substr(0, dash), rest = dash == std::string::npos ? "" : name.substr(dash);
  for (const char* article : {", The", ", A", ", An"})
  {
    const size_t length = std::char_traits<char>::length(article);
    if (head.size() > length && head.compare(head.size() - length, length, article) == 0)
    {
      head = std::string(article + 2) + " " + head.substr(0, head.size() - length);
      break;
    }
  }
  *title = head + rest;
  if (title->empty())
    *title = stem;
  region->clear();
  tags->clear();
  // The groups after the title: the first known region, the rest are tags.
  for (size_t i = end; i < stem.size();)
  {
    const char open = stem[i];
    const char close = open == '(' ? ')' : open == '[' ? ']' : 0;
    if (!close)
    {
      i++;
      continue;
    }
    const size_t stop = stem.find(close, i + 1);
    if (stop == std::string::npos)
      break;
    const std::string group = stem.substr(i, stop - i + 1);
    const std::string inside = Lower(stem.substr(i + 1, stop - i - 1));
    const auto known = Regions().find(inside);
    if (open == '(' && region->empty() && known != Regions().end())
      *region = known->second;
    else
      *tags += (tags->empty() ? "" : " ") + group;
    i = stop + 1;
  }
}

std::vector<Game> Scan(const std::string& folder, SystemId system, bool open_zips)
{
  std::vector<Game> games;
  ScanFolder(folder, 0, GetSystem(system), open_zips, &games);
  std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
    const std::string la = Lower(a.title), lb = Lower(b.title);
    return la != lb ? la < lb : a.id < b.id;
  });
  return games;
}

int CountGames(const std::string& folder, SystemId system)
{
  int count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
  {
    std::error_code type_error;
    const System& console = GetSystem(system);
    if (!entry.is_directory(type_error) &&
        (IsGameOf(entry.path(), console) ||
         (GameBoyFamily(&console) && GameBoyFamily(SystemOfFile(entry.path().filename().string())))))
      count++;
  }
  return count;
}

bool ReadRom(const std::string& path, std::vector<uint8_t>* data, std::string* error)
{
  data->clear();
  if (!HasExtension(path, {".zip"}))
  {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
    {
      *error = "the file can't be opened";
      return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > MAX_ROM)
    {
      fclose(file);
      *error = "not a game (its size)";
      return false;
    }
    data->resize(static_cast<size_t>(size));
    const bool ok = fread(data->data(), 1, data->size(), file) == data->size();
    fclose(file);
    if (!ok)
      *error = "the file couldn't be read";
    return ok;
  }
  unzFile zip = unzOpen(path.c_str());
  if (!zip)
  {
    *error = "the .zip can't be opened";
    return false;
  }
  bool found = false;
  for (int result = unzGoToFirstFile(zip); result == UNZ_OK; result = unzGoToNextFile(zip))
  {
    char name[512];
    unz_file_info info;
    if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
      continue;
    if (!IsRomFile(name) || info.uncompressed_size == 0 ||
        info.uncompressed_size > MAX_ROM)
    {
      continue;
    }
    if (unzOpenCurrentFile(zip) != UNZ_OK)
      break;
    data->resize(info.uncompressed_size);
    const int read = unzReadCurrentFile(zip, data->data(), static_cast<unsigned>(data->size()));
    unzCloseCurrentFile(zip);
    found = read == static_cast<int>(data->size());
    break;
  }
  unzClose(zip);
  if (!found)
    *error = "no game in the .zip";
  return found;
}
}  // namespace Library
