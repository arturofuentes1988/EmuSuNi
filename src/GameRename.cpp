// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameRename.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "CoverFiles.h"

namespace GameRename
{
namespace
{
std::string Lower(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool Exists(const std::string& path)
{
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

// A file renamed; a change of case only (one file on exFAT and FAT32) through a name of its own first.
bool Rename(const std::string& from, const std::string& to, std::string* error)
{
  std::error_code ec;
  if (Lower(from) == Lower(to) && from != to)
  {
    const std::string between = from + ".emusuni-renaming";
    std::filesystem::rename(from, between, ec);
    if (!ec)
    {
      std::filesystem::rename(between, to, ec);
      if (ec)
      {
        std::error_code back;
        std::filesystem::rename(between, from, back);
      }
    }
  }
  else
  {
    std::filesystem::rename(from, to, ec);
  }
  if (ec && error)
    *error = ec.message();
  return !ec;
}

// The discs' files: a single one of them is still a disc (its console's core keeps saves of its own by its name).
bool DiscFile(const std::string& extension)
{
  static const char* const DISCS[] = {".cue", ".chd", ".iso", ".ccd", ".toc", ".m3u", ".gdi", ".cdi", ".img", ".pbp",
                                      ".cso", ".mds"};
  const std::string lower = Lower(extension);
  return std::any_of(std::begin(DISCS), std::end(DISCS), [&lower](const char* disc) { return lower == disc; });
}
}  // namespace

bool ConsoleTakes(SystemId system)
{
  switch (system)
  {
  case SystemId::SNES:
  case SystemId::NES:
  case SystemId::GB:
  case SystemId::GBC:
  case SystemId::GBA:
  case SystemId::SG1000:
  case SystemId::SMS:
  case SystemId::GG:
  case SystemId::MD:
  case SystemId::PCE:
  case SystemId::N64:
  case SystemId::NDS:
  case SystemId::THREEDS:
  case SystemId::A2600:
  case SystemId::A7800:
  case SystemId::JAGUAR:
    return true;
  default:
    return false;
  }
}

std::string FileName(const std::string& name)
{
  std::string out;
  for (const char c : name)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == ':')
      out += " - ";
    else if (c == '/' || c == '\\' || c == '|')
      out += '-';
    else if (c == '"')
      out += '\'';
    else if (c == '?' || c == '*' || c == '<' || c == '>' || u < 0x20 || u == 0x7F)
      continue;
    else
      out += c;
  }
  // One space between words; none, and no dot, at the ends.
  std::string tidy;
  for (const char c : out)
  {
    if (c == ' ' && (tidy.empty() || tidy.back() == ' '))
      continue;
    tidy += c;
  }
  constexpr size_t MOST = 180;
  if (tidy.size() > MOST)
  {
    size_t cut = MOST;
    while (cut > 0 && (static_cast<unsigned char>(tidy[cut]) & 0xC0) == 0x80)
      cut--;
    tidy.resize(cut);
  }
  while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '.'))
    tidy.pop_back();
  while (!tidy.empty() && (tidy.front() == ' ' || tidy.front() == '.'))
    tidy.erase(tidy.begin());
  return tidy;
}

bool HasData(const std::string& user_dir, const std::string& key)
{
  if (user_dir.empty())
    return false;
  for (const char* extension : {".srm", ".rtc", ".disc", ".sav", ".eep", ".sra", ".fla", ".mpk", ".dsv"})
  {
    if (Exists(user_dir + "/saves/" + key + extension))
      return true;
  }
  for (int slot = 0; slot <= 9; slot++)
  {
    if (Exists(user_dir + "/states/" + key + ".state" + std::to_string(slot)))
      return true;
  }
  return Exists(user_dir + "/cheats/" + key + ".cht");
}

std::vector<Plan> MakePlans(const std::vector<Wanted>& wanted, std::set<std::string> ids, const std::string& user_dir,
                            std::vector<std::pair<std::string, Kept>>* kept)
{
  std::vector<Plan> plans;
  std::set<std::string> new_paths;  // lower-case: the plans' own, already given
  for (const Wanted& w : wanted)
  {
    const size_t slash = w.path.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : w.path.substr(0, slash);
    const std::string file = slash == std::string::npos ? w.path : w.path.substr(slash + 1);
    const size_t dot = file.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? file : file.substr(0, dot);
    const std::string extension = dot == std::string::npos || dot == 0 ? std::string() : file.substr(dot);
    auto keep = [&](Kept why) {
      if (kept)
        kept->emplace_back(w.key, why);
    };
    if (w.files > 1 || DiscFile(extension))
    {
      keep(Kept::Files);
      continue;
    }
    // (The key is the console's prefix and the file's name: anything else isn't a file this can rename.)
    if (!Exists(w.path) || w.key.size() < stem.size() || w.key.compare(w.key.size() - stem.size(), stem.size(), stem) != 0)
    {
      keep(Kept::Missing);
      continue;
    }
    const std::string base = FileName(w.name);
    if (base.empty())
    {
      keep(Kept::Name);
      continue;
    }
    if (base == stem)
    {
      keep(Kept::Same);
      continue;
    }
    // A free name: not another game's key, nor a file there, nor another plan's, nor one with data of its own ("(2)",
    // "(3)"... after it). Its own file's in another case is free: the same file.
    const std::string prefix = w.key.substr(0, w.key.size() - stem.size());
    std::string name = base;
    for (int n = 2;; n++)
    {
      const std::string lower = Lower(name);
      const bool own = lower == Lower(stem);
      const std::string path = dir + "/" + name + extension;
      if (own || (!ids.count(lower) && !Exists(path) && !new_paths.count(Lower(path)) &&
                  !HasData(user_dir, prefix + name)))
        break;
      name = base + " (" + std::to_string(n) + ")";
    }
    Plan plan;
    plan.from = w.path;
    plan.to = dir + "/" + name + extension;
    plan.old_key = w.key;
    plan.new_key = prefix + name;
    ids.insert(Lower(name));
    new_paths.insert(Lower(plan.to));
    plans.push_back(plan);
  }
  return plans;
}

bool Apply(const Plan& plan, const std::string& user_dir, const std::vector<std::string>& cover_places, Moved* moved)
{
  std::string error;
  if (!Rename(plan.from, plan.to, &error))
  {
    moved->error = error.empty() ? "couldn't be renamed" : error;
    return false;
  }
  moved->files.emplace_back(plan.from, plan.to);
  // What goes by its key, in the player's data folder (src/Saves.h's paths, Menu.cpp's, Emulator.cpp's bezels).
  std::vector<std::pair<std::string, std::string>> data;
  for (const char* extension : {".srm", ".rtc", ".disc", ".sav", ".eep", ".sra", ".fla", ".mpk", ".dsv"})
    data.emplace_back(user_dir + "/saves/" + plan.old_key + extension, user_dir + "/saves/" + plan.new_key + extension);
  for (int slot = 0; slot <= 9; slot++)
  {
    const std::string state = ".state" + std::to_string(slot);
    data.emplace_back(user_dir + "/states/" + plan.old_key + state, user_dir + "/states/" + plan.new_key + state);
    data.emplace_back(user_dir + "/states/" + plan.old_key + state + ".png",
                      user_dir + "/states/" + plan.new_key + state + ".png");
  }
  data.emplace_back(user_dir + "/cheats/" + plan.old_key + ".cht", user_dir + "/cheats/" + plan.new_key + ".cht");
  std::vector<std::string> places = cover_places;
  places.push_back(user_dir + "/covers-before");
  for (const std::string& place : places)
  {
    const std::string from = CoverFiles::NewFile(place, plan.old_key), to = CoverFiles::NewFile(place, plan.new_key);
    const std::string from_base = from.substr(0, from.rfind('.')), to_base = to.substr(0, to.rfind('.'));
    for (const char* extension : {".png", ".jpg", ".jpeg", ".txt"})
      data.emplace_back(from_base + extension, to_base + extension);
  }
  const System& system = SystemOfKey(plan.old_key);
  const std::string old_name = plan.old_key.substr(std::string(system.prefix).size());
  const std::string new_name = plan.new_key.substr(std::string(system.prefix).size());
  const std::string bezels = user_dir + "/bezels/" + system.key + "/";
  data.emplace_back(bezels + old_name + ".png", bezels + new_name + ".png");
  for (const auto& [from, to] : data)
  {
    if (!Exists(from))
      continue;
    if (Exists(to) && Lower(from) != Lower(to))
    {
      moved->left.push_back(from);  // (never over another file)
      continue;
    }
    if (Rename(from, to, nullptr))
      moved->files.emplace_back(from, to);
    else
      moved->left.push_back(from);
  }
  return true;
}
}  // namespace GameRename
