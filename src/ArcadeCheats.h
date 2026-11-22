// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// (01.26, the owner) The Arcade's cheats, FinalBurn Neo's own. The core reads them as the game loads from
// system/fbneo/cheats (<set>.ini, <set>.dat, cheat.dat) and makes a core option of each one (SET_VARIABLES):
//   key   "fbneo-cheat-<n>-<set>-<name, its spaces, = : # as _>"
//   value "[Cheat][<file>] <heading><name>; 0 - Disabled|1 - Enabled|..." (each value "<i> - <its name>", the
//         default first and not again)
// It puts each back to its default at every start and reset (SET_VARIABLE) and turns on what GET_VARIABLE then says
// (its first frame; a change later: GET_VARIABLE_UPDATE). Value 0 turns the cheat off.
// Here: those options kept for the in-game menu's Cheats page (each cheat's value, Left / Right; Disable All), the
// player's choices kept per game (<UserDir>/cheats/<game key>.fbneo, "key=value" lines, only what isn't the cheat's
// default) and given to the core instead of the defaults at the start and after a reset. Online, none: the defaults
// (one console's would be the match's), as the other consoles' cheats. Header only (the build's list of files stays).
// The cheat files of github.com/finalburnneo/FBNeo-cheats: the game's own, downloaded when the player asks
// (CheatDownload's RunArcade) as <set>.ini.new, put in place here before the game's next start (the core reads its
// cheats at the start: a file changed under a running game would number them differently).

#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "Log.h"

namespace ArcadeCheats
{
struct Cheat
{
  std::string key;                  // the core's option
  std::string name;                 // as the core names it (a heading line of the file before it, joined)
  std::string file;                 // the file it comes from ("mslug.ini")
  std::vector<std::string> values;  // the core's, "<i> - <name>", in their order (values[0]: off)
  size_t value = 0;                 // the one chosen now
  size_t normal = 0;                // the file's default
};

namespace detail
{
struct Data
{
  std::mutex mutex;
  std::vector<Cheat> cheats;
  std::map<std::string, std::string> chosen;  // the game's file: key -> value
  std::string path;                           // that file
  bool online = false;
  bool changed = false;  // the menu chose something the core hasn't been told
  int given = 0;         // choices given to the core instead of its defaults (this start)
};
inline Data& Get()
{
  static Data data;
  return data;
}
inline constexpr const char* PREFIX = "fbneo-cheat-";

// "0 - Disabled" -> 0; not of that form: -1.
inline int ValueIndex(const std::string& value)
{
  const size_t dash = value.find(" - ");
  if (dash == std::string::npos || dash == 0)
    return -1;
  int index = 0;
  for (size_t i = 0; i < dash; i++)
  {
    if (value[i] < '0' || value[i] > '9' || index > 1000)
      return -1;
    index = index * 10 + (value[i] - '0');
  }
  return index;
}

inline void Save(Data& d)
{
  // (The choices kept also for a list the core sends again while the game runs: Declare.)
  std::ostringstream text;
  text << "# EmuSuNi: this game's FinalBurn Neo cheats as chosen in the in-game menu (key=value; the rest: default)\n";
  d.chosen.clear();
  for (const Cheat& c : d.cheats)
  {
    if (c.value != c.normal && c.value < c.values.size())
    {
      text << c.key << "=" << c.values[c.value] << "\n";
      d.chosen[c.key] = c.values[c.value];
    }
  }
  const size_t slash = d.path.rfind('/');
  if (slash != std::string::npos)
    MakeDirs(d.path.substr(0, slash));
  if (!WriteTextFile(d.path, text.str()))
    LogF("arcade: cheats: %s couldn't be written", d.path.c_str());
}
}  // namespace detail

// Before the game loads: its choices read (none online), a cheat file downloaded for it put in place
// (cheats_dir/<set>.ini.new -> <set>.ini).
inline void Begin(const std::string& choices_path, bool online, const std::string& cheats_dir, const std::string& set)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  d.cheats.clear();
  d.chosen.clear();
  d.path = choices_path;
  d.online = online;
  d.changed = false;
  d.given = 0;
  if (!set.empty())
  {
    // (The set's name as the core's driver has it: in lower case.)
    std::string name = set;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    const std::string file = cheats_dir + "/" + name + ".ini";
    if (FileExists(file + ".new"))
    {
      std::remove(file.c_str());
      const bool moved = std::rename((file + ".new").c_str(), file.c_str()) == 0;
      LogF("arcade: cheats: the downloaded %s.ini %s", name.c_str(), moved ? "put in place" : "couldn't be put in place");
    }
  }
  if (online)
    return;
  std::istringstream lines(ReadTextFile(choices_path));
  for (std::string line; std::getline(lines, line);)
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t equals = line.find('=', std::string(detail::PREFIX).size());
    if (line.rfind(detail::PREFIX, 0) == 0 && equals != std::string::npos)
      d.chosen[line.substr(0, equals)] = line.substr(equals + 1);
  }
}

// SET_VARIABLES (the Arcade's): its cheat options kept (the list before replaced). Any struct with key and value
// (retro_variable), the array ending in a null key. Returns how many.
template <typename Variable>
inline size_t Declare(const Variable* variables)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  d.cheats.clear();
  for (const Variable* v = variables; v && v->key; v++)
  {
    const std::string key = v->key;
    if (key.rfind(detail::PREFIX, 0) != 0 || !v->value)
      continue;
    const std::string text = v->value;
    const size_t semicolon = text.find("; ");
    if (semicolon == std::string::npos)
      continue;
    Cheat c;
    c.key = key;
    c.name = text.substr(0, semicolon);
    if (c.name.rfind("[Cheat][", 0) == 0)
    {
      const size_t close = c.name.find("] ", 8);
      if (close != std::string::npos)
      {
        c.file = c.name.substr(8, close - 8);
        c.name = c.name.substr(close + 2);
      }
    }
    // "<default>|<the others>": libretro's first form puts the default first, the others after it without it again -
    // each put back in its place by its number.
    std::vector<std::string> parts;
    std::string rest = text.substr(semicolon + 2);
    for (size_t bar; (bar = rest.find('|')) != std::string::npos; rest = rest.substr(bar + 1))
      parts.push_back(rest.substr(0, bar));
    parts.push_back(rest);
    std::vector<std::pair<int, std::string>> numbered;
    for (size_t i = 0; i < parts.size(); i++)
    {
      const int index = detail::ValueIndex(parts[i]);
      if (index >= 0 && std::none_of(numbered.begin(), numbered.end(), [&](const auto& n) { return n.first == index; }))
        numbered.push_back({index, parts[i]});
    }
    std::sort(numbered.begin(), numbered.end());
    for (const auto& n : numbered)
      c.values.push_back(n.second);
    if (c.values.empty())
      continue;
    const auto normal = std::find(c.values.begin(), c.values.end(), parts[0]);
    c.normal = normal == c.values.end() ? 0 : static_cast<size_t>(normal - c.values.begin());
    c.value = c.normal;
    const auto mine = d.chosen.find(key);
    if (mine != d.chosen.end())
    {
      const auto chosen = std::find(c.values.begin(), c.values.end(), mine->second);
      if (chosen != c.values.end())
        c.value = static_cast<size_t>(chosen - c.values.begin());
    }
    d.cheats.push_back(std::move(c));
  }
  return d.cheats.size();
}

// SET_VARIABLE (the core puts a cheat back to its default as the game starts and at a reset): the value to keep -
// the player's (not online), else the core's.
inline std::string Starting(const std::string& key, const std::string& value)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  if (d.online || key.rfind(detail::PREFIX, 0) != 0)
    return value;
  for (const Cheat& c : d.cheats)
  {
    if (c.key == key && c.value != c.normal && c.value < c.values.size())
    {
      d.given++;
      return c.values[c.value];
    }
  }
  return value;
}

// ---- The in-game menu's ----
inline std::vector<Cheat> List()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  return d.cheats;
}
inline bool Online()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  return d.online;
}
// The player's choices given at this start (for the log).
inline int Given()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  return d.given;
}
// "1 - Enabled" -> "Enabled".
inline std::string ValueName(const std::string& value)
{
  const size_t dash = value.find(" - ");
  return detail::ValueIndex(value) >= 0 ? value.substr(dash + 3) : value;
}
// The cheat's next (or previous: direction < 0) value, round; it is kept. Returns its name.
inline std::string Step(size_t index, int direction)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  if (index >= d.cheats.size() || d.cheats[index].values.empty())
    return {};
  Cheat& c = d.cheats[index];
  const size_t count = c.values.size();
  c.value = direction < 0 ? (c.value + count - 1) % count : (c.value + 1) % count;
  d.changed = true;
  detail::Save(d);
  return ValueName(c.values[c.value]);
}
// Every cheat to its value 0 (off). Returns how many were on.
inline int DisableAll()
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  int were_on = 0;
  for (Cheat& c : d.cheats)
  {
    were_on += c.value != 0 ? 1 : 0;
    c.value = 0;
  }
  d.changed = true;
  detail::Save(d);
  return were_on;
}

// ---- The emulator's ----
// After the menu changed a cheat: every cheat's value into the core's options (they may have been made again
// without them: a setting changed). True: the core is to be told (GET_VARIABLE_UPDATE). Online, nothing.
template <typename Map>
inline bool TakeChanges(Map* variables)
{
  detail::Data& d = detail::Get();
  std::lock_guard lock(d.mutex);
  if (!d.changed || d.online)
    return false;
  d.changed = false;
  for (const Cheat& c : d.cheats)
  {
    if (c.value < c.values.size())
      (*variables)[c.key] = c.values[c.value];
  }
  return !d.cheats.empty();
}
}  // namespace ArcadeCheats
