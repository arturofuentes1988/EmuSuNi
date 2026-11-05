// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Arcade.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

#include "Log.h"

#ifdef __PS4__
#include <minizip/unzip.h>
#endif

#ifdef EMUSUNI_ARCADE
// scripts/build-arcade-core.sh: every driver of the core, one call of `add` each.
extern "C" unsigned fbneo_emusuni_list(void (*add)(void* user, const char* name, const char* title,
                                                   const char* parent, const char* bios, const char* year,
                                                   const char* maker, const char* system, unsigned flags,
                                                   unsigned hardware),
                                       void* user);
#endif

namespace Arcade
{
namespace
{
// FinalBurn Neo's burn.h: the driver flags, and the hardware codes of its console drivers (the ones
// its "arcade only" DAT leaves out, dat.cpp), which aren't zips of arcade boards.
constexpr unsigned BDF_ORIENTATION_VERTICAL = 1u << 2, BDF_BOARDROM = 1u << 3;
constexpr unsigned HARDWARE_PUBLIC_MASK = 0x7FFF0000u;
constexpr unsigned CONSOLE_HARDWARE[] = {
    0x0c000000u,  // Mega Drive
    0x17010000u,  // PC Engine
    0x17020000u,  // TurboGrafx-16
    0x17030000u,  // SuperGrafx
    0x17040000u,  // PC Engine CD
    0x19000000u,  // SG-1000
    0x1A000000u,  // ColecoVision
    0x18000000u,  // Master System
    0x12000000u,  // Game Gear
    0x1C000000u,  // MSX
    0x1D000000u,  // ZX Spectrum
    0x1E000000u,  // NES
    0x1F000000u,  // Famicom Disk System
    0x22000000u,  // Super Nintendo
    0x25000000u,  // Game Boy Advance
    0x20000000u,  // Neo Geo Pocket
    0x21000000u,  // Channel F
    0x24000000u,  // Astrocade (home)
    0x05030000u,  // Neo Geo CD (a disc image, not a zip)
};

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// (00.90) The names of the files in a zip, in lower case and without their folders, added to `names` (none from a .7z,
// a zip that isn't there or can't be read; none on the PC, whose tests link no zip reader).
void AddZipNames(const std::string& path, std::set<std::string>* names)
{
#ifdef __PS4__
  unzFile zip = unzOpen(path.c_str());
  if (!zip)
    return;
  for (int at = unzGoToFirstFile(zip); at == UNZ_OK; at = unzGoToNextFile(zip))
  {
    unz_file_info info;
    char name[256];
    if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
      continue;
    const std::string file = name;
    names->insert(Lower(file.substr(file.find_last_of('/') + 1)));
  }
  unzClose(zip);
#else
  (void)path;
  (void)names;
#endif
}

std::mutex s_mutex;
std::vector<Driver> s_drivers;
std::map<std::string, size_t> s_by_name;  // the driver of a set name
bool s_read = false;

#ifdef EMUSUNI_ARCADE
void Add(void* user, const char* name, const char* title, const char* parent, const char* bios, const char* year,
         const char* maker, const char* system, unsigned flags, unsigned hardware)
{
  auto* drivers = static_cast<std::vector<Driver>*>(user);
  Driver driver;
  driver.name = name ? name : "";
  driver.title = title ? title : driver.name;
  driver.parent = parent ? parent : "";
  driver.bios = bios ? bios : "";
  driver.year = year ? year : "";
  driver.maker = maker ? maker : "";
  driver.board = system ? system : "";
  driver.flags = flags;
  driver.hardware = hardware;
  driver.arcade = (flags & BDF_BOARDROM) == 0 && driver.name != "neogeo" && !ConsoleHardware(hardware);
  driver.vertical = (flags & BDF_ORIENTATION_VERTICAL) != 0;
  drivers->push_back(std::move(driver));
}
#endif

void ReadLocked()
{
  if (s_read)
    return;
  s_read = true;
#ifdef EMUSUNI_ARCADE
  const unsigned count = fbneo_emusuni_list(Add, &s_drivers);
  size_t arcade = 0, bios = 0;
  for (size_t i = 0; i < s_drivers.size(); i++)
  {
    s_by_name[s_drivers[i].name] = i;
    arcade += s_drivers[i].arcade ? 1 : 0;
    bios += (s_drivers[i].flags & BDF_BOARDROM) ? 1 : 0;
  }
  LogF("arcade: %u drivers, %zu arcade games (%zu BIOS sets)", count, arcade, bios);
#endif
}

// The title's head: its name before its first group of parentheses ("Ghouls'n Ghosts (World)" ->
// "Ghouls'n Ghosts").
std::string Head(const std::string& title)
{
  const size_t open = title.find(" (");
  return open == std::string::npos ? title : title.substr(0, open);
}
}  // namespace

bool ConsoleHardware(unsigned hardware)
{
  const unsigned kind = hardware & HARDWARE_PUBLIC_MASK;
  return std::find(std::begin(CONSOLE_HARDWARE), std::end(CONSOLE_HARDWARE), kind) != std::end(CONSOLE_HARDWARE);
}

const std::vector<Driver>& Drivers()
{
  std::lock_guard lock(s_mutex);
  ReadLocked();
  return s_drivers;
}

const Driver* Find(const std::string& set)
{
  std::lock_guard lock(s_mutex);
  ReadLocked();
  const auto it = s_by_name.find(Lower(set));
  return it == s_by_name.end() ? nullptr : &s_drivers[it->second];
}

const Driver* Parent(const Driver& driver)
{
  return driver.parent.empty() ? nullptr : Find(driver.parent);
}

std::string Title(const Driver& driver)
{
  const Driver* parent = Parent(driver);
  if (!parent || driver.title == parent->title)
    return driver.title;
  // The parent's name with what the clone's adds: its own parentheses when it starts like the parent
  // ("Street Fighter II - The World Warrior (Japan 911210)"), else its whole name.
  const std::string head = Head(parent->title);
  if (driver.title.compare(0, head.size(), head) == 0)
    return driver.title;
  return head + " (" + driver.title + ")";
}

std::string Region(const Driver& driver)
{
  // The words of the name's parentheses ("(World 910522)", "(USA, Euro)"): the first one that is a region.
  static const std::pair<const char*, const char*> REGIONS[] = {
      {"world", "World"},   {"usa", "USA"},      {"us", "USA"},         {"japan", "Japan"},     {"europe", "Europe"},
      {"euro", "Europe"},   {"asia", "Asia"},    {"korea", "Korea"},    {"hispanic", "Hispanic"}, {"brazil", "Brazil"},
      {"italy", "Italy"},   {"spain", "Spain"},  {"germany", "Germany"}, {"france", "France"},   {"uk", "UK"},
      {"china", "China"},   {"taiwan", "Taiwan"}, {"hong kong", "Hong Kong"}, {"australia", "Australia"},
  };
  const std::string& title = driver.title;
  for (size_t open = title.find('('); open != std::string::npos; open = title.find('(', open + 1))
  {
    const size_t close = title.find(')', open);
    if (close == std::string::npos)
      break;
    const std::string inside = Lower(title.substr(open + 1, close - open - 1));
    size_t at = 0;
    while (at < inside.size())
    {
      const size_t end = inside.find_first_of(" ,/", at);
      const std::string word = inside.substr(at, end == std::string::npos ? std::string::npos : end - at);
      for (const auto& [key, region] : REGIONS)
      {
        if (word == key || (std::string(key).find(' ') != std::string::npos && inside.compare(at, std::char_traits<char>::length(key), key) == 0))
          return region;
      }
      if (end == std::string::npos)
        break;
      at = end + 1;
    }
  }
  return {};
}

std::string CoverName(const std::string& title)
{
  std::string name = title;
  for (char& c : name)
  {
    if (c == '&' || c == '*' || c == '/' || c == ':' || c == '`' || c == '<' || c == '>' || c == '?' || c == '\\' ||
        c == '|' || c == '"')
      c = '_';
  }
  return name;
}

std::vector<std::string> Missing(const Driver& driver, const std::string& folder, const std::string& system_dir)
{
  return Missing(driver, folder, system_dir, FileExists);
}

std::vector<std::string> Missing(const Driver& driver, const std::string& folder, const std::string& system_dir,
                                 const std::function<bool(const std::string&)>& exists)
{
  std::vector<std::string> missing;
  for (const std::string& set : {driver.bios, driver.parent})
  {
    if (set.empty())
      continue;
    bool found = false;
    for (const std::string& dir : {folder, system_dir + "/fbneo", system_dir})
    {
      for (const char* extension : {".zip", ".7z"})
        found = found || exists(dir + "/" + set + extension);
    }
    if (!found)
      missing.push_back(set + ".zip");
  }
  return missing;
}

std::string LoadError(const std::string& core_text, const std::vector<std::string>& not_found,
                      const std::string& system_dir)
{
  return LoadError(core_text, not_found, system_dir, std::string());
}

std::string LoadError(const std::string& core_text, const std::vector<std::string>& not_found,
                      const std::string& system_dir, const std::string& game_path)
{
  const auto has = [&](const char* part) { return core_text.find(part) != std::string::npos; };
  std::string error;
  bool known = false;  // the romset is known: what it lacks is files (or the zips it goes with)
  if (has("Romset is unknown"))
  {
    error = "FinalBurn Neo doesn't know this romset\n(a zip is named after its driver: mslug.zip)";
  }
  else if (has("missing files for THIS VERSION"))
  {
    known = true;
    error = "This romset's files don't match\nFinalBurn Neo's DAT v1.0.0.03";
  }
  else if (has("not working"))
  {
    error = "FinalBurn Neo knows this romset\nbut it doesn't work yet";
  }
  else if (has("Bioses aren't"))
  {
    error = "This zip is a BIOS set, not a game";
  }
  else
  {
    known = true;
    error = "The emulator couldn't load this romset";
  }
  if (!known)
    return error;
  // The core's own lines that name things - the romsets to verify, the ROMs it wants and hasn't found -
  // without its general advice. Each is a short heading (the language file translates it) and, under it,
  // its data (a set's name, a ROM's: never translated): the page cuts a line longer than its width at the
  // end, and a cut heading would lose its translation. At most MAX_ROMS ROMs are listed (it names up to
  // 18): the page is short.
  constexpr size_t MAX_ROMS = 6;
  const std::string with_name = "ROM with name ", with_crc = " and CRC 0x", is_missing = " is missing";
  std::string verify;
  std::vector<std::string> roms, rom_names;
  size_t more = 0;
  bool none = false;
  for (size_t start = 0; start < core_text.size();)
  {
    const size_t end = std::min(core_text.find('\n', start), core_text.size());
    const std::string line = core_text.substr(start, end - start);
    start = end + 1;
    if (line.rfind("Verify the following romsets", 0) == 0)
    {
      const size_t colon = line.find(':');
      verify = colon == std::string::npos ? std::string() : line.substr(colon + 1);
      while (!verify.empty() && verify.front() == ' ')
        verify.erase(verify.begin());
      while (!verify.empty() && verify.back() == ' ')
        verify.pop_back();
    }
    else if (line.rfind(with_name, 0) == 0)
    {
      const size_t crc = line.find(with_crc), tail = line.find(is_missing);
      if (crc != std::string::npos && tail != std::string::npos && tail > crc)
      {
        rom_names.push_back(line.substr(with_name.size(), crc - with_name.size()));
        roms.push_back(rom_names.back() + " (CRC " + line.substr(crc + with_crc.size(), tail - crc - with_crc.size()) +
                       ")");
      }
    }
    else if (line.rfind("And ", 0) == 0 && line.size() > 12 && line.compare(line.size() - 8, 8, " more...") == 0)
    {
      more += static_cast<size_t>(std::atoi(line.c_str() + 4));
    }
    else if (line.find("None of those archives") != std::string::npos)
    {
      none = true;
    }
  }
  // (00.90) A ROM it wants whose name is in the game's zip, or in its parent's or BIOS set's where the core looks for
  // them, is there with other contents (the core takes ROMs by their CRCs): a romset of another version - MAME's, or
  // another FinalBurn Neo's -, the usual case of "the arcade games don't load" (players, 30-10-2026).
  if (has("missing files for THIS VERSION") && !rom_names.empty() && !game_path.empty())
  {
    std::set<std::string> names;
    AddZipNames(game_path, &names);
    const size_t slash = game_path.find_last_of('/');
    const std::string folder = slash == std::string::npos ? std::string(".") : game_path.substr(0, slash);
    std::string set = game_path.substr(slash == std::string::npos ? 0 : slash + 1);
    set = set.substr(0, set.rfind('.'));
    if (const Driver* driver = Find(set))
    {
      for (const std::string& zip_set : {driver->bios, driver->parent})
      {
        if (zip_set.empty())
          continue;
        for (const std::string& dir : {folder, system_dir + "/fbneo", system_dir})
          AddZipNames(dir + "/" + zip_set + ".zip", &names);
      }
    }
    size_t other = 0;
    for (const std::string& rom : rom_names)
      other += names.count(Lower(rom));
    if (other > 0)
    {
      LogF("arcade: %zu of the %zu ROMs it lacks are in its zips with other contents: another version of the romset",
           other, rom_names.size());
      error = "This romset is another version:\nits ROMs are there with other data.\nEmuSuNi needs the romsets made for\n"
              "FinalBurn Neo 1.0.0.03 (not MAME's)";
    }
  }
  // First what can be done at once: the zips that go with the game and aren't beside it (a Neo Geo game
  // without neogeo.zip is the usual case), and where the core looks for them.
  if (!not_found.empty())
  {
    std::string zips;
    for (const std::string& zip : not_found)
      zips += (zips.empty() ? "" : ", ") + zip;
    error += "\nZips not found:\n" + zips + "\nCopy the zips next to the game, or to:\n" + system_dir + "/fbneo";
  }
  if (!verify.empty())
    error += "\nRomsets to verify:\n" + verify;
  if (!roms.empty())
  {
    error += "\nMissing files:";
    for (size_t i = 0; i < roms.size() && i < MAX_ROMS; i++)
      error += "\n" + roms[i];
  }
  if (roms.size() > MAX_ROMS)
    more += roms.size() - MAX_ROMS;
  if (more > 0)
    error += "\nAnd " + std::to_string(more) + " more...";
  if (none)
    error += "\nNo file of the romset was found";
  return error;
}

void Rotate(const uint16_t* in, size_t pitch, unsigned width, unsigned height, unsigned rotation,
            std::vector<uint16_t>* out, unsigned* w, unsigned* h)
{
  const bool sideways = rotation == 1 || rotation == 3;
  *w = sideways ? height : width;
  *h = sideways ? width : height;
  out->resize(static_cast<size_t>(width) * height);
  uint16_t* dst = out->data();
  for (unsigned y = 0; y < height; y++)
  {
    const uint16_t* row = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(in) + static_cast<size_t>(y) * pitch);
    switch (rotation & 3)
    {
    case 1:  // 90 degrees anticlockwise: the top row becomes the left column, read from the bottom up
      for (unsigned x = 0; x < width; x++)
        dst[static_cast<size_t>(width - 1 - x) * height + y] = row[x];
      break;
    case 2:  // upside down
      for (unsigned x = 0; x < width; x++)
        dst[static_cast<size_t>(height - 1 - y) * width + (width - 1 - x)] = row[x];
      break;
    case 3:  // 90 degrees clockwise: the top row becomes the right column, read from the top down
      for (unsigned x = 0; x < width; x++)
        dst[static_cast<size_t>(x) * height + (height - 1 - y)] = row[x];
      break;
    default:
      std::copy(row, row + width, dst + static_cast<size_t>(y) * width);
      break;
    }
  }
}
}  // namespace Arcade
