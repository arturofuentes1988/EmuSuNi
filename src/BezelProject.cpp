// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "BezelProject.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <unordered_map>

#include <orbis/libkernel.h>
#include <zlib.h>

#include "Arcade.h"
#include "CoverDownload.h"
#include "Log.h"
#include "Naomi.h"
#include "Net.h"
#include "Saves.h"
#include "Systems.h"

extern "C" int emusuni_disk_space_known(const char* path, int64_t* out);  // port/ps4_diskfree.c

namespace BezelProject
{
namespace
{
constexpr const char* OWNER = "thebezelproject";
constexpr const char* BRANCH = "master";  // (every project's: its RetroPie script clones them so)
constexpr int64_t MIB = 1024 * 1024;
constexpr int64_t DAY = 24 * 3600;
constexpr int64_t SPARE = 64 * MIB;  // free space kept on the drive besides the bezels
constexpr const char* RECORD_HEAD =
    "# The Bezel Project's bezels downloaded by EmuSuNi (Settings -> Game Bezels): file, bezel, bytes, Git hash.\n"
    "# Delete Downloaded Bezels deletes these files (as they were written) and nothing else.\n";

void Log(const std::string& text)
{
  LogLine("bezels: " + text);
}

std::string Lower(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string Mb(int64_t bytes)
{
  return std::to_string((bytes + MIB / 2) / MIB);
}

std::vector<std::string> Lines(const std::string& text)
{
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> Fields(const std::string& line)
{
  std::vector<std::string> fields;
  size_t start = 0;
  for (;;)
  {
    const size_t tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos)
      return fields;
    start = tab + 1;
  }
}

int64_t Number(const std::string& text)
{
  return std::strtoll(text.c_str(), nullptr, 10);  // (never std::stoll: an exception would close the app)
}

// ---- JSON (GitHub's tree listings) ----
int HexDigit(char c)
{
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// Four hex digits at json[at], else -1.
long Hex4(const std::string& json, size_t at)
{
  if (at + 4 > json.size())
    return -1;
  long value = 0;
  for (size_t i = at; i < at + 4; i++)
  {
    const int digit = HexDigit(json[i]);
    if (digit < 0)
      return -1;
    value = value * 16 + digit;
  }
  return value;
}

void AppendUtf8(std::string* out, uint32_t code)
{
  if (code < 0x80)
  {
    *out += static_cast<char>(code);
  }
  else if (code < 0x800)
  {
    *out += static_cast<char>(0xC0 | (code >> 6));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
  else if (code < 0x10000)
  {
    *out += static_cast<char>(0xE0 | (code >> 12));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
  else
  {
    *out += static_cast<char>(0xF0 | (code >> 18));
    *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// The JSON string whose opening quote is json[*at]: its text (escapes undone, \u as UTF-8); *at ends past its closing
// quote.
std::string JsonString(const std::string& json, size_t* at)
{
  std::string out;
  size_t i = *at + 1;
  while (i < json.size() && json[i] != '"')
  {
    if (json[i] != '\\' || i + 1 >= json.size())
    {
      out += json[i++];
      continue;
    }
    const char escaped = json[i + 1];
    i += 2;
    switch (escaped)
    {
    case 'n': out += '\n'; break;
    case 't': out += '\t'; break;
    case 'r': out += '\r'; break;
    case 'b': out += '\b'; break;
    case 'f': out += '\f'; break;
    case 'u':
    {
      long code = Hex4(json, i);
      if (code < 0)
      {
        out += 'u';
        break;
      }
      i += 4;
      if (code >= 0xD800 && code < 0xDC00 && i + 6 <= json.size() && json[i] == '\\' && json[i + 1] == 'u')
      {
        const long low = Hex4(json, i + 2);
        if (low >= 0xDC00 && low < 0xE000)
        {
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
      }
      AppendUtf8(&out, static_cast<uint32_t>(code));
      break;
    }
    default: out += escaped;  // \" \\ \/
    }
  }
  *at = i + 1;
  return out;
}

// ---- SHA-1 (a downloaded file checked against the Git hash in GitHub's list) ----
class Sha1
{
public:
  void Add(const void* data, size_t size)
  {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    m_total += size;
    while (size > 0)
    {
      const size_t take = std::min(size, sizeof(m_block) - m_used);
      std::memcpy(m_block + m_used, bytes, take);
      m_used += take;
      bytes += take;
      size -= take;
      if (m_used == sizeof(m_block))
      {
        Block();
        m_used = 0;
      }
    }
  }
  std::string Hex()
  {
    const uint64_t bits = m_total * 8;
    const uint8_t one = 0x80, zero = 0;
    Add(&one, 1);
    while (m_used != 56)
      Add(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++)
      length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Add(length, 8);
    char hex[41];
    for (int i = 0; i < 5; i++)
      std::snprintf(hex + 8 * i, 9, "%08x", m_h[i]);
    return std::string(hex, 40);
  }

private:
  static uint32_t Rol(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }
  void Block()
  {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
    {
      w[i] = static_cast<uint32_t>(m_block[4 * i]) << 24 | static_cast<uint32_t>(m_block[4 * i + 1]) << 16 |
             static_cast<uint32_t>(m_block[4 * i + 2]) << 8 | m_block[4 * i + 3];
    }
    for (int i = 16; i < 80; i++)
      w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
    for (int i = 0; i < 80; i++)
    {
      uint32_t f, k;
      if (i < 20)
      {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      }
      else if (i < 40)
      {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      }
      else if (i < 60)
      {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      }
      else
      {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t t = Rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rol(b, 30);
      b = a;
      a = t;
    }
    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
  }
  uint32_t m_h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t m_block[64] = {};
  size_t m_used = 0;
  uint64_t m_total = 0;
};

// A gzip answer (GitHub's lists come so: about an eighth of their size) made plain again.
bool Gunzip(const std::string& in, std::string* out)
{
  z_stream zs = {};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
    return false;
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  out->clear();
  char buffer[16384];
  int result = Z_OK;
  while (result == Z_OK)
  {
    zs.next_out = reinterpret_cast<Bytef*>(buffer);
    zs.avail_out = sizeof(buffer);
    result = inflate(&zs, Z_NO_FLUSH);
    out->append(buffer, sizeof(buffer) - zs.avail_out);
  }
  inflateEnd(&zs);
  return result == Z_STREAM_END;
}

// ---- A project's list, kept on the PS4 ----
std::string CacheDir(const std::string& data_dir)
{
  return data_dir + "/cache/bezels";
}

std::string CachePath(const std::string& data_dir, const Source& source)
{
  return CacheDir(data_dir) + "/" + source.repo + ".txt";
}

struct Cached
{
  std::vector<Entry> entries;
  std::string etag;
  int64_t date = 0;  // when it came (or GitHub last said it was the same)
  bool there = false;
};

Cached ReadCache(const std::string& path, int source)
{
  Cached cached;
  const std::string text = ReadTextFile(path);
  for (const std::string& line : Lines(text))
  {
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "etag" && f.size() >= 2)
      cached.etag = f[1];
    else if (f[0] == "date" && f.size() >= 2)
      cached.date = Number(f[1]);
    else if (f[0] == "png" && f.size() >= 4 && !f[1].empty())
      cached.entries.push_back({f[1], Number(f[2]), f[3], source});
  }
  cached.there = !cached.entries.empty();
  return cached;
}

bool WriteCache(const std::string& path, const Source& source, const Cached& cached)
{
  std::string text = std::string("# EmuSuNi: the bezels of github.com/") + OWNER + "/bezelproject-" + source.repo + " (" +
                     source.folder + "), as GitHub's list gave them: name, bytes, Git hash.\n";
  text += "etag\t" + cached.etag + "\n";
  text += "date\t" + std::to_string(cached.date) + "\n";
  for (const Entry& entry : cached.entries)
    text += "png\t" + entry.name + "\t" + std::to_string(entry.size) + "\t" + entry.sha + "\n";
  return WriteTextFile(path, text);
}

struct ListAnswer
{
  std::vector<Entry> entries;
  std::string error;  // none came: why, in plain words
  bool limit = false; // GitHub's limit for the hour was reached
  int64_t reset = 0;  // when it is reset (Unix time)
};

// A project's list: the copy kept (a day old at most, or any age when `fresh` isn't asked for), else GitHub's API -
// with the copy's ETag: "the same" (304) downloads nothing again (it still counts as one of the hour's 60 requests:
// GitHub's conditional requests are free only with an account's token) -, else the copy as it is.
ListAnswer ReadList(const std::string& data_dir, const Source& source, int index, bool fresh, Net::Session* session,
                    std::atomic<bool>* cancel)
{
  ListAnswer answer;
  const std::string cache = CachePath(data_dir, source);
  Cached cached = ReadCache(cache, index);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (cached.there && (!fresh || (now - cached.date >= 0 && now - cached.date < DAY)))
  {
    Log(std::string(source.repo) + ": the list kept (" + std::to_string(cached.entries.size()) + " bezels, " +
        std::to_string((now - cached.date) / 3600) + " h old)");
    answer.entries = std::move(cached.entries);
    return answer;
  }
  Net::Request request;
  request.url = std::string("https://api.github.com/repos/") + OWNER + "/bezelproject-" + source.repo + "/git/trees/" +
                BRANCH + ":" + source.folder;
  request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28",
                     "Accept-Encoding: gzip"};
  if (cached.there && !cached.etag.empty())
    request.headers.push_back("If-None-Match: " + cached.etag);
  request.timeout_seconds = 90;
  request.max_bytes = 48 << 20;
  request.cancel = cancel;
  const auto start = std::chrono::steady_clock::now();
  const Net::Response reply = Net::Fetch(request, session);
  const long long ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  const std::string left = reply.Header("X-RateLimit-Remaining");
  const std::string hour = left.empty() ? std::string() : ", " + left + " of GitHub's requests left this hour";
  if (reply.status == 304 && cached.there)
  {
    Log(std::string(source.repo) + ": GitHub's list is the same (304, " + std::to_string(ms) + " ms" + hour + ")");
    cached.date = now;
    WriteCache(cache, source, cached);
    answer.entries = std::move(cached.entries);
    return answer;
  }
  if (reply.ok())
  {
    std::string json;
    const std::string& body = reply.body;
    const bool zipped = body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x1f &&
                        static_cast<unsigned char>(body[1]) == 0x8b;
    if (zipped && !Gunzip(body, &json))
    {
      Log(std::string(source.repo) + ": GitHub's list came damaged (gzip)");
      json.clear();
    }
    else if (!zipped)
    {
      json = body;
    }
    bool truncated = false;
    std::vector<Entry> entries = ParseTree(json, &truncated);
    for (Entry& entry : entries)
      entry.source = index;
    Log(std::string(source.repo) + ": GitHub's list: " + std::to_string(entries.size()) + " bezels (" +
        std::to_string(body.size()) + " bytes" + (zipped ? " gzip, " + std::to_string(json.size()) + " plain" : "") +
        ", " + std::to_string(ms) + " ms" + hour + (truncated ? ", CUT SHORT by GitHub" : "") + ")");
    if (!entries.empty())
    {
      cached.entries = std::move(entries);
      cached.etag = reply.Header("ETag");
      cached.date = now;
      MakeDirs(CacheDir(data_dir));
      WriteCache(cache, source, cached);
      answer.entries = std::move(cached.entries);
      return answer;
    }
    answer.error = "GitHub's list of The Bezel Project's bezels couldn't be read";
  }
  else if ((reply.status == 403 || reply.status == 429) && left == "0")
  {
    answer.limit = true;
    answer.reset = Number(reply.Header("X-RateLimit-Reset"));
    answer.error = "GitHub's limit of 60 lists an hour was reached";
    Log(std::string(source.repo) + ": GitHub's limit for the hour was reached (HTTP " + std::to_string(reply.status) +
        ", it resets at " + reply.Header("X-RateLimit-Reset") + ")");
  }
  else
  {
    answer.error = reply.plain.empty() ? Net::Plain(reply.status, reply.error) : reply.plain;
    Log(std::string(source.repo) + ": GitHub's list didn't come (HTTP " + std::to_string(reply.status) +
        (reply.error.empty() ? std::string() : ", " + reply.error) + ")");
  }
  if (cached.there)
  {
    Log(std::string(source.repo) + ": the list kept is used (" + std::to_string(cached.entries.size()) + " bezels)");
    answer.entries = std::move(cached.entries);
    answer.error.clear();
  }
  return answer;
}

// ---- The record of the bezels downloaded ----
struct Recorded
{
  std::string bezel;
  int64_t size = 0;
  std::string sha;
};

std::map<std::string, Recorded> ReadRecord(const std::string& path)
{
  std::map<std::string, Recorded> record;
  for (const std::string& line : Lines(ReadTextFile(path)))
  {
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> f = Fields(line);
    if (f.size() >= 4 && !f[0].empty() && f[0].find('/') == std::string::npos)
      record[f[0]] = {f[1], Number(f[2]), f[3]};
  }
  return record;
}

bool AppendRecord(const std::string& path, const std::string& line)
{
  const bool there = FileExists(path);
  FILE* file = std::fopen(path.c_str(), "ab");
  if (!file)
    return false;
  bool ok = true;
  if (!there)
    ok = std::fputs(RECORD_HEAD, file) >= 0;
  ok = std::fputs(line.c_str(), file) >= 0 && ok;
  ok = std::fflush(file) == 0 && ok;
  ok = fsync(fileno(file)) == 0 && ok;
  ok = std::fclose(file) == 0 && ok;
  return ok;
}

int64_t FileSize(const std::string& path)
{
  struct stat info = {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) ? static_cast<int64_t>(info.st_size) : -1;
}

bool IsPng(const std::string& path)
{
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;
  unsigned char head[8] = {};
  const bool png = std::fread(head, 1, 8, file) == 8 && std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0;
  std::fclose(file);
  return png;
}

// ---- The download that didn't finish ----
std::string PendingPath(const std::string& data_dir)
{
  return CacheDir(data_dir) + "/pending.txt";
}

void WritePending(const std::string& data_dir, const std::vector<std::string>& consoles)
{
  const std::string path = PendingPath(data_dir);
  if (consoles.empty())
  {
    std::remove(path.c_str());
    return;
  }
  std::string text;
  for (const std::string& key : consoles)
    text += key + "\n";
  MakeDirs(CacheDir(data_dir));
  WriteTextFile(path, text);
}

// "18:40" (the PS4's time zone) for a Unix time.
std::string Clock(int64_t when)
{
  const time_t t = static_cast<time_t>(when);
  struct tm local = {};
  if (when <= 0 || !localtime_r(&t, &local))
    return "an hour";
  char text[16];
  std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
  return text;
}
}  // namespace

// ---- The consoles' projects ----
std::vector<Source> Sources(const std::string& console)
{
  // The Super Nintendo's games are the SNES project's (America, Europe) and the SFC one's (Japan); the NES's the NES,
  // Famicom and Famicom Disk System ones'; a Game Boy game in the Game Boy Color's folder (and the other way) is in the
  // other project. The arcade's are named after the MAME sets (FinalBurn Neo's are named so too).
  static const std::map<std::string, std::vector<Source>> SOURCES = {
      {"snes", {{"SNES", "retroarch/overlay/GameBezels/SNES"}, {"SFC", "retroarch/overlay/GameBezels/SFC"}}},
      {"nes",
       {{"NES", "retroarch/overlay/GameBezels/NES"},
        {"Famicom", "retroarch/overlay/GameBezels/Famicom"},
        {"FDS", "retroarch/overlay/GameBezels/FDS"}}},
      {"n64", {{"N64", "retroarch/overlay/GameBezels/N64"}}},
      {"gb", {{"GB", "retroarch/overlay/GameBezels/GB"}, {"GBC", "retroarch/overlay/GameBezels/GBC"}}},
      {"gbc", {{"GBC", "retroarch/overlay/GameBezels/GBC"}, {"GB", "retroarch/overlay/GameBezels/GB"}}},
      {"gba", {{"GBA", "retroarch/overlay/GameBezels/GBA"}}},
      {"ps1", {{"PSX", "retroarch/overlay/GameBezels/PSX"}}},
      {"nds", {{"NDS", "retroarch/overlay/GameBezels/NDS"}}},
      {"dc", {{"Dreamcast", "retroarch/overlay/GameBezels/Dreamcast"}}},
      {"sg1000", {{"SG-1000", "retroarch/overlay/GameBezels/SG-1000"}}},
      {"sms", {{"MasterSystem", "retroarch/overlay/GameBezels/MasterSystem"}}},
      {"gg", {{"GameGear", "retroarch/overlay/GameBezels/GameGear"}}},
      {"md", {{"MegaDrive", "retroarch/overlay/GameBezels/MegaDrive"}}},
      {"arcade", {{"MAME", "retroarch/overlay/ArcadeBezels"}}},
      {"3ds", {{"N3DS", "retroarch/overlay/GameBezels/N3DS"}}},
      {"naomi", {{"Naomi", "retroarch/overlay/GameBezels/Naomi"}}},
      {"atomiswave", {{"Atomiswave", "retroarch/overlay/GameBezels/Atomiswave"}}},
      {"saturn", {{"Saturn", "retroarch/overlay/GameBezels/Saturn"}}},
      // (00.82) The PC Engine's are the PC Engine and TurboGrafx-16 projects' (Japan's, America's).
      {"pce", {{"PCEngine", "retroarch/overlay/GameBezels/PCEngine"}, {"TG16", "retroarch/overlay/GameBezels/TG16"}}},
  };
  const auto it = SOURCES.find(console);
  return it == SOURCES.end() ? std::vector<Source>{} : it->second;
}

bool List(const std::string& data_dir, const std::string& console, bool fresh, std::vector<Entry>* entries,
          std::string* error, int64_t* reset, Net::Session* session, std::atomic<bool>* cancel)
{
  entries->clear();
  error->clear();
  *reset = 0;
  const std::vector<Source> sources = Sources(console);
  if (sources.empty())
  {
    *error = "The Bezel Project has no bezels for this console's games";
    return false;
  }
  for (size_t s = 0; s < sources.size(); s++)
  {
    ListAnswer list = ReadList(data_dir, sources[s], static_cast<int>(s), fresh, session, cancel);
    if (list.limit)
      *reset = std::max(*reset, list.reset);
    if (list.entries.empty())
    {
      *error = list.error.empty() ? std::string("GitHub's list of The Bezel Project's bezels couldn't be read") :
                                    list.error;
      entries->clear();
      return false;
    }
    for (Entry& entry : list.entries)
      entries->push_back(std::move(entry));
  }
  return true;
}

std::vector<Entry> ParseTree(const std::string& json, bool* truncated)
{
  std::vector<Entry> entries;
  *truncated = false;
  {
    const size_t at = json.rfind("\"truncated\"");
    if (at != std::string::npos)
    {
      const size_t value = json.find_first_not_of(" \t\r\n:", at + 11);
      *truncated = value != std::string::npos && json.compare(value, 4, "true") == 0;
    }
  }
  const size_t tree = json.find("\"tree\"");
  if (tree == std::string::npos)
    return entries;
  size_t i = json.find('[', tree);
  if (i == std::string::npos)
    return entries;
  i++;
  // Each object of the array, by its "path", "type", "size" and "sha" (its strings read whole: a name may hold any
  // sign).
  while (i < json.size() && json[i] != ']')
  {
    if (json[i] == '"')
    {
      JsonString(json, &i);  // (a string between the objects: none in GitHub's lists)
      continue;
    }
    if (json[i] != '{')
    {
      i++;
      continue;
    }
    i++;
    std::string path, type, sha;
    int64_t size = -1;
    while (i < json.size() && json[i] != '}')
    {
      if (json[i] != '"')
      {
        i++;
        continue;
      }
      const std::string key = JsonString(json, &i);
      while (i < json.size() && (json[i] == ' ' || json[i] == ':' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
        i++;
      if (i < json.size() && json[i] == '"')
      {
        std::string value = JsonString(json, &i);
        if (key == "path")
          path = std::move(value);
        else if (key == "type")
          type = std::move(value);
        else if (key == "sha")
          sha = std::move(value);
      }
      else
      {
        const size_t start = i;
        while (i < json.size() && json[i] != ',' && json[i] != '}')
          i++;
        if (key == "size")
          size = Number(json.substr(start, i - start));
      }
    }
    if (i < json.size())
      i++;  // (its '}')
    if (type == "blob" && size >= 0 && path.size() > 4 && Lower(path.substr(path.size() - 4)) == ".png" &&
        path.find('/') == std::string::npos && path.find('\n') == std::string::npos &&
        path.find('\t') == std::string::npos)
    {
      entries.push_back({path.substr(0, path.size() - 4), size, sha, 0});
    }
  }
  return entries;
}

std::string GitBlobSha(const std::string& path)
{
  const int64_t size = FileSize(path);
  FILE* file = size >= 0 ? std::fopen(path.c_str(), "rb") : nullptr;
  if (!file)
    return {};
  Sha1 sha;
  const std::string head = "blob " + std::to_string(size);
  sha.Add(head.c_str(), head.size() + 1);  // (with its '\0')
  char buffer[16384];
  size_t read;
  int64_t total = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
  {
    sha.Add(buffer, read);
    total += static_cast<int64_t>(read);
  }
  std::fclose(file);
  return total == size ? sha.Hex() : std::string();
}

// ---- Matching ----
struct Matcher::Index
{
  std::shared_ptr<const CoverDownload::Prepared> prepared;
  std::unordered_map<std::string, size_t> sets;  // a name in lower case -> its entry (the romsets' way)
};

Matcher::Matcher(std::vector<Entry> entries)
{
  // A name in two projects (the SNES's and the SFC's): the first project's.
  std::set<std::string> seen;
  for (Entry& entry : entries)
  {
    if (seen.insert(entry.name).second)
      m_entries.push_back(std::move(entry));
  }
  m_names.reserve(m_entries.size());
  for (const Entry& entry : m_entries)
    m_names.push_back(entry.name);
  m_index = new Index;
  m_index->prepared = CoverDownload::Prepare(m_names);
  for (size_t i = 0; i < m_entries.size(); i++)
    m_index->sets.emplace(Lower(m_entries[i].name), i);
}

Matcher::~Matcher()
{
  delete m_index;
}

const Entry* Matcher::Find(const Library::Game& game, bool romset, const std::vector<std::string>& others,
                           std::string* how) const
{
  how->clear();
  if (romset)
  {
    std::vector<std::string> names = {game.id};
    names.insert(names.end(), others.begin(), others.end());
    for (size_t i = 0; i < names.size(); i++)
    {
      const auto it = m_index->sets.find(Lower(names[i]));
      if (it != m_index->sets.end())
      {
        *how = i == 0 ? "its set's name" : "its parent's set, " + names[i];
        return &m_entries[it->second];
      }
    }
    return nullptr;
  }
  const std::string* name = CoverDownload::Match(*m_index->prepared, game, how);
  if (!name || name < m_names.data() || name >= m_names.data() + m_names.size())
    return nullptr;
  return &m_entries[static_cast<size_t>(name - m_names.data())];
}

// ---- What was downloaded ----
std::string RecordPath(const std::string& data_dir, const std::string& console)
{
  return data_dir + "/bezels/" + console + "/bezelproject.txt";
}

Downloaded Count(const std::string& data_dir, const std::string& console)
{
  Downloaded count;
  const std::string dir = data_dir + "/bezels/" + console + "/";
  for (const auto& [file, recorded] : ReadRecord(RecordPath(data_dir, console)))
  {
    const int64_t size = FileSize(dir + file);
    if (size >= 0 && size == recorded.size)
    {
      count.files++;
      count.bytes += size;
    }
  }
  return count;
}

Downloaded DeleteAll(const std::string& data_dir, const std::vector<std::string>& consoles)
{
  Downloaded deleted;
  for (const std::string& console : consoles)
  {
    const std::string dir = data_dir + "/bezels/" + console + "/";
    const std::string record = RecordPath(data_dir, console);
    if (!FileExists(record))
      continue;
    int files = 0, kept = 0;
    for (const auto& [file, recorded] : ReadRecord(record))
    {
      const int64_t size = FileSize(dir + file);
      if (size < 0)
        continue;
      if (size != recorded.size)
      {
        kept++;  // (changed since: the player's own now)
        continue;
      }
      if (std::remove((dir + file).c_str()) == 0)
      {
        files++;
        deleted.files++;
        deleted.bytes += size;
      }
      // (A .part of the same name: a download of it that was cut.)
      std::remove((dir + file + ".part").c_str());
    }
    std::remove(record.c_str());
    Log(console + ": " + std::to_string(files) + " downloaded bezels deleted" +
        (kept > 0 ? ", " + std::to_string(kept) + " changed since kept" : std::string()));
  }
  // The downloads' parts of bezels never finished (only they make "<name>.png.part" there).
  for (const std::string& console : consoles)
  {
    const std::string dir = data_dir + "/bezels/" + console;
    if (!FileExists(dir))
      continue;
    // (Their names are those of the games: the console's pending list says nothing of them, so the folder is read.)
    std::vector<std::string> parts;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
      const std::string name = it->path().filename().string();
      if (name.size() > 9 && Lower(name.substr(name.size() - 9)) == ".png.part")
        parts.push_back(dir + "/" + name);
    }
    for (const std::string& part : parts)
    {
      const int64_t size = FileSize(part);
      if (std::remove(part.c_str()) == 0 && size > 0)
        deleted.bytes += size;
    }
  }
  WritePending(data_dir, {});
  Log("deleted: " + std::to_string(deleted.files) + " bezels, " + Mb(deleted.bytes) + " MB");
  return deleted;
}

// ---- The download ----
std::vector<std::string> Downloader::Pending(const std::string& data_dir)
{
  std::vector<std::string> consoles;
  for (const std::string& line : Lines(ReadTextFile(PendingPath(data_dir))))
  {
    if (!line.empty() && line[0] != '#' && Has(line) &&
        std::find(consoles.begin(), consoles.end(), line) == consoles.end())
      consoles.push_back(line);
  }
  return consoles;
}

bool Downloader::Start(const std::string& data_dir, const std::vector<Library::Game>& games,
                       const std::vector<std::string>& consoles, bool resumed)
{
  if (m_busy)
    return false;
  if (m_started)
  {
    pthread_join(m_thread, nullptr);
    m_started = false;
  }
  m_data_dir = data_dir;
  m_resumed = resumed;
  m_cancel = false;
  m_forget = false;
  m_ended = false;
  m_work.clear();
  Progress progress;
  std::vector<std::string> pending = Pending(data_dir);
  for (const std::string& key : consoles)
  {
    const System* system = SystemByKey(key);
    if (!system || !Has(key))
      continue;
    std::vector<Job> jobs;
    for (const Library::Game& game : games)
    {
      if (game.system != system->id)
        continue;
      Job job{game, {}};
      // A romset's parent (a clone's bezel is often its parent's), looked for here, on the menu's thread.
      if (system->id == SystemId::ARCADE)
      {
        if (const Arcade::Driver* driver = Arcade::Find(game.id); driver && !driver->parent.empty())
          job.others.push_back(driver->parent);
      }
      else if (IsFlycastBoard(system->id))
      {
        if (const Naomi::Set* set = Naomi::Find(game.id); set && !set->parent.empty())
          job.others.push_back(set->parent);
      }
      jobs.push_back(std::move(job));
    }
    if (jobs.empty())
      continue;
    Console console;
    console.key = key;
    console.name = system->name;
    console.games = static_cast<int>(jobs.size());
    progress.consoles.push_back(console);
    m_work.emplace_back(key, std::move(jobs));
    if (std::find(pending.begin(), pending.end(), key) == pending.end())
      pending.push_back(key);
  }
  if (m_work.empty())
    return false;
  WritePending(data_dir, pending);
  progress.running = true;
  {
    std::lock_guard lock(m_mutex);
    m_progress = std::move(progress);
  }
  Changed();
  m_busy = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);  // (the app's other small threads' size: libthr keeps one for them)
  m_started = pthread_create(&m_thread, &attr, &Downloader::Main, this) == 0;
  pthread_attr_destroy(&attr);
  if (!m_started)
  {
    m_busy = false;
    std::lock_guard lock(m_mutex);
    m_progress.running = false;
    m_progress.error = "the app couldn't start it (no thread)";
    Log("no thread to download the bezels");
    return false;
  }
  Log(std::string(resumed ? "the download cut before goes on: " : "downloading the bezels of ") +
      std::to_string(m_work.size()) + (m_work.size() == 1 ? " console" : " consoles"));
  return true;
}

void Downloader::Stop(bool forget)
{
  if (!m_started)
    return;
  if (forget)
    m_forget = true;
  m_cancel = true;
  pthread_join(m_thread, nullptr);
  m_started = false;
  m_busy = false;
}

Downloader::Progress Downloader::Now() const
{
  std::lock_guard lock(m_mutex);
  Progress progress = m_progress;
  progress.serial = m_serial;
  return progress;
}

void Downloader::Changed()
{
  m_serial++;
}

bool Downloader::Wait()
{
  bool told = false;
  while (m_pause && !m_cancel)
  {
    if (!told)
    {
      {
        std::lock_guard lock(m_mutex);
        m_progress.paused = true;
      }
      Changed();
      told = true;
    }
    sceKernelUsleep(200000);
  }
  if (told)
  {
    {
      std::lock_guard lock(m_mutex);
      m_progress.paused = false;
    }
    Changed();
  }
  return !m_cancel;
}

void* Downloader::Main(void* self)
{
  static_cast<Downloader*>(self)->Run();
  return nullptr;
}

void Downloader::Run()
{
  const auto started = std::chrono::steady_clock::now();
  struct Item
  {
    size_t console;
    const Job* job;
    Entry entry;
    std::string target;
    int64_t part;  // its part there already (a download cut before): counted as come
  };
  std::vector<Item> items;
  // Each console's matchers' sources (its projects) for the addresses, and whether its lists all came.
  std::vector<std::vector<Source>> sources(m_work.size());
  std::vector<bool> listed(m_work.size(), false);
  Net::Session api;  // (api.github.com's connection, kept for the next list; raw.githubusercontent.com's below)
  auto set = [this](size_t c, auto&& change) {
    {
      std::lock_guard lock(m_mutex);
      change(m_progress.consoles[c]);
    }
    Changed();
  };
  // 1. Each console's lists, and its games matched with them.
  for (size_t c = 0; c < m_work.size() && Wait(); c++)
  {
    const std::string& key = m_work[c].first;
    const System* system = SystemByKey(key);
    set(c, [](Console& console) { console.step = Step::Listing; });
    sources[c] = Sources(key);
    std::vector<Entry> all;
    std::string error;
    int64_t reset = 0;
    if (!List(m_data_dir, key, !m_resumed, &all, &error, &reset, &api, &m_cancel))
    {
      // (Every list or none: a game matched with some of them could get another game's bezel.)
      if (m_cancel)
        error = "stopped";
      else if (reset > 0)
        error += ": again after " + Clock(reset);
      Log(key + ": no bezels this time: " + error);
      set(c, [&error](Console& console) {
        console.step = Step::Failed;
        console.error = error;
      });
      continue;
    }
    listed[c] = true;
    const Matcher matcher(std::move(all));
    const std::string dir = m_data_dir + "/bezels/" + key;
    MakeDirs(dir);
    const std::map<std::string, Recorded> record = ReadRecord(RecordPath(m_data_dir, key));
    const bool romset = system && IsRomsetSystem(system->id);
    int matched = 0, already = 0, own = 0, none = 0;
    int64_t bytes = 0;
    std::set<std::string> targets;  // (two games of one name: one file)
    for (const Job& job : m_work[c].second)
    {
      const std::string file = job.game.id + ".png";
      const std::string target = dir + "/" + file;
      if (!targets.insert(file).second)
        continue;
      const auto recorded = record.find(file);
      const int64_t size = FileSize(target);
      if (recorded != record.end() && size == recorded->second.size)
      {
        matched++;
        already++;
        continue;
      }
      if (size >= 0 && recorded == record.end())
      {
        own++;  // (a bezel of the player's own, named after the game: Your Own's; never written over)
        continue;
      }
      std::string how;
      const Entry* entry = matcher.Find(job.game, romset, job.others, &how);
      if (!entry)
      {
        none++;
        continue;
      }
      matched++;
      int64_t part = FileSize(target + ".part");
      if (part > entry->size)
      {
        std::remove((target + ".part").c_str());
        part = 0;
      }
      part = std::max<int64_t>(0, part);
      bytes += entry->size - part;
      if (how != "same name" && how != "its set's name")
        Log(key + ": " + job.game.id + " -> " + entry->name + " (" + how + ")");
      items.push_back({c, &job, *entry, target, part});
    }
    Log(key + ": " + std::to_string(m_work[c].second.size()) + " games, " + std::to_string(matched) +
        " with a bezel at The Bezel Project (" + std::to_string(already) + " downloaded before), " +
        std::to_string(none) + " without one" + (own > 0 ? ", " + std::to_string(own) + " of your own kept" : "") +
        (bytes > 0 ? "; " + Mb(bytes) + " MB to download" : std::string()));
    set(c, [&](Console& console) {
      console.matched = matched;
      console.already = already;
      console.own = own;
      console.bytes = bytes;
      console.step = bytes > 0 || matched > already ? Step::Downloading : Step::Done;
    });
  }
  // 2. Room for them all.
  int64_t total = 0;
  for (const Item& item : items)
    total += item.entry.size - item.part;
  bool stop = m_cancel;
  int64_t free = -1;
  if (!stop && total > 0 && emusuni_disk_space_known(m_data_dir.c_str(), &free) && free < total + SPARE)
  {
    const std::string error = "Not enough free space: " + Mb(total + SPARE) + " MB needed, " + Mb(free) + " MB free";
    Log(error + " (" + m_data_dir + ")");
    {
      std::lock_guard lock(m_mutex);
      m_progress.error = error;
      for (Console& console : m_progress.consoles)
      {
        if (console.step == Step::Downloading)
          console.step = Step::Waiting;
      }
    }
    Changed();
    stop = true;
    items.clear();
    // (Not resumed by itself: the player makes room and asks again.)
    std::fill(listed.begin(), listed.end(), true);
  }
  else if (total > 0)
  {
    Log(Mb(total) + " MB to download" + (free >= 0 ? " (" + Mb(free) + " MB free)" : std::string()));
  }
  // 3. The files, one at a time.
  Net::Session raw;
  std::vector<int> left(m_work.size(), 0);
  for (const Item& item : items)
    left[item.console]++;
  int cuts = 0;  // connections failed in a row
  for (size_t i = 0; i < items.size() && !stop; i++)
  {
    const Item& item = items[i];
    const Source& source = sources[item.console][static_cast<size_t>(item.entry.source)];
    const std::string& key = m_work[item.console].first;
    {
      std::lock_guard lock(m_mutex);
      m_progress.game = item.job->game.title;
    }
    Changed();
    int64_t before = 0;  // the console's bytes counted before this file
    {
      std::lock_guard lock(m_mutex);
      before = m_progress.consoles[item.console].done;
    }
    bool ok = false;
    std::string why;
    int slowed = 0;  // GitHub's "too many requests" waits for this file
    for (int attempt = 0; !ok && !stop;)
    {
      if (!Wait())
      {
        stop = true;
        break;
      }
      Net::Request request;
      request.url = std::string("https://raw.githubusercontent.com/") + OWNER + "/bezelproject-" + source.repo + "/" +
                    BRANCH + "/" + source.folder + "/" + Net::UrlEncode(item.entry.name + ".png");
      request.timeout_seconds = 0;  // (a slow connection: only a stall stops it)
      request.stall_seconds = 30;
      request.cancel = &m_cancel;
      const int64_t part = FileSize(item.target + ".part");
      request.resume_from = part > 0 && part < item.entry.size ? part : 0;
      if (request.resume_from > 0)
        Log(key + ": " + item.entry.name + ".png goes on from byte " + std::to_string(request.resume_from) + " of " +
            std::to_string(item.entry.size));
      const size_t c = item.console;
      const int64_t counted = item.part;  // (the part there before this download: not this run's bytes)
      request.progress = [&, c, counted](int64_t done, int64_t) {
        {
          std::lock_guard lock(m_mutex);
          m_progress.consoles[c].done = before + std::max<int64_t>(0, done - counted);
        }
        Changed();
        return !m_pause && !m_cancel;  // (a game started: the part stays, it goes on afterwards)
      };
      const Net::Response reply = Net::Download(request, item.target, &raw);
      if (reply.ok())
      {
        cuts = 0;
        const int64_t size = FileSize(item.target);
        const std::string sha = GitBlobSha(item.target);
        if (!IsPng(item.target))
        {
          std::remove(item.target.c_str());
          why = "what came isn't a picture";
          break;
        }
        if (sha != item.entry.sha)
          Log(key + ": " + item.entry.name + ".png isn't the file GitHub's list has (changed since?): kept, " +
              std::to_string(size) + " bytes");
        AppendRecord(RecordPath(m_data_dir, key), item.job->game.id + ".png\t" + source.repo + "/" + item.entry.name +
                                                      "\t" + std::to_string(size) + "\t" + sha + "\n");
        ok = true;
        break;
      }
      if (m_cancel)
      {
        stop = true;
        break;
      }
      if (m_pause)
        continue;  // (stopped for a game: Wait(), then from its part)
      why = reply.plain.empty() ? Net::Plain(reply.status, reply.error) : reply.plain;
      Log(key + ": " + item.entry.name + ".png: " +
          (reply.status > 0 ? "HTTP " + std::to_string(reply.status) : reply.error) +
          (reply.bytes > 0 ? " (" + std::to_string(reply.bytes) + " bytes kept)" : std::string()));
      if (reply.status == 0 && reply.error.find("couldn't write") != std::string::npos)
      {
        // The drive is full (or can't be written): the rest would fail the same way.
        std::lock_guard lock(m_mutex);
        m_progress.error = why;
        stop = true;
        break;
      }
      if (reply.status == 404 || reply.status == 410)
        break;  // (not there anymore: the list is asked for again a day later)
      if ((reply.status == 429 || reply.status == 503) && ++slowed <= 10)
      {
        // GitHub asks to slow down (too many files in a row): the time it says (Retry-After), else a minute, then
        // the same file again - not a failure. (A game started meanwhile waits in Wait(); a Stop ends it.)
        int64_t seconds = Number(reply.Header("Retry-After"));
        seconds = seconds > 0 ? std::min<int64_t>(seconds, 300) : 60;
        Log(key + ": GitHub asks to slow down (HTTP " + std::to_string(reply.status) + "): again in " +
            std::to_string(seconds) + " s");
        for (int64_t waited = 0; waited < seconds * 5 && !m_cancel; waited++)
          sceKernelUsleep(200000);
        continue;
      }
      if (++attempt >= 3)
      {
        if (++cuts >= 3)
        {
          // Three files in a row without a connection: the rest waits for the next time (pending).
          std::lock_guard lock(m_mutex);
          m_progress.error = why;
          stop = true;
        }
        break;
      }
      sceKernelUsleep(static_cast<unsigned int>(2000000 * attempt));
    }
    if (stop && !ok)
      break;
    left[item.console]--;
    set(item.console, [&](Console& console) {
      if (ok)
        console.downloaded++;
      else
        console.failed++;
      console.done = before + (ok ? item.entry.size - item.part : 0);
      if (left[item.console] == 0)
        console.step = Step::Done;
    });
    if (!ok)
      Log(key + ": " + item.job->game.id + ": its bezel didn't come: " + why);
  }
  // 4. The end: the consoles done leave the list of the download to go on with at the next start (all of them when
  // the player stopped it).
  std::vector<std::string> pending;
  for (size_t c = 0; c < m_work.size(); c++)
  {
    const bool done = listed[c] && left[c] == 0;
    if (!done && !m_forget)
      pending.push_back(m_work[c].first);
  }
  {
    // (A console asked for while this one ran, by another Start: none - one download at a time.)
    std::vector<std::string> others = Pending(m_data_dir);
    for (const std::string& key : others)
    {
      bool ours = false;
      for (const auto& [work_key, jobs] : m_work)
        ours = ours || work_key == key;
      if (!ours && !m_forget)
        pending.push_back(key);
    }
  }
  WritePending(m_data_dir, pending);
  int downloaded = 0, failed = 0;
  int64_t bytes = 0;
  {
    std::lock_guard lock(m_mutex);
    for (Console& console : m_progress.consoles)
    {
      downloaded += console.downloaded;
      failed += console.failed;
      bytes += console.done;
      if (console.step == Step::Listing || (console.step == Step::Downloading && m_cancel))
        console.step = Step::Waiting;
    }
    m_progress.running = false;
    m_progress.paused = false;
    m_progress.stopped = m_cancel && m_forget;
    m_progress.game.clear();
  }
  const long long seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
  Log("the download ended" + std::string(m_cancel ? " (stopped)" : "") + ": " + std::to_string(downloaded) +
      (downloaded == 1 ? " bezel, " : " bezels, ") + Mb(bytes) + " MB in " + std::to_string(seconds) + " s" +
      (failed > 0 ? ", " + std::to_string(failed) + " failed" : std::string()) +
      (pending.empty() ? std::string() : "; to go on with at the next start: " + std::to_string(pending.size()) +
                                             (pending.size() == 1 ? " console" : " consoles")));
  m_ended = true;
  m_busy = false;
  Changed();
}

// ---- A bezel's window ----
Window FindWindow(ImageRGBA* image, float ratio, float screen_w, float screen_h)
{
  Window window;
  const uint32_t w = image->width, h = image->height;
  if (w < 4 || h < 4 || image->pixels.size() < static_cast<size_t>(w) * h * 4)
  {
    window.why = " isn't a picture";
    return window;
  }
  const auto clear = [image, w](uint32_t x, uint32_t y) {
    return image->pixels[(static_cast<size_t>(y) * w + x) * 4 + 3] < 128;
  };
  // The window: see-through from the middle out along the middle row, then along its own middle column (an old
  // TV's screen is off to one side).
  const uint32_t middle_y = h / 2;
  uint32_t x0 = w / 2, x1 = x0;
  if (!clear(x0, middle_y))
  {
    window.why = " isn't see-through in the middle";
    return window;
  }
  while (x0 > 0 && clear(x0 - 1, middle_y))
    x0--;
  while (x1 + 1 < w && clear(x1 + 1, middle_y))
    x1++;
  const uint32_t middle_x = (x0 + x1) / 2;
  uint32_t y0 = middle_y, y1 = middle_y;
  while (y0 > 0 && clear(middle_x, y0 - 1))
    y0--;
  while (y1 + 1 < h && clear(middle_x, y1 + 1))
    y1++;
  if (x1 - x0 < w / 4 || y1 - y0 < h / 4)
  {
    window.why = "'s window is too small";
    return window;
  }
  window.found = true;
  window.x0 = x0;
  window.y0 = y0;
  window.x1 = x1;
  window.y1 = y1;
  window.left = static_cast<float>(x0);
  window.top = static_cast<float>(y0);
  window.right = static_cast<float>(x1 + 1);
  window.bottom = static_cast<float>(y1 + 1);
  uint32_t clear_x0 = x0, clear_y0 = y0, clear_x1 = x1, clear_y1 = y1;
  if (ratio > 0.0f && screen_w > 0.0f && screen_h > 0.0f)
  {
    // The see-through run of each row of the window around its middle column (none: the row isn't see-through there).
    const uint32_t rows = y1 - y0 + 1;
    std::vector<int32_t> run_left(rows, 1), run_right(rows, 0);
    for (uint32_t y = y0; y <= y1; y++)
    {
      if (!clear(middle_x, y))
        continue;
      uint32_t l = middle_x, r = middle_x;
      while (l > x0 && clear(l - 1, y))
        l--;
      while (r < x1 && clear(r + 1, y))
        r++;
      run_left[y - y0] = static_cast<int32_t>(l);
      run_right[y - y0] = static_cast<int32_t>(r);
    }
    // The picture's shape in the PNG's pixels (the PNG fills the screen: a smaller one is stretched).
    const float png_ratio = ratio * (screen_h / static_cast<float>(h)) / (screen_w / static_cast<float>(w));
    const float centre_x = (static_cast<float>(x0) + static_cast<float>(x1) + 1.0f) * 0.5f;
    const float centre_y = (static_cast<float>(y0) + static_cast<float>(y1) + 1.0f) * 0.5f;
    // A rectangle `height` rows tall, centred: its pixels' columns and rows (inclusive), all see-through?
    auto box = [&](int height, int32_t* bx0, int32_t* by0, int32_t* bx1, int32_t* by1) {
      const float width = static_cast<float>(height) * png_ratio;
      *by0 = static_cast<int32_t>(std::floor(centre_y - static_cast<float>(height) * 0.5f + 0.001f));
      *by1 = *by0 + height - 1;
      *bx0 = static_cast<int32_t>(std::floor(centre_x - width * 0.5f + 0.001f));
      *bx1 = static_cast<int32_t>(std::ceil(centre_x + width * 0.5f - 0.001f)) - 1;
    };
    auto fits = [&](int height) {
      int32_t bx0, by0, bx1, by1;
      box(height, &bx0, &by0, &bx1, &by1);
      if (by0 < static_cast<int32_t>(y0) || by1 > static_cast<int32_t>(y1) || bx0 < static_cast<int32_t>(x0) ||
          bx1 > static_cast<int32_t>(x1) || bx1 < bx0)
        return false;
      for (int32_t y = by0; y <= by1; y++)
      {
        const size_t row = static_cast<size_t>(y - static_cast<int32_t>(y0));
        if (run_left[row] > bx0 || run_right[row] < bx1)
          return false;
      }
      return true;
    };
    // The biggest that fits (a smaller one centred the same is inside it: halving finds it).
    int low = 0, high = static_cast<int>(rows);
    while (low < high)
    {
      const int mid = (low + high + 1) / 2;
      if (fits(mid))
        low = mid;
      else
        high = mid - 1;
    }
    if (low < static_cast<int>(h / 4))
    {
      window.found = false;
      window.why = "'s window has no room for the picture";
      return window;
    }
    int32_t bx0, by0, bx1, by1;
    box(low, &bx0, &by0, &bx1, &by1);
    window.left = static_cast<float>(bx0);
    window.top = static_cast<float>(by0);
    window.right = static_cast<float>(bx1 + 1);
    window.bottom = static_cast<float>(by1 + 1);
    clear_x0 = static_cast<uint32_t>(bx0);
    clear_y0 = static_cast<uint32_t>(by0);
    clear_x1 = static_cast<uint32_t>(bx1);
    clear_y1 = static_cast<uint32_t>(by1);
  }
  // The picture goes in that window: there the bezel is fully see-through (00.69). The built-in PNGs' edges are soft
  // (made at twice the size, then smaller): up to 10% of their colour reached 1-2 pixels into the window, a faint line
  // over the picture's edges. Only what counts as see-through is cleared: the opaque parts of a window that isn't a
  // rectangle (the Retro TV's rounded corners) stay.
  for (uint32_t y = clear_y0; y <= clear_y1; y++)
  {
    for (uint32_t x = clear_x0; x <= clear_x1; x++)
    {
      uint8_t* pixel = &image->pixels[(static_cast<size_t>(y) * w + x) * 4];
      if (pixel[3] < 128)
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
    }
  }
  return window;
}

float FitScale(const Window& window, float ratio, uint32_t image_w, uint32_t image_h, float screen_w, float screen_h)
{
  if (!window.found || ratio <= 0.0f || image_w == 0 || image_h == 0 || screen_w <= 0.0f || screen_h <= 0.0f)
    return 0.0f;
  const float sx = screen_w / static_cast<float>(image_w), sy = screen_h / static_cast<float>(image_h);
  const float area_w = (window.right - window.left) * sx, area_h = (window.bottom - window.top) * sy;
  // The picture's height fitted in the window, and on the whole screen.
  const float framed = std::min(area_h, area_w / ratio);
  const float whole = std::min(screen_h, screen_w / ratio);
  return whole > 0.0f ? framed / whole : 0.0f;
}
}  // namespace BezelProject
