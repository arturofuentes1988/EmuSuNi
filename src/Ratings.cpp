// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Ratings.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <set>

#include <minizip/unzip.h>
#include <orbis/libkernel.h>

#include "Arcade.h"
#include "CoverDownload.h"
#include "Log.h"
#include "Naomi.h"
#include "Net.h"
#include "Systems.h"

namespace Ratings
{
namespace
{
void Log(const std::string& text)
{
  LogLine("ratings: " + text);
}

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// LaunchBox's platforms of each console (scripts/make-ratings.py has the same table): the console's own first.
struct ConsolePlatforms
{
  SystemId system;
  std::vector<std::string> platforms;
};
const std::vector<ConsolePlatforms>& Table()
{
  static const std::vector<ConsolePlatforms> TABLE = {
      {SystemId::SNES, {"Super Nintendo Entertainment System", "Nintendo Satellaview"}},
      {SystemId::NES, {"Nintendo Entertainment System", "Nintendo Famicom Disk System"}},
      {SystemId::N64, {"Nintendo 64"}},
      {SystemId::GB, {"Nintendo Game Boy"}},
      {SystemId::GBC, {"Nintendo Game Boy Color"}},
      {SystemId::GBA, {"Nintendo Game Boy Advance"}},
      {SystemId::NDS, {"Nintendo DS"}},
      {SystemId::THREEDS, {"Nintendo 3DS"}},
      {SystemId::PS1, {"Sony Playstation"}},
      {SystemId::PSP, {"Sony PSP", "Sony PSP Minis"}},
      {SystemId::DC, {"Sega Dreamcast"}},
      {SystemId::SATURN, {"Sega Saturn"}},
      {SystemId::SG1000, {"Sega SG-1000"}},
      {SystemId::SMS, {"Sega Master System"}},
      {SystemId::GG, {"Sega Game Gear"}},
      {SystemId::MD, {"Sega Genesis"}},
      // (00.82: LaunchBox has one platform for the PC Engine and the TurboGrafx-16.)
      {SystemId::PCE, {"NEC TurboGrafx-16"}},
      {SystemId::ARCADE, {"Arcade", "SNK Neo Geo MVS", "SNK Neo Geo AES"}},
      {SystemId::NAOMI, {"Sega Naomi", "Sega Naomi 2", "Arcade"}},
      {SystemId::ATOMISWAVE, {"Sammy Atomiswave", "Arcade"}},
  };
  return TABLE;
}

const std::vector<std::string>& PlatformsOf(SystemId system)
{
  static const std::vector<std::string> NONE;
  for (const ConsolePlatforms& entry : Table())
  {
    if (entry.system == system)
      return entry.platforms;
  }
  return NONE;
}

// The platforms a copy is made for, in the table's order, each once ("Super Nintendo Entertainment System|...").
std::string PlatformsLine()
{
  std::vector<std::string> all;
  for (const ConsolePlatforms& entry : Table())
  {
    for (const std::string& platform : entry.platforms)
    {
      if (std::find(all.begin(), all.end(), platform) == all.end())
        all.push_back(platform);
    }
  }
  std::string line;
  for (const std::string& platform : all)
    line += (line.empty() ? "" : "|") + platform;
  return line;
}

bool WantedPlatform(const std::string& platform)
{
  static const std::set<std::string> WANTED = [] {
    std::set<std::string> wanted;
    for (const ConsolePlatforms& entry : Table())
      wanted.insert(entry.platforms.begin(), entry.platforms.end());
    return wanted;
  }();
  return WANTED.count(platform) > 0;
}

// The Game Boy's and the Game Boy Color's games are matched against both consoles' names at once: a Game Boy game in
// the Game Boy Color's folder plays there (Pokemon Yellow, "CGB+SGB Enhanced"), and among the Game Boy Color's names
// alone a hack of it would be the closest ("Pokemon Jade Version - Special Pikachu Edition").
std::vector<SystemId> Family(SystemId system)
{
  if (system == SystemId::GB || system == SystemId::GBC)
    return {SystemId::GB, SystemId::GBC};
  return {system};
}

// LaunchBox's names of a console's games count when this many players rated the game: the matching takes a name
// that begins like another (a hack "Pokemon Negro Absoluto" for "Pokemon Negro"), and one vote is one player's taste.
constexpr int EXTRA_VOTES = 3;
constexpr int64_t MAX_AGE = 60LL * 24 * 3600;  // the copy is downloaded again after 60 days

int HalfStars(double rating)
{
  return std::clamp(static_cast<int>(rating * 2.0 + 0.5), 0, 10);
}

// The copy (CopyPath): each game's platform, rating and names (its own first), by its LaunchBox id.
struct CopyGame
{
  std::string platform;
  Rating rating;
  std::vector<std::string> names;
};
struct CopyData
{
  std::vector<CopyGame> games;  // in the copy's order (LaunchBox's)
  std::unordered_map<uint32_t, size_t> by_id;
};

void ReadCopyData(const std::string& path, CopyData* copy)
{
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    std::vector<std::string> fields;
    for (size_t start = 0;;)
    {
      const size_t tab = line.find('\t', start);
      fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
      if (tab == std::string::npos)
        break;
      start = tab + 1;
    }
    if (fields.size() < 5)
      continue;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(fields[0].c_str(), nullptr, 10));
    CopyGame game;
    game.platform = fields[1];
    game.rating.half_stars = std::atoi(fields[2].c_str());
    game.rating.votes = std::max(0, std::atoi(fields[3].c_str()));
    // (00.83: a copy of 00.80 to 00.82 also has the games of the consoles gone since - the Sega CD's, the 32X's, the PC
    // Engine CD's, the Neo Geo Pockets', the WonderSwans' -: left out.)
    if (id == 0 || game.rating.half_stars < 0 || game.rating.half_stars > 10 || !WantedPlatform(game.platform))
      continue;
    game.names.assign(fields.begin() + 4, fields.end());
    copy->by_id.emplace(id, copy->games.size());
    copy->games.push_back(std::move(game));
  }
}

// A LaunchBox name as the index writes one: plain, ": " between a title and its subtitle as " - ", one space between
// words (scripts/make-ratings.py's lb_name_app).
std::string LbName(const std::string& name)
{
  const std::string plain = Plain(name);
  std::string out;
  for (size_t i = 0; i < plain.size(); i++)
  {
    const char c = plain[i] == '\t' ? ' ' : plain[i];
    if (c == ':' && i + 1 < plain.size() && plain[i + 1] == ' ')
    {
      while (!out.empty() && out.back() == ' ')
        out.pop_back();
      out += " - ";
      while (i + 1 < plain.size() && plain[i + 1] == ' ')
        i++;
      continue;
    }
    if (c == ' ' && (out.empty() || out.back() == ' '))
      continue;
    out += c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
    out.pop_back();
  size_t begin = 0;
  while (begin < out.size() && (out[begin] == ' ' || out[begin] == '-'))
    begin++;
  return out.substr(begin);
}

// A console's names and their ratings (Names()): its index's lines whose game is rated, then - for a console of
// cartridges or discs - LaunchBox's names of its games that no name before has the words of.
struct File
{
  bool read = false;
  std::vector<std::string> names;
  std::vector<Rating> ratings;
  std::unordered_map<std::string, size_t> exact;            // romsets: each name in lower case
  std::shared_ptr<const CoverDownload::Prepared> prepared;  // made the first time a game needs it
};

void ReadIndex(const std::string& path, const CopyData& copy, std::set<std::string>* written, File* file)
{
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0)
      continue;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(line.c_str() + tab + 1, nullptr, 10));
    const std::string name = line.substr(0, tab);
    // (A name of the index is its game's even when that one has no rating: no LaunchBox name takes its place.)
    if (written)
      written->insert(CoverDownload::NameKey(name));
    const auto it = copy.by_id.find(id);
    if (it == copy.by_id.end())
      continue;  // a game LaunchBox's players haven't rated (or not in this copy)
    file->names.push_back(name);
    file->ratings.push_back(copy.games[it->second].rating);
  }
}

void BuildFile(SystemId system, const std::string& index_dir, const CopyData& copy, File* file)
{
  file->read = true;
  const bool romsets = IsRomsetSystem(system);
  std::set<std::string> written;
  for (SystemId member : Family(system))
    ReadIndex(index_dir + "/" + GetSystem(member).key + ".tsv", copy, romsets ? nullptr : &written, file);
  if (romsets)
  {
    for (size_t i = 0; i < file->names.size(); i++)
      file->exact.emplace(Lower(file->names[i]), i);
    return;
  }
  // LaunchBox's names: the console's own platform's games first, the most voted for first.
  std::vector<std::string> platforms;
  for (SystemId member : Family(system))
    platforms.insert(platforms.end(), PlatformsOf(member).begin(), PlatformsOf(member).end());
  std::vector<const CopyGame*> games;
  for (const CopyGame& game : copy.games)
  {
    if (game.rating.votes >= EXTRA_VOTES &&
        std::find(platforms.begin(), platforms.end(), game.platform) != platforms.end())
      games.push_back(&game);
  }
  auto rank = [&platforms](const CopyGame* game) {
    return std::find(platforms.begin(), platforms.end(), game->platform) - platforms.begin();
  };
  std::stable_sort(games.begin(), games.end(), [&](const CopyGame* a, const CopyGame* b) {
    return rank(a) != rank(b) ? rank(a) < rank(b) : a->rating.votes > b->rating.votes;
  });
  for (const CopyGame* game : games)
  {
    for (const std::string& raw : game->names)
    {
      const std::string name = LbName(raw);
      const std::string key = CoverDownload::NameKey(name);
      if (key.empty() || !written.insert(key).second)
        continue;
      file->names.push_back(name);
      file->ratings.push_back(game->rating);
    }
  }
}

constexpr size_t NONE = static_cast<size_t>(-1);

// A romset's line: its set's name, else its parent's (a clone is rated as its parent); a NAOMI cartridge image by
// its set's full name (its covers' names after its own file's).
size_t RomsetLine(const File& file, const Library::Game& game)
{
  std::vector<std::string> names = {Lower(game.id)};
  if (game.system == SystemId::ARCADE)
  {
    if (const Arcade::Driver* driver = Arcade::Find(game.id); driver && !driver->parent.empty())
      names.push_back(Lower(driver->parent));
  }
  else if (const Naomi::Set* set = Naomi::Find(game.id))
  {
    if (!set->parent.empty())
      names.push_back(Lower(set->parent));
  }
  else
  {
    for (const std::string& name : game.cover_names)
      names.push_back(Lower(name));
  }
  for (const std::string& name : names)
  {
    if (const auto it = file.exact.find(name); it != file.exact.end())
      return it->second;
  }
  return NONE;
}

// A console game's line: the name given it with Rename, then its file's, matched as the covers are (Closest()'s
// sure matches: its words, its parts, close spellings, a list's numbering in front of it left out).
size_t GameLine(File& file, const Library::Game& game)
{
  if (file.names.empty())
    return NONE;
  if (!file.prepared)
    file.prepared = CoverDownload::Prepare(file.names);
  std::vector<std::string> stems;
  if (game.custom_title)
    stems.push_back(Plain(game.title));
  stems.push_back(Plain(game.id));
  for (const std::string& stem : stems)
  {
    bool sure = false;
    const std::string* name = CoverDownload::Closest(*file.prepared, stem, game, &sure);
    if (name && sure)
      return static_cast<size_t>(name - file.names.data());
  }
  return NONE;
}

// "&amp;" and the others, "&#233;" and "&#xE9;" (as UTF-8).
std::string Unescape(const char* begin, const char* end)
{
  std::string out;
  out.reserve(static_cast<size_t>(end - begin));
  for (const char* p = begin; p < end; p++)
  {
    if (*p != '&')
    {
      out += *p;
      continue;
    }
    const size_t room = static_cast<size_t>(std::min<ptrdiff_t>(end - p, 10));
    const char* semi = static_cast<const char*>(std::memchr(p, ';', room));
    if (!semi)
    {
      out += *p;
      continue;
    }
    const std::string entity(p + 1, semi);
    unsigned code = 0;
    if (entity == "amp")
      code = '&';
    else if (entity == "lt")
      code = '<';
    else if (entity == "gt")
      code = '>';
    else if (entity == "quot")
      code = '"';
    else if (entity == "apos")
      code = '\'';
    else if (entity.size() > 1 && entity[0] == '#')
      code = static_cast<unsigned>(entity[1] == 'x' || entity[1] == 'X' ?
                                       std::strtoul(entity.c_str() + 2, nullptr, 16) :
                                       std::strtoul(entity.c_str() + 1, nullptr, 10));
    if (code == 0 || code > 0x10FFFF)
    {
      out += *p;
      continue;
    }
    if (code < 0x80)
      out += static_cast<char>(code);
    else if (code < 0x800)
    {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
    p = semi;
  }
  return out;
}

// A line's element's text: "<Name>Super Mario World</Name>" (or with attributes, '<AlternateName
// xml:space="preserve"> Text</AlternateName>') -> true with "Super Mario World".
bool Element(const char* begin, const char* end, const char* tag, std::string* value)
{
  const size_t length = std::strlen(tag);
  if (static_cast<size_t>(end - begin) < length * 2 + 5 || begin[0] != '<' ||
      std::memcmp(begin + 1, tag, length) != 0 || (begin[length + 1] != '>' && begin[length + 1] != ' '))
    return false;
  const char* close = end - (length + 3);
  if (close[0] != '<' || close[1] != '/' || std::memcmp(close + 2, tag, length) != 0 || close[length + 2] != '>')
    return false;
  const char* text =
      static_cast<const char*>(std::memchr(begin + length + 1, '>', static_cast<size_t>(close - (begin + length + 1))));
  if (!text || text >= close)
    return false;
  *value = Unescape(text + 1, close);
  return true;
}

// A name of nothing but spaces (LaunchBox has some) is none.
bool Blank(const std::string& text)
{
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool Is(const char* begin, const char* end, const char* text)
{
  const size_t length = std::strlen(text);
  return static_cast<size_t>(end - begin) == length && std::memcmp(begin, text, length) == 0;
}

std::string Clean(std::string text)
{
  std::replace(text.begin(), text.end(), '\t', ' ');
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  return text;
}
}  // namespace

std::string Plain(const std::string& name)
{
  // Latin-1's letters U+00C0 to U+00FF without their accents (three spelt out below).
  static const char LATIN[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size(); i++)
  {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const unsigned char next = i + 1 < name.size() ? static_cast<unsigned char>(name[i + 1]) : 0;
    if (c == 0xC3 && next >= 0x80 && next <= 0xBF)
    {
      const unsigned code = 0xC0 + (next - 0x80);
      out += code == 0xC6 ? "AE" : code == 0xE6 ? "ae" : code == 0xDF ? "ss" : std::string(1, LATIN[code - 0xC0]);
      i++;
      continue;
    }
    if (c == 0xC2 && next >= 0xA0 && next <= 0xBF)
    {
      // No-break space, ° and · as spaces; ¡ ¿ ® © left out; ½ ² ³ spelt.
      out += next == 0xA0 || next == 0xB0 || next == 0xB7 ? " " : next == 0xBD ? "1/2" : next == 0xB2 ? "2" :
             next == 0xB3                                ? "3" : "";
      i++;
      continue;
    }
    if ((c == 0xC4 || c == 0xC5) && next >= 0x80 && next <= 0xBF)
    {
      // Latin Extended-A: the macron vowels (Japanese names: "Kyuukyoku" may be "Kyūkyoku"), œ, ł, š, ž, č.
      static const std::map<unsigned, const char*> EXTENDED = {
          {0x100, "A"}, {0x101, "a"}, {0x102, "A"}, {0x103, "a"}, {0x104, "A"},  {0x105, "a"},  {0x106, "C"},
          {0x107, "c"}, {0x10C, "C"}, {0x10D, "c"}, {0x112, "E"}, {0x113, "e"},  {0x12A, "I"},  {0x12B, "i"},
          {0x141, "L"}, {0x142, "l"}, {0x14C, "O"}, {0x14D, "o"}, {0x152, "OE"}, {0x153, "oe"}, {0x160, "S"},
          {0x161, "s"}, {0x16A, "U"}, {0x16B, "u"}, {0x17D, "Z"}, {0x17E, "z"}};
      const auto spelt = EXTENDED.find(((c & 0x1Fu) << 6) | (next & 0x3Fu));
      if (spelt != EXTENDED.end())
      {
        out += spelt->second;
        i++;
        continue;
      }
    }
    if (c == 0xE2 && i + 2 < name.size() && (next == 0x80 || next == 0x84))
    {
      const unsigned char last = static_cast<unsigned char>(name[i + 2]);
      const char* spelt = next == 0x84 ? (last == 0xA2 ? "" : nullptr) :  // ™
                          last == 0x98 || last == 0x99 ? "'" : last == 0x9C || last == 0x9D ? "\"" :
                          last == 0x93 || last == 0x94 ? "-" : last == 0xA6 ? "..." : last == 0xA2 ? " " : nullptr;
      if (spelt)
      {
        out += spelt;
        i += 2;
        continue;
      }
    }
    out += name[i];
  }
  return out;
}

std::string CopyPath(const std::string& data_dir)
{
  return data_dir + "/cache/ratings/launchbox.tsv";
}

Copy ReadCopy(const std::string& data_dir)
{
  Copy copy;
  const std::string path = CopyPath(data_dir);
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || st.st_size <= 0)
    return copy;
  copy.there = true;
  copy.made = static_cast<int64_t>(st.st_mtime);
  // Its second line: "# LaunchBox Metadata.xml of 2026-10-22: 34190 rated games" (Extract's); its fourth (00.80) the
  // platforms it was made for.
  std::ifstream in(path);
  std::string line;
  for (int i = 0; i < 4 && std::getline(in, line); i++)
  {
    if (line.rfind("# platforms: ", 0) == 0)
    {
      copy.platforms = line.substr(13);
      continue;
    }
    const size_t at = line.find("Metadata.xml of ");
    if (line.empty() || line[0] != '#' || at == std::string::npos)
      continue;
    copy.date = line.substr(at + 16, 10);
    const size_t colon = line.find(": ", at);
    if (colon != std::string::npos)
      copy.games = std::atoi(line.c_str() + colon + 2);
  }
  return copy;
}

bool CopyOld(const Copy& copy)
{
  // (00.80: also a copy made for fewer consoles - the Sega CD's and the 32X's games weren't kept before -: made again.
  // 00.83: a copy made for more - 00.80's to 00.82's, with the consoles gone since - is kept: every platform of this
  // version's is in it.)
  if (!copy.there || copy.games == 0 || std::time(nullptr) - copy.made > MAX_AGE)
    return true;
  const std::string has = "|" + copy.platforms + "|";
  const std::string wanted = PlatformsLine();
  for (size_t start = 0; start <= wanted.size();)
  {
    size_t bar = wanted.find('|', start);
    if (bar == std::string::npos)
      bar = wanted.size();
    if (has.find("|" + wanted.substr(start, bar - start) + "|") == std::string::npos)
      return true;
    start = bar + 1;
  }
  return false;
}

bool Extract(const std::string& zip_path, const std::string& path, std::string* error, int* count,
             const std::atomic<bool>* cancel, const std::atomic<bool>* pause)
{
  *count = 0;
  unzFile zip = unzOpen(zip_path.c_str());
  if (!zip)
  {
    *error = "the zip can't be opened";
    return false;
  }
  unz_file_info info = {};
  if (unzLocateFile(zip, "Metadata.xml", 1) != UNZ_OK ||
      unzGetCurrentFileInfo(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
      unzOpenCurrentFile(zip) != UNZ_OK)
  {
    unzClose(zip);
    *error = "Metadata.xml isn't in the zip";
    return false;
  }
  char date[16];
  snprintf(date, sizeof(date), "%04u-%02u-%02u", static_cast<unsigned>(info.tmu_date.tm_year),
           static_cast<unsigned>(info.tmu_date.tm_mon + 1), static_cast<unsigned>(info.tmu_date.tm_mday));
  struct Game
  {
    uint32_t id;
    std::string platform;
    Rating rating;
    std::vector<std::string> names;
  };
  std::vector<Game> games;
  std::unordered_map<uint32_t, size_t> by_id;
  // The lines: <Game> ... </Game> (every game, first in the file), then <GameAlternateName> ... (their other names),
  // then <GameImage> (not needed: the reading stops there). LaunchBox writes an element a line; a text's '<' is
  // written "&lt;", so a line beginning with '<' is always markup.
  enum class In
  {
    Nothing,
    Game,
    Alternate,
  } in = In::Nothing;
  std::string name, platform, id_text, rating_text, votes_text, value;
  bool alternates = false, done = false;
  auto line = [&](const char* begin, const char* end) {
    while (begin < end && (*begin == ' ' || *begin == '\t'))
      begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\t'))
      end--;
    if (begin == end || *begin != '<')
      return;
    if (in == In::Nothing)
    {
      if (Is(begin, end, "<Game>"))
      {
        in = In::Game;
        name.clear();
        platform.clear();
        id_text.clear();
        rating_text.clear();
        votes_text.clear();
      }
      else if (Is(begin, end, "<GameAlternateName>"))
      {
        in = In::Alternate;
        alternates = true;
        name.clear();
        id_text.clear();
      }
      else if (alternates && Is(begin, end, "<GameImage>"))
      {
        done = true;
      }
      return;
    }
    if (in == In::Game)
    {
      if (Is(begin, end, "</Game>"))
      {
        in = In::Nothing;
        const int votes = std::atoi(votes_text.c_str());
        const uint32_t id = static_cast<uint32_t>(std::strtoul(id_text.c_str(), nullptr, 10));
        if (id != 0 && !rating_text.empty() && votes > 0 && !Blank(name) && WantedPlatform(platform))
        {
          Game game{id, platform, {HalfStars(std::strtod(rating_text.c_str(), nullptr)), votes}, {Clean(name)}};
          by_id.emplace(id, games.size());
          games.push_back(std::move(game));
        }
      }
      else if (Element(begin, end, "Name", &value))
        name = value;
      else if (Element(begin, end, "Platform", &value))
        platform = value;
      else if (Element(begin, end, "DatabaseID", &value))
        id_text = value;
      else if (Element(begin, end, "CommunityRating", &value))
        rating_text = value;
      else if (Element(begin, end, "CommunityRatingCount", &value))
        votes_text = value;
      return;
    }
    if (Is(begin, end, "</GameAlternateName>"))
    {
      in = In::Nothing;
      const auto it = by_id.find(static_cast<uint32_t>(std::strtoul(id_text.c_str(), nullptr, 10)));
      if (it != by_id.end() && !Blank(name))
        games[it->second].names.push_back(Clean(name));
    }
    else if (Element(begin, end, "AlternateName", &value))
      name = value;
    else if (Element(begin, end, "DatabaseID", &value))
      id_text = value;
  };
  std::vector<char> buffer(1 << 20);
  std::string carry;
  bool failed = false;
  while (!done)
  {
    while (pause && *pause && !(cancel && *cancel))
      sceKernelUsleep(200000);
    if (cancel && *cancel)
    {
      failed = true;
      *error = "stopped";
      break;
    }
    const int got = unzReadCurrentFile(zip, buffer.data(), static_cast<unsigned>(buffer.size()));
    if (got < 0)
    {
      failed = true;
      *error = "Metadata.xml can't be read (" + std::to_string(got) + ")";
      break;
    }
    if (got == 0)
      break;
    const char* data = buffer.data();
    const char* stop = data + got;
    const char* start = data;
    for (const char* p = data; p < stop && !done; p++)
    {
      if (*p != '\n')
        continue;
      if (!carry.empty())
      {
        carry.append(start, p);
        line(carry.data(), carry.data() + carry.size());
        carry.clear();
      }
      else
      {
        line(start, p);
      }
      start = p + 1;
    }
    if (!done)
    {
      carry.append(start, stop);
      if (carry.size() > (1u << 20))
        carry.clear();  // (a line of over a megabyte is an Overview's text: not needed)
    }
  }
  unzCloseCurrentFile(zip);
  unzClose(zip);
  if (failed)
    return false;
  if (games.empty())
  {
    *error = "no rated game of the app's consoles in Metadata.xml";
    return false;
  }
  const std::string part = path + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out << "# EmuSuNi star ratings: the LaunchBox Games Database's community ratings of the app's consoles "
           "(src/Ratings.h), from "
        << URL << "\n# LaunchBox Metadata.xml of " << date << ": " << games.size()
        << " rated games\n# id<TAB>platform<TAB>half stars (0-10)<TAB>votes<TAB>name<TAB>other names...\n"
        << "# platforms: " << PlatformsLine() << "\n";
    for (const Game& game : games)
    {
      out << game.id << '\t' << game.platform << '\t' << game.rating.half_stars << '\t' << game.rating.votes;
      for (const std::string& each : game.names)
        out << '\t' << each;
      out << '\n';
    }
    out.flush();
    if (!out)
    {
      std::remove(part.c_str());
      *error = "the copy couldn't be written (is the storage full?)";
      return false;
    }
  }
  if (std::rename(part.c_str(), path.c_str()) != 0)
  {
    std::remove(part.c_str());
    *error = "the copy couldn't be written";
    return false;
  }
  *count = static_cast<int>(games.size());
  return true;
}

// ---- The download ----
std::string Downloader::Reason() const
{
  std::lock_guard lock(m_mutex);
  return m_reason;
}

bool Downloader::Start(const std::string& data_dir)
{
  if (Busy())
    return false;
  if (m_started)
  {
    pthread_join(m_thread, nullptr);
    m_started = false;
  }
  m_data_dir = data_dir;
  m_cancel = false;
  m_ended = false;
  m_bytes = 0;
  m_total = -1;
  {
    std::lock_guard lock(m_mutex);
    m_reason.clear();
  }
  m_state = State::Downloading;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);  // (the app's other small threads' size: libthr keeps one for them)
  m_started = pthread_create(&m_thread, &attr, &Downloader::Main, this) == 0;
  pthread_attr_destroy(&attr);
  if (!m_started)
  {
    m_state = State::Failed;
    std::lock_guard lock(m_mutex);
    m_reason = "the app couldn't start it (no thread)";
    Log("no thread to download the ratings");
  }
  return m_started;
}

void Downloader::Stop()
{
  if (!m_started)
    return;
  m_cancel = true;
  pthread_join(m_thread, nullptr);
  m_started = false;
}

bool Downloader::Finished(bool* ok)
{
  if (!m_ended.exchange(false))
    return false;
  *ok = m_state == State::Done;
  return true;
}

void* Downloader::Main(void* self)
{
  static_cast<Downloader*>(self)->Run();
  return nullptr;
}

void Downloader::Run()
{
  const auto start = std::chrono::steady_clock::now();
  const std::string dir = m_data_dir + "/cache/ratings";
  const std::string zip = dir + "/Metadata.zip";
  MakeDirs(dir);
  auto fail = [this](const std::string& reason, const std::string& log) {
    {
      std::lock_guard lock(m_mutex);
      m_reason = reason;
    }
    Log(log);
    m_state = State::Failed;
    m_ended = true;
  };
  // LaunchBox's whole database (one zip, updated each day): a part kept from a cut download goes on (Range) - not one
  // of hours ago, the zip may be another day's now -; a cut or a server's passing failure tried again twice; while a
  // game runs it waits (the part kept).
  {
    struct stat st = {};
    if (stat((zip + ".part").c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime > 6 * 3600)
      std::remove((zip + ".part").c_str());
  }
  Net::Response answer;
  bool got = false;
  for (int attempt = 0; attempt < 3 && !m_cancel;)
  {
    while (m_pause && !m_cancel)
      sceKernelUsleep(200000);
    if (m_cancel)
      break;
    Net::Request request;
    request.url = URL;
    request.timeout_seconds = 0;  // (110 MB: only a minute without a byte stops it)
    struct stat st = {};
    request.resume_from = stat((zip + ".part").c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : 0;
    request.cancel = &m_cancel;
    request.progress = [this](int64_t done, int64_t total) {
      m_bytes = done;
      m_total = total;
      return !m_pause && !m_cancel;
    };
    answer = Net::Download(request, zip);
    if (answer.ok())
    {
      got = true;
      break;
    }
    if (m_cancel)
      break;
    if (m_pause)
      continue;  // a game started: on after it, from where it stopped
    attempt++;
    Log("download: " + (answer.error.empty() ? "HTTP " + std::to_string(answer.status) : answer.error) +
        (attempt < 3 ? ": trying again" : ""));
    if (answer.status >= 400 && answer.status < 500)
      break;  // (a refusal or "not there" is the server's answer)
    for (int i = 0; i < 20 && !m_cancel; i++)
      sceKernelUsleep(100000);
  }
  if (m_cancel)
  {
    m_state = State::Idle;
    m_ended = true;
    return;
  }
  if (!got)
  {
    const std::string plain = answer.plain.empty() ? Net::Plain(answer.status, answer.error) : answer.plain;
    if (answer.error.rfind("couldn't write", 0) == 0 || answer.error.rfind("couldn't create", 0) == 0)
      std::remove((zip + ".part").c_str());  // (a full storage: what came isn't kept to fill it)
    fail(plain, "LaunchBox's ratings couldn't be downloaded from " + std::string(URL) + " (HTTP " +
                    std::to_string(answer.status) + ", " + std::to_string(answer.bytes) + " bytes): " +
                    (answer.error.empty() ? plain : answer.error));
    return;
  }
  const double download_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Log("LaunchBox's Metadata.zip downloaded (" + std::to_string(answer.bytes) + " bytes, " +
      std::to_string(static_cast<int>(download_s)) + " s)");
  // Its Metadata.xml read into the copy; the zip goes.
  m_state = State::Reading;
  const auto read_start = std::chrono::steady_clock::now();
  std::string error;
  int games = 0;
  const bool ok = Extract(zip, CopyPath(m_data_dir), &error, &games, &m_cancel, &m_pause);
  std::remove(zip.c_str());
  if (m_cancel)
  {
    m_state = State::Idle;
    m_ended = true;
    return;
  }
  if (!ok)
  {
    fail(error == "the copy couldn't be written (is the storage full?)" ?
             std::string("the storage is full or can't be written") :
             std::string("what came isn't LaunchBox's database (it is downloaded again the next time)"),
         "LaunchBox's Metadata.zip couldn't be read: " + error);
    return;
  }
  const double read_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start).count();
  char text[160];
  snprintf(text, sizeof(text), "%d rated games of the app's consoles kept in %s (Metadata.xml read in %.1f s)", games,
           CopyPath(m_data_dir).c_str(), read_s);
  Log(text);
  m_state = State::Done;
  m_ended = true;
}

// ---- A game's rating ----
std::vector<std::pair<std::string, Rating>> Names(SystemId system, const std::string& index_dir,
                                                  const std::string& copy)
{
  CopyData data;
  ReadCopyData(copy, &data);
  File file;
  BuildFile(system, index_dir, data, &file);
  std::vector<std::pair<std::string, Rating>> out;
  for (size_t i = 0; i < file.names.size(); i++)
    out.emplace_back(file.names[i], file.ratings[i]);
  return out;
}

std::unordered_map<std::string, Rating> Match(const std::vector<Library::Game>& games, const std::string& index_dir,
                                              const std::string& copy, const std::atomic<bool>* cancel,
                                              const std::atomic<bool>* pause,
                                              std::unordered_map<std::string, std::string>* matched)
{
  const auto start = std::chrono::steady_clock::now();
  std::unordered_map<std::string, Rating> out;
  CopyData data;
  bool data_read = false;
  std::map<SystemId, File> files;  // by the first console of each Family()
  auto file_of = [&](SystemId system) -> File& {
    File& file = files[Family(system).front()];
    if (!file.read)
    {
      if (!data_read)
      {
        ReadCopyData(copy, &data);
        data_read = true;
      }
      BuildFile(system, index_dir, data, &file);
    }
    return file;
  };
  int rated = 0;
  for (const Library::Game& game : games)
  {
    while (pause && *pause && !(cancel && *cancel))
      sceKernelUsleep(200000);
    if (cancel && *cancel)
      break;
    File& file = file_of(game.system);
    const size_t line = IsRomsetSystem(game.system) ? RomsetLine(file, game) : GameLine(file, game);
    out[game.key] = line == NONE ? Rating{} : file.ratings[line];
    if (line != NONE && matched)
      (*matched)[game.key] = file.names[line];
    rated += line == NONE ? 0 : 1;
  }
  const long ms = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
  std::string read;
  for (const auto& [system, file] : files)
  {
    read += (read.empty() ? "" : ", ") + std::to_string(file.names.size()) + " " +
            (system == SystemId::GB ? std::string("GB+GBC") : std::string(GetSystem(system).tab));
  }
  Log(std::to_string(rated) + " of " + std::to_string(out.size()) + " games rated (" + std::to_string(ms) + " ms; " +
      (data_read ? std::to_string(data.games.size()) + " rated games in the copy" : std::string("no copy read")) +
      "; names: " + (read.empty() ? std::string("none") : read) + ")");
  return out;
}

Matcher::~Matcher()
{
  Stop();
}

void Matcher::Stop()
{
  if (!m_started)
    return;
  m_cancel = true;
  pthread_join(m_thread, nullptr);
  m_started = false;
  m_cancel = false;
}

bool Matcher::Start(const std::vector<Library::Game>& games, const std::string& index_dir, const std::string& copy)
{
  Stop();
  m_games = games;
  m_index_dir = index_dir;
  m_copy = copy;
  m_results.clear();
  m_done = false;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);  // (the size of the app's other small threads: libthr reuses one)
  m_started = pthread_create(&m_thread, &attr, &Matcher::Main, this) == 0;
  pthread_attr_destroy(&attr);
  if (!m_started)
    Log("no thread to match the games: no stars this time");
  return m_started;
}

void* Matcher::Main(void* self)
{
  Matcher& matcher = *static_cast<Matcher*>(self);
  matcher.m_results = Match(matcher.m_games, matcher.m_index_dir, matcher.m_copy, &matcher.m_cancel, &matcher.m_pause);
  matcher.m_done = true;
  return nullptr;
}

bool Matcher::Take(std::unordered_map<std::string, Rating>* out)
{
  if (!m_started || !m_done)
    return false;
  pthread_join(m_thread, nullptr);
  m_started = false;
  *out = std::move(m_results);
  m_results.clear();
  m_games.clear();
  return true;
}
}  // namespace Ratings
