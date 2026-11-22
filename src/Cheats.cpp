// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Cheats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// (01.26, the owner: the Dreamcast's cheats) A Dreamcast entry of RetroArch's "advanced" form as libretro's collection
// has them (cheatN_address: an offset in the 16 MB of RAM, decimal; _value; _memory_search_size: 3 / 4 / 5 for 8 / 16
// / 32 bits; _cheat_type 1: set to the value; RetroArch writes the RAM itself, cheatN_code empty) as the GameShark pair
// Flycast's cheat engine takes: "0SAAAAAA VVVVVVVV", S 0 / 1 / 2 for 8 / 16 / 32 bits. Another kind, size, a repeat
// or a big-endian one: "" (left out).
std::string DreamcastCode(std::map<std::string, std::string>& values, const std::string& prefix)
{
  const auto number = [&](const char* key, long long fallback) {
    const auto it = values.find(prefix + key);
    if (it == values.end() || it->second.empty())
      return fallback;
    char* end = nullptr;
    const long long n = std::strtoll(it->second.c_str(), &end, 10);
    return end && *end == '\0' ? n : -1LL;
  };
  if (number("cheat_type", 0) != 1 || number("repeat_count", 1) > 1 || values[prefix + "big_endian"] == "true")
    return {};
  const long long size = number("memory_search_size", -1);
  const int s = size == 3 ? 0 : size == 4 ? 1 : size == 5 ? 2 : -1;
  const long long address = number("address", -1), value = number("value", -1);
  if (s < 0 || address < 0 || address >= 0x1000000 || value < 0)
    return {};
  const unsigned long long mask = s == 0 ? 0xFFull : s == 1 ? 0xFFFFull : 0xFFFFFFFFull;
  char code[24];
  std::snprintf(code, sizeof(code), "%02X%06llX %08llX", s, static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(value) & mask);
  return code;
}
}  // namespace

std::vector<Entry> Parse(const std::string& text, bool dreamcast)
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
    if (entry.code.empty() && dreamcast)
      entry.code = DreamcastCode(values, prefix);  // ((01.26) an address, a value: a GameShark pair)
    if (entry.code.empty())
      continue;
    if (entry.name.empty())
      entry.name = entry.code;
    entries.push_back(std::move(entry));
  }
  return entries;
}

void List::Load(const std::string& data_dir, const std::string& game_id, bool dreamcast)
{
  m_path = data_dir + "/cheats/" + game_id + ".cht";
  std::ifstream file(m_path, std::ios::binary);
  m_entries =
      Parse(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()), dreamcast);
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

bool List::Remove(size_t index)
{
  if (index >= m_entries.size())
    return false;
  LogF("cheats: deleted \"%s\" (%zu left)", m_entries[index].name.c_str(), m_entries.size() - 1);
  m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
  Save();
  return true;
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
  // ((01.36) The last one deleted: the file written with none, else the codes would come back from it.)
  if (!m_entries.empty() || std::ifstream(m_path).good())
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

// ---- (01.89) Import Cheats ----
bool Importable(SystemId system)
{
  // ((01.89) the Dreamcast and the 3DS too: their cores take cheats since 01.26)
  bool none = (IsFlycast(system) && system != SystemId::DC) || system == SystemId::ARCADE ||
              system == SystemId::A2600 || system == SystemId::A7800 || IsVita(system);
#ifdef EMUSUNI_OPENBOR
  none = none || system == SystemId::OPENBOR;
#endif
#ifdef EMUSUNI_MUGEN
  none = none || system == SystemId::MUGEN;
#endif
  return !none;
}

namespace
{
std::string LowerName(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// RetroArch's own form: "cheats = N" (N a number, quoted or not), and a key "cheat<i>_..." - read as Parse reads them
// (the key from the line's start, the spaces before "=" left out), so a file that passes is one Parse takes.
bool IsRetroArchCht(const std::string& text)
{
  bool count = false, entry = false;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line) && !(count && entry);)
  {
    const size_t equals = line.find('=');
    if (equals == std::string::npos || line[0] == '#')
      continue;
    std::string key = line.substr(0, equals);
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    if (key == "cheats")
    {
      const std::string value = Unquote(line.substr(equals + 1));
      count = !value.empty() && std::all_of(value.begin(), value.end(),
                                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }
    else if (key.size() > 6 && key.compare(0, 5, "cheat") == 0 && std::isdigit(static_cast<unsigned char>(key[5])))
    {
      size_t at = 5;
      while (at < key.size() && std::isdigit(static_cast<unsigned char>(key[at])))
        at++;
      entry = entry || (at < key.size() && key[at] == '_');
    }
  }
  return count && entry;
}

bool IsHex8(const std::string& token)
{
  return token.size() == 8 &&
         std::all_of(token.begin(), token.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

// (01.89) A code the 3DS's core takes (scripts/build-3ds-core.sh 01.26): Gateway / Action Replay lines of 8 + 8
// hexadecimal digits, apart by '+', ';', a line's end or a space.
bool IsGatewayCode(const std::string& code)
{
  size_t tokens = 0;
  std::string token;
  for (size_t i = 0; i <= code.size(); i++)
  {
    const char c = i < code.size() ? code[i] : ' ';
    if (c == '+' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      if (!token.empty() && !IsHex8(token))
        return false;
      tokens += token.empty() ? 0 : 1;
      token.clear();
    }
    else
    {
      token += c;
    }
  }
  return tokens > 0 && tokens % 2 == 0;
}

// (01.89) An Azahar (Citra) cheats file: "[Name]", then its lines of 8 + 8 hexadecimal digits ("*citra_enabled" and
// "{...}" lines: Azahar's own; left out of the list). True: a name with a code line.
bool IsAzaharTxt(const std::string& text)
{
  int cheats = 0;
  bool named = false, coded = false;
  auto finish = [&]() {
    cheats += named && coded ? 1 : 0;
    coded = false;
  };
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);)
  {
    line = Unquote(line);
    if (line.empty())
      continue;
    if (line.front() == '[' && line.back() == ']')
    {
      finish();
      named = true;
      continue;
    }
    if (named && line.size() == 17 && line[8] == ' ' && IsHex8(line.substr(0, 8)) && IsHex8(line.substr(9)))
      coded = true;
  }
  finish();
  return cheats > 0;
}

// (01.90) Entries as a RetroArch .cht's text, as List::Save writes them.
std::string ChtText(const std::vector<Entry>& entries)
{
  std::ostringstream file;
  file << "cheats = " << entries.size() << "\n\n";
  for (size_t i = 0; i < entries.size(); i++)
  {
    file << "cheat" << i << "_desc = \"" << entries[i].name << "\"\n";
    file << "cheat" << i << "_code = \"" << entries[i].code << "\"\n";
    file << "cheat" << i << "_enable = " << (entries[i].enabled ? "true" : "false") << "\n\n";
  }
  return file.str();
}
}  // namespace

const char* ImportProblem(Import result)
{
  switch (result)
  {
  case Import::NotCht:
    return "Not a RetroArch .cht file: nothing was changed";
  case Import::NotAzahar:
    return "Not an Azahar cheats file ([Name], then lines of 8 + 8 hexadecimal digits): nothing was changed";
  case Import::No3dsCodes:
    return "No 3DS codes (Gateway / Action Replay) in this file: nothing was changed";
  case Import::TooBig:
    return "Too big for a .cht file: nothing was changed";
  case Import::Unreadable:
    return "The file can't be read: nothing was changed";
  case Import::NotWritten:
    return "The cheats couldn't be written: nothing was changed";
  default:
    return "";
  }
}

Kind KindFor(SystemId system, const std::string& file)
{
  if (system == SystemId::DC)
    return Kind::ChtDreamcast;
  if (system != SystemId::THREEDS)
    return Kind::Cht;
  return file.size() > 4 && LowerName(file.substr(file.size() - 4)) == ".txt" ? Kind::Azahar : Kind::Cht3ds;
}

Folder ReadFolder(const std::string& dir, bool txt)
{
  Folder folder;
  constexpr size_t MOST = 5000;
  std::error_code ec;
  // (Not a range-for: its ++ throws when the drive goes away meanwhile; increment(ec) just stops.)
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
  {
    folder.readable = true;
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.' || name == "System Volume Information" || name == "$RECYCLE.BIN")
      continue;
    if (folder.folders.size() + folder.files.size() >= MOST)
    {
      folder.cut = true;
      break;
    }
    std::error_code type_error;
    if (it->is_directory(type_error))
      folder.folders.push_back(name);
    else if (name.size() > 4 &&
             (LowerName(name.substr(name.size() - 4)) == ".cht" || (txt && LowerName(name.substr(name.size() - 4)) == ".txt")) &&
             it->is_regular_file(type_error))
      folder.files.emplace_back(name, it->file_size(type_error));
  }
  if (!folder.readable && ec)
    folder.error = ec.message();
  const auto by_name = [](const std::string& a, const std::string& b) { return LowerName(a) < LowerName(b); };
  std::sort(folder.folders.begin(), folder.folders.end(), by_name);
  std::sort(folder.files.begin(), folder.files.end(), [&](const auto& a, const auto& b) { return by_name(a.first, b.first); });
  return folder;
}

Import ImportFile(const std::string& from, const std::string& to, Kind kind, bool replace, int* codes)
{
  *codes = 0;
  // (Why not, in the log: the menus say it in a toast.)
  const auto failed = [&from](Import why, const char* what) {
    LogF("cheats: %s not imported (%s): nothing changed", from.c_str(), what);
    return why;
  };
  std::error_code ec;
  const uintmax_t bytes = std::filesystem::file_size(from, ec);
  if (ec)
    return failed(Import::Unreadable, "it can't be read");
  if (bytes > (4u << 20))  // (libretro's biggest are a few hundred KB)
    return failed(Import::TooBig, "over 4 MB");
  std::ifstream in(from, std::ios::binary);
  std::string text(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
  if (!in.good() && !in.eof())
    return failed(Import::Unreadable, "it can't be read");
  // (A UTF-8 mark at its start - a file saved by Notepad - left out of the copy: the list's reading wouldn't find its
  // "cheats" line.)
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);
  if (kind == Kind::Azahar)
  {
    // ((01.90, the owner: "si quedan invisibles ... importa y no pasa nada") Its cheats made the game's list, as Find
    // Cheats Online makes CTRPF's file (Parse: "[Name]" and its code lines), all off: turned on one by one in Cheats;
    // its Gateway / Action Replay ones only. Azahar's own file isn't written: none is on twice.
    if (!IsAzaharTxt(text))
      return failed(Import::NotAzahar, "not Azahar's cheats file");
    std::vector<Entry> entries = Parse(text);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& e) { return !IsGatewayCode(e.code); }),
                  entries.end());
    for (Entry& entry : entries)
      entry.enabled = false;
    if (entries.empty())
      return failed(Import::No3dsCodes, "no Gateway / Action Replay code");
    *codes = static_cast<int>(entries.size());
    text = ChtText(entries);
  }
  else if (!IsRetroArchCht(text))
  {
    return failed(Import::NotCht, "not a RetroArch .cht");
  }
  else
  {
    // (The codes as the list reads them: the Dreamcast's addresses made codes; the 3DS's Gateway / Action Replay only.)
    const std::vector<Entry> entries = Parse(text, kind == Kind::ChtDreamcast);
    *codes = kind != Kind::Cht3ds ? static_cast<int>(entries.size()) :
                                    static_cast<int>(std::count_if(entries.begin(), entries.end(),
                                                                   [](const Entry& e) { return IsGatewayCode(e.code); }));
    if (kind == Kind::Cht3ds && *codes == 0)
      return failed(Import::No3dsCodes, "no Gateway / Action Replay code");
  }

  const bool there = std::filesystem::file_size(to, ec) > 0 && !ec;
  if (there && !replace)
    return Import::Exists;
  // Written beside it first, then put in its place: the one before (when there is one) becomes <its file>.bak.
  std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);
  const std::string temp = to + ".new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out.good())
    {
      out.close();
      std::filesystem::remove(temp, ec);
      return failed(Import::NotWritten, "it couldn't be written");
    }
  }
  if (there)
  {
    std::filesystem::rename(to, to + ".bak", ec);
    if (ec)
    {
      std::filesystem::remove(temp, ec);
      return failed(Import::NotWritten, "the one before couldn't be kept as .bak");
    }
  }
  std::filesystem::rename(temp, to, ec);
  if (ec)
  {
    if (there)
      std::filesystem::rename(to + ".bak", to, ec);  // (the one before back in its place)
    std::filesystem::remove(temp, ec);
    return failed(Import::NotWritten, "it couldn't be put in its place");
  }
  LogF("cheats: %s imported to %s (%d codes%s)", from.c_str(), to.c_str(), *codes,
       there ? "; the one before kept as .bak" : "");
  return Import::Done;
}
}  // namespace Cheats
