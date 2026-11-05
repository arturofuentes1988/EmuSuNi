// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Cheats.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include <libretro.h>

#include "Log.h"

namespace Cheats
{
namespace
{
std::string Unquote(std::string value)
{
  while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
    value.pop_back();
  size_t start = 0;
  while (start < value.size() && value[start] == ' ')
    start++;
  value = value.substr(start);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  return value;
}

bool IsHex(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// (00.90) A line of a PlayStation code as DuckStation and PCSXR write them: "80012345 0064" (an address, a value; '?'
// for one chosen in DuckStation's menu). Its two parts as RetroArch's codes have them, or empty.
std::string CodeLine(const std::string& line)
{
  if (line.size() < 13 || !std::all_of(line.begin(), line.begin() + 8, IsHex) ||
      (line[8] != ' ' && line[8] != ':' && line[8] != '-'))
    return {};
  size_t end = 9;
  while (end < line.size() && (IsHex(line[end]) || line[end] == '?'))
    end++;
  if (end - 9 < 4 || (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != ';'))
    return {};
  return line.substr(0, 8) + " " + line.substr(9, end - 9);
}

// (00.90) A cheat file in DuckStation's or PCSXR's form (players' DuckStation cheats, which only RetroArch's form
// loaded before): "[Name]" (PCSXR's "[*Name]": on) or "#Name" begins a cheat, its code lines follow, joined by '+' as
// RetroArch's are. DuckStation's "Key = Value" lines (Type, Activation, Author...), its database's ":SLUS-00000"
// lines and ';' comments are skipped; a cheat with a value to choose ('?') is left out.
std::vector<Entry> ParseCodeLines(const std::string& text)
{
  std::vector<Entry> entries;
  Entry entry;
  bool choose = false;
  auto finish = [&]() {
    if (!entry.code.empty() && !choose)
    {
      if (entry.name.empty())
        entry.name = entry.code;
      entries.push_back(std::move(entry));
    }
    entry = {};
    choose = false;
  };
  std::istringstream file(text);
  std::string line;
  while (std::getline(file, line) && entries.size() < 20000)
  {
    line = Unquote(line);
    if (line.empty() || line[0] == ';' || line[0] == ':')
      continue;
    if (line[0] == '[' && line.back() == ']')
    {
      finish();
      entry.name = Unquote(line.substr(1, line.size() - 2));
      if (!entry.name.empty() && entry.name[0] == '*')
      {
        entry.enabled = true;
        entry.name = Unquote(entry.name.substr(1));
      }
      continue;
    }
    if (line[0] == '#')
    {
      finish();
      entry.name = Unquote(line.substr(1));
      continue;
    }
    const std::string code = CodeLine(line);
    if (code.empty())
      continue;
    if (code.find('?') != std::string::npos)
      choose = true;
    entry.code += (entry.code.empty() ? "" : "+") + code;
  }
  finish();
  return entries;
}
}  // namespace

std::vector<Entry> Parse(const std::string& text)
{
  std::istringstream file(text);
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(file, line))
  {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#')
      continue;
    std::string key = line.substr(0, eq);
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    values[key] = Unquote(line.substr(eq + 1));
  }
  if (values.find("cheats") == values.end())
    return ParseCodeLines(text);  // (00.90) not RetroArch's form: DuckStation's or PCSXR's
  std::vector<Entry> entries;
  // (GoldenEye 007's list in libretro's collection has 7232 codes.)
  const int count = std::atoi(values["cheats"].c_str());
  for (int i = 0; i < count && i < 20000; i++)
  {
    const std::string prefix = "cheat" + std::to_string(i) + "_";
    Entry entry;
    entry.name = values[prefix + "desc"];
    entry.code = values[prefix + "code"];
    entry.enabled = values[prefix + "enable"] == "true";
    if (entry.code.empty())
      continue;
    if (entry.name.empty())
      entry.name = entry.code;
    entries.push_back(std::move(entry));
  }
  return entries;
}

void List::Load(const std::string& data_dir, const std::string& game_id)
{
  m_path = data_dir + "/cheats/" + game_id + ".cht";
  std::ifstream file(m_path, std::ios::binary);
  m_entries = Parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

int List::Add(const std::vector<Entry>& entries)
{
  int added = 0;
  for (const Entry& entry : entries)
  {
    const bool have = std::any_of(m_entries.begin(), m_entries.end(), [&entry](const Entry& e) {
      return e.code == entry.code && e.name == entry.name;
    });
    if (have)
      continue;
    Entry copy = entry;
    copy.enabled = false;
    m_entries.push_back(std::move(copy));
    added++;
  }
  if (added > 0)
    Save();
  return added;
}

void List::Toggle(size_t index)
{
  if (index >= m_entries.size())
    return;
  m_entries[index].enabled = !m_entries[index].enabled;
  Save();
}

void List::SetAll(bool on)
{
  for (Entry& entry : m_entries)
    entry.enabled = on;
  Save();
}

void List::Save() const
{
  std::ostringstream file;
  file << "cheats = " << m_entries.size() << "\n\n";
  for (size_t i = 0; i < m_entries.size(); i++)
  {
    file << "cheat" << i << "_desc = \"" << m_entries[i].name << "\"\n";
    file << "cheat" << i << "_code = \"" << m_entries[i].code << "\"\n";
    file << "cheat" << i << "_enable = " << (m_entries[i].enabled ? "true" : "false") << "\n\n";
  }
  if (!m_entries.empty())
  {
    MakeDirs(m_path.substr(0, m_path.rfind('/')));
    WriteTextFile(m_path, file.str());
  }
}

void List::ApplyToCore(const CoreApi& core, SystemId system) const
{
  // Snes9x copies the code into a 256-byte buffer, FCEUmm into 1024 bytes, Mupen64Plus-Next splits
  // it into at most 256 parts, Genesis Plus GX keeps 255 characters; the other cores take any length
  // (a PSP cheat can be long).
  const bool gpgx = IsGpgx(system);
  const size_t longest = system == SystemId::SNES ? 250 : system == SystemId::NES ? 1000 :
                         system == SystemId::N64  ? 1200 :
                         gpgx                     ? 255 :
                                                    16000;
  // Genesis Plus GX has room for 150 codes in all (one entry's several, joined by '+', count each) and
  // writes past its list after that: an entry that doesn't fit any more is left out.
  size_t room = 150;
  core.cheat_reset();
  unsigned index = 0;
  for (const Entry& entry : m_entries)
  {
    if (!entry.enabled)
      continue;
    std::string code = entry.code.substr(0, longest);
    if (gpgx)
    {
      // Cut short inside a code: its whole codes only (a part of one could read as another).
      const size_t plus = code.rfind('+');
      if (code.size() < entry.code.size() && entry.code[code.size()] != '+' && plus != std::string::npos)
        code.resize(plus);
      const size_t codes = 1 + static_cast<size_t>(std::count(code.begin(), code.end(), '+'));
      if (codes > room)
        continue;
      room -= codes;
    }
    core.cheat_set(index++, true, code.c_str());
  }
}
}  // namespace Cheats
