// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "CheatDownload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <thread>

#include <sys/stat.h>

#include <minizip/unzip.h>

#include "CoverDownload.h"
#include "Library.h"
#include "Log.h"
#include "Net.h"
#include "Portable.h"  // ((01.92) the portable data's drive: the collection's other folder)
#include "ThreeDsCia.h"

namespace CheatDownload
{
namespace
{
constexpr const char* URL = "http://buildbot.libretro.com/assets/frontend/cheats.zip";
// (01.26, the owner) The 3DS's: the repository's list of files (one answer of GitHub's API), then the game's file.
constexpr const char* CTRPF_TREE =
    "https://api.github.com/repos/iSharingan/CTRPF-AR-CHEAT-CODES/git/trees/master?recursive=1";
constexpr const char* CTRPF_RAW = "https://raw.githubusercontent.com/iSharingan/CTRPF-AR-CHEAT-CODES/master/";
constexpr size_t CTRPF_TREE_MOST = 4u << 20, CTRPF_FILE_MOST = 1u << 20;
// (01.26, the owner) The Arcade's: FinalBurn Neo's own collection, a set's file (the largest .ini there: 120 KB).
constexpr const char* FBNEO_RAW = "https://raw.githubusercontent.com/finalburnneo/FBNeo-cheats/master/cheats/";
constexpr size_t FBNEO_FILE_MOST = 1u << 20;

// A path for a URL: its spaces, brackets and the like as %XX (the folders are the games' names).
std::string UrlPath(const std::string& path)
{
  std::string out;
  for (const unsigned char c : path)
  {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
      out += static_cast<char>(c);
    else
    {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// The paths of GitHub's tree answer ("path":"...") that end in "/<name>", case aside.
std::vector<std::string> TreePathsEnding(const std::string& json, const std::string& name)
{
  std::vector<std::string> paths;
  std::string lower_name = name;
  std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (size_t at = json.find("\"path\":"); at != std::string::npos; at = json.find("\"path\":", at + 7))
  {
    const size_t open = json.find('"', at + 7);
    const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    if (close == std::string::npos)
      break;
    std::string path = json.substr(open + 1, close - open - 1);
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.size() > lower_name.size() && lower.compare(lower.size() - lower_name.size(), lower_name.size(),
                                                          lower_name) == 0 &&
        lower[lower.size() - lower_name.size() - 1] == '/')
      paths.push_back(path);
  }
  return paths;
}
constexpr int64_t MAX_AGE = 30 * 24 * 3600;      // a month: libretro adds lists now and then
constexpr size_t MAX_LIST = 8u * 1024 * 1024;    // one list (GoldenEye 007's 7232 codes: 400 KB)
constexpr size_t MAX_RESULTS = 15;

// The collection's folders for a console (libretro's names, as its thumbnails').
std::vector<std::string> Folders(SystemId system)
{
  std::vector<std::string> folders = {GetSystem(system).thumbnails};
  if (system == SystemId::NES)
    folders.push_back("Nintendo - Family Computer Disk System");
  return folders;
}

// A PSP disc's ID ("ULES-01586"): UMD_DATA.BIN at the root of its ISO 9660 image. Empty for a
// compressed image (.cso, .chd) or anything else.
std::string PspDiscId(const std::string& path)
{
  const size_t dot = path.rfind('.');
  std::string extension = dot == std::string::npos ? "" : path.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".iso")
    return {};
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return {};
  auto read_at = [file](uint64_t offset, void* out, size_t size) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && fread(out, 1, size, file) == size;
  };
  auto le32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
  };
  std::string id;
  uint8_t volume[2048];
  if (read_at(16 * 2048, volume, sizeof(volume)) && volume[0] == 1 && std::memcmp(volume + 1, "CD001", 5) == 0)
  {
    // The root directory's record (its sector and size), then the records in it.
    const uint8_t* root = volume + 156;
    const uint32_t size = std::min<uint32_t>(le32(root + 10), 64 * 1024);
    std::vector<uint8_t> dir(size);
    if (size > 0 && read_at(static_cast<uint64_t>(le32(root + 2)) * 2048, dir.data(), size))
    {
      for (size_t at = 0; at + 34 <= size;)
      {
        const uint8_t length = dir[at];
        if (length == 0)
        {
          at = (at / 2048 + 1) * 2048;  // the rest of this sector is empty
          continue;
        }
        const size_t name_length = std::min<size_t>(dir[at + 32], size - at - 33);
        const std::string name(reinterpret_cast<const char*>(&dir[at + 33]), name_length);
        if (name.rfind("UMD_DATA.BIN", 0) == 0)
        {
          char text[11] = {};
          if (read_at(static_cast<uint64_t>(le32(&dir[at + 2])) * 2048, text, 10))
            id = text;
          break;
        }
        at += length;
      }
    }
  }
  fclose(file);
  return id.size() == 10 && id[4] == '-' ? id : std::string();
}

std::string ReadEntry(unzFile zip, const std::string& entry, std::string* error)
{
  std::string text;
  unz_file_info info;
  if (unzLocateFile(zip, entry.c_str(), 1) != UNZ_OK ||
      unzGetCurrentFileInfo(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
  {
    *error = "The list isn't in the collection";
    return text;
  }
  if (info.uncompressed_size == 0 || info.uncompressed_size > MAX_LIST || unzOpenCurrentFile(zip) != UNZ_OK)
  {
    *error = "The list can't be read";
    return text;
  }
  text.resize(info.uncompressed_size);
  const int read = unzReadCurrentFile(zip, text.data(), static_cast<unsigned>(text.size()));
  unzCloseCurrentFile(zip);
  if (read != static_cast<int>(text.size()))
  {
    *error = "The list can't be read";
    text.clear();
  }
  return text;
}

std::string s_n64_list = std::string(App::ASSETS_DIR) + "/n64/mupencheat.txt";
constexpr size_t N64_CHOICES_MOST = 256, N64_LINES_MOST = 100;  // (the list's most: 140 boxers; its longest: 94 lines)
}  // namespace

// ---- (01.57) The N64's cheats by the cartridge (mupencheat.txt) ----

void SetN64CheatList(const std::string& path)
{
  s_n64_list = path;
}

bool ReadN64Cart(const std::string& path, N64Cart* cart)
{
  uint8_t header[0x40] = {};
  bool read = false;
  const size_t dot = path.rfind('.');
  std::string extension = dot == std::string::npos ? "" : path.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension == ".zip")
  {
    // (Its first .z64, .v64 or .n64: 64 bytes of it read, not the whole ROM.)
    if (unzFile zip = unzOpen(path.c_str()))
    {
      for (int result = unzGoToFirstFile(zip); result == UNZ_OK && !read; result = unzGoToNextFile(zip))
      {
        char name[512];
        unz_file_info info;
        if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
          continue;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() < 4 || (lower.compare(lower.size() - 4, 4, ".z64") != 0 &&
                                 lower.compare(lower.size() - 4, 4, ".v64") != 0 &&
                                 lower.compare(lower.size() - 4, 4, ".n64") != 0))
          continue;
        if (unzOpenCurrentFile(zip) == UNZ_OK)
        {
          read = unzReadCurrentFile(zip, header, sizeof(header)) == static_cast<int>(sizeof(header));
          unzCloseCurrentFile(zip);
        }
      }
      unzClose(zip);
    }
  }
  else if (FILE* file = std::fopen(path.c_str(), "rb"))
  {
    read = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    std::fclose(file);
  }
  if (!read)
    return false;
  // The cartridge's byte order: .z64 as it is (80 37 12 40), .v64 its bytes swapped in pairs, .n64 in fours.
  if (header[0] == 0x37 && header[1] == 0x80)
  {
    for (size_t i = 0; i < sizeof(header); i += 2)
      std::swap(header[i], header[i + 1]);
  }
  else if (header[0] == 0x40 && header[1] == 0x12)
  {
    for (size_t i = 0; i < sizeof(header); i += 4)
    {
      std::swap(header[i], header[i + 3]);
      std::swap(header[i + 1], header[i + 2]);
    }
  }
  if (header[0] != 0x80 || header[1] != 0x37)
    return false;
  const auto be32 = [&header](size_t at) {
    return static_cast<uint32_t>(header[at]) << 24 | static_cast<uint32_t>(header[at + 1]) << 16 |
           static_cast<uint32_t>(header[at + 2]) << 8 | header[at + 3];
  };
  cart->crc1 = be32(0x10);
  cart->crc2 = be32(0x14);
  cart->country = header[0x3E];
  return true;
}

size_t FindN64Cheats(const std::string& text, const N64Cart& cart, std::string* name)
{
  // "crc 80F41131-384645F6-C:4A" then "gn AeroGauge (J) (V1.1)": the country's own first, else the same CRCs'.
  size_t found = std::string::npos;
  for (size_t at = text.find("\ncrc "); at != std::string::npos; at = text.find("\ncrc ", at + 1))
  {
    const size_t line = at + 1;
    unsigned crc1 = 0, crc2 = 0, country = 0;
    const int fields = std::sscanf(text.c_str() + line, "crc %8X-%8X-C:%2X", &crc1, &crc2, &country);
    if (fields < 2 || crc1 != cart.crc1 || crc2 != cart.crc2)
      continue;
    if (fields == 3 && country == cart.country)
    {
      found = line;
      break;
    }
    if (found == std::string::npos)
      found = line;
  }
  if (found != std::string::npos && name)
  {
    const size_t gn = text.find("\ngn ", found);
    const size_t next = text.find("\ncrc ", found + 1);
    name->clear();
    if (gn != std::string::npos && (next == std::string::npos || gn < next))
    {
      const size_t end = text.find_first_of("\r\n", gn + 4);
      *name = text.substr(gn + 4, end == std::string::npos ? std::string::npos : end - gn - 4);
    }
  }
  return found;
}

std::vector<Cheats::Entry> ParseN64Cheats(const std::string& text, size_t at, int* left_out)
{
  std::vector<Cheats::Entry> entries;
  int dropped = 0;
  struct Line
  {
    std::string address, value;                              // "8013CD0E", "0001" or "????"
    std::vector<std::pair<std::string, std::string>> choices;  // a "????" line's: value, label
  };
  std::string name;
  std::vector<Line> lines;
  // A cheat done: its entry (its lines "AAAAAAAA VVVV+..."), or one per choice; else left out.
  const auto finish = [&]() {
    if (name.empty() || lines.empty())
    {
      name.clear();
      lines.clear();
      return;
    }
    // (Several "????" lines take one value: the list is on one of them - "80163655 ????", "80163682 ???? 000D:...".)
    const std::vector<std::pair<std::string, std::string>>* choices = nullptr;
    bool usable = lines.size() <= N64_LINES_MOST, asks = false;
    for (const Line& line : lines)
    {
      if (line.value != "????")
        continue;
      asks = true;
      if (line.choices.empty())
        continue;
      if (line.choices.size() > N64_CHOICES_MOST || (choices && *choices != line.choices))
        usable = false;
      choices = &line.choices;
    }
    if (asks && !choices)
      usable = false;  // ("????" and no list of values anywhere)
    const auto code = [&lines](const std::string& choice) {
      std::string out;
      for (const Line& line : lines)
        out += (out.empty() ? "" : "+") + line.address + " " + (line.value == "????" ? choice : line.value);
      return out;
    };
    if (!usable)
      dropped++;
    else if (!choices)
      entries.push_back({name, code({}), false});
    else
    {
      for (const auto& [value, label] : *choices)
        entries.push_back({name + ": " + label, code(value), false});
    }
    name.clear();
    lines.clear();
  };
  const auto hex = [](const std::string& word, size_t size) {
    return word.size() == size &&
           std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
  };
  // The block: from its "crc" line to the next one.
  for (size_t line_at = text.find('\n', at); line_at != std::string::npos && line_at + 1 < text.size();)
  {
    const size_t begin = line_at + 1;
    size_t end = text.find('\n', begin);
    if (end == std::string::npos)
      end = text.size();
    line_at = end < text.size() ? end : std::string::npos;
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("crc ", 0) == 0)
      break;
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue;
    line.erase(0, first);
    if (line.rfind("cn ", 0) == 0)
    {
      finish();
      name = line.substr(3);
      for (size_t slash; (slash = name.find('\\')) != std::string::npos;)
        name.replace(slash, 1, " - ");  // ("No Damage\Player 1": a group and its cheat)
      continue;
    }
    if (name.empty() || line.size() < 13)
      continue;  // ("gn", "cd" - a cheat's note -, "//")
    Line code{line.substr(0, 8), line.substr(9, 4), {}};
    if (!hex(code.address, 8) || line[8] != ' ' || (code.value != "????" && !hex(code.value, 4)))
      continue;
    if (code.value == "????")
    {
      // Its choices: 0001:"1 Lap",0002:"2 Laps" (a label may hold a comma).
      for (size_t pos = line.find_first_not_of(' ', 13); pos != std::string::npos && pos + 6 < line.size();)
      {
        const std::string value = line.substr(pos, 4);
        const size_t open = pos + 5;
        const size_t close = line.find('"', open + 1);
        if (!hex(value, 4) || line[pos + 4] != ':' || line[open] != '"' || close == std::string::npos)
          break;
        code.choices.emplace_back(value, line.substr(open + 1, close - open - 1));
        pos = line.find_first_not_of(", ", close + 1);
      }
    }
    lines.push_back(std::move(code));
  }
  finish();
  if (left_out)
    *left_out = dropped;
  return entries;
}

Finder::~Finder()
{
  Stop();
}

void Finder::Stop()
{
  m_stop = true;
  if (m_thread.joinable())
    m_thread.join();
  m_stop = false;
  if (m_state != State::Done)
    m_state = State::Idle;
}

void Finder::Start(const std::string& data_dir, SystemId system, const std::string& game_path, const std::string& title)
{
  Stop();
  {
    std::lock_guard lock(m_mutex);
    m_error.clear();
    m_reason.clear();
    m_results.clear();
    m_entries.clear();
    m_codes.clear();
    m_picked.clear();
    m_open_name.clear();
    m_system = system;
    m_title_id.clear();
    m_3ds_text.clear();
  }
  m_mb = 0;
  m_total_mb = 0;
  m_cheat_count = 0;
  m_cartridge = 0;
  m_state = State::Searching;
  m_thread = std::thread(&Finder::Run, this, data_dir, system, game_path, title);
}

std::string Finder::Source() const
{
  std::lock_guard lock(m_mutex);
  return m_system == SystemId::THREEDS ? "CTRPF-AR-CHEAT-CODES (GitHub)" :
         m_system == SystemId::ARCADE  ? "FBNeo-cheats (GitHub)" :
                                         "libretro";
}

std::string Finder::TitleId() const
{
  std::lock_guard lock(m_mutex);
  return m_title_id;
}

// (01.26, the owner) The 3DS game's cheats: its title ID (the file's header), the repository's list (GitHub's API),
// its file there, downloaded into memory (Open reads it). None there: Done without a list.
void Finder::Run3ds(const std::string& game_path)
{
  auto fail = [this](const std::string& error, const std::string& reason = {}) {
    std::lock_guard lock(m_mutex);
    m_error = error;
    m_reason = reason;
    m_state = State::Failed;
    LogLine("cheats: " + error + (reason.empty() ? "" : ": " + reason));
  };
  const uint64_t title = ThreeDsCia::RomTitleId(game_path);
  if (title == 0)
  {
    fail("This game's title ID can't be read",
         "a homebrew (.3dsx) has none; a .3ds, .cci or .cxi file has it in its header");
    return;
  }
  const std::string id = ThreeDsCia::Hex(title);
  {
    std::lock_guard lock(m_mutex);
    m_title_id = id;
  }
  m_state = State::Downloading;
  Net::Request request;
  request.url = CTRPF_TREE;
  request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
  request.max_bytes = CTRPF_TREE_MOST;
  request.timeout_seconds = 60;
  const Net::Response tree = Net::Fetch(request);
  if (m_stop)
    return;
  if (!tree.ok())
  {
    fail("The cheat collection couldn't be downloaded",
         tree.status == 403 || tree.status == 429 ? std::string("GitHub's limit of questions for this connection was "
                                                                "reached: try again in an hour") :
                                                    Net::Plain(tree.status, tree.error));
    return;
  }
  const std::vector<std::string> paths = TreePathsEnding(tree.body, id + ".txt");
  LogF("cheats: CTRPF-AR-CHEAT-CODES has %zu file(s) for %s", paths.size(), id.c_str());
  if (paths.empty())
  {
    m_state = State::Done;
    return;
  }
  // The first (a title ID is one game): its folder's name - the game and its region - shown.
  const std::string& path = paths.front();
  request = {};
  request.url = std::string(CTRPF_RAW) + UrlPath(path);
  request.max_bytes = CTRPF_FILE_MOST;
  const Net::Response file = Net::Fetch(request);
  if (m_stop)
    return;
  if (!file.ok() || file.body.empty())
  {
    fail("The game's cheats couldn't be downloaded", Net::Plain(file.status, file.error));
    return;
  }
  const size_t slash = path.rfind('/');
  const size_t before = slash == std::string::npos || slash == 0 ? std::string::npos : path.rfind('/', slash - 1);
  const std::string folder = slash == std::string::npos ? path :
                             path.substr(before == std::string::npos ? 0 : before + 1,
                                         slash - (before == std::string::npos ? 0 : before + 1));
  LogF("cheats: %s's file downloaded (%zu bytes)", id.c_str(), file.body.size());
  {
    std::lock_guard lock(m_mutex);
    m_3ds_text = file.body;
    m_results = {folder};
    m_entries = {path};
  }
  m_state = State::Done;
}

// (01.26, the owner) The Arcade game's cheats: its set's file of FBNeo-cheats, downloaded into memory, checked to be
// one, saved as <set>.ini.new in the core's cheats folder (in place at the next start: src/ArcadeCheats.h). None
// there (404): Done without a list.
void Finder::RunArcade(const std::string& cheats_dir, const std::string& game_path)
{
  auto fail = [this](const std::string& error, const std::string& reason = {}) {
    std::lock_guard lock(m_mutex);
    m_error = error;
    m_reason = reason;
    m_state = State::Failed;
    LogLine("cheats: " + error + (reason.empty() ? "" : ": " + reason));
  };
  // The set: the zip's (or .7z's) name, as the core's driver has it (lower case; letters, digits and _ only).
  const size_t slash = game_path.find_last_of('/');
  std::string set = slash == std::string::npos ? game_path : game_path.substr(slash + 1);
  const size_t dot = set.rfind('.');
  if (dot != std::string::npos && dot > 0)
    set.resize(dot);
  std::transform(set.begin(), set.end(), set.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (set.empty() || set.size() > 32 ||
      !std::all_of(set.begin(), set.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
  {
    fail("This game's set name can't be read", "an arcade game is a .zip or .7z under its set's name (mslug.zip)");
    return;
  }
  {
    std::lock_guard lock(m_mutex);
    m_title_id = set;
  }
  m_state = State::Downloading;
  Net::Request request;
  request.url = std::string(FBNEO_RAW) + set + ".ini";
  request.max_bytes = FBNEO_FILE_MOST;
  request.timeout_seconds = 60;
  const Net::Response file = Net::Fetch(request);
  if (m_stop)
    return;
  if (file.status == 404)
  {
    LogF("cheats: FBNeo-cheats has no %s.ini", set.c_str());
    m_state = State::Done;
    return;
  }
  if (!file.ok() || file.body.empty())
  {
    fail("The game's cheats couldn't be downloaded",
         file.status == 403 || file.status == 429 ? std::string("GitHub's limit of questions for this connection was "
                                                                "reached: try again in an hour") :
                                                    Net::Plain(file.status, file.error));
    return;
  }
  // Its cheats ("cheat "<name>"" lines; a blank name - " " - is a separator, not one).
  int count = 0;
  for (size_t at = 0; at < file.body.size();)
  {
    size_t end = file.body.find('\n', at);
    if (end == std::string::npos)
      end = file.body.size();
    const std::string line = file.body.substr(at, end - at);
    if (line.rfind("cheat \"", 0) == 0 && line.rfind("cheat \" \"", 0) != 0 && line.rfind("cheat \"\"", 0) != 0)
      count++;
    at = end + 1;
  }
  if (count == 0)
  {
    fail("The game's cheats couldn't be downloaded",
         "what came isn't a cheat file (a web page: does the network ask to sign in?)");
    return;
  }
  MakeDirs(cheats_dir);
  const std::string path = cheats_dir + "/" + set + ".ini.new";
  if (!WriteTextFile(path, file.body))
  {
    fail("The game's cheats couldn't be saved", "the storage is full or can't be written");
    return;
  }
  LogF("cheats: %s.ini from FBNeo-cheats (%zu bytes, %d cheats), in place at the game's next start", set.c_str(),
       file.body.size(), count);
  {
    std::lock_guard lock(m_mutex);
    m_results = {set + ".ini"};
  }
  m_cheat_count = count;
  m_state = State::Done;
}

std::string Finder::Error() const
{
  std::lock_guard lock(m_mutex);
  return m_error;
}

std::string Finder::Reason() const
{
  std::lock_guard lock(m_mutex);
  return m_reason;
}

void Finder::Run(std::string data_dir, SystemId system, std::string game_path, std::string title)
{
  // ((01.57) An N64 cartridge's own list - mupencheat.txt, by its header -: found first, offline; with it, libretro's
  // collection failing still gives it alone.)
  std::vector<std::string> cart_names, cart_entries;
  m_cartridge = 0;
  if (system == SystemId::N64)
  {
    N64Cart cart;
    const std::string list = ReadN64Cart(game_path, &cart) ? ReadTextFile(s_n64_list) : std::string();
    std::string name;
    const size_t at = list.empty() ? std::string::npos : FindN64Cheats(list, cart, &name);
    if (at != std::string::npos)
    {
      cart_names.push_back(name.empty() ? std::string("The cartridge's own") : name);
      cart_entries.push_back("mupencheat:" + std::to_string(at));
    }
    LogF("cheats: n64 cartridge %08X-%08X country %02X: %s", cart.crc1, cart.crc2, cart.country,
         at != std::string::npos ? ("in mupencheat.txt as \"" + name + "\"").c_str() :
         list.empty()             ? "no header or no mupencheat.txt" :
                                    "not in mupencheat.txt");
  }
  auto fail = [this, &cart_names, &cart_entries](const std::string& error, const std::string& reason = {}) {
    std::lock_guard lock(m_mutex);
    if (!cart_names.empty())
    {
      LogLine("cheats: " + error + (reason.empty() ? "" : ": " + reason) + " - the cartridge's own list alone");
      m_results = cart_names;
      m_entries = cart_entries;
      m_cartridge = cart_names.size();
      m_state = State::Done;
      return;
    }
    m_error = error;
    m_reason = reason;
    m_state = State::Failed;
    LogLine("cheats: " + error + (reason.empty() ? "" : ": " + reason));
  };
  if (system == SystemId::THREEDS)
  {
    Run3ds(game_path);  // ((01.26) not in libretro's collection)
    return;
  }
  if (system == SystemId::ARCADE)
  {
    RunArcade(data_dir, game_path);  // ((01.26) FinalBurn Neo's own collection; data_dir: the core's cheats folder)
    return;
  }
  // The collection: kept for a month; a failed download keeps an older one. ((01.92, Lei on a PS5 in portable mode: "no
  // se pudo guardar ... lleno" with a drive far from full) This PS4's cache first; where it can't be written - a PS5's
  // /data - the portable data's drive's (its cache folder). The one that has it is read.
  std::vector<std::string> dirs = {data_dir + "/cache"};
  if (Portable::Active() && Portable::UserDir() + "/cache" != dirs[0])
    dirs.push_back(Portable::UserDir() + "/cache");
  std::string dir = dirs[0];
  struct stat st = {};
  bool have = false;
  for (const std::string& d : dirs)
  {
    struct stat s;
    if (stat((d + "/cheats.zip").c_str(), &s) == 0 && s.st_size > 0 && (!have || s.st_mtime > st.st_mtime))
    {
      dir = d;
      st = s;
      have = true;
    }
  }
  std::string zip_path = dir + "/cheats.zip";
  const std::string had_zip = zip_path;  // (the one there before: kept when the download fails)
  MakeDirs(dir);
  if (!have || std::time(nullptr) - st.st_mtime > MAX_AGE)
  {
    // 00.68 (users' "error" downloading it, nothing more said): no limit for the whole download any more (600 s
    // stopped any connection under 62 KB/s: the collection is 35 MB), only for a minute without a byte (Net); a cut
    // or a server's passing failure tried again twice; what came checked to be the zip, whole; and the reason said.
    m_state = State::Downloading;
    std::string temp = zip_path + ".new";
    long status = 0;
    std::string error;
    size_t bytes = 0;
    bool write_failed = false;
    // ((01.92) Why it couldn't be written - the folder, errno -: said as it is, not always "full".)
    int save_errno = 0;
    std::string save_dir;
    std::atomic<int64_t> expected{-1};
    for (int attempt = 0; attempt < 3 && !m_stop && !write_failed; attempt++)
    {
      if (attempt > 0)
      {
        LogF("cheats: trying again (%s)", error.empty() ? ("HTTP " + std::to_string(status)).c_str() : error.c_str());
        for (int i = 0; i < 20 && !m_stop; i++)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));  // (2 s: a moment for the network)
      }
      FILE* file = fopen(temp.c_str(), "wb");
      if (!file)
      {
        // ((01.92) Not there: the other folder - the portable drive's -, else why.)
        save_errno = errno;
        save_dir = dir;
        LogF("cheats: %s can't be written: %s", temp.c_str(), std::strerror(save_errno));
        for (const std::string& other : dirs)
        {
          if (file || other == dir)
            continue;
          MakeDirs(other);
          const std::string other_temp = other + "/cheats.zip.new";
          file = fopen(other_temp.c_str(), "wb");
          if (file)
          {
            dir = other;
            zip_path = dir + "/cheats.zip";
            temp = other_temp;
            LogF("cheats: the collection goes to %s instead", dir.c_str());
          }
          else
          {
            save_errno = errno;
            save_dir = other;
            LogF("cheats: %s can't be written either: %s", other_temp.c_str(), std::strerror(save_errno));
          }
        }
      }
      if (!file)
      {
        write_failed = true;
        break;
      }
      bytes = 0;
      error.clear();
      expected = -1;
      Net::Request request;
      request.url = URL;
      request.timeout_seconds = 0;
      request.expected = &expected;
      status = Net::Get(
          request,
          [&](const char* data, size_t size) {
            if (m_stop)
              return false;
            if (fwrite(data, 1, size, file) != size)
            {
              save_errno = errno;
              save_dir = dir;
              write_failed = true;
              return false;
            }
            bytes += size;
            m_mb = static_cast<int>(bytes >> 20);
            m_total_mb = expected > 0 ? static_cast<int>((expected + (1 << 19)) >> 20) : 0;
            return true;
          },
          &error);
      if (fclose(file) != 0)
      {
        if (!write_failed)
        {
          save_errno = errno;
          save_dir = dir;
        }
        write_failed = true;
      }
      const bool whole = status >= 200 && status < 300 && bytes > 0 && (expected <= 0 || bytes == static_cast<size_t>(expected));
      if (whole || write_failed || m_stop || (status >= 400 && status < 500))
        break;  // (a refusal or "not there" is the server's answer: not tried again)
    }
    // What came: a zip ("PK\3\4"), all of it.
    bool zip = false;
    if (!write_failed && !m_stop && status >= 200 && status < 300 && bytes > 0)
    {
      if (FILE* file = fopen(temp.c_str(), "rb"))
      {
        char magic[4] = {};
        zip = fread(magic, 1, 4, file) == 4 && std::memcmp(magic, "PK\x03\x04", 4) == 0;
        fclose(file);
      }
    }
    const bool whole = expected <= 0 || bytes == static_cast<size_t>(expected);
    if (zip && whole)
    {
      std::rename(temp.c_str(), zip_path.c_str());
      LogF("cheats: libretro's collection downloaded (%zu bytes)", bytes);
    }
    else
    {
      std::remove(temp.c_str());
      zip_path = had_zip;  // ((01.92) the one there before, if any)
      if (m_stop)
        return;
#ifdef EDQUOT
      const bool full = save_errno == ENOSPC || save_errno == EDQUOT;
#else
      const bool full = save_errno == ENOSPC;
#endif
      const std::string reason =
          write_failed && full ? "it couldn't be saved in " + save_dir + ": the storage is full (it needs about 40 MB free)" :
          write_failed ? "it couldn't be saved in " + save_dir + ": " + std::strerror(save_errno != 0 ? save_errno : EIO) :
          status >= 200 && status < 300 && bytes > 0 && !zip ?
                         std::string("what came isn't the collection (a web page: does the network ask to sign in?)") :
          status >= 200 && status < 300 && !whole ? std::string("the connection was cut") :
                                                    Net::Plain(status, error);
      LogF("cheats: the collection couldn't be downloaded from %s into %s (HTTP %ld, %zu of %lld bytes, %s): %s", URL,
           temp.c_str(), status, bytes, static_cast<long long>(expected), error.empty() ? "no curl error" : error.c_str(),
           reason.c_str());
      if (!have)
      {
        fail("The cheat collection couldn't be downloaded", reason);
        return;
      }
      LogLine("cheats: the collection downloaded before is used");
    }
    m_state = State::Searching;
  }
  // The console's lists, the game's most like it first: by its file's name, then by its own title;
  // a PSP disc's ID before them.
  unzFile zip = unzOpen(zip_path.c_str());
  if (!zip)
  {
    std::remove(zip_path.c_str());  // a broken file: downloaded again next time
    fail("The cheat collection can't be read", "the file is damaged: it is downloaded again the next time");
    return;
  }
  std::vector<std::string> names, entries;
  const std::vector<std::string> folders = Folders(system);
  for (int result = unzGoToFirstFile(zip); result == UNZ_OK && !m_stop; result = unzGoToNextFile(zip))
  {
    char name[512];
    unz_file_info info;
    if (unzGetCurrentFileInfo(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
      continue;
    const std::string entry = name;
    if (entry.size() < 5 || entry.compare(entry.size() - 4, 4, ".cht") != 0)
      continue;
    for (const std::string& folder : folders)
    {
      if (entry.size() > folder.size() + 1 && entry.compare(0, folder.size(), folder) == 0 &&
          entry[folder.size()] == '/' && entry.find('/', folder.size() + 1) == std::string::npos)
      {
        names.push_back(entry.substr(folder.size() + 1, entry.size() - folder.size() - 5));
        entries.push_back(entry);
      }
    }
  }
  unzClose(zip);
  if (m_stop)
    return;
  const size_t slash = game_path.rfind('/');
  std::string stem = slash == std::string::npos ? game_path : game_path.substr(slash + 1);
  if (stem.rfind('.') != std::string::npos)
    stem = stem.substr(0, stem.rfind('.'));
  std::vector<std::string> found;
  auto add = [&found](const std::string& name) {
    if (found.size() < MAX_RESULTS && std::find(found.begin(), found.end(), name) == found.end())
      found.push_back(name);
  };
  if (system == SystemId::PSP)
  {
    const std::string id = PspDiscId(game_path);
    if (!id.empty())
    {
      LogF("cheats: the disc's ID is %s", id.c_str());
      for (const std::string& name : names)
      {
        if (name.find("[" + id + "]") != std::string::npos)
          add(name);
      }
    }
  }
  // The names most like the game's: the same game first (the same title before its groups), then
  // the game's own region; the demos, kiosk copies, betas and fan versions ("Mario Kart DS (USA)
  // (CTGP Nitro (v1.0.0))") last.
  std::vector<std::string> like = CoverDownload::Search(names, stem, 200);
  if (!title.empty() && title != stem)
  {
    for (const std::string& name : CoverDownload::Search(names, title, 60))
    {
      if (std::find(like.begin(), like.end(), name) == like.end())
        like.push_back(name);
    }
  }
  std::string game_title, region, tags;
  Library::ParseName(stem, &game_title, &region, &tags);
  // A title's letters and digits only, in lower case ("God Of War - Ghost Of Sparta [US]" ->
  // "godofwarghostofsparta").
  auto key = [](const std::string& text) {
    std::string out;
    for (const char c : text.substr(0, text.find_first_of("([")))
    {
      if (std::isalnum(static_cast<unsigned char>(c)))
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
  };
  const std::string game_key = key(stem), title_key = key(title);
  std::vector<std::string> marks;  // the region as the lists write it
  if (region.find("USA") != std::string::npos)
    marks = {"usa", "[us"};
  else if (region == "Japan")
    marks = {"japan", "[jp"};
  else if (!region.empty() && region != "World")
    marks = {"europe", "[eu"};
  if (region == "Spain")
    marks.insert(marks.end(), {"spain", "[es]", "(s)"});
  const bool umd = system == SystemId::PSP && !PspDiscId(game_path).empty();
  std::vector<std::pair<double, std::string>> ranked;
  for (size_t i = 0; i < like.size(); i++)
  {
    std::string name = like[i];
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    double score = -0.1 * static_cast<double>(i);  // the name's own likeness
    const std::string name_key = key(like[i]);
    if (!name_key.empty() && (name_key == game_key || name_key == title_key))
      score += 100.0;
    if (std::any_of(marks.begin(), marks.end(), [&name](const std::string& m) { return name.find(m) != std::string::npos; }))
      score += 15.0;
    for (const char* other : {"demo", "kiosk", "beta", "proto", "download station", "sample", "hack"})
    {
      if (name.find(other) != std::string::npos)
        score -= 30.0;
    }
    // A group inside a group: a fan-made version of the game.
    int depth = 0;
    for (const char c : name)
    {
      depth += c == '(' ? 1 : c == ')' ? -1 : 0;
      if (depth >= 2)
      {
        score -= 20.0;
        break;
      }
    }
    if (umd && (name.find("playstation store") != std::string::npos || name.find("psn") != std::string::npos))
      score -= 10.0;
    ranked.push_back({score, like[i]});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [score, name] : ranked)
    add(name);
  std::vector<std::string> found_entries;
  for (const std::string& name : found)
  {
    const auto at = std::find(names.begin(), names.end(), name);
    found_entries.push_back(entries[static_cast<size_t>(at - names.begin())]);
  }
  LogF("cheats: %zu lists of %zu for \"%s\"", found.size(), names.size(), stem.c_str());
  {
    std::lock_guard lock(m_mutex);
    m_zip_path = zip_path;
    // ((01.57) the N64 cartridge's own first, libretro's after it)
    m_results = std::move(cart_names);
    m_entries = std::move(cart_entries);
    m_cartridge = m_results.size();
    m_results.insert(m_results.end(), found.begin(), found.end());
    m_entries.insert(m_entries.end(), found_entries.begin(), found_entries.end());
  }
  m_state = State::Done;
}

bool Finder::Open(size_t result, std::string* error)
{
  if (m_state != State::Done || result >= m_entries.size())
    return false;
  std::string text;
  if (m_entries[result].rfind("mupencheat:", 0) == 0)
  {
    // ((01.57) the N64 cartridge's own list: its block of mupencheat.txt; what was left out said by a title line)
    const std::string list = ReadTextFile(s_n64_list);
    const size_t at = std::strtoull(m_entries[result].c_str() + 11, nullptr, 10);
    int left_out = 0;
    m_codes = at < list.size() ? ParseN64Cheats(list, at, &left_out) : std::vector<Cheats::Entry>();
    if (left_out > 0)
      m_codes.push_back({std::to_string(left_out) + (left_out == 1 ? " cheat left out: too many choices or lines" :
                                                                     " cheats left out: too many choices or lines"),
                         "", false});
    m_picked.assign(m_codes.size(), false);
    m_open_name = m_results[result];
    LogF("cheats: %s from mupencheat.txt: %zu entries, %d left out", m_open_name.c_str(), m_codes.size(), left_out);
    const bool any = m_codes.size() > (left_out > 0 ? 1u : 0u);
    if (!any)
      *error = "This list has no codes";
    return any;
  }
  if (m_system == SystemId::THREEDS)
  {
    std::lock_guard lock(m_mutex);
    text = m_3ds_text;  // ((01.26) downloaded by Run3ds: memory only)
  }
  else
  {
    unzFile zip = unzOpen(m_zip_path.c_str());
    if (!zip)
    {
      *error = "The cheat collection can't be read";
      return false;
    }
    text = ReadEntry(zip, m_entries[result], error);
    unzClose(zip);
  }
  if (text.empty())
    return false;
  // ((01.26) The Dreamcast's lists: addresses and values, made GameShark pairs.)
  m_codes = Cheats::Parse(text, m_system == SystemId::DC);
  for (Cheats::Entry& entry : m_codes)
    entry.enabled = false;
  m_picked.assign(m_codes.size(), false);
  m_open_name = m_results[result];
  if (m_codes.empty())
    *error = "This list has no codes";
  return !m_codes.empty();
}

bool Finder::IsTitle(size_t index) const
{
  if (index >= m_codes.size())
    return false;
  // Nothing but zeros (and the separators of the codes' formats) is no cheat.
  for (const char c : m_codes[index].code)
  {
    if (c != '0' && c != ' ' && c != '+' && c != '_' && c != 'L' && c != 'x' && c != 'X' && c != ':' && c != '-')
      return false;
  }
  return true;
}

void Finder::Pick(size_t index, bool on)
{
  if (index < m_picked.size() && !IsTitle(index))
    m_picked[index] = on;
}

void Finder::PickAll(bool on)
{
  for (size_t i = 0; i < m_picked.size(); i++)
    Pick(i, on);
}

int Finder::PickedCount() const
{
  return static_cast<int>(std::count(m_picked.begin(), m_picked.end(), true));
}

std::vector<Cheats::Entry> Finder::PickedCodes() const
{
  std::vector<Cheats::Entry> codes;
  for (size_t i = 0; i < m_codes.size(); i++)
  {
    if (Picked(i))
      codes.push_back(m_codes[i]);
  }
  return codes;
}
}  // namespace CheatDownload
