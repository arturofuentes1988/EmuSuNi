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
