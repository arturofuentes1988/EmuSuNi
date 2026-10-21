// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <minizip/unzip.h>

#include "Arcade.h"
#include "Log.h"
#include "Naomi.h"

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

// The file's extension is one of the console's own.
bool HasOwnExtension(const System& system, const std::string& name)
{
  const std::string lower = Lower(name);
  const size_t dot = lower.rfind('.');
  return dot != std::string::npos &&
         std::find(system.extensions.begin(), system.extensions.end(), lower.substr(dot)) != system.extensions.end();
}

// The ROM a .zip holds, made its current file; its console (nullptr: none). The first file with one of
// the console's own extensions (without a console, or none there, any console's): a .md or a .bin -
// a readme's or a CD track's too - only when there is no other (then the biggest).
const System* GoToZipRom(unzFile zip, const System* system)
{
  const System* found = nullptr;
  int best = -1;
  uLong best_size = 0;
  unz_file_pos at = {};
  for (int result = unzGoToFirstFile(zip); result == UNZ_OK; result = unzGoToNextFile(zip))
  {
    char name[512];
    unz_file_info info;
    if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
      continue;
    const System* of = SystemOfFile(name);
    if (!of || info.uncompressed_size == 0 || info.uncompressed_size > MAX_ROM)
      continue;
    const bool own = system && HasOwnExtension(*system, name), plain = !HasExtension(name, {".md", ".bin"});
    const int rank = (own ? 2 : 0) + (plain ? 1 : 0);
    if (rank > best || (rank == best && !plain && info.uncompressed_size > best_size))
    {
      found = own ? system : of;
      best = rank;
      best_size = info.uncompressed_size;
      unzGetFilePos(zip, &at);
      if (best == (system ? 3 : 1))
        break;  // none can be better (a zip of many files isn't read to its end)
    }
  }
  if (found)
    unzGoToFilePos(zip, &at);
  return found;
}

// The console of the ROM inside a .zip (nullptr: none).
const System* SystemOfZip(const std::string& path)
{
  unzFile zip = unzOpen(path.c_str());
  if (!zip)
    return nullptr;
  const System* found = GoToZipRom(zip, nullptr);
  unzClose(zip);
  return found;
}

// The Game Boy and the Game Boy Color: their games' files are alike (a game made for both, like
// Pokémon Yellow, is a .gb).
bool GameBoyFamily(const System* system)
{
  return system && (system->id == SystemId::GB || system->id == SystemId::GBC);
}

// The PlayStation and the PSP: a .iso, .chd or .pbp can be either's.
bool DiscFamily(const System* system)
{
  return system && (system->id == SystemId::PS1 || system->id == SystemId::PSP || system->id == SystemId::DC);
}

// The files a list names, beside it unless their path is absolute: an .m3u's discs, a .cue's tracks
// (FILE "..." BINARY), a .gdi's tracks (each line after the count: number, LBA, type, sector size,
// the file - in quotes when it has spaces -, offset).
std::vector<std::string> ListedFiles(const std::string& path)
{
  std::vector<std::string> files;
  const bool m3u = HasExtension(path, {".m3u"}), cue = HasExtension(path, {".cue"}), gdi = HasExtension(path, {".gdi"});
  if (!m3u && !cue && !gdi)
    return files;
  std::ifstream list(path);
  const std::filesystem::path folder = std::filesystem::path(path).parent_path();
  std::string line;
  bool count = gdi;  // a .gdi's first line: how many tracks
  while (std::getline(list, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
      continue;
    line.erase(0, start);
    std::string name;
    if (m3u)
    {
      if (line[0] == '#')
        continue;
      name = line;
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    }
    else
    {
      // The file's field: a .cue's after "FILE", a .gdi's after four others.
      size_t at = 0;
      if (cue)
      {
        if (Lower(line.substr(0, 5)) != "file " && Lower(line.substr(0, 5)) != "file\t")
          continue;
        at = 4;
      }
      else
      {
        if (count)
        {
          count = false;
          continue;
        }
        for (int field = 0; field < 4 && at != std::string::npos; field++)
        {
          at = line.find_first_not_of(" \t", at);
          at = at == std::string::npos ? at : line.find_first_of(" \t", at);
        }
        if (at == std::string::npos)
          continue;
      }
      at = line.find_first_not_of(" \t", at);
      if (at == std::string::npos)
        continue;
      if (line[at] == '"')
      {
        const size_t close = line.find('"', at + 1);
        if (close == std::string::npos)
          continue;
        name = line.substr(at + 1, close - at - 1);
      }
      else
      {
        name = line.substr(at, line.find_first_of(" \t", at) - at);
      }
    }
    if (name.empty())
      continue;
    const std::filesystem::path file = std::filesystem::path(name).is_absolute() ? std::filesystem::path(name) : folder / name;
    files.push_back(file.lexically_normal().string());
  }
  return files;
}

// A Dreamcast disc by what is inside it: a GD-ROM CHD (chdman from a .gdi) has "CHGD" track metadata
// (a CD's: "CHT2", a DVD's: "DVD "); a Redump GD-ROM .cue has a "HIGH-DENSITY AREA" remark. (A
// Dreamcast CD-R made into a CHD looks like a CD: its folder decides.) An .m3u by its first disc.
bool IsDreamcastDisc(const std::string& path)
{
  if (HasExtension(path, {".gdi", ".cdi"}))
    return true;  // only the Dreamcast's
  if (HasExtension(path, {".m3u"}))
  {
    const std::vector<std::string> discs = ListedFiles(path);
    return !discs.empty() && !HasExtension(discs.front(), {".m3u"}) && IsDreamcastDisc(discs.front());
  }
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  bool dc = false;
  uint8_t b[16] = {};
  auto read_at = [&](uint64_t offset, size_t size) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && fread(b, 1, size, file) == size;
  };
  auto be32 = [&b](int at) {
    return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
           static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3];
  };
  if (HasExtension(path, {".cue"}))
  {
    char text[8192] = {};
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    text[read] = 0;
    dc = std::strstr(text, "HIGH-DENSITY AREA") != nullptr;
  }
  else if (HasExtension(path, {".chd"}) && read_at(0, 16) && std::memcmp(b, "MComprHD", 8) == 0)
  {
    const uint32_t version = be32(12);
    if (read_at(version >= 5 ? 48 : 36, 8))
    {
      uint64_t meta = static_cast<uint64_t>(be32(0)) << 32 | be32(4);
      for (int i = 0; i < 16 && meta != 0 && !dc && read_at(meta, 16); i++)
      {
        dc = std::memcmp(b, "CHGD", 4) == 0;
        meta = static_cast<uint64_t>(be32(8)) << 32 | be32(12);
      }
    }
  }
  fclose(file);
  return dc;
}

// A folder named only after a disc - "Disc 1", "Disk2", "CD 1", "GD-ROM 2" - its number; 0: another name.
int BareDiscLabel(const std::string& name)
{
  const std::string lower = Lower(name);
  for (const char* label : {"gd-rom", "gdrom", "disc", "disk", "cd"})
  {
    const size_t length = std::char_traits<char>::length(label);
    if (lower.compare(0, length, label) != 0)
      continue;
    size_t at = length;
    if (at < lower.size() && (lower[at] == ' ' || lower[at] == '_' || lower[at] == '-'))
      at++;
    const size_t digits = lower.size() - at;
    if (digits == 0 || digits > 2 ||
        !std::all_of(lower.begin() + static_cast<std::ptrdiff_t>(at), lower.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; }))
      return 0;
    return std::atoi(lower.c_str() + at);
  }
  return 0;
}

// A .iso, .chd or .pbp by what is inside it: is it a PSP game (not a PlayStation one)? A UMD image's
// volume descriptor says "PSP GAME"; a PSP game's CHD is a DVD's (chdman createdvd: metadata "DVD "),
// a PlayStation one's a CD's; a PlayStation game's PBP (PSN) holds a disc image ("PSISOIMG",
// "PSTITLEIMG" for several discs) where a PSP game's has its data.
bool IsPspDisc(const std::string& path)
{
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return false;
  uint8_t b[64] = {};
  auto read_at = [&](uint64_t offset, size_t size) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && fread(b, 1, size, file) == size;
  };
  auto be32 = [&b](int at) {
    return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
           static_cast<uint32_t>(b[at + 2]) << 8 | b[at + 3];
  };
  bool psp = false;
  if (HasExtension(path, {".iso"}))
  {
    psp = read_at(16 * 2048 + 8, 8) && std::memcmp(b, "PSP GAME", 8) == 0;  // sector 16's system name
  }
  else if (HasExtension(path, {".pbp"}))
  {
    // "\0PBP", its version, then eight offsets: the last one DATA.PSAR's.
    if (read_at(0, 40) && std::memcmp(b, "\0PBP", 4) == 0)
    {
      const uint32_t psar = b[36] | b[37] << 8 | b[38] << 16 | static_cast<uint32_t>(b[39]) << 24;
      psp = !(read_at(psar, 8) && (std::memcmp(b, "PSISOIMG", 8) == 0 || std::memcmp(b, "PSTITLEI", 8) == 0));
    }
  }
  else if (HasExtension(path, {".chd"}) && read_at(0, 64) && std::memcmp(b, "MComprHD", 8) == 0)
  {
    // The metadata's offset: version 5 at 48, versions 3 and 4 at 36 (big-endian, 64 bits); then
    // each entry's tag, flags and length, and the next one's offset.
    const uint32_t version = be32(12);
    const int at = version >= 5 ? 48 : 36;
    uint64_t meta = static_cast<uint64_t>(be32(at)) << 32 | be32(at + 4);
    for (int i = 0; i < 8 && meta != 0 && read_at(meta, 16); i++)
    {
      if (std::memcmp(b, "DVD ", 4) == 0)
      {
        psp = true;
        break;
      }
      meta = static_cast<uint64_t>(be32(8)) << 32 | be32(12);
    }
  }
  fclose(file);
  return psp;
}

// Is the file one of the console's games (a ROM, or a .zip with one inside; without open_zips,
// any .zip)? Without open_zips (the console's folder is its own) a Game Boy game in the Game Boy
// Color's folder is the Game Boy Color's, and the other way round: the folder says where it goes.
// So for a .iso, .chd or .pbp in the PlayStation's or the PSP's own folder; in a shared folder
// what is inside it says.
bool IsGameOf(const std::filesystem::path& path, const System& system, bool open_zips = true)
{
  const std::string name = path.filename().string();
  if (system.id == SystemId::ARCADE)
  {
    // A romset: a .zip or .7z named after one of FinalBurn Neo's arcade drivers (mslug.zip), never
    // opened (the core reads it); its BIOS sets (neogeo.zip) and console games aren't games here.
    if (!HasExtension(name, {".zip", ".7z"}))
      return false;
    const Arcade::Driver* driver = Arcade::Find(path.stem().string());
    return driver && driver->arcade;
  }
  if (IsFlycastBoard(system.id))
  {
    // A NAOMI or Atomiswave game: a .zip or .7z named after one of Flycast's sets of that board (mvsc2.zip),
    // never opened (the core reads it); a BIOS's zip (naomi.zip, awbios.zip) is no set of its list. A NAOMI's
    // can also be a cartridge image, a .dat or .bin whose header says NAOMI (src/Naomi.h; loose, not zipped).
    if (system.id == SystemId::NAOMI && Naomi::IsImageName(name))
    {
      Naomi::Image image;
      return Naomi::ReadImage(path.string(), &image);
    }
    if (!HasExtension(name, {".zip", ".7z"}))
      return false;
    const Naomi::Set* set = Naomi::Find(path.stem().string());
    return set && Naomi::Plays(system.id, *set);
  }
  if (HasExtension(name, {".zip"}))
    return !system.by_path && (!open_zips || SystemOfZip(path.string()) == &system);  // no zipped CDs
  const System* of = SystemOfFile(name);
  if (DiscFamily(of) && DiscFamily(&system) && HasOwnExtension(system, name))
  {
    // A disc image's extension of two or three of the PlayStation, the PSP and the Dreamcast: in a
    // console's own folder it is that console's; in a shared one what is inside it says.
    int owners = 0;
    for (SystemId id : {SystemId::PS1, SystemId::PSP, SystemId::DC})
      owners += HasSystem(id) && HasOwnExtension(GetSystem(id), name) ? 1 : 0;
    if (owners < 2 || !open_zips)
      return true;
    const SystemId disc = HasSystem(SystemId::DC) && IsDreamcastDisc(path.string()) ? SystemId::DC :
                          HasSystem(SystemId::PSP) && IsPspDisc(path.string())    ? SystemId::PSP :
                                                                                    SystemId::PS1;
    return disc == system.id;
  }
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

// Plain names: a game's file named only "disc", "game", "EBOOT" (a PBP)... is named after its folder.
const std::set<std::string>& GenericNames()
{
  static const std::set<std::string> GENERIC = {"disc", "disk", "game", "image", "gdi", "cd", "gdrom", "gd-rom", "eboot"};
  return GENERIC;
}

// An arcade game from its driver (src/Arcade.h): the romset's name in lower case is its id (the
// drive's letters' case doesn't matter, the core's does), the driver's full name its title (a clone's
// with its parent's), the region what the name says, no tags; its covers by the full name, then the
// parent's; the zips it may need and hasn't got beside it, in system/fbneo or in system.
void ArcadeGame(Game* game, const std::string& folder)
{
  game->id = Lower(game->id);
  game->key = GetSystem(SystemId::ARCADE).prefix + game->id;
  const Arcade::Driver* driver = Arcade::Find(game->id);
  if (!driver)
    return;
  game->title = Arcade::Title(*driver);
  game->region = Arcade::Region(*driver);
  game->tags.clear();
  game->cover_names = {Arcade::CoverName(driver->title)};
  if (const Arcade::Driver* parent = Arcade::Parent(*driver))
    game->cover_names.push_back(Arcade::CoverName(parent->title));
  game->missing = Arcade::Missing(*driver, folder, std::string(App::DATA_DIR) + "/system");
}

// A NAOMI or Atomiswave game from Flycast's set (src/Naomi.h), as an arcade game: the set's name in lower case is
// its id, its full name its title (a clone's with its parent's), the region what the name says, no tags; its
// covers by the full name, then the parent's; what it needs that isn't there (its BIOS's zip, a clone's parent, a
// GD-ROM game's disc image) in the places the core looks.
void BoardGame(Game* game, const std::string& folder)
{
  // A NAOMI cartridge image (.dat, .bin): its file's name is its id, as a console's ROM's; its title its set's name
  // when the file's or the header's names one (Naomi::MatchImage), else the file's (or the header's), with the
  // file's own region and tags; its covers by the file's name (libretro's names for these dumps), then the set's and
  // its parent's; it needs naomi.zip.
  if (Naomi::IsImageName(game->path))
  {
    Naomi::Image image;
    Naomi::ReadImage(game->path, &image);
    const Naomi::Set* set = Naomi::MatchImage(game->title, image);
    game->cover_names = {Arcade::CoverName(std::filesystem::path(game->path).stem().string())};
    if (set)
    {
      game->cover_names.push_back(Arcade::CoverName(set->title));
      if (const Naomi::Set* parent = Naomi::Parent(*set))
        game->cover_names.push_back(Arcade::CoverName(parent->title));
    }
    game->title = Naomi::ImageTitle(game->title, image, set);
    game->missing = Naomi::ImageMissing(folder, std::string(App::DATA_DIR) + "/system");
    return;
  }
  game->id = Lower(game->id);
  game->key = GetSystem(game->system).prefix + game->id;
  const Naomi::Set* set = Naomi::Find(game->id);
  if (!set)
    return;
  game->title = Naomi::Title(*set);
  game->region = Naomi::Region(*set);
  game->tags.clear();
  game->cover_names = {Arcade::CoverName(set->title)};
  if (const Naomi::Set* parent = Naomi::Parent(*set))
    game->cover_names.push_back(Arcade::CoverName(parent->title));
  game->missing = Naomi::Missing(*set, folder, std::string(App::DATA_DIR) + "/system");
}

// The name a file's folder gives it - the folder's, or the game's folder above a folder per disc named
// only so ("Shenmue/Disc 1/disc.gdi") - and its disc (from the file's name or that folder's; 0: none).
void FolderName(const std::filesystem::path& folder, int depth, int stem_disc, std::string* owner, int* disc)
{
  const int folder_disc = depth > 1 ? BareDiscLabel(folder.filename().string()) : 0;
  *owner = folder_disc > 0 ? folder.parent_path().filename().string() : folder.filename().string();
  *disc = stem_disc > 0 ? stem_disc : folder_disc;
}

void ScanFolder(const std::filesystem::path& folder, int depth, const System& system, bool open_zips,
                std::vector<Game>* games)
{
  const size_t first = games->size();
  // A CD's .cue or .gdi here: its tracks aren't cartridges (below), for a console of .bin files.
  const bool bin = !system.by_path && HasOwnExtension(system, ".bin");
  std::vector<std::string> lists;
  std::error_code ec;
  // (Not a range-for: its ++ throws when the drive goes away during the scan; increment(ec) just stops.)
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::filesystem::directory_entry& entry = *it;
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
    if (bin && HasExtension(name, {".cue", ".gdi"}))
      lists.push_back(entry.path().string());
    if (!IsGameOf(entry.path(), system, open_zips))
      continue;
    Game game;
    game.path = entry.path().string();
    game.id = entry.path().stem().string();
    // A plain name in a folder named after the game - "disc.gdi", "EBOOT.PBP", or only the disc's
    // ("CD1.chd") - the folder names it (with the disc).
    const int stem_disc = BareDiscLabel(game.id);
    if (system.by_path && !IsRomsetSystem(system.id) && depth > 0 &&
        (stem_disc > 0 || GenericNames().count(Lower(game.id)) > 0))
    {
      std::string owner;
      int disc = 0;
      FolderName(folder, depth, stem_disc, &owner, &disc);
      game.id = disc > 0 ? owner + " (Disc " + std::to_string(disc) + ")" : owner;
    }
    game.system = system.id;
    game.key = system.prefix + game.id;
    std::error_code size_error;
    game.size = entry.file_size(size_error);
    ParseName(game.id, &game.title, &game.region, &game.tags);
    if (system.id == SystemId::ARCADE)
      ArcadeGame(&game, folder.string());
    else if (IsFlycastBoard(system.id))
      BoardGame(&game, folder.string());
    games->push_back(std::move(game));
  }
  if (IsRomsetSystem(system.id))
    return;  // (a romset is one zip: no discs, no tracks)
  if (!system.by_path)
  {
    // A .bin is a Mega Drive cartridge's file and a CD's track: the files a .cue or .gdi here names (a
    // PlayStation or Dreamcast disc's, in a folder the Mega Drive shares) aren't cartridges. In any
    // letters' case: the USB drive's exFAT finds "GAME.BIN" for a game.bin.
    std::set<std::string> tracks;
    for (const std::string& list : lists)
    {
      for (const std::string& track : ListedFiles(list))
        tracks.insert(Lower(track));
    }
    if (!tracks.empty())
    {
      games->erase(std::remove_if(games->begin() + static_cast<std::ptrdiff_t>(first), games->end(),
                                  [&tracks](const Game& game) {
                                    const std::filesystem::path path = std::filesystem::path(game.path).lexically_normal();
                                    return tracks.count(Lower(path.string())) > 0;
                                  }),
                   games->end());
    }
    return;
  }
  // A game on several discs: its .m3u lists the discs' files, which aren't games of their own.
  std::set<std::string> discs;
  for (size_t i = first; i < games->size(); i++)
  {
    if (!HasExtension((*games)[i].path, {".m3u"}))
      continue;
    for (const std::string& disc : ListedFiles((*games)[i].path))
      discs.insert(disc);
  }
  if (!discs.empty())
  {
    games->erase(std::remove_if(games->begin() + static_cast<std::ptrdiff_t>(first), games->end(),
                                [&discs](const Game& game) {
                                  return discs.count(std::filesystem::path(game.path).lexically_normal().string()) > 0;
                                }),
                 games->end());
  }
  // Different games right in this folder ("disc.gdi" and "game.cdi"; not one game's .m3u and its discs,
  // nor one game in two kinds of file, "disc.gdi" and "disc.chd"): the folder can't name them all - its
  // name with each file's ("Twins - disc", "Crazy Taxi - disc (USA)"), so each keeps a key of its own.
  if (depth == 0)
    return;
  std::set<std::string> stems;
  for (size_t i = first; i < games->size(); i++)
  {
    const std::filesystem::path path((*games)[i].path);
    if (path.parent_path() == folder)
      stems.insert(Lower(path.stem().string()));
  }
  if (stems.size() < 2)
    return;
  for (size_t i = first; i < games->size(); i++)
  {
    Game& game = (*games)[i];
    const std::filesystem::path path(game.path);
    const std::string stem = path.stem().string();
    if (path.parent_path() != folder || GenericNames().count(Lower(stem)) == 0)
      continue;
    std::string owner;
    int disc = 0;
    FolderName(folder, depth, 0, &owner, &disc);
    // (Before the folder's region and tags - where ParseName ends a title -, which then still read as
    // such: "Crazy Taxi - disc (USA)", "Crazy_Taxi - disc (USA)" for "Crazy_Taxi_(USA)".)
    const size_t tag = owner.find_first_of("([", 1);
    std::string head = owner.substr(0, tag);
    while (!head.empty() && (head.back() == ' ' || head.back() == '_'))
      head.pop_back();
    game.id = head + " - " + stem + (tag == std::string::npos ? std::string() : " " + owner.substr(tag));
    if (disc > 0)
      game.id += " (Disc " + std::to_string(disc) + ")";
    game.key = system.prefix + game.id;
    ParseName(game.id, &game.title, &game.region, &game.tags);
  }
}

// The discs of one game side by side - names alike but for "(Disc 1)", "(Disc 2)"..., the same kind of
// file - become one game named without the disc's number, its discs in order (Game::discs). Each disc
// can be in a folder of its own named so ("Shenmue (Disc 1)/disc.gdi"), then alone in it.
void GroupDiscs(const System& system, const std::string& root, std::vector<Game>* games)
{
  // (The console's own folder isn't a game's: "Roms/dc/Disc 1/disc.gdi" is no game's disc 1.)
  std::filesystem::path top = std::filesystem::path(root).lexically_normal();
  if (top.filename().empty())
    top = top.parent_path();
  std::map<std::string, int> in_folder;  // games per folder
  for (const Game& game : *games)
    in_folder[std::filesystem::path(game.path).parent_path().string()]++;
  struct Disc
  {
    int number;
    size_t index;
    std::string base;
  };
  std::map<std::string, std::vector<Disc>> sets;  // by folder, name without the number, extension
  for (size_t i = 0; i < games->size(); i++)
  {
    const std::filesystem::path path((*games)[i].path);
    const std::filesystem::path folder = path.parent_path();
    std::string base, folder_base;
    int number = DiscNumber(path.stem().string(), &base);
    const bool alone = in_folder[folder.string()] == 1;
    const int folder_number = alone ? DiscNumber(folder.filename().string(), &folder_base) : 0;
    const int bare_number = alone && folder_number == 0 ? BareDiscLabel(folder.filename().string()) : 0;
    const int stem_number = number == 0 ? BareDiscLabel(path.stem().string()) : 0;
    std::filesystem::path side_by_side = folder;
    if (number > 0 && folder_number == number)
    {
      side_by_side = folder.parent_path();  // "Shenmue (Disc 1)/Shenmue (Disc 1).gdi"
    }
    else if (number == 0 && folder_number > 0)
    {
      number = folder_number;  // "Shenmue (Disc 1)/disc.gdi"
      base = folder_base;
      side_by_side = folder.parent_path();
    }
    else if (bare_number > 0 && (number == 0 || number == bare_number) &&
             folder.parent_path().lexically_normal() != top)
    {
      // "Shenmue/Disc 1/disc.gdi", "Shenmue/Disc 1/Shenmue (Disc 1).gdi": the discs' folders side by side
      // in the game's (named after it: one key per game folder).
      number = bare_number;
      base = folder.parent_path().filename().string();
      side_by_side = folder.parent_path().parent_path();
    }
    else if (stem_number > 0 && folder.lexically_normal() != top)
    {
      number = stem_number;  // "FF7/CD1.chd", "FF7/CD2.chd": side by side in the game's folder
      base = folder.filename().string();
    }
    if (number <= 0 || base.empty())
      continue;
    sets[side_by_side.string() + '\n' + Lower(base) + '\n' + Lower(path.extension().string())].push_back(
        {number, i, base});
  }
  std::vector<bool> joined(games->size(), false);
  std::vector<Game> several;
  for (auto& [key, discs] : sets)
  {
    std::stable_sort(discs.begin(), discs.end(), [](const Disc& a, const Disc& b) { return a.number < b.number; });
    // One file per disc (another "Disc 1" stays a game of its own).
    discs.erase(std::unique(discs.begin(), discs.end(), [](const Disc& a, const Disc& b) { return a.number == b.number; }),
                discs.end());
    if (discs.size() < 2)
      continue;
    Game game = (*games)[discs.front().index];
    game.id = discs.front().base;
    game.key = system.prefix + game.id;
    ParseName(game.id, &game.title, &game.region, &game.tags);
    game.size = 0;
    game.discs.clear();
    for (const Disc& disc : discs)
    {
      game.discs.push_back((*games)[disc.index].path);
      game.size += (*games)[disc.index].size;
      joined[disc.index] = true;
    }
    several.push_back(std::move(game));
  }
  if (several.empty())
    return;
  std::vector<Game> rest;
  for (size_t i = 0; i < games->size(); i++)
  {
    if (!joined[i])
      rest.push_back(std::move((*games)[i]));
  }
  for (Game& game : several)
    rest.push_back(std::move(game));
  *games = std::move(rest);
}

// The files the lists in `start` name (in the list's own folder or below, there on the drive), and
// what those name in turn, besides `start` itself.
std::vector<std::string> NamedFiles(const std::vector<std::string>& start)
{
  std::vector<std::string> files = start;
  for (size_t i = 0; i < files.size(); i++)
  {
    const std::string folder = std::filesystem::path(files[i]).parent_path().string() + "/";
    for (const std::string& named : ListedFiles(files[i]))
    {
      std::error_code ec;
      if (named.compare(0, folder.size(), folder) == 0 && std::find(files.begin(), files.end(), named) == files.end() &&
          std::filesystem::is_regular_file(named, ec))
      {
        files.push_back(named);
      }
    }
  }
  return files;
}
}  // namespace

bool IsRomName(const std::string& name)
{
  return IsRomFile(name) || HasExtension(name, {".zip", ".7z"});
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

int DiscNumber(const std::string& name, std::string* base)
{
  const std::string lower = Lower(name);
  auto digits = [&lower](size_t* at) {
    const size_t from = *at;
    while (*at < lower.size() && std::isdigit(static_cast<unsigned char>(lower[*at])))
      (*at)++;
    return *at - from;
  };
  for (size_t open = 0; open < lower.size(); open++)
  {
    if (lower[open] != '(' && lower[open] != '[')
      continue;
    const char close = lower[open] == '(' ? ')' : ']';
    size_t at = open + 1;
    size_t word = 0;
    for (const char* disc : {"disc", "disk", "cd"})
    {
      const size_t length = std::char_traits<char>::length(disc);
      if (lower.compare(at, length, disc) == 0)
      {
        word = length;
        break;
      }
    }
    if (word == 0)
      continue;
    at += word;
    if (at < lower.size() && (lower[at] == ' ' || lower[at] == '-' || lower[at] == '_'))
      at++;
    const size_t number_at = at;
    const size_t number_digits = digits(&at);
    if (number_digits == 0 || number_digits > 2)
      continue;
    // "(Disc 2 of 3)", "(CD 2/3)"
    if (lower.compare(at, 4, " of ") == 0)
    {
      at += 4;
      if (digits(&at) == 0)
        continue;
    }
    else if (at < lower.size() && lower[at] == '/')
    {
      at++;
      if (digits(&at) == 0)
        continue;
    }
    const int number = std::atoi(lower.c_str() + number_at);
    if (at >= lower.size() || lower[at] != close || number <= 0)
      continue;
    size_t from = open, to = at + 1;
    if (from > 0 && name[from - 1] == ' ')
      from--;
    else if (to < name.size() && name[to] == ' ')
      to++;
    *base = name.substr(0, from) + name.substr(to);
    while (!base->empty() && base->back() == ' ')
      base->pop_back();
    return number;
  }
  return 0;
}

std::vector<std::string> Discs(const Game& game)
{
  if (game.discs.size() > 1)
    return game.discs;
  if (HasExtension(game.path, {".m3u"}))
    return ListedFiles(game.path);
  return {};
}

std::vector<std::string> GameFiles(const Game& game)
{
  std::vector<std::string> own;
  for (const std::string& file : game.discs.size() > 1 ? game.discs : std::vector<std::string>{game.path})
    own.push_back(std::filesystem::path(file).lexically_normal().string());
  const std::vector<std::string> reach = NamedFiles(own);
  // The other lists beside them (another game's .cue of the same .bin): what they name stays.
  std::set<std::string> folders;
  for (const std::string& file : reach)
    folders.insert(std::filesystem::path(file).parent_path().string());
  std::vector<std::string> others;
  for (const std::string& folder : folders)
  {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
      const std::string path = it->path().lexically_normal().string();
      if (HasExtension(path, {".cue", ".gdi", ".m3u"}) && std::find(reach.begin(), reach.end(), path) == reach.end())
        others.push_back(path);
    }
  }
  const std::vector<std::string> named_by_others = NamedFiles(others);
  const std::set<std::string> stays(named_by_others.begin(), named_by_others.end());
  std::vector<std::string> files;
  for (const std::string& file : reach)
  {
    if (std::find(own.begin(), own.end(), file) != own.end() || stays.count(file) == 0)
      files.push_back(file);
  }
  return files;
}

std::vector<Game> Scan(const std::string& folder, SystemId system, bool open_zips)
{
  std::vector<Game> games;
  ScanFolder(folder, 0, GetSystem(system), open_zips, &games);
  if (system == SystemId::PS1 || system == SystemId::DC)
    GroupDiscs(GetSystem(system), folder, &games);
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
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::filesystem::directory_entry& entry = *it;
    std::error_code type_error;
    const System& console = GetSystem(system);
    if (!entry.is_directory(type_error) &&
        (IsGameOf(entry.path(), console) ||
         (GameBoyFamily(&console) && GameBoyFamily(SystemOfFile(entry.path().filename().string())))))
      count++;
  }
  return count;
}

bool ReadRom(const std::string& path, SystemId system, std::vector<uint8_t>* data, std::string* error)
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
  // (The console's own file: a Game Boy Advance game's .gba, not a README.md beside it - .md is the
  // Mega Drive's too.)
  bool found = false;
  unz_file_info info;
  if (GoToZipRom(zip, &GetSystem(system)) &&
      unzGetCurrentFileInfo(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) == UNZ_OK &&
      unzOpenCurrentFile(zip) == UNZ_OK)
  {
    data->resize(info.uncompressed_size);
    const int read = unzReadCurrentFile(zip, data->data(), static_cast<unsigned>(data->size()));
    unzCloseCurrentFile(zip);
    found = read == static_cast<int>(data->size());
  }
  unzClose(zip);
  if (!found)
    *error = "no game in the .zip";
  return found;
}

// ---- Where the games are found ----

namespace
{
// A path tidied up: no "." or "..", no '/' at its end.
std::string TidyPath(const std::string& path)
{
  if (path.empty())
    return path;
  std::string out = std::filesystem::path(path).lexically_normal().generic_string();
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  return out;
}

bool IsFolder(const std::string& path)
{
  std::error_code ec;
  return !path.empty() && std::filesystem::is_directory(path, ec);
}

std::string JoinPath(const std::string& folder, const std::string& name)
{
  return folder == "/" ? "/" + name : folder + "/" + name;
}

// The folders in `folder`: their names, sorted.
std::vector<std::string> FolderNames(const std::string& folder)
{
  std::vector<std::string> names;
  if (folder.empty())
    return names;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
  {
    std::error_code type_error;
    if (it->is_directory(type_error))
      names.push_back(it->path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

// The one of `names` that is `name` in any letters' case (the same name first), else "".
std::string PickName(const std::vector<std::string>& names, const std::string& name)
{
  if (std::find(names.begin(), names.end(), name) != names.end())
    return name;
  const std::string lower = Lower(name);
  for (const std::string& each : names)
  {
    if (Lower(each) == lower)
      return each;
  }
  return {};
}

// A USB drive's top folder at the start of the path ("/usb0", or "/mnt/usb0" outside the sandbox): its
// length, else 0.
size_t DriveRootLength(const std::string& path)
{
  for (const char* start : {"/usb", "/mnt/usb"})
  {
    const size_t length = std::char_traits<char>::length(start);
    if (path.size() > length && path.compare(0, length, start) == 0 &&
        std::isdigit(static_cast<unsigned char>(path[length])) && (path.size() == length + 1 || path[length + 1] == '/'))
      return length + 1;
  }
  return 0;
}
}  // namespace

std::vector<std::string> UsbDrives(const std::string& root)
{
  // (A mount point without a drive is an empty folder: FTP shows /mnt/usb1 so on a PS4 with one stick.)
  std::vector<std::string> drives;
  for (int i = 0; i < 8; i++)
  {
    const std::string drive = root + "/usb" + std::to_string(i);
    std::error_code ec;
    if (IsFolder(drive) && std::filesystem::directory_iterator(drive, ec) != std::filesystem::directory_iterator())
      drives.push_back(drive);
  }
  return drives;
}

std::string FindEntry(const std::string& folder, const std::string& name)
{
  const std::string found = name.empty() ? std::string() : PickName(FolderNames(folder), name);
  return found.empty() ? std::string() : JoinPath(TidyPath(folder), found);
}

Places FindPlaces(const std::string& data_dir, const std::vector<std::string>& drives)
{
  Places places;
  places.drives = drives;
  std::vector<std::string> tops = {data_dir};
  tops.insert(tops.end(), drives.begin(), drives.end());
  for (const std::string& top : tops)
  {
    // Every Roms folder there in any letters' case, and in each every console's folder in any case: the PS4's
    // own storage tells case apart ("Roms" and "ROMS", "snes" and "SNES" are two folders each there), a drive's
    // FAT or exFAT doesn't (one of each; ConsoleFolders keeps one folder once).
    for (const std::string& roms_name : FolderNames(top))
    {
      if (Lower(roms_name) != "roms")
        continue;
      const std::string roms = JoinPath(TidyPath(top), roms_name);
      for (const std::string& name : FolderNames(roms))  // one look at the Roms folder for every console
      {
        const std::string lower = Lower(name);
        for (const System& system : Systems())
        {
          if (lower == system.key)
            places.folders[system.key].push_back(JoinPath(roms, name));
        }
      }
    }
  }
  return places;
}

std::string PathKey(const std::string& path, const std::vector<std::string>& drives)
{
  std::string key = TidyPath(path);
  if (key.compare(0, 8, "/mnt/usb") == 0 && DriveRootLength(key) == 9)
    key.erase(0, 4);  // "/mnt/usb0/..." is "/usb0/..." in the app
  const std::string lower = Lower(key);
  bool drive = DriveRootLength(key) > 0;
  for (const std::string& each : drives)
  {
    const std::string top = Lower(TidyPath(each));
    if (!top.empty() && (lower == top || lower.compare(0, top.size() + 1, top + "/") == 0))
      drive = true;
  }
  return drive ? lower : key;
}

std::vector<std::string> ConsoleFolders(const System& system, const std::string& chosen, const Places& places)
{
  std::vector<std::string> candidates;
  if (!chosen.empty())
    candidates.push_back(TidyPath(chosen));
  const auto found = places.folders.find(system.key);
  if (found != places.folders.end())
  {
    for (const std::string& folder : found->second)
      candidates.push_back(TidyPath(folder));
  }
  std::vector<std::string> folders, keys;
  for (const std::string& candidate : candidates)
  {
    const std::string key = PathKey(candidate, places.drives);
    const auto same = std::find(keys.begin(), keys.end(), key);
    if (same == keys.end())
    {
      folders.push_back(candidate);
      keys.push_back(key);
      continue;
    }
    std::string& kept = folders[static_cast<size_t>(same - keys.begin())];
    if (!IsFolder(kept) && IsFolder(candidate))
      kept = candidate;  // "/usb0/Roms/n64" asked for, "/usb0/ROMS/N64" there (a drive that tells case)
  }
  return folders;
}

bool IsRomsFolder(const std::string& folder, const std::string& key, const std::string& data_dir)
{
  const std::string path = Lower(TidyPath(folder)), tail = "/roms/" + Lower(key);
  if (key.empty() || path.size() <= tail.size() || path.compare(path.size() - tail.size(), tail.size(), tail) != 0)
    return false;
  const std::string top = path.substr(0, path.size() - tail.size());
  return top == Lower(TidyPath(data_dir)) || (DriveRootLength(top) > 0 && DriveRootLength(top) == top.size());
}

Found ScanAll(const std::array<std::string, SYSTEM_COUNT>& chosen, const Places& places)
{
  Found found;
  std::array<std::vector<std::string>, SYSTEM_COUNT> keys;
  for (const System& system : Systems())
  {
    const size_t id = static_cast<size_t>(system.id);
    found.folders[id] = ConsoleFolders(system, chosen[id], places);
    for (const std::string& folder : found.folders[id])
      keys[id].push_back(PathKey(folder, places.drives));
  }
  // Another console's folder is this one, or inside it, or around it.
  auto within = [](const std::string& path, const std::string& folder) {
    const std::string start = folder == "/" ? folder : folder + "/";
    return path.size() > start.size() && path.compare(0, start.size(), start) == 0;
  };
  auto shared = [&keys, &within](size_t id, const std::string& mine) {
    for (size_t other = 0; other < keys.size(); other++)
    {
      for (const std::string& theirs : keys[other])
      {
        if (other != id && (theirs == mine || within(theirs, mine) || within(mine, theirs)))
          return true;
      }
    }
    return false;
  };
  for (const System& system : Systems())
  {
    const size_t id = static_cast<size_t>(system.id);
    std::set<std::string> seen;
    for (size_t f = 0; f < found.folders[id].size(); f++)
    {
      int here = 0;
      for (Game& game : Scan(found.folders[id][f], system.id, shared(id, keys[id][f])))
      {
        if (!seen.insert(PathKey(game.path, places.drives)).second)
          continue;  // (a folder inside another one of the console's)
        found.games.push_back(std::move(game));
        here++;
      }
      found.found_in[id].push_back(here);
      found.counts[id] += here;
    }
  }
  return found;
}

std::vector<SystemId> ShownConsoles(const std::vector<SystemId>& order, const std::string& mode,
                                     const std::array<int, SYSTEM_COUNT>& games, const std::string& off)
{
  std::set<std::string> hidden;
  std::string key;
  for (size_t i = 0; i <= off.size(); i++)
  {
    if (i < off.size() && off[i] != ',')
    {
      if (off[i] != ' ')
        key += off[i];
      continue;
    }
    if (!key.empty())
      hidden.insert(Lower(key));
    key.clear();
  }
  std::vector<SystemId> all, shown;
  for (const SystemId id : order)
  {
    const int index = static_cast<int>(id);
    if (index < 0 || index >= SYSTEM_COUNT || !HasSystem(id) || std::find(all.begin(), all.end(), id) != all.end())
      continue;
    all.push_back(id);
    const bool on = mode == "games"  ? games[static_cast<size_t>(index)] > 0 :
                    mode == "chosen" ? hidden.count(GetSystem(id).key) == 0 :
                                       true;
    if (on)
      shown.push_back(id);
  }
  return shown.empty() ? all : shown;  // never no tab at all
}

std::string UsbCoversFolder(const std::vector<std::string>& drives)
{
  for (const std::string& drive : drives)
  {
    const std::string app = FindEntry(drive, "EmuSuNi");
    if (!app.empty())
    {
      const std::string covers = FindEntry(app, "covers");
      return covers.empty() ? app + "/covers" : covers;
    }
  }
  for (const std::string& drive : drives)
  {
    if (!FindEntry(drive, "Roms").empty())
      return TidyPath(drive) + "/EmuSuNi/covers";
  }
  return drives.empty() ? std::string() : TidyPath(drives.front()) + "/EmuSuNi/covers";
}

std::vector<std::string> UsbCoversFolders(const std::vector<std::string>& drives)
{
  std::vector<std::string> folders;
  for (const std::string& drive : drives)
  {
    const std::string covers = FindEntry(FindEntry(drive, "EmuSuNi"), "covers");
    if (!covers.empty())
      folders.push_back(covers);
  }
  return folders;
}

// ---- The consoles' folders, made by the app ----

bool LooksLikePs4Storage(const std::string& drive)
{
  const std::string user = FindEntry(drive, "user");
  if (user.empty())
    return false;
  for (const char* name : {"app", "patch", "addcont"})
  {
    if (!FindEntry(user, name).empty())
      return true;
  }
  return false;
}

std::vector<std::string> GameRomsFolders(const std::vector<std::string>& drives)
{
  std::vector<std::string> folders;
  for (const std::string& drive : drives)
  {
    const std::string roms = FindEntry(drive, "Roms");  // its own spelling
    if (!roms.empty() && !LooksLikePs4Storage(drive))
      folders.push_back(roms);
  }
  if (!folders.empty())
    return folders;
  // No drive with games yet: the first USB port's, when it is a player's drive.
  for (const std::string& drive : drives)
  {
    const std::string tidy = TidyPath(drive);
    if (std::filesystem::path(tidy).filename() == "usb0" && !LooksLikePs4Storage(drive))
      folders.push_back(JoinPath(tidy, "Roms"));
  }
  return folders;
}

FolderMaking MakeConsoleFolders(const std::string& roms)
{
  FolderMaking result;
  result.consoles = static_cast<int>(Systems().size());
  std::error_code ec;
  if (!IsFolder(roms))
  {
    std::filesystem::create_directory(roms, ec);
    if (!IsFolder(roms))
    {
      result.failed = result.consoles;
      result.error = ec ? ec.message() : "it can't be made";
      return result;
    }
  }
  const std::vector<std::string> names = FolderNames(roms);  // ("PSP" is the psp folder: any letters' case)
  for (const System& system : Systems())
  {
    bool there = false;
    for (const std::string& name : names)
      there = there || Lower(name) == system.key;
    if (there)
    {
      result.there++;
      continue;
    }
    std::error_code make_error;
    if (std::filesystem::create_directory(JoinPath(roms, system.key), make_error))
    {
      result.made++;
    }
    else if (!make_error)
    {
      result.there++;  // (made meanwhile)
    }
    else
    {
      result.failed = result.consoles - result.made - result.there;
      result.error = make_error.message();
      break;
    }
  }
  return result;
}
}  // namespace Library
