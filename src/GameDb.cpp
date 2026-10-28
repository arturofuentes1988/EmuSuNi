// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "GameDb.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>

#include "Log.h"

namespace GameDb
{
namespace
{
std::string_view Trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

bool SameNoCase(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
  {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z')
      x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z')
      y = static_cast<char>(y - 'a' + 'A');
    if (x != y)
      return false;
  }
  return true;
}

using Keys = std::vector<std::pair<std::string_view, std::string_view>>;

// Each section of mupen64plus.ini ("[<MD5>]", then "Key=Value" lines; ";" starts a comment) to `each(md5, keys)`,
// until it returns true.
template <typename Each>
void Sections(std::string_view text, Each each)
{
  std::string_view md5;
  Keys keys;
  bool in_section = false;
  size_t pos = 0;
  while (pos <= text.size())
  {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    const std::string_view line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line.front() == ';')
      continue;
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
    {
      if (in_section && each(md5, keys))
        return;
      md5 = line.substr(1, line.size() - 2);
      keys.clear();
      in_section = true;
      continue;
    }
    const size_t eq = line.find('=');
    if (in_section && eq != std::string_view::npos)
      keys.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
  }
  if (in_section)
    each(md5, keys);
}

std::string_view Value(const Keys& keys, std::string_view key)
{
  for (const auto& [k, v] : keys)
  {
    if (k == key)
      return v;
  }
  return {};
}

// The keys the core applies (main.c, rom.c): the others (GoodName, CRC, Players, Status...) only describe the game.
bool Applied(std::string_view key)
{
  for (const char* known : {"SaveType", "CountPerOp", "CountPerOpDenomPot", "DisableExtraMem", "SiDmaDuration",
                            "AiDmaModifier", "Mempak", "Rumble", "Transferpak", "Biopak"})
  {
    if (key == known)
      return true;
  }
  return key.substr(0, 5) == "Cheat";
}

void Add(N64Entry* entry, const Keys& keys)
{
  for (const auto& [k, v] : keys)
  {
    if (!Applied(k))
      continue;
    bool have = false;
    for (const auto& setting : entry->settings)
      have = have || setting.first == k;
    if (!have)
      entry->settings.emplace_back(std::string(k), std::string(v));
  }
}
}  // namespace

void Log(const char* core, const std::string& game, const std::string& setting, const std::string& value,
         const std::string& source)
{
  LogF("gamedb: %s %s: %s = %s (%s)", core, game.c_str(), setting.c_str(), value.c_str(), source.c_str());
}

bool FindN64(const std::string& text, uint32_t crc1, uint32_t crc2, N64Entry* entry)
{
  bool found = false;
  std::string ref;
  Sections(text, [&](std::string_view md5, const Keys& keys) {
    const std::string_view crc = Value(keys, "CRC");
    // (The list writes a few CRCs without their leading zeros: "FC70E272 8FFE7AA".)
    unsigned a = 0, b = 0;
    const std::string crc_text(crc);
    if (crc.empty() || sscanf(crc_text.c_str(), "%x %x", &a, &b) != 2 || a != crc1 || b != crc2)
      return false;
    const std::string_view name = Value(keys, "GoodName");
    const bool good = name.find("[!]") != std::string_view::npos;
    if (found && !good)
      return false;
    *entry = {};
    entry->md5 = std::string(md5);
    entry->good_name = std::string(name);
    Add(entry, keys);
    ref = std::string(Value(keys, "RefMD5"));
    found = true;
    return good;  // a good dump's entry ends the search
  });
  // An entry that refers to another (RefMD5) takes what it doesn't say from it, as the core's list does.
  for (int depth = 0; found && !ref.empty() && depth < 4; depth++)
  {
    std::string next;
    Sections(text, [&](std::string_view md5, const Keys& keys) {
      if (!SameNoCase(md5, ref))
        return false;
      if (entry->good_name.empty())
        entry->good_name = std::string(Value(keys, "GoodName"));
      Add(entry, keys);
      next = std::string(Value(keys, "RefMD5"));
      return true;
    });
    ref = next;
  }
  return found;
}

const N64Correction* FindN64Correction(uint32_t crc1, uint32_t crc2)
{
  // mupen64plus-core's own list (github.com/mupen64plus/mupen64plus-core, data/mupen64plus.ini) after the copy in
  // Mupen64Plus-Next. Not taken: CountPerOp=1 for Quake II and 007 - Goldfinger (Mupen64Plus-Next left it out on
  // purpose: "cripples perf"), SiDmaDuration=8192 for Pokemon Stadium 1 and 2 (for their GB Tower with a Transfer Pak,
  // which the app doesn't plug in), ROM hacks and 64DD disks.
  static const N64Correction LIST[] = {
      {"Wipeout 64 (U)", 0x132D2732, 0xC70E9118, "CountPerOp", "2",
       "mupen64plus-core 2025-12, issue #1160: with CountPerOp 1 each press counts more than once; the European "
       "version keeps 1"},
  };
  for (const N64Correction& correction : LIST)
  {
    if (correction.crc1 == crc1 && correction.crc2 == crc2)
      return &correction;
  }
  return nullptr;
}

void LogN64(const std::string& ini_path, uint32_t crc1, uint32_t crc2, const std::string& header_name)
{
  std::ifstream file(ini_path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (text.empty())
  {
    LogF("gamedb: n64 %s: the core's list (%s) wasn't there to read", header_name.c_str(), ini_path.c_str());
    return;
  }
  N64Entry entry;
  if (!FindN64(text, crc1, crc2, &entry))
  {
    LogF("gamedb: n64 %s: not in mupen64plus.ini (CRC %08X %08X): the core's defaults (EEPROM 4 KB saves, "
         "CountPerOp 2, every controller pak)",
         header_name.c_str(), static_cast<unsigned>(crc1), static_cast<unsigned>(crc2));
    return;
  }
  const std::string game = entry.good_name.empty() ? header_name : entry.good_name;
  int fixes = 0;
  bool any = false;
  for (const auto& [key, value] : entry.settings)
  {
    // Rumble and Controller Pak are Yes unless the list says No: only a No tells something.
    if ((key == "Rumble" || key == "Mempak") && value != "No")
      continue;
    if (key.compare(0, 5, "Cheat") == 0)
    {
      fixes++;
      continue;
    }
    Log("n64", game, key, value, "mupen64plus.ini");
    any = true;
  }
  if (fixes > 0)
    Log("n64", game, "fix patches", std::to_string(fixes), "mupen64plus.ini");
  if (!any && fixes == 0)
    LogF("gamedb: n64 %s: in mupen64plus.ini with the core's defaults", game.c_str());
}
}  // namespace GameDb
