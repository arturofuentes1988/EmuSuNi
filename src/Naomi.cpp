// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Naomi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "Arcade.h"
#include "Log.h"

#ifdef EMUSUNI_DC
// scripts/build-dc-core.sh: every game of Flycast's list (hw/naomi/naomi_roms.cpp), one call of `add` each; the
// board is Flycast's DC_PLATFORM_* (core/build.h), the flags 1 vertical, 2 analog, 4 light gun, 8 linked boards.
extern "C" unsigned flycast_emusuni_arcade_list(void (*add)(void* user, const char* name, const char* parent,
                                                            const char* title, const char* bios_zip, const char* gdrom,
                                                            int platform, unsigned size, unsigned flags),
                                                void* user);
// (00.65) What would make the set's start fail, looked for as the core looks, without loading anything or a C++
// exception: 1 when it would start, else 0 and Flycast's message. bios_dir: Flycast's system/dc.
extern "C" int flycast_emusuni_check_set(const char* path, const char* bios_dir, char* error, unsigned error_size);
#endif

namespace Naomi
{
namespace
{
// Flycast's core/build.h (DC_PLATFORM_NAOMI is 2: every other set).
constexpr int PLATFORM_NAOMI2 = 3, PLATFORM_ATOMISWAVE = 4, PLATFORM_SYSTEMSP = 5;
constexpr unsigned FLAG_VERTICAL = 1, FLAG_ANALOG = 2, FLAG_GUN = 4, FLAG_LINKED = 8;

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::mutex s_mutex;
std::vector<Set> s_sets;
std::map<std::string, size_t> s_by_name;  // the set of a name
bool s_read = false;

#ifdef EMUSUNI_DC
void Add(void* user, const char* name, const char* parent, const char* title, const char* bios_zip, const char* gdrom,
         int platform, unsigned size, unsigned flags)
{
  auto* sets = static_cast<std::vector<Set>*>(user);
  Set set;
  set.name = name ? name : "";
  set.parent = parent ? parent : "";
  set.title = title && *title ? title : set.name;
  set.bios = bios_zip ? bios_zip : "naomi";
  set.gdrom = gdrom ? gdrom : "";
  set.board = platform == PLATFORM_ATOMISWAVE ? Board::Atomiswave :
              platform == PLATFORM_NAOMI2     ? Board::Naomi2 :
              platform == PLATFORM_SYSTEMSP   ? Board::SystemSp :
                                                Board::Naomi;
  set.size = size;
  set.vertical = (flags & FLAG_VERTICAL) != 0;
  set.analog = (flags & FLAG_ANALOG) != 0;
  set.gun = (flags & FLAG_GUN) != 0;
  set.linked = (flags & FLAG_LINKED) != 0;
  // (A System SP game's "GD-ROM" name is its media's, another kind of disc: not looked for.)
  if (set.board == Board::SystemSp)
    set.gdrom.clear();
  sets->push_back(std::move(set));
}
#endif

void ReadLocked()
{
  if (s_read)
    return;
  s_read = true;
#ifdef EMUSUNI_DC
  const unsigned count = flycast_emusuni_arcade_list(Add, &s_sets);
  size_t naomi = 0, naomi2 = 0, atomiswave = 0, gdrom = 0;
  for (size_t i = 0; i < s_sets.size(); i++)
  {
    s_by_name.emplace(Lower(s_sets[i].name), i);
    naomi += s_sets[i].board == Board::Naomi ? 1 : 0;
    naomi2 += s_sets[i].board == Board::Naomi2 ? 1 : 0;
    atomiswave += s_sets[i].board == Board::Atomiswave ? 1 : 0;
    gdrom += s_sets[i].gdrom.empty() ? 0 : 1;
  }
  LogF("naomi: %u sets in Flycast's list: NAOMI %zu, NAOMI 2 %zu, Atomiswave %zu (GD-ROM %zu), System SP %zu", count,
       naomi, naomi2, atomiswave, gdrom, s_sets.size() - naomi - naomi2 - atomiswave);
#endif
}

// The title's head: its name before its first group of parentheses.
std::string Head(const std::string& title)
{
  const size_t open = title.find(" (");
  return open == std::string::npos ? title : title.substr(0, open);
}

bool AnyFile(const std::string& stem, std::initializer_list<const char*> extensions)
{
  for (const char* extension : extensions)
  {
    if (FileExists(stem + extension))
      return true;
  }
  return false;
}

// A name's words in small letters, letters and digits only, for comparing names ("Virtua Tennis 2 / Power Smash
// 2" and "Virtua Tennis 2 ~ Power Smash 2" are both "virtua tennis 2 power smash 2").
std::string Key(const std::string& name)
{
  std::string key;
  bool gap = false;
  for (const unsigned char c : name)
  {
    if (!std::isalnum(c))
    {
      gap = true;
      continue;
    }
    if (gap && !key.empty())
      key += ' ';
    key += static_cast<char>(std::tolower(c));
    gap = false;
  }
  return key;
}

// A cartridge header's title without its region and its filler: "VIRTUA TENNIS 2 IN USA --" -> "VIRTUA TENNIS 2",
// "MARVEL VS CAPCOM 2 JAPAN" -> "MARVEL VS CAPCOM 2", " POWER SMASH 2 -----------" -> "POWER SMASH 2".
std::string HeaderName(const std::string& title)
{
  static const std::set<std::string> TAIL = {"japan", "jpn", "usa", "us", "export", "exp", "korea", "australia",
                                             "asia", "europe", "world", "in", "for", "ver", "version"};
  std::vector<std::string> words;
  std::stringstream in(title);
  for (std::string word; in >> word;)
  {
    if (std::any_of(word.begin(), word.end(), [](unsigned char c) { return std::isalnum(c); }))
      words.push_back(word);  // (not "-----": filler)
  }
  while (!words.empty() && TAIL.count(Lower(words.back())) > 0)
    words.pop_back();
  std::string name;
  for (const std::string& word : words)
    name += (name.empty() ? "" : " ") + word;
  return name;
}

// "MARVEL VS CAPCOM 2" -> "Marvel vs Capcom 2".
std::string TitleCase(const std::string& upper)
{
  static const std::set<std::string> SMALL = {"vs", "of", "the", "and", "in", "on", "to", "de", "a"};
  std::string out;
  std::stringstream in(upper);
  for (std::string word; in >> word;)
  {
    word = Lower(word);
    if (out.empty() || SMALL.count(word) == 0)
      word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
    out += (out.empty() ? "" : " ") + word;
  }
  return out;
}
}  // namespace

const std::vector<Set>& Sets()
{
  std::lock_guard lock(s_mutex);
  ReadLocked();
  return s_sets;
}

const Set* Find(const std::string& name)
{
  std::lock_guard lock(s_mutex);
  ReadLocked();
  const auto it = s_by_name.find(Lower(name));
  return it == s_by_name.end() ? nullptr : &s_sets[it->second];
}

const Set* Parent(const Set& set)
{
  return set.parent.empty() ? nullptr : Find(set.parent);
}

bool Plays(SystemId system, const Set& set)
{
  if (system == SystemId::NAOMI)
    return set.board == Board::Naomi || set.board == Board::Naomi2;
  if (system == SystemId::ATOMISWAVE)
    return set.board == Board::Atomiswave;
  return false;
}

std::string Title(const Set& set)
{
  const Set* parent = Parent(set);
  if (!parent || set.title == parent->title)
    return set.title;
  // The parent's name with what the clone's adds: its own parentheses when it starts like the parent
  // ("Marvel vs. Capcom 2 New Age of Heroes (USA)"), else its whole name.
  const std::string head = Head(parent->title);
  if (set.title.compare(0, head.size(), head) == 0)
    return set.title;
  return head + " (" + set.title + ")";
}

std::string Region(const Set& set)
{
  // The words of the name's parentheses ("(Export, Korea)", "(Japan, Rev A)"): the first one that is a region.
  static const std::pair<const char*, const char*> REGIONS[] = {
      {"world", "World"},   {"usa", "USA"},       {"us", "USA"},       {"japan", "Japan"},     {"jpn", "Japan"},
      {"export", "Export"}, {"europe", "Europe"}, {"euro", "Europe"},  {"asia", "Asia"},       {"korea", "Korea"},
      {"taiwan", "Taiwan"}, {"china", "China"},   {"hong", "Hong Kong"}, {"australia", "Australia"}, {"italy", "Italy"},
  };
  const std::string& title = set.title;
  for (size_t open = title.find('('); open != std::string::npos; open = title.find('(', open + 1))
  {
    const size_t close = title.find(')', open);
    if (close == std::string::npos)
      break;
    const std::string inside = Lower(title.substr(open + 1, close - open - 1));
    size_t at = 0;
    while (at <= inside.size())
    {
      const size_t end = inside.find_first_of(" ,/", at);
      const std::string word = inside.substr(at, end == std::string::npos ? std::string::npos : end - at);
      for (const auto& [key, region] : REGIONS)
      {
        if (word == key)
          return region;
      }
      if (end == std::string::npos)
        break;
      at = end + 1;
    }
  }
  return {};
}

std::string BoardName(const Set& set)
{
  const bool gd = !set.gdrom.empty();
  switch (set.board)
  {
  case Board::Naomi2:
    return gd ? "NAOMI 2 GD-ROM" : "NAOMI 2";
  case Board::Atomiswave:
    return "Atomiswave";
  case Board::SystemSp:
    return "System SP";
  default:
    return gd ? "NAOMI GD-ROM" : "NAOMI";
  }
}

std::string BiosZip(const Set& set)
{
  return set.bios + ".zip";
}

std::string GdromFile(const Set& set)
{
  return set.gdrom.empty() ? std::string() : set.name + "/" + set.gdrom + ".chd";
}

std::vector<std::string> Missing(const Set& set, const std::string& folder, const std::string& system_dir)
{
  std::vector<std::string> missing;
  // The BIOS: <system>/dc/<zip>, then beside the game (Flycast's findNaomiBios, a .zip, then a .7z).
  if (!AnyFile(system_dir + "/dc/" + set.bios, {".zip", ".7z"}) && !AnyFile(folder + "/" + set.bios, {".zip", ".7z"}))
    missing.push_back(BiosZip(set));
  // A clone's parent: beside it (OpenArchive: .7z, .7Z, .zip, .ZIP).
  if (!set.parent.empty() && !AnyFile(folder + "/" + set.parent, {".zip", ".7z", ".ZIP", ".7Z"}))
    missing.push_back(set.parent + ".zip");
  // A GD-ROM game's disc: <folder>/<set>/<gdrom>.chd, else its parent's folder (GDCartridge::Init).
  if (!set.gdrom.empty() && !FileExists(folder + "/" + GdromFile(set)) &&
      (set.parent.empty() || !FileExists(folder + "/" + set.parent + "/" + set.gdrom + ".chd")))
    missing.push_back(GdromFile(set));
  return missing;
}

std::string LoadError(const std::string& core_message, const Set* set, const std::vector<std::string>& missing,
                      const std::string& system_dir)
{
  const auto has = [&](const char* part) { return core_message.find(part) != std::string::npos; };
  const auto absent = [&](const std::string& file) {
    return std::find(missing.begin(), missing.end(), file) != missing.end();
  };
  const std::string bios = set ? BiosZip(*set) : std::string("naomi.zip");
  if (has("Unknown game"))
    return "Flycast doesn't know this set\n(a zip is named after its set,\nin small letters: mvsc2.zip)";
  if (has("cannot load BIOS"))
  {
    // Not there, or there without the files the game boots from (another BIOS's zip, another version).
    if (absent(bios))
      return "This game needs its board's BIOS:\n" + bios + "\nCopy it next to the game, or to:\n" + system_dir + "/dc";
    return "The BIOS zip has none of the files\nthis game boots from:\n" + bios + "\n(MAME's set of that name works)";
  }
  if (has("Naomi GDROM") || has(".chd"))
  {
    const std::string disc = set ? GdromFile(*set) : std::string();
    return "This GD-ROM game needs its disc image\n(in a folder named after the game,\nbeside its zip):\n" +
           (disc.empty() ? core_message : disc);
  }
  if (has("Memory allocation failed") || has("Out of memory"))
    return "Not enough memory for this game";
  if (has("Cannot find ") || has("Invalid ROM"))
  {
    // A file of the set isn't in its zip (nor in its parent's): another version's set, or a clone without its parent.
    std::string error = "This set's files don't match\nFlycast's list";
    const size_t at = core_message.find("Cannot find ");
    if (at != std::string::npos)
      error += "\nMissing file:\n" + core_message.substr(at + 12);
    if (set && !set->parent.empty() && absent(set->parent + ".zip"))
      error += "\nZips not found:\n" + set->parent + ".zip\nCopy it next to the game's zip";
    return error;
  }
  if (has("Cannot open"))
    return "The game's zip can't be opened";
  // A cartridge image (loadDecryptedRom): not there any more, unreadable, or empty.
  if (has("can't open") || has("Failed to load BIN/DAT") || has("Invalid empty ROM"))
    return "The cartridge image can't be read";
  return core_message.empty() ? std::string("The emulator couldn't load this game") :
                                "The emulator couldn't load this game\n" + core_message;
}

std::string CheckSet(const std::string& path, const std::string& system_dir)
{
#ifdef EMUSUNI_DC
  char error[256] = {};
  const auto start = std::chrono::steady_clock::now();
  const bool starts = flycast_emusuni_check_set(path.c_str(), (system_dir + "/dc").c_str(), error, sizeof(error)) != 0;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  LogF("naomi: Flycast's check of %s: %s (%.1f ms)", path.c_str(), starts ? "it would start" : error, ms);
  if (starts)
    return std::string();
  return error[0] != '\0' ? std::string(error) : std::string("Unknown game");
#else
  (void)path;
  (void)system_dir;
  return std::string();
#endif
}

bool IsImageName(const std::string& name)
{
  const std::string lower = Lower(name);
  return lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".dat") == 0 ||
                              lower.compare(lower.size() - 4, 4, ".bin") == 0);
}

bool ReadImage(const std::string& path, Image* image)
{
  // RomBootID: the board's name (16 bytes: "NAOMI" and spaces; a NAOMI 2's "Naomi2", an Atomiswave's
  // "SYSTEM_X_APP", a System SP's "SystemSP"), the maker's (32), eight regions' titles (32 each: Japan, USA,
  // export...), ...; the monitor's way up at 0x42B (2: vertical). Flycast reads it at the image's start.
  char header[0x430] = {};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(header, sizeof(header)))
    return false;
  if (std::memcmp(header, "NAOMI", 5) != 0 || (header[5] != ' ' && header[5] != '\0'))
    return false;
  auto title = [&header](int region) {
    std::string text(header + 0x30 + region * 32, 32);
    text = text.substr(0, text.find('\0'));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0'))
      text.pop_back();
    return text;
  };
  image->japan = title(0);
  image->usa = title(1);
  image->export_title = title(2);
  image->vertical = static_cast<unsigned char>(header[0x42B]) == 2;
  return true;
}

const Set* MatchImage(const std::string& title, const Image& image)
{
  // The file's title first, then the header's (export, USA, Japan); each word for word, else as the set's name's
  // start (two words at least: one alone is too often the start of another game's name). Among the sets that match
  // alike, a cartridge's before a GD-ROM's, a parent before its clones.
  const std::string names[] = {Key(title), Key(HeaderName(image.export_title)), Key(HeaderName(image.usa)),
                               Key(HeaderName(image.japan))};
  const Set* best = nullptr;
  int best_rank = INT_MAX;
  for (const Set& set : Sets())
  {
    if (set.board != Board::Naomi)
      continue;
    const std::string head = Key(Head(set.title));
    for (int i = 0; i < 4; i++)
    {
      const std::string& name = names[i];
      if (name.empty())
        continue;
      const bool same = head == name;
      const bool start = !same && name.find(' ') != std::string::npos && head.size() > name.size() &&
                         head.compare(0, name.size() + 1, name + " ") == 0;
      if (!same && !start)
        continue;
      const int rank = i * 8 + (same ? 0 : 4) + (set.gdrom.empty() ? 0 : 2) + (set.parent.empty() ? 0 : 1);
      if (rank < best_rank)
      {
        best_rank = rank;
        best = &set;
      }
    }
  }
  return best;
}

std::string ImageTitle(const std::string& file_title, const Image& image, const Set* set)
{
  if (set)
    return Head(set->title);
  // A file's own name, unless it says little ("mvc2", "game"): then the header's.
  const std::string header = HeaderName(!image.export_title.empty() ? image.export_title :
                                        !image.usa.empty()          ? image.usa :
                                                                      image.japan);
  const bool says_little = file_title.find(' ') == std::string::npos && file_title.size() < 8;
  if (header.empty() || (!says_little && !file_title.empty()))
    return file_title;
  return TitleCase(header);
}

std::vector<std::string> ImageMissing(const std::string& folder, const std::string& system_dir)
{
  // (loadDecryptedRom: always naomi.zip; findNaomiBios: system/dc, then the image's folder; a .zip, then a .7z.)
  if (AnyFile(system_dir + "/dc/naomi", {".zip", ".7z"}) || AnyFile(folder + "/naomi", {".zip", ".7z"}))
    return {};
  return {"naomi.zip"};
}
}  // namespace Naomi
