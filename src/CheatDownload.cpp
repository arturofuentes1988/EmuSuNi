// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "CheatDownload.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <thread>

#include <sys/stat.h>

#include <minizip/unzip.h>

#include "CoverDownload.h"
#include "Library.h"
#include "Log.h"
#include "Net.h"

namespace CheatDownload
{
namespace
{
constexpr const char* URL = "http://buildbot.libretro.com/assets/frontend/cheats.zip";
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
}  // namespace

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
  }
  m_mb = 0;
  m_total_mb = 0;
  m_state = State::Searching;
  m_thread = std::thread(&Finder::Run, this, data_dir, system, game_path, title);
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
  auto fail = [this](const std::string& error, const std::string& reason = {}) {
    std::lock_guard lock(m_mutex);
    m_error = error;
    m_reason = reason;
    m_state = State::Failed;
    LogLine("cheats: " + error + (reason.empty() ? "" : ": " + reason));
  };
  // The collection: kept for a month; a failed download keeps an older one.
  const std::string dir = data_dir + "/cache";
  const std::string zip_path = dir + "/cheats.zip";
  MakeDirs(dir);
  struct stat st;
  const bool have = stat(zip_path.c_str(), &st) == 0 && st.st_size > 0;
  if (!have || std::time(nullptr) - st.st_mtime > MAX_AGE)
  {
    // 00.68 (users' "error" downloading it, nothing more said): no limit for the whole download any more (600 s
    // stopped any connection under 62 KB/s: the collection is 35 MB), only for a minute without a byte (Net); a cut
    // or a server's passing failure tried again twice; what came checked to be the zip, whole; and the reason said.
    m_state = State::Downloading;
    const std::string temp = zip_path + ".new";
    long status = 0;
    std::string error;
    size_t bytes = 0;
    bool write_failed = false;
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
        write_failed = true;
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
      if (m_stop)
        return;
      const std::string reason =
          write_failed ? std::string("it couldn't be saved: the PS4's storage may be full (it needs about 40 MB free)") :
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
    m_results = std::move(found);
    m_entries = std::move(found_entries);
  }
  m_state = State::Done;
}

bool Finder::Open(size_t result, std::string* error)
{
  if (m_state != State::Done || result >= m_entries.size())
    return false;
  unzFile zip = unzOpen(m_zip_path.c_str());
  if (!zip)
  {
    *error = "The cheat collection can't be read";
    return false;
  }
  const std::string text = ReadEntry(zip, m_entries[result], error);
  unzClose(zip);
  if (text.empty())
    return false;
  m_codes = Cheats::Parse(text);
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
