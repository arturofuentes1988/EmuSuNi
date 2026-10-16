// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Cheats.h"

#include <cstdlib>
#include <fstream>
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

void List::Load(const std::string& data_dir, const std::string& game_id)
{
  m_entries.clear();
  m_path = data_dir + "/cheats/" + game_id + ".cht";
  std::ifstream file(m_path);
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(file, line))
  {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#')
      continue;
    std::string key = line.substr(0, eq);
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    values[key] = Unquote(line.substr(eq + 1));
  }
  const int count = std::atoi(values["cheats"].c_str());
  for (int i = 0; i < count && i < 1000; i++)
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
    m_entries.push_back(std::move(entry));
  }
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

void List::ApplyToCore(const CoreApi& core) const
{
  core.cheat_reset();
  unsigned index = 0;
  for (const Entry& entry : m_entries)
  {
    // The core copies the code into a 256-byte buffer.
    if (entry.enabled)
      core.cheat_set(index++, true, entry.code.substr(0, 250).c_str());
  }
}
}  // namespace Cheats
