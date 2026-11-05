// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <minizip/unzip.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Arcade.h"
#include "DrivePaths.h"
#include "Log.h"
#include "Naomi.h"

namespace Library
{
namespace
{
constexpr long MAX_ROM = 96L * 1024 * 1024;  // the biggest N64 cartridges are 64 MiB
// (00.90) The biggest .bin taken for an Atari 2600 cartridge: a Genesis cartridge is 128 KiB or more.
constexpr uint64_t A2600_BIN_MAX = 64 * 1024;

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
#ifdef EMUSUNI_A2600
    // (00.90) A small .bin is an Atari 2600 cartridge, not a Genesis one (IsGameOf).
    if (of->id == SystemId::MD && HasExtension(name, {".bin"}) && info.uncompressed_size <= A2600_BIN_MAX)
      of = &GetSystem(SystemId::A2600);
#endif
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

// The consoles of CD images: a .iso, .chd, .cue or .m3u can be the PlayStation's, the PSP's, the Dreamcast's or the
// Saturn's.
bool DiscFamily(const System* system)
{
  return system && (system->id == SystemId::PS1 || system->id == SystemId::PSP || system->id == SystemId::DC ||
                    system->id == SystemId::SATURN);
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

#ifdef EMUSUNI_SATURN
// A Saturn disc's first sector: it begins with "SEGA SEGASATURN " (at 0 in a 2048-byte image, at 16 in a raw 2352-byte
// one). The disc is an .iso's, the first track of a .cue (its first FILE), the .mdf beside an .mds (Reader::Saturn); a
// .chd is compressed: its folder says (Roms/saturn).
bool SaturnSector(const std::string& image)
{
  FILE* file = fopen(image.c_str(), "rb");
  if (!file)
    return false;
  char b[32] = {};
  const size_t got = fread(b, 1, sizeof(b), file);
  fclose(file);
  static const char SATURN[] = "SEGA SEGASATURN ";
  return (got >= 16 && std::memcmp(b, SATURN, 16) == 0) || (got >= 32 && std::memcmp(b + 16, SATURN, 16) == 0);
}
#endif

// A CD's data track by what the file holds (00.81: a PlayStation .bin left without its .cue - players' games didn't
// show): data, an ISO 9660 volume descriptor ("\1CD001") in its sector 16 - at 16 * 2352 + 24 in a raw image (2352
// bytes a sector, each beginning with the sync; Mode 2's user data after its 24 bytes of sync, header and subheader;
// Mode 1's after 16), at 16 * 2048 in a cooked one (an ISO: the user data only); playstation, that descriptor's system
// name is "PLAYSTATION" (every PlayStation disc's). A cartridge, a BIOS or a CD's audio track has neither.
struct CdTrack
{
  bool data = false;
  bool playstation = false;
};
CdTrack ReadCdTrack(const std::string& path)
{
  CdTrack track;
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return track;
  static const uint8_t SYNC[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  uint8_t b[24] = {};
  auto read_at = [&](uint64_t offset, size_t size) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && fread(b, 1, size, file) == size;
  };
  const bool raw = read_at(0, 12) && std::memcmp(b, SYNC, 12) == 0;
  for (const uint64_t at : raw ? std::initializer_list<uint64_t>{16 * 2352 + 24, 16 * 2352 + 16} :
                                 std::initializer_list<uint64_t>{16 * 2048})
  {
    // The descriptor's type (1), "CD001", its version; the system name at 8.
    if (read_at(at, 6) && std::memcmp(b, "\1CD001", 6) == 0)
    {
      track.data = true;
      track.playstation = read_at(at + 8, 11) && std::memcmp(b, "PLAYSTATION", 11) == 0;
      break;
    }
  }
  fclose(file);
  return track;
}

// A CD track's number in a file's name as Redump writes it - "(Track 1)", "(Track 01)" - and the name without it
// (base). 0: none.
int TrackNumber(const std::string& name, std::string* base)
{
  const std::string lower = Lower(name);
  const size_t open = lower.rfind("(track ");
  if (open == std::string::npos)
    return 0;
  size_t at = open + 7;
  while (at < lower.size() && std::isdigit(static_cast<unsigned char>(lower[at])))
    at++;
  if (at == open + 7 || at - (open + 7) > 2 || at >= lower.size() || lower[at] != ')')
    return 0;
  const size_t from = open > 0 && name[open - 1] == ' ' ? open - 1 : open;
  *base = name.substr(0, from) + name.substr(at + 1);
  while (!base->empty() && base->back() == ' ')
    base->pop_back();
  return std::atoi(lower.c_str() + open + 7);
}

// ---- The read of the folders (00.85: big libraries) ----

// FileFacts::looked: what was looked at of the file.
constexpr unsigned LOOKED_ZIP = 1, LOOKED_CD = 2, LOOKED_DREAMCAST = 4, LOOKED_PSP = 8, LOOKED_NAOMI = 16,
                   LOOKED_LISTED = 32, LOOKED_SATURN = 64;

// One read of the folders (ScanAll's, Scan's, CountGames'): each folder's entries read once, each file's size and date
// asked once, and what is inside a file looked at once, whichever console asks - a folder shared by several consoles
// (the same one, or one inside another) was read once per console before 00.85, each .zip of it opened once per
// console: on a PS4, where each look at a USB drive costs about a millisecond, a drive whose top folder every console
// was given took most of an hour - and a file an earlier read looked inside (`before`: the menu's cache) isn't opened
// again while its size and date are the same. What this read learnt of the files goes to `after`. The games found are
// those the files themselves would give: the same answers, asked of the drive fewer times.
class Reader
{
public:
  Reader(Contents* before, Contents* after, ScanProgress* progress)
      : m_before(before), m_after(after ? after : &m_own), m_progress(progress)
  {
  }
  struct Entry
  {
    std::string name;  // its file name (its path: the folder's / name, as the folder's directory_iterator gives it)
    bool folder = false;
    bool link = false;  // a symbolic link: Exists asks the drive about it
  };
  struct Listing
  {
    std::vector<Entry> entries;  // in the folder's own order
    bool failed = false;         // it couldn't be read (to its end): Exists asks the drive
  };

  // A folder's entries, read the first time (as each loop over a directory_iterator did: up to the first error; a
  // folder that isn't there has none).
  const Listing& List(const std::string& folder)
  {
    const auto known = m_listings.find(folder);
    if (known != m_listings.end())
      return known->second;
    Listing listing;
    std::error_code ec;
    // (Not a range-for: its ++ throws when the drive goes away during the read; increment(ec) just stops.)
    std::filesystem::directory_iterator it(folder, ec), end;
    if (ec)
    {
      listing.failed = ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory;
    }
    else
    {
      for (; !ec && it != end; it.increment(ec))
      {
        Entry entry;
        entry.name = it->path().filename().string();
        std::error_code type_error;
        entry.folder = it->is_directory(type_error);
        entry.link = it->is_symlink(type_error);
        listing.entries.push_back(std::move(entry));
      }
      listing.failed = static_cast<bool>(ec);
    }
    return m_listings.emplace(folder, std::move(listing)).first->second;
  }

  // A file's size as std::filesystem::file_size gives it (a file that isn't a regular one, or isn't there: -1).
  uint64_t Size(const std::string& path)
  {
    const Stat& stat = StatOf(path);
    return stat.regular ? stat.size : static_cast<uint64_t>(-1);
  }

  // Does the file exist (as FileExists says)? Answered from its folder's entries when they say it surely: its name
  // there (not a link), or no name like it in any letters' case - a USB drive's FAT or exFAT finds "MSLUG.ZIP" for
  // mslug.zip, the PS4's own storage doesn't -; else (a name only in another case, a link, a folder that couldn't be
  // read) the drive is asked, once.
  bool Exists(const std::string& path)
  {
    const auto known = m_exists.find(path);
    if (known != m_exists.end())
      return known->second;
    bool exists = false, sure = false;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0 && slash + 1 < path.size())
    {
      const Listing& listing = List(path.substr(0, slash));
      const std::string name = path.substr(slash + 1), lower = Lower(name);
      bool like = false;
      for (const Entry& entry : listing.entries)
      {
        if (entry.name == name && !entry.link)
        {
          exists = sure = true;
          break;
        }
        like = like || Lower(entry.name) == lower;
      }
      sure = sure || (!like && !listing.failed);
    }
    if (!sure)
      exists = FileExists(path);
    m_exists.emplace(path, exists);
    return exists;
  }

  // What is inside a file, looked at once (or the earlier read's, the file the same).
  const System* ZipSystem(const std::string& path)
  {
    FileFacts* facts = Facts(path);
    if (!facts)
      return SystemOfZip(path);
    if (!(facts->looked & LOOKED_ZIP))
    {
      const System* system = SystemOfZip(path);
      facts->zip = system ? static_cast<int>(system->id) : -1;
      facts->looked |= LOOKED_ZIP;
    }
    const SystemId id = static_cast<SystemId>(facts->zip);
    return facts->zip >= 0 && facts->zip < SYSTEM_COUNT && HasSystem(id) ? &GetSystem(id) : nullptr;
  }
  CdTrack Track(const std::string& path)
  {
    FileFacts* facts = Facts(path);
    if (!facts)
      return ReadCdTrack(path);
    if (!(facts->looked & LOOKED_CD))
    {
      const CdTrack track = ReadCdTrack(path);
      facts->cd_data = track.data;
      facts->cd_playstation = track.playstation;
      facts->looked |= LOOKED_CD;
    }
    CdTrack track;
    track.data = facts->cd_data;
    track.playstation = facts->cd_playstation;
    return track;
  }
  // IsDreamcastDisc: a .gdi or .cdi by its name, a .cue or .chd by what it holds, an .m3u by its first disc (that
  // file's own fact), anything else isn't.
  bool Dreamcast(const std::string& path)
  {
    if (HasExtension(path, {".gdi", ".cdi"}))
      return true;
    if (HasExtension(path, {".m3u"}))
    {
      const auto known = m_dreamcast_lists.find(path);
      if (known != m_dreamcast_lists.end())
        return known->second;
      const std::vector<std::string>& discs = Listed(path);
      const bool dc = !discs.empty() && !HasExtension(discs.front(), {".m3u"}) && Dreamcast(discs.front());
      m_dreamcast_lists.emplace(path, dc);
      return dc;
    }
    if (!HasExtension(path, {".cue", ".chd"}))
      return false;
    FileFacts* facts = Facts(path);
    if (!facts)
      return IsDreamcastDisc(path);
    if (!(facts->looked & LOOKED_DREAMCAST))
    {
      facts->dreamcast = IsDreamcastDisc(path);
      facts->looked |= LOOKED_DREAMCAST;
    }
    return facts->dreamcast;
  }
  bool Psp(const std::string& path)
  {
    if (!HasExtension(path, {".iso", ".pbp", ".chd"}))
      return false;  // (IsPspDisc's answer for any other file)
    FileFacts* facts = Facts(path);
    if (!facts)
      return IsPspDisc(path);
    if (!(facts->looked & LOOKED_PSP))
    {
      facts->psp = IsPspDisc(path);
      facts->looked |= LOOKED_PSP;
    }
    return facts->psp;
  }
#ifdef EMUSUNI_SATURN
  // A Saturn disc (until 00.85 IsSaturnDisc): the first sector of an .iso, of a .cue's first track, of the .mdf beside
  // an .mds (each file's own fact: a track changed is looked at again).
  bool Saturn(const std::string& path)
  {
    std::string image = path;
    if (HasExtension(path, {".cue"}))
    {
      const std::vector<std::string>& tracks = Listed(path);
      if (tracks.empty())
        return false;
      image = tracks.front();
    }
    else if (HasExtension(path, {".mds"}))
    {
      image = std::filesystem::path(path).replace_extension(".mdf").string();
    }
    else if (!HasExtension(path, {".iso"}))
    {
      return false;
    }
    FileFacts* facts = Facts(image);
    if (!facts)
      return SaturnSector(image);
    if (!(facts->looked & LOOKED_SATURN))
    {
      facts->saturn = SaturnSector(image);
      facts->looked |= LOOKED_SATURN;
    }
    return facts->saturn;
  }
#endif
  bool NaomiImage(const std::string& path, Naomi::Image* image)
  {
    FileFacts* facts = Facts(path);
    if (!facts)
      return Naomi::ReadImage(path, image);
    if (!(facts->looked & LOOKED_NAOMI))
    {
      Naomi::Image read;
      facts->naomi = Naomi::ReadImage(path, &read);
      facts->naomi_japan = read.japan;
      facts->naomi_usa = read.usa;
      facts->naomi_export = read.export_title;
      facts->naomi_vertical = read.vertical;
      facts->looked |= LOOKED_NAOMI;
    }
    if (facts->naomi)
    {
      image->japan = facts->naomi_japan;
      image->usa = facts->naomi_usa;
      image->export_title = facts->naomi_export;
      image->vertical = facts->naomi_vertical;
    }
    return facts->naomi;
  }
  // ListedFiles(path), read once.
  const std::vector<std::string>& Listed(const std::string& path)
  {
    FileFacts* facts = Facts(path);
    if (!facts)
    {
      m_unkept_lists[path] = ListedFiles(path);
      return m_unkept_lists[path];
    }
    if (!(facts->looked & LOOKED_LISTED))
    {
      facts->listed = ListedFiles(path);
      facts->looked |= LOOKED_LISTED;
    }
    return facts->listed;
  }

  // Asked to stop (a game running meanwhile: waits until it ends)?
  bool Stopped()
  {
    if (!m_progress)
      return false;
    while (m_progress->pause.load() && !m_progress->stop.load())
      usleep(100 * 1000);
    return m_progress->stop.load();
  }
  void GameFound()
  {
    if (m_progress)
      m_progress->games++;
  }
  // The end of a read: the earlier read's facts of the files this one didn't reach kept (it stopped before its end:
  // they are still the same files, as far as anyone knows), else dropped (files gone); only the files looked inside
  // are kept.
  void Finish(bool stopped)
  {
    for (auto it = m_after->begin(); it != m_after->end();)
      it = it->second.looked == 0 ? m_after->erase(it) : std::next(it);
    if (stopped && m_before)
    {
      for (auto& [path, facts] : *m_before)
        m_after->emplace(path, std::move(facts));
    }
    if (m_before)
      m_before->clear();
  }

private:
  struct Stat
  {
    bool ok = false, regular = false;
    uint64_t size = 0;
    int64_t time = 0;
  };
  const Stat& StatOf(const std::string& path)
  {
    const auto known = m_stats.find(path);
    if (known != m_stats.end())
      return known->second;
    Stat stat;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
    {
      stat.ok = true;
      stat.regular = S_ISREG(st.st_mode);
      stat.size = static_cast<uint64_t>(st.st_size);
      stat.time = static_cast<int64_t>(st.st_mtime);
    }
    return m_stats.emplace(path, stat).first->second;
  }
  // The file's facts in this read (the earlier read's when the file has the same size and date); nullptr when the
  // drive can't say its size (then each question goes to the file itself, as before).
  FileFacts* Facts(const std::string& path)
  {
    const auto known = m_after->find(path);
    if (known != m_after->end())
      return &known->second;
    const Stat& stat = StatOf(path);
    if (!stat.ok)
      return nullptr;
    FileFacts facts;
    if (m_before)
    {
      const auto old = m_before->find(path);
      if (old != m_before->end())
      {
        if (old->second.size == stat.size && old->second.time == stat.time)
          facts = std::move(old->second);
        m_before->erase(old);
      }
    }
    facts.size = stat.size;
    facts.time = stat.time;
    return &m_after->emplace(path, std::move(facts)).first->second;
  }

  Contents* m_before;
  Contents m_own;
  Contents* m_after;
  ScanProgress* m_progress;
  std::unordered_map<std::string, Listing> m_listings;
  std::unordered_map<std::string, Stat> m_stats;
  std::unordered_map<std::string, bool> m_exists, m_dreamcast_lists;
  std::unordered_map<std::string, std::vector<std::string>> m_unkept_lists;
};

// The files the .cue files in a folder name (their tracks), in lower case: the .bin files that aren't games of their
// own. (A PlayStation .bin is a game only when no .cue names it: 00.81.)
std::set<std::string> CueTracks(Reader& reader, const std::filesystem::path& folder)
{
  std::set<std::string> tracks;
  for (const Reader::Entry& entry : reader.List(folder.string()).entries)
  {
    const std::string path = (folder / entry.name).string();
    if (!HasExtension(path, {".cue"}))
      continue;
    for (const std::string& track : reader.Listed(path))
      tracks.insert(Lower(std::filesystem::path(track).lexically_normal().string()));
  }
  return tracks;
}

// Is the file one of the console's games (a ROM, or a .zip with one inside; without open_zips,
// any .zip)? Without open_zips (the console's folder is its own) a Game Boy game in the Game Boy
// Color's folder is the Game Boy Color's, and the other way round: the folder says where it goes.
// So for a .iso, .chd or .pbp in the PlayStation's or the PSP's own folder; in a shared folder
// what is inside it says.
// (00.85: what is inside a file through the read's Reader, looked at once whichever console asks.)
bool IsGameOf(Reader& reader, const std::filesystem::path& path, const System& system, bool open_zips = true)
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
      return reader.NaomiImage(path.string(), &image);
    }
    if (!HasExtension(name, {".zip", ".7z"}))
      return false;
    const Naomi::Set* set = Naomi::Find(path.stem().string());
    return set && Naomi::Plays(system.id, *set);
  }
  if (HasExtension(name, {".zip"}))
    return !system.by_path && (!open_zips || reader.ZipSystem(path.string()) == &system);  // no zipped CDs
  // A PlayStation disc's .bin without its .cue (00.81: two players' games didn't show; the core reads it as one data
  // track, its sector size from what it holds): a CD's data track; in a folder another console shares, a PlayStation
  // disc's (its volume's system name). Not a BIOS, a CD's audio track or a cartridge. (One a .cue names is that
  // .cue's track: ScanFolder leaves it out before this.)
  if (system.id == SystemId::PS1 && HasExtension(name, {".bin"}))
  {
    const CdTrack track = reader.Track(path.string());
    return track.data && (!open_zips || track.playstation);
  }
  // (00.82) A PC Engine CD System Card (syscard3.pce, gexpress.pce, a "[BIOS] ..." one) is no HuCard game (00.82's
  // players were told to put theirs beside the HuCards, in Roms/pce; the PC Engine CD is gone in 00.83).
  if (system.id == SystemId::PCE)
  {
    const std::string lower = Lower(name);
    if (lower.rfind("syscard", 0) == 0 || lower.rfind("gexpress", 0) == 0 || lower.find("[bios]") != std::string::npos)
      return false;
  }
#ifdef EMUSUNI_A2600
  // (00.90) A .bin is a Genesis cartridge's file and an Atari 2600 cartridge's (AtariAge's sets and most others): the
  // 2600's are 64 KiB or less (a few homebrews more: their .a26), the Genesis's 128 KiB or more - its size says whose.
  if ((system.id == SystemId::A2600 || system.id == SystemId::MD) && HasExtension(name, {".bin"}))
  {
    const uint64_t size = reader.Size(path.string());
    const bool atari = size > 0 && size <= A2600_BIN_MAX;
    if (system.id == SystemId::A2600)
      return atari;
    if (atari)
      return false;
  }
#endif
  // ...and a CD's data track is no Genesis cartridge in a folder they share with the PlayStation.
  if (system.id == SystemId::MD && open_zips && HasSystem(SystemId::PS1) &&
      HasExtension(name, {".bin"}) && reader.Track(path.string()).data)
    return false;
  const System* of = SystemOfFile(name);
  if (DiscFamily(of) && DiscFamily(&system) && HasOwnExtension(system, name))
  {
    // A disc image's extension of two or more of the PlayStation, the PSP, the Dreamcast and the Saturn: in a
    // console's own folder it is that console's; in a shared one what is inside it says.
    int owners = 0;
    for (SystemId id : {SystemId::PS1, SystemId::PSP, SystemId::DC, SystemId::SATURN})
      owners += HasSystem(id) && HasOwnExtension(GetSystem(id), name) ? 1 : 0;
    if (owners < 2 || !open_zips)
      return true;
    SystemId disc = HasSystem(SystemId::DC) && reader.Dreamcast(path.string()) ? SystemId::DC :
                    HasSystem(SystemId::PSP) && reader.Psp(path.string())      ? SystemId::PSP :
                                                                                 SystemId::PS1;
#ifdef EMUSUNI_SATURN
    if (disc == SystemId::PS1 && reader.Saturn(path.string()))
      disc = SystemId::SATURN;
#endif
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
void ArcadeGame(Reader& reader, Game* game, const std::string& folder)
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
  game->missing = Arcade::Missing(*driver, folder, SystemFolder(SystemId::ARCADE),
                                  [&reader](const std::string& path) { return reader.Exists(path); });
}

// A NAOMI or Atomiswave game from Flycast's set (src/Naomi.h), as an arcade game: the set's name in lower case is
// its id, its full name its title (a clone's with its parent's), the region what the name says, no tags; its
// covers by the full name, then the parent's; what it needs that isn't there (its BIOS's zip, a clone's parent, a
// GD-ROM game's disc image) in the places the core looks.
void BoardGame(Reader& reader, Game* game, const std::string& folder)
{
  const auto exists = [&reader](const std::string& path) { return reader.Exists(path); };
  // A NAOMI cartridge image (.dat, .bin): its file's name is its id, as a console's ROM's; its title its set's name
  // when the file's or the header's names one (Naomi::MatchImage), else the file's (or the header's), with the
  // file's own region and tags; its covers by the file's name (libretro's names for these dumps), then the set's and
  // its parent's; it needs naomi.zip.
  if (Naomi::IsImageName(game->path))
  {
    Naomi::Image image;
    reader.NaomiImage(game->path, &image);
    const Naomi::Set* set = Naomi::MatchImage(game->title, image);
    game->cover_names = {Arcade::CoverName(std::filesystem::path(game->path).stem().string())};
    if (set)
    {
      game->cover_names.push_back(Arcade::CoverName(set->title));
      if (const Naomi::Set* parent = Naomi::Parent(*set))
        game->cover_names.push_back(Arcade::CoverName(parent->title));
    }
    game->title = Naomi::ImageTitle(game->title, image, set);
    game->missing = Naomi::ImageMissing(folder, SystemFolder(game->system), exists);
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
  game->missing = Naomi::Missing(*set, folder, SystemFolder(game->system), exists);
}

// The name a file's folder gives it - the folder's, or the game's folder above a folder per disc named
// only so ("Shenmue/Disc 1/disc.gdi") - and its disc (from the file's name or that folder's; 0: none).
void FolderName(const std::filesystem::path& folder, int depth, int stem_disc, std::string* owner, int* disc)
{
  const int folder_disc = depth > 1 ? BareDiscLabel(folder.filename().string()) : 0;
  *owner = folder_disc > 0 ? folder.parent_path().filename().string() : folder.filename().string();
  *disc = stem_disc > 0 ? stem_disc : folder_disc;
}

void ScanFolder(Reader& reader, const std::filesystem::path& folder, int depth, const System& system, bool open_zips,
                std::vector<Game>* games)
{
  const size_t first = games->size();
  // A CD's .cue or .gdi here: its tracks aren't cartridges (below), for a console of .bin files.
  const bool bin = !system.by_path && HasOwnExtension(system, ".bin");
  std::vector<std::string> lists;
  // The PlayStation's: the .bin files the .cue files here name, which aren't games of their own (00.81: a .bin no .cue
  // names is one). Without reading a .cue for the usual names - "Name.bin" or "Name (Track N).bin" beside "Name.cue"
  // (the folder's .cue names, listed at its first .bin) -; the .cue files read only for another .bin (once).
  bool cue_names_listed = false, cue_tracks_read = false;
  std::set<std::string> cue_names, cue_tracks;
  // (00.85: the folder's entries as the read has them - read once, whichever console comes -; the drive going away
  // during the read ends them, as it ended the directory_iterator.)
  const Reader::Listing& listing = reader.List(folder.string());
  for (const Reader::Entry& entry : listing.entries)
  {
    if (reader.Stopped())
      return;
    const std::string& name = entry.name;
    if (name.empty() || name[0] == '.' || name == "System Volume Information" ||
        name == "$RECYCLE.BIN" ||
        strcasecmp(name.c_str(), PORTABLE_FOLDER) == 0)  // (00.87: the portable data's folder, src/Portable.h)
    {
      continue;
    }
    const std::filesystem::path path = folder / name;
    if (entry.folder)
    {
      if (depth < 4)
        ScanFolder(reader, path, depth + 1, system, open_zips, games);
      continue;
    }
    if (bin && HasExtension(name, {".cue", ".gdi"}))
      lists.push_back(path.string());
    if (system.id == SystemId::PS1 && HasExtension(name, {".bin"}))
    {
      if (!cue_names_listed)
      {
        for (const Reader::Entry& cue : listing.entries)
        {
          if (HasExtension(cue.name, {".cue"}))
            cue_names.insert(Lower(std::filesystem::path(cue.name).stem().string()));
        }
        cue_names_listed = true;
      }
      const std::string stem = Lower(path.stem().string());
      std::string untracked;
      if (cue_names.count(stem) > 0 || (TrackNumber(stem, &untracked) > 0 && cue_names.count(untracked) > 0))
        continue;
      if (!cue_tracks_read && !cue_names.empty())
        cue_tracks = CueTracks(reader, folder);
      cue_tracks_read = true;
      if (cue_tracks.count(Lower(path.lexically_normal().string())) > 0)
        continue;
    }
    if (!IsGameOf(reader, path, system, open_zips))
      continue;
    Game game;
    game.path = path.string();
    game.id = path.stem().string();
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
    // A PlayStation disc's first track without its .cue (00.81): named as the disc ("Tekken 3 (USA)", not "... (Track
    // 1)"): the same key, saves and cover as with its .cue.
    std::string untracked;
    if (system.id == SystemId::PS1 && HasExtension(name, {".bin"}) && TrackNumber(game.id, &untracked) == 1 &&
        !untracked.empty())
      game.id = untracked;
    game.system = system.id;
    game.key = system.prefix + game.id;
    game.size = reader.Size(game.path);
    ParseName(game.id, &game.title, &game.region, &game.tags);
    if (system.id == SystemId::ARCADE)
      ArcadeGame(reader, &game, folder.string());
    else if (IsFlycastBoard(system.id))
      BoardGame(reader, &game, folder.string());
    games->push_back(std::move(game));
    reader.GameFound();
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
      for (const std::string& track : reader.Listed(list))
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
    for (const std::string& disc : reader.Listed((*games)[i].path))
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
    // (A PlayStation disc's first track without its .cue, "X (Disc 1) (Track 1).bin": the game is "X".)
    std::string untracked;
    if (number > 0 && system.id == SystemId::PS1 && HasExtension(path.string(), {".bin"}) &&
        TrackNumber(base, &untracked) == 1 && !untracked.empty())
      base = untracked;
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
  // A PlayStation disc's tracks without their .cue (00.81): the other tracks beside its first one are its files too.
  std::vector<std::string> start = own;
  if (game.system == SystemId::PS1)
  {
    for (const std::string& file : own)
    {
      const std::vector<std::string> tracks = LoneTracks(file);
      for (size_t i = 1; i < tracks.size(); i++)
        start.push_back(std::filesystem::path(tracks[i]).lexically_normal().string());
    }
  }
  const std::vector<std::string> reach = NamedFiles(start);
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

std::vector<std::string> LoneTracks(const std::string& path)
{
  const std::filesystem::path file(path);
  std::string base;
  if (!HasExtension(path, {".bin"}) || TrackNumber(file.stem().string(), &base) != 1 || base.empty())
    return {};
  // The folder's files by their names in lower case (the USB drive's exFAT doesn't tell the letters' case apart).
  std::map<std::string, std::string> files;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(file.parent_path(), ec), end; !ec && it != end; it.increment(ec))
    files[Lower(it->path().filename().string())] = it->path().string();
  const bool two_digits = Lower(file.stem().string()).find("(track 01)") != std::string::npos;
  std::vector<std::string> tracks = {path};
  for (int number = 2; number < 100; number++)
  {
    char text[8];
    std::snprintf(text, sizeof(text), two_digits ? "%02d" : "%d", number);
    const auto found = files.find(Lower(base + " (Track " + text + ").bin"));
    if (found == files.end())
      break;
    tracks.push_back(found->second);
  }
  if (tracks.size() < 2)
    tracks.clear();
  return tracks;
}

namespace
{
// One folder of one console (Scan's work) with the read's Reader: by title, then by file name. (00.85: the titles in
// lower case made once each, not twice per comparison - the same comparisons, so the same order.)
std::vector<Game> ScanWith(Reader& reader, const std::string& folder, SystemId system, bool open_zips)
{
  std::vector<Game> games;
  ScanFolder(reader, folder, 0, GetSystem(system), open_zips, &games);
  if (system == SystemId::PS1 || system == SystemId::DC)
    GroupDiscs(GetSystem(system), folder, &games);
  // (What is sorted holds a string, as a Game does: libc++'s std::sort works the same way on both - it sorts longer
  // runs by insertion for a type cheap to copy -, so two games alike in both, one title and one file name found in two
  // folders, come out in the order sorting the games themselves gave them.)
  struct Keyed
  {
    std::string lower;
    size_t index;
  };
  std::vector<Keyed> order(games.size());
  for (size_t i = 0; i < games.size(); i++)
    order[i] = {Lower(games[i].title), i};
  std::sort(order.begin(), order.end(), [&games](const Keyed& a, const Keyed& b) {
    return a.lower != b.lower ? a.lower < b.lower : games[a.index].id < games[b.index].id;
  });
  std::vector<Game> sorted;
  sorted.reserve(games.size());
  for (const Keyed& keyed : order)
    sorted.push_back(std::move(games[keyed.index]));
  return sorted;
}
}  // namespace

std::vector<Game> Scan(const std::string& folder, SystemId system, bool open_zips)
{
  Reader reader(nullptr, nullptr, nullptr);
  return ScanWith(reader, folder, system, open_zips);
}

int CountGames(const std::string& folder, SystemId system)
{
  int count = 0;
  Reader reader(nullptr, nullptr, nullptr);
  // (A PlayStation .bin a .cue here names is that .cue's track, not a game: 00.81.)
  const std::set<std::string> cue_tracks =
      system == SystemId::PS1 ? CueTracks(reader, std::filesystem::path(folder)) : std::set<std::string>();
  for (const Reader::Entry& entry : reader.List(folder).entries)
  {
    const std::filesystem::path path = std::filesystem::path(folder) / entry.name;
    const System& console = GetSystem(system);
    if (cue_tracks.count(Lower(path.lexically_normal().string())) > 0)
      continue;
    if (!entry.folder &&
        (IsGameOf(reader, path, console) ||
         (GameBoyFamily(&console) && GameBoyFamily(SystemOfFile(entry.name)))))
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

namespace
{
std::array<std::string, SYSTEM_COUNT> s_system_folders;  // (00.87) SetSystemFolder's ("": data_dir/system)
}

void SetSystemFolder(SystemId system, const std::string& folder)
{
  const size_t id = static_cast<size_t>(system);
  if (id < s_system_folders.size())
    s_system_folders[id] = folder;
}

std::string SystemFolder(SystemId system)
{
  const size_t id = static_cast<size_t>(system);
  return id < s_system_folders.size() && !s_system_folders[id].empty() ? s_system_folders[id] :
                                                                         std::string(App::DATA_DIR) + "/system";
}

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

Found ScanAll(const std::array<std::string, SYSTEM_COUNT>& chosen, const Places& places, ScanProgress* progress,
              Contents* contents)
{
  // (00.85) One Reader for every console: a folder two consoles share is read once, what is inside a file looked at
  // once; the contents an earlier read learnt come in, this read's go out.
  Contents before;
  if (contents)
    before.swap(*contents);
  Reader reader(&before, contents, progress);
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
      found.stopped = found.stopped || reader.Stopped();
      if (found.stopped)
        break;  // (the games found are dropped: no folder read more)
      int here = 0;
      for (Game& game : ScanWith(reader, found.folders[id][f], system.id, shared(id, keys[id][f])))
      {
        if (!seen.insert(PathKey(game.path, places.drives)).second)
          continue;  // (a folder inside another one of the console's)
        found.games.push_back(std::move(game));
        here++;
      }
      found.found_in[id].push_back(here);
      found.counts[id] += here;
      found.stopped = found.stopped || reader.Stopped();
    }
  }
  if (contents)
    reader.Finish(found.stopped);
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

// ---- The library's cache (00.85) ----

namespace
{
// The file's own layout; FACTS_FORMAT: what FileFacts' fields mean (a change of how a file is looked inside: more).
constexpr int CACHE_FORMAT = 1, FACTS_FORMAT = 1;

// A text as the file has it: a tab, a line's end, a backslash and the lists' separator (0x1F) escaped.
void PutEscaped(std::string* line, const std::string& text)
{
  for (const char c : text)
  {
    switch (c)
    {
    case '\\': *line += "\\\\"; break;
    case '\t': *line += "\\t"; break;
    case '\n': *line += "\\n"; break;
    case '\r': *line += "\\r"; break;
    case '\x1f': *line += "\\u"; break;
    default: line->push_back(c);
    }
  }
}

void PutField(std::string* line, const std::string& text)
{
  line->push_back('\t');
  PutEscaped(line, text);
}

// A list in one field: how many, then each item escaped after a 0x1F (the only ones there: an item's own is escaped).
void PutList(std::string* line, const std::vector<std::string>& list)
{
  *line += "\t" + std::to_string(list.size());
  for (const std::string& item : list)
  {
    line->push_back('\x1f');
    PutEscaped(line, item);
  }
}

// A line's fields as the file has them (escaped: the tabs left are the fields' separators).
std::vector<std::string> FieldsOf(const std::string& line)
{
  std::vector<std::string> fields;
  size_t start = 0;
  for (;;)
  {
    const size_t tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos)
      break;
    start = tab + 1;
  }
  return fields;
}

std::string TextOf(const std::string& field)
{
  std::string text;
  text.reserve(field.size());
  for (size_t i = 0; i < field.size(); i++)
  {
    if (field[i] != '\\' || i + 1 >= field.size())
    {
      text.push_back(field[i]);
      continue;
    }
    const char next = field[++i];
    text.push_back(next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : next == 'u' ? '\x1f' : next);
  }
  return text;
}

// PutList's field back (false: not one of its fields).
bool ListOf(const std::string& field, std::vector<std::string>* list)
{
  list->clear();
  size_t at = field.find('\x1f');
  const size_t count = std::strtoul(field.substr(0, at).c_str(), nullptr, 10);
  while (at != std::string::npos)
  {
    const size_t next = field.find('\x1f', at + 1);
    list->push_back(TextOf(field.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1)));
    at = next;
  }
  return list->size() == count;
}

// FNV-1a, 64 bits.
void Mix(uint64_t* sum, const std::string& text)
{
  for (const char c : text)
  {
    *sum ^= static_cast<unsigned char>(c);
    *sum *= 0x100000001B3ull;
  }
  *sum ^= 0xFF;  // (between fields: "ab"+"c" isn't "a"+"bc")
  *sum *= 0x100000001B3ull;
}

// The consoles' table as far as a file's facts go (each console's key, extensions and way of reading its games): the
// facts of another table (another version that added a console or an extension) aren't taken.
std::string FactsSignature()
{
  uint64_t sum = 0xCBF29CE484222325ull;
  Mix(&sum, std::to_string(FACTS_FORMAT));
  for (const System& system : Systems())
  {
    Mix(&sum, std::to_string(static_cast<int>(system.id)) + system.key + (system.by_path ? "p" : "c"));
    for (const std::string& extension : system.extensions)
      Mix(&sum, extension);
  }
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(sum));
  return text;
}
}  // namespace

uint64_t Fingerprint(const Found& found)
{
  uint64_t sum = 0xCBF29CE484222325ull;
  for (const Game& game : found.games)
  {
    Mix(&sum, std::to_string(static_cast<int>(game.system)));
    for (const std::string* field : {&game.path, &game.id, &game.title, &game.region, &game.tags, &game.key})
      Mix(&sum, *field);
    Mix(&sum, std::to_string(game.size) + (game.custom_title ? "+" : "-"));
    for (const std::vector<std::string>* list : {&game.discs, &game.cover_names, &game.missing})
    {
      Mix(&sum, std::to_string(list->size()));
      for (const std::string& item : *list)
        Mix(&sum, item);
    }
  }
  for (size_t id = 0; id < found.folders.size(); id++)
  {
    for (size_t f = 0; f < found.folders[id].size(); f++)
      Mix(&sum, std::to_string(id) + ":" + found.folders[id][f] + ":" +
                    (f < found.found_in[id].size() ? std::to_string(found.found_in[id][f]) : std::string("-")));
  }
  return sum;
}

bool SaveCache(const std::string& path, const std::string& version, const std::array<std::string, SYSTEM_COUNT>& chosen,
               const Places& places, const Found& found, const Contents& contents, const std::string& drive,
               const std::string& writer)
{
  // (00.87) A path on the portable data's drive, relative to it (src/DrivePaths.h); as it is without one.
  auto stored = [&drive](const std::string& text) { return DrivePaths::Stored(text, drive); };
  auto stored_list = [&](const std::vector<std::string>& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const std::string& item : list)
      out.push_back(stored(item));
    return out;
  };
  // Line by line into a temporary file (no copy of it all in memory: 30 000 games are several MB), then in place.
  const std::string temp = path + ".tmp";
  FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file)
    return false;
  bool ok = true;
  std::string line;
  auto put = [&]() {
    line.push_back('\n');
    ok = ok && std::fwrite(line.data(), 1, line.size(), file) == line.size();
    line.clear();
  };
  line = "# EmuSuNi: the games as the last read found them, shown at the next start while it reads them again "
         "(made by the app; deleting it only makes that start read them first)";
  put();
  line = "V\t" + std::to_string(CACHE_FORMAT);
  PutField(&line, version);
  PutField(&line, FactsSignature());
  put();
  if (!writer.empty())
  {
    line = "W";
    PutField(&line, writer);
    put();
  }
  for (const System& system : Systems())
  {
    line = std::string("C\t") + system.key;
    PutField(&line, chosen[static_cast<size_t>(system.id)]);  // (00.87: as the menu gives them - Menu.cpp's StoredChosen)
    put();
  }
  for (const std::string& each : places.drives)
  {
    line = "D";
    PutField(&line, stored(each));
    put();
  }
  for (const auto& [key, folders] : places.folders)
  {
    for (const std::string& folder : folders)
    {
      line = "P";
      PutField(&line, key);
      PutField(&line, stored(folder));
      put();
    }
  }
  for (const System& system : Systems())
  {
    const size_t id = static_cast<size_t>(system.id);
    for (size_t f = 0; f < found.folders[id].size() && f < found.found_in[id].size(); f++)
    {
      line = std::string("L\t") + system.key;
      PutField(&line, stored(found.folders[id][f]));
      line += "\t" + std::to_string(found.found_in[id][f]);
      put();
    }
  }
  for (const Game& game : found.games)
  {
    line = std::string("G\t") + GetSystem(game.system).key;
    PutField(&line, stored(game.path));
    for (const std::string* field : {&game.id, &game.title, &game.region, &game.tags})
      PutField(&line, *field);
    line += "\t" + std::to_string(game.size);
    PutField(&line, game.key);
    line += game.custom_title ? "\t1" : "\t0";
    PutList(&line, drive.empty() ? game.discs : stored_list(game.discs));
    PutList(&line, game.cover_names);
    PutList(&line, game.missing);
    put();
  }
  for (const auto& [file_path, facts] : contents)
  {
    line = "F";
    PutField(&line, stored(file_path));
    line += "\t" + std::to_string(facts.size) + "\t" + std::to_string(facts.time) + "\t" + std::to_string(facts.looked) +
            "\t" + std::to_string(facts.zip) + "\t" + (facts.cd_data ? "d" : "-") + (facts.cd_playstation ? "p" : "-") +
            (facts.dreamcast ? "g" : "-") + (facts.psp ? "u" : "-") + (facts.naomi ? "n" : "-") +
            (facts.naomi_vertical ? "v" : "-") + (facts.saturn ? "s" : "-");
    PutField(&line, facts.naomi_japan);
    PutField(&line, facts.naomi_usa);
    PutField(&line, facts.naomi_export);
    PutList(&line, drive.empty() ? facts.listed : stored_list(facts.listed));
    put();
  }
  // The end: a file cut short (no end, other counts) is no cache.
  line = "E\t" + std::to_string(found.games.size()) + "\t" + std::to_string(contents.size());
  put();
  ok = std::fflush(file) == 0 && ok;
  fsync(fileno(file));
  ok = std::fclose(file) == 0 && ok;
  if (!ok || std::rename(temp.c_str(), path.c_str()) != 0)
  {
    std::remove(temp.c_str());
    return false;
  }
  return true;
}

bool LoadCache(const std::string& path, Cache* cache, const std::string& drive)
{
  // (00.87) A path relative to the portable data's drive (src/DrivePaths.h): on the drive as it is now.
  auto path_of = [&drive](const std::string& field) { return DrivePaths::Resolved(TextOf(field), drive); };
  auto resolve = [&drive](std::vector<std::string>* list) {
    if (!drive.empty())
    {
      for (std::string& item : *list)
        item = DrivePaths::Resolved(item, drive);
    }
  };
  *cache = Cache();
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  bool versioned = false, ended = false;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> f = FieldsOf(line);
    const std::string& type = f[0];
    if (!versioned)
    {
      // The first line: the layout this reads, the version that made it, its consoles' table.
      if (type != "V" || f.size() < 4 || std::atoi(f[1].c_str()) != CACHE_FORMAT)
        return false;
      cache->version = TextOf(f[2]);
      cache->facts_ok = f[3] == FactsSignature();
      versioned = true;
      continue;
    }
    if (ended)
      return false;  // (something after the end: not this app's file)
    if (type == "C" && f.size() >= 3)
    {
      if (const System* system = SystemByKey(f[1]))
        cache->chosen[static_cast<size_t>(system->id)] = TextOf(f[2]);  // (as written: compared as they are)
    }
    else if (type == "W" && f.size() >= 2)
    {
      cache->writer = TextOf(f[1]);  // (00.87)
    }
    else if (type == "D" && f.size() >= 2)
    {
      cache->places.drives.push_back(path_of(f[1]));
    }
    else if (type == "P" && f.size() >= 3)
    {
      cache->places.folders[TextOf(f[1])].push_back(path_of(f[2]));
    }
    else if (type == "L" && f.size() >= 4)
    {
      const System* system = SystemByKey(f[1]);
      if (!system)
        return false;
      const size_t id = static_cast<size_t>(system->id);
      const int found_in = std::atoi(f[3].c_str());
      cache->found.folders[id].push_back(path_of(f[2]));
      cache->found.found_in[id].push_back(found_in);
      cache->found.counts[id] += found_in;
    }
    else if (type == "G" && f.size() >= 13)
    {
      const System* system = SystemByKey(f[1]);
      if (!system)
        return false;  // (a console this build hasn't: another build's file)
      Game game;
      game.system = system->id;
      game.path = path_of(f[2]);
      game.id = TextOf(f[3]);
      game.title = TextOf(f[4]);
      game.region = TextOf(f[5]);
      game.tags = TextOf(f[6]);
      game.size = std::strtoull(f[7].c_str(), nullptr, 10);
      game.key = TextOf(f[8]);
      game.custom_title = f[9] == "1";
      if (!ListOf(f[10], &game.discs) || !ListOf(f[11], &game.cover_names) || !ListOf(f[12], &game.missing))
        return false;
      resolve(&game.discs);
      cache->found.games.push_back(std::move(game));
    }
    else if (type == "F" && f.size() >= 11)
    {
      if (!cache->facts_ok)
        continue;
      FileFacts facts;
      facts.size = std::strtoull(f[2].c_str(), nullptr, 10);
      facts.time = std::strtoll(f[3].c_str(), nullptr, 10);
      facts.looked = static_cast<unsigned>(std::strtoul(f[4].c_str(), nullptr, 10));
      facts.zip = std::atoi(f[5].c_str());
      const std::string& flags = f[6];
      auto flag = [&flags](size_t at, char c) { return at < flags.size() && flags[at] == c; };
      facts.cd_data = flag(0, 'd');
      facts.cd_playstation = flag(1, 'p');
      facts.dreamcast = flag(2, 'g');
      facts.psp = flag(3, 'u');
      facts.naomi = flag(4, 'n');
      facts.naomi_vertical = flag(5, 'v');
      facts.saturn = flag(6, 's');
      facts.naomi_japan = TextOf(f[7]);
      facts.naomi_usa = TextOf(f[8]);
      facts.naomi_export = TextOf(f[9]);
      if (!ListOf(f[10], &facts.listed))
        return false;
      resolve(&facts.listed);
      cache->contents[path_of(f[1])] = std::move(facts);
    }
    else if (type == "E" && f.size() >= 3)
    {
      ended = std::strtoull(f[1].c_str(), nullptr, 10) == cache->found.games.size() &&
              (!cache->facts_ok || std::strtoull(f[2].c_str(), nullptr, 10) == cache->contents.size());
      if (!ended)
        return false;
    }
    else
    {
      return false;  // (a line this version doesn't know: no cache rather than a part of one)
    }
  }
  return versioned && ended;
}
}  // namespace Library
