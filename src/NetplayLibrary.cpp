// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "NetplayLibrary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <unordered_map>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "Log.h"
#include "Netplay.h"

namespace NetplayLibrary
{
namespace
{
struct Known
{
  uint64_t size = 0;       // (01.05) its content's (ContentId's)
  uint64_t file_size = 0;  // the file's, as hashed
  int64_t mtime = 0;
  uint32_t crc = 0;
  bool ok = false;    // hashed (false: it couldn't be read)
  // (01.06) The whole file's CRC32 (FileId: v00.23's), its size file_size; file_ok false: not yet (a copy of 01.05).
  uint32_t file_crc = 0;
  bool file_ok = false;
};

struct Job
{
  std::string path;
  std::string console;  // ((01.05) ContentId's: an Arcade set's zip)
  uint64_t size = 0;
  int64_t mtime = 0;
};

// ---- (01.05) A game's content (ContentId): a file's bytes, or a zip entry's, a piece at a time ----
constexpr uint64_t WHOLE = 64ull << 20, PART = 8ull << 20;

std::string LowerExtension(const std::string& name)
{
  const size_t slash = name.find_last_of("/\\"), dot = name.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  std::string ext = name.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

bool OneOf(const std::string& ext, std::initializer_list<const char*> list)
{
  for (const char* e : list)
  {
    if (ext == e)
      return true;
  }
  return false;
}

uint32_t Le32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// The bytes of a file from where it is (left: how many), or of a zip's entry (stored, or deflated: zlib's raw inflate,
// 64 KiB read at a time).
class Source
{
public:
  Source(FILE* f, uint64_t left, int method) : m_f(f), m_left(left), m_method(method)
  {
    if (m_method == 8)
    {
      std::memset(&m_z, 0, sizeof(m_z));
      m_ok = inflateInit2(&m_z, -MAX_WBITS) == Z_OK;
      m_inflating = m_ok;
    }
    else
      m_ok = m_method == 0;
  }
  ~Source()
  {
    if (m_inflating)
      inflateEnd(&m_z);
  }
  // As many bytes as asked, unless the content ends (fewer then: Ok() says whether it was the end or an error).
  size_t Read(uint8_t* out, size_t n)
  {
    size_t got = 0;
    if (!m_ok)
      return 0;
    if (m_method == 0)
    {
      while (got < n && m_left > 0)
      {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(n - got, m_left));
        const size_t r = std::fread(out + got, 1, want, m_f);
        if (r == 0)
        {
          m_ok = false;
          break;
        }
        got += r;
        m_left -= r;
      }
      return got;
    }
    m_z.next_out = out;
    m_z.avail_out = static_cast<uInt>(n);
    while (m_z.avail_out > 0 && !m_end)
    {
      if (m_z.avail_in == 0 && m_left > 0)
      {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(m_in.size(), m_left));
        const size_t r = std::fread(m_in.data(), 1, want, m_f);
        if (r == 0)
        {
          m_ok = false;
          break;
        }
        m_left -= r;
        m_z.next_in = m_in.data();
        m_z.avail_in = static_cast<uInt>(r);
      }
      const int z = inflate(&m_z, Z_NO_FLUSH);
      if (z == Z_STREAM_END)
        m_end = true;
      else if (z != Z_OK && !(z == Z_BUF_ERROR && m_z.avail_in == 0 && m_left > 0))
      {
        m_ok = false;  // (damaged, or cut before its end)
        break;
      }
    }
    return n - m_z.avail_out;
  }
  bool Ok() const { return m_ok; }

private:
  FILE* m_f;
  uint64_t m_left;
  int m_method;
  bool m_ok = false, m_inflating = false, m_end = false;
  z_stream m_z;
  std::vector<uint8_t> m_in = std::vector<uint8_t>(65536);  // (not on the hashing thread's small stack)
};

// What of a content isn't the game's (No-Intro's DATs have none of it): a header to skip, an order to undo.
enum class Order
{
  Plain,
  Smd,  // 16 KiB blocks, each its odd bytes then its even ones
  V64,  // bytes swapped in pairs
  N64,  // bytes reversed in fours
};
struct Rule
{
  uint64_t skip = 0;
  Order order = Order::Plain;
};
Rule RuleFor(const std::string& ext, const uint8_t* head, size_t head_n, uint64_t size)
{
  Rule r;
  const auto magic = [&](size_t at, const char* m, size_t len) {
    return head_n >= at + len && std::memcmp(head + at, m, len) == 0;
  };
  if (magic(0, "NES\x1A", 4))
    r.skip = 16 + (head_n > 6 && (head[6] & 4) ? 512 : 0);  // (iNES / NES 2.0; a trainer after it)
  else if (magic(0, "FDS\x1A", 4))
    r.skip = 16;
  else if (magic(1, "ATARI7800", 9))
    r.skip = 128;
  else if (magic(0, "LYNX", 4))
    r.skip = 64;
  else if (OneOf(ext, {".smc", ".sfc", ".swc", ".fig"}) && size % 1024 == 512)
    r.skip = 512;
  else if (OneOf(ext, {".pce", ".sgx"}) && size % 8192 == 512)
    r.skip = 512;
  else if (OneOf(ext, {".sms", ".gg", ".sg", ".sc"}) && size % 16384 == 512)
    r.skip = 512;
  else if (OneOf(ext, {".smd", ".md", ".gen", ".bin"}) && size % 16384 == 512 && size > 512 && head_n >= 10 &&
           head[8] == 0xAA && head[9] == 0xBB)
  {
    r.skip = 512;
    r.order = Order::Smd;
  }
  else if (OneOf(ext, {".v64", ".n64", ".z64"}) && head_n >= 4 && size % 4 == 0)
  {
    if (head[0] == 0x37 && head[1] == 0x80 && head[2] == 0x40 && head[3] == 0x12)
      r.order = Order::V64;
    else if (head[0] == 0x40 && head[1] == 0x12 && head[2] == 0x37 && head[3] == 0x80)
      r.order = Order::N64;
  }
  if (r.skip >= size)
    r = Rule();
  return r;
}

// A content's CRC32 and size by its rule: its head (already read) first, then the rest from the source.
bool HashContent(Source& src, const uint8_t* head, size_t head_n, uint64_t total, const Rule& rule, uint32_t* crc,
                 uint64_t* size)
{
  size_t head_at = 0;
  const auto read = [&](uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n && head_at < head_n)
      out[got++] = head[head_at++];
    if (got < n)
      got += src.Read(out + got, n - got);
    return got;
  };
  std::vector<uint8_t> buf(1 << 20), swapped;
  uint64_t skip = rule.skip;
  while (skip > 0)
  {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(skip, buf.size()));
    if (read(buf.data(), want) != want)
      return false;
    skip -= want;
  }
  const uint64_t content = total - rule.skip;
  uLong c = crc32(0L, Z_NULL, 0);
  uint64_t done = 0;
  const size_t step = rule.order == Order::Smd ? 16384 : buf.size();
  if (rule.order == Order::Smd)
    swapped.resize(16384);
  while (done < content)
  {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(step, content - done));
    const size_t got = read(buf.data(), want);
    if (got != want)
      return false;
    if (rule.order == Order::Smd)
    {
      if (got != 16384)
        return false;
      for (size_t i = 0; i < 8192; i++)
      {
        swapped[i * 2] = buf[8192 + i];
        swapped[i * 2 + 1] = buf[i];
      }
      c = crc32(c, swapped.data(), static_cast<uInt>(got));
    }
    else
    {
      if (rule.order == Order::V64)
      {
        for (size_t i = 0; i + 1 < got; i += 2)
          std::swap(buf[i], buf[i + 1]);
      }
      else if (rule.order == Order::N64)
      {
        for (size_t i = 0; i + 3 < got; i += 4)
        {
          std::swap(buf[i], buf[i + 3]);
          std::swap(buf[i + 1], buf[i + 2]);
        }
      }
      c = crc32(c, buf.data(), static_cast<uInt>(got));
    }
    done += got;
  }
  *crc = static_cast<uint32_t>(c);
  *size = content;
  return true;
}

// A zip's entries as its central directory lists them (directories left out).
struct ZipEntry
{
  std::string name;
  uint32_t crc = 0;
  uint64_t packed = 0, size = 0, local = 0;
  int method = 0;
};
bool ZipEntries(FILE* f, uint64_t file_size, std::vector<ZipEntry>* out)
{
  const uint64_t tail = std::min<uint64_t>(file_size, 65535 + 22);
  std::vector<uint8_t> end(static_cast<size_t>(tail));
  if (tail < 22 || std::fseek(f, static_cast<long>(file_size - tail), SEEK_SET) != 0 ||
      std::fread(end.data(), 1, end.size(), f) != end.size())
    return false;
  size_t at = SIZE_MAX;
  for (size_t i = end.size() - 22 + 1; i-- > 0;)
  {
    if (Le32(&end[i]) == 0x06054b50)
    {
      at = i;
      break;
    }
  }
  if (at == SIZE_MAX)
    return false;
  const uint16_t count = Le16(&end[at + 10]);
  const uint32_t dir_size = Le32(&end[at + 12]), dir_at = Le32(&end[at + 16]);
  if (static_cast<uint64_t>(dir_at) + dir_size > file_size || dir_size > (64u << 20))
    return false;
  std::vector<uint8_t> dir(dir_size);
  if (std::fseek(f, static_cast<long>(dir_at), SEEK_SET) != 0 || std::fread(dir.data(), 1, dir.size(), f) != dir.size())
    return false;
  size_t p = 0;
  for (uint32_t i = 0; i < count && p + 46 <= dir.size() && Le32(&dir[p]) == 0x02014b50; i++)
  {
    const size_t name_n = Le16(&dir[p + 28]), extra_n = Le16(&dir[p + 30]), comment_n = Le16(&dir[p + 32]);
    if (p + 46 + name_n > dir.size())
      break;
    ZipEntry e;
    e.method = Le16(&dir[p + 10]);
    e.crc = Le32(&dir[p + 16]);
    e.packed = Le32(&dir[p + 20]);
    e.size = Le32(&dir[p + 24]);
    e.local = Le32(&dir[p + 42]);
    e.name.assign(reinterpret_cast<const char*>(&dir[p + 46]), name_n);
    if (!e.name.empty() && e.name.back() != '/')
      out->push_back(std::move(e));
    p += 46 + name_n + extra_n + comment_n;
  }
  return !out->empty();
}

// The cartridges' extensions (a zip's game is its largest entry of these: a plain one before a .bin or .md).
bool CartridgeExtension(const std::string& ext)
{
  return OneOf(ext, {".nes", ".fds", ".unf", ".unif", ".unh", ".sfc", ".smc", ".swc", ".fig", ".bs", ".st", ".md",
                     ".smd", ".gen", ".bin", ".sms", ".gg", ".sg", ".sc", ".pce", ".sgx", ".a26", ".a78", ".lnx",
                     ".lyx", ".gb", ".gbc", ".sgb", ".gba", ".agb", ".n64", ".v64", ".z64", ".ngp", ".ngc", ".ws",
                     ".wsc", ".col", ".32x", ".j64", ".jag", ".vec", ".int"});
}

std::mutex s_mutex;
std::unordered_map<std::string, Known> s_known;  // by path
std::deque<Job> s_jobs;
std::string s_cache;       // data_dir/cache/netplay-ids.txt
bool s_loaded = false, s_running = false, s_paused = false, s_dirty = false;
int s_done = 0, s_total = 0;
std::chrono::steady_clock::time_point s_saved = std::chrono::steady_clock::now();  // (the copy last written)

bool FileState(const std::string& path, uint64_t* size, int64_t* mtime)
{
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  *size = static_cast<uint64_t>(st.st_size);
  *mtime = static_cast<int64_t>(st.st_mtime);
  return true;
}

// (01.06) The whole file's CRC32 is the content's when the content is the file itself: not a zip, nothing left out
// (the same size), the bytes in their order (not an N64's .v64 or .n64) - no second read then.
bool ContentIsFile(const std::string& path, uint64_t content_size, uint64_t file_size)
{
  const std::string ext = LowerExtension(path);
  return content_size == file_size && ext != ".zip" && ext != ".v64" && ext != ".n64";
}
// Whose whole file's CRC32 is kept: every console's but the Arcade's - a room finds an Arcade set by its name already
// (Menu.cpp's FindOnlineGame: a romset), and its zips are the big ones.
bool WantsFileId(const std::string& console)
{
  return console != "arcade";
}

// The copy: "crc<TAB>size<TAB>file size<TAB>mtime<TAB>path" lines - (01.05) the content's CRC32 and size, then the
// file's own size and time (a file that couldn't be read isn't kept). (01.06) Before the path, "=" and the whole file's
// CRC32 and a tab, once known (a line of 01.05's has none: FileId again, once - only for the files whose content isn't
// the file).
void LoadLocked()
{
  if (s_loaded)
    return;
  s_loaded = true;
  const std::string text = ReadTextFile(s_cache);
  size_t at = 0, lines = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    unsigned crc = 0;
    unsigned long long size = 0, file_size = 0;
    long long mtime = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%x\t%llu\t%llu\t%lld\t%n", &crc, &size, &file_size, &mtime, &used) != 4 ||
        used <= 0 || static_cast<size_t>(used) >= line.size())
      continue;
    Known k;
    k.crc = crc;
    k.size = size;
    k.file_size = file_size;
    k.mtime = mtime;
    k.ok = true;
    unsigned file_crc = 0;
    int more = 0;
    if (line[static_cast<size_t>(used)] == '=' &&
        std::sscanf(line.c_str() + used, "=%x\t%n", &file_crc, &more) == 1 && more > 0 &&
        static_cast<size_t>(used + more) < line.size())
    {
      k.file_crc = file_crc;
      k.file_ok = true;
      used += more;
    }
    const std::string path = line.substr(static_cast<size_t>(used));
    if (!k.file_ok && ContentIsFile(path, k.size, k.file_size))
    {
      k.file_crc = k.crc;
      k.file_ok = true;
    }
    s_known[path] = k;
    lines++;
  }
  LogF("netplay: %zu files' CRC32s known (%s)", lines, s_cache.c_str());
}

void SaveLocked()
{
  std::string text;
  text.reserve(s_known.size() * 80);
  for (const auto& [path, k] : s_known)
  {
    if (!k.ok)
      continue;
    char head[112];
    std::snprintf(head, sizeof(head), "%08x\t%llu\t%llu\t%lld\t", k.crc, static_cast<unsigned long long>(k.size),
                  static_cast<unsigned long long>(k.file_size), static_cast<long long>(k.mtime));
    text += head;
    if (k.file_ok)
    {
      char file[16];
      std::snprintf(file, sizeof(file), "=%08x\t", k.file_crc);
      text += file;
    }
    text += path;
    text += '\n';
  }
  WriteTextFile(s_cache, text);
  s_dirty = false;
}

void* HashThread(void*)
{
  for (;;)
  {
    Job job;
    {
      std::lock_guard lock(s_mutex);
      if (s_jobs.empty())
      {
        if (s_dirty)
          SaveLocked();
        s_running = false;
        LogF("netplay: the library hashed (%d files)", s_total);
        return nullptr;
      }
      if (s_paused)
        job.path.clear();
      else
      {
        job = s_jobs.front();
        s_jobs.pop_front();
      }
    }
    if (job.path.empty())
    {
      usleep(200 * 1000);  // (a game runs: no file read meanwhile)
      continue;
    }
    // The file as it is (on this thread: a USB drive's thousands of files aren't the menu's wait); one known by its
    // size and time isn't read again.
    if (!FileState(job.path, &job.size, &job.mtime))
    {
      std::lock_guard lock(s_mutex);
      s_known.erase(job.path);
      s_done++;
      continue;
    }
    // ((01.06) Known but without its whole file's CRC32 - a copy of 01.05's -: that one alone.)
    bool content = true;
    uint32_t crc = 0;
    uint64_t size = 0;
    {
      std::lock_guard lock(s_mutex);
      const auto it = s_known.find(job.path);
      if (it != s_known.end() && it->second.file_size == job.size && it->second.mtime == job.mtime)
      {
        if (!it->second.ok || it->second.file_ok || !WantsFileId(job.console))
        {
          s_done++;
          continue;
        }
        content = false;
        crc = it->second.crc;
        size = it->second.size;
      }
    }
    const bool ok = !content || ContentId(job.path, job.console, &crc, &size);
    // (01.06) Its whole file's CRC32 too (v00.23's rooms know a game by it): the content's when they're the same bytes.
    uint32_t file_crc = 0;
    uint64_t file_size = 0;
    bool file_ok = false;
    if (ok && WantsFileId(job.console))
    {
      if (ContentIsFile(job.path, size, job.size))
      {
        file_crc = crc;
        file_ok = true;
      }
      else
        file_ok = FileId(job.path, &file_crc, &file_size) && file_size == job.size;
    }
    std::lock_guard lock(s_mutex);
    Known& k = s_known[job.path];
    k.ok = ok;
    k.crc = crc;
    k.size = size;
    k.file_size = job.size;
    k.mtime = job.mtime;
    k.file_crc = file_crc;
    k.file_ok = file_ok;
    s_done++;
    s_dirty = true;
    // (A cut - the app closed - keeps what was done.) 00.85: every 30 s of hashing, not every 50 files - the file has
    // every file's CRC: a library of 30 000 games had it written whole 600 times, each under the lock the menu waits
    // on for the progress.
    const auto now = std::chrono::steady_clock::now();
    if (now - s_saved > std::chrono::seconds(30))
    {
      SaveLocked();
      s_saved = now;
    }
  }
}

// ---- The players ----
std::mutex s_players_mutex;
bool s_players_loaded = false;
std::unordered_map<std::string, int> s_players;  // console + '\1' + kind + key -> players

void LoadPlayersLocked()
{
  if (s_players_loaded)
    return;
  s_players_loaded = true;
  std::string path = std::string(App::ASSETS_DIR) + "/xmb/netplay/players.bin";
#ifndef __PS4__
  if (const char* test = std::getenv("EMUSUNI_PLAYERS_DB"))
    path = test;
#endif
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
  {
    LogF("netplay: no players' list (%s)", path.c_str());
    return;
  }
  std::vector<uint8_t> packed;
  uint8_t buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    packed.insert(packed.end(), buf, buf + n);
  std::fclose(f);
  if (packed.size() < 4)
    return;
  const uLongf raw_size = packed[0] | (packed[1] << 8) | (packed[2] << 16) | (static_cast<uLongf>(packed[3]) << 24);
  if (raw_size > (16u << 20))
    return;
  std::string text(raw_size, '\0');
  uLongf out = raw_size;
  if (uncompress(reinterpret_cast<Bytef*>(&text[0]), &out, packed.data() + 4, static_cast<uLong>(packed.size() - 4)) !=
      Z_OK)
  {
    LogF("netplay: the players' list %s is damaged", path.c_str());
    return;
  }
  text.resize(out);
  // "console<TAB>kind<TAB>key<TAB>players" (kind: c CRC32, t name's key, s arcade set; '#' a comment).
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.empty() || line[0] == '#')
      continue;
    const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
                 c = b == std::string::npos ? b : line.find('\t', b + 1);
    if (c == std::string::npos)
      continue;
    const int players = std::atoi(line.c_str() + c + 1);
    if (players > 0)
      s_players[line.substr(0, a) + '\1' + line.substr(a + 1, b - a - 1) + line.substr(b + 1, c - b - 1)] = players;
  }
  LogF("netplay: the players' list: %zu entries", s_players.size());
}
}  // namespace

std::string TitleKey(const std::string& name)
{
  const size_t cut = name.find_first_of("([");
  const std::string head = cut == std::string::npos ? name : name.substr(0, cut);
  std::string key, word;
  auto flush = [&]() {
    if (!word.empty() && word != "the")
      key += word;
    word.clear();
  };
  for (const char ch : head)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x80 && std::isalnum(c))
      word += static_cast<char>(std::tolower(c));
    else
      flush();
  }
  flush();
  return key;
}

void Start(const std::string& data_dir, const std::vector<File>& files)
{
  std::lock_guard lock(s_mutex);
  MakeDirs(data_dir + "/cache");
  s_cache = data_dir + "/cache/netplay-ids.txt";  // ((01.05) the contents' ids: not 01.04's netplay-crcs.txt)
  if (!s_loaded)
    std::remove((data_dir + "/cache/netplay-crcs.txt").c_str());  // ((01.05) 01.04's whole files' CRC32s: no use now)
  LoadLocked();
  s_jobs.clear();
  s_done = 0;
  s_total = static_cast<int>(files.size());
  for (const File& file : files)
  {
    Job job;
    job.path = file.path;
    job.console = file.console;
    s_jobs.push_back(job);
  }
  LogF("netplay: the library: %d files that play online, looked at on a thread", s_total);
  if (s_jobs.empty() || s_running)
    return;
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);  // (the app's other small threads' size: libthr keeps one for them)
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  s_running = pthread_create(&thread, &attr, &HashThread, nullptr) == 0;
  pthread_attr_destroy(&attr);
  if (!s_running)
  {
    LogLine("netplay: the library's hashing thread couldn't start");
    s_jobs.clear();
  }
}

Progress GetProgress()
{
  std::lock_guard lock(s_mutex);
  Progress p;
  p.done = s_done;
  p.total = s_total;
  p.finished = s_jobs.empty() && !s_running && s_loaded;
  return p;
}

bool Lookup(const std::string& path, uint32_t* crc, uint64_t* size)
{
  std::lock_guard lock(s_mutex);
  const auto it = s_known.find(path);
  if (it == s_known.end() || !it->second.ok)
    return false;
  *crc = it->second.crc;
  *size = it->second.size;
  return true;
}

bool LookupFile(const std::string& path, uint32_t* crc, uint64_t* size)
{
  std::lock_guard lock(s_mutex);
  const auto it = s_known.find(path);
  if (it == s_known.end() || !it->second.ok || !it->second.file_ok)
    return false;
  *crc = it->second.file_crc;
  *size = it->second.file_size;
  return true;
}

// (01.06) v00.23's Netplay::GameCrc as it was: the file's bytes as they are.
bool FileId(const std::string& path, uint32_t* crc, uint64_t* size)
{
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long end = std::ftell(f);
  if (end < 0)
  {
    std::fclose(f);
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(end);
  std::vector<uint8_t> buf(256 * 1024);  // (the hashing thread's stack is small: on the heap)
  uLong c = crc32(0L, Z_NULL, 0);
  auto add = [&](uint64_t from, uint64_t n) {
    std::fseek(f, static_cast<long>(from), SEEK_SET);
    while (n > 0)
    {
      const size_t want = static_cast<size_t>(std::min<uint64_t>(n, buf.size()));
      const size_t got = std::fread(buf.data(), 1, want, f);
      if (got == 0)
        return false;
      c = crc32(c, buf.data(), static_cast<uInt>(got));
      n -= got;
    }
    return true;
  };
  const bool ok = total <= WHOLE ? add(0, total) : add(0, PART) && add(total - PART, PART);
  std::fclose(f);
  if (!ok)
    return false;
  *crc = static_cast<uint32_t>(c);
  *size = total;
  return true;
}

void Pause(bool paused)
{
  std::lock_guard lock(s_mutex);
  s_paused = paused;
}

int Players(const std::string& console, const std::string& stem, uint32_t crc)
{
  std::lock_guard lock(s_players_mutex);
  LoadPlayersLocked();
  char hex[16];
  std::snprintf(hex, sizeof(hex), "%08x", crc);
  const std::string base = console + '\1';
  auto find = [&](const std::string& key) {
    const auto it = s_players.find(key);
    return it == s_players.end() ? 0 : it->second;
  };
  if (crc != 0)
  {
    if (const int n = find(base + "c" + hex))
      return n;
  }
  if (console == "arcade")
  {
    std::string set = stem;
    std::transform(set.begin(), set.end(), set.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (const int n = find(base + "s" + set))
      return n;
  }
  return find(base + "t" + TitleKey(stem));
}
// ---- (01.05) A game's content ----
bool ContentId(const std::string& path, const std::string& console, uint32_t* crc, uint64_t* size)
{
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long end = std::ftell(f);
  if (end < 0)
  {
    std::fclose(f);
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(end);
  bool ok = false;
  const bool zip = LowerExtension(path) == ".zip";
  if (zip)
  {
    std::vector<ZipEntry> entries;
    if (ZipEntries(f, total, &entries))
    {
      // The game's entry: a cartridge's (the largest; a plain one before a .bin or .md) - not an Arcade set's.
      const ZipEntry* game = nullptr;
      if (console != "arcade")
      {
        for (const ZipEntry& e : entries)
        {
          const std::string ext = LowerExtension(e.name);
          if (!CartridgeExtension(ext) || e.size == 0)
            continue;
          const bool plain = ext != ".bin" && ext != ".md";
          const std::string game_ext = game ? LowerExtension(game->name) : std::string();
          const bool game_plain = game && game_ext != ".bin" && game_ext != ".md";
          if (!game || (plain && !game_plain) || (plain == game_plain && e.size > game->size))
            game = &e;
        }
      }
      if (!game)
      {
        // A set: its entries' CRC32s, sorted, as one (and their sizes summed).
        std::vector<uint32_t> crcs;
        uint64_t sum = 0;
        for (const ZipEntry& e : entries)
        {
          if (e.size == 0)
            continue;
          crcs.push_back(e.crc);
          sum += e.size;
        }
        std::sort(crcs.begin(), crcs.end());
        std::vector<uint8_t> bytes;
        for (const uint32_t c : crcs)
        {
          for (int k = 0; k < 4; k++)
            bytes.push_back(static_cast<uint8_t>(c >> (8 * k)));
        }
        ok = !crcs.empty();
        *crc = static_cast<uint32_t>(crc32(crc32(0L, Z_NULL, 0), bytes.data(), static_cast<uInt>(bytes.size())));
        *size = sum;
      }
      else if (game->method != 0 && game->method != 8)
      {
        // (Packed otherwise than stored or deflated: the zip's own CRC32 and size of it, no header looked for.)
        *crc = game->crc;
        *size = game->size;
        ok = true;
      }
      else
      {
        // The entry's head: whether a rule applies (a plain content's CRC32 and size are the zip's own).
        uint8_t local[30];
        if (std::fseek(f, static_cast<long>(game->local), SEEK_SET) == 0 && std::fread(local, 1, 30, f) == 30 &&
            Le32(local) == 0x04034b50 &&
            std::fseek(f, static_cast<long>(game->local + 30 + Le16(local + 26) + Le16(local + 28)), SEEK_SET) == 0)
        {
          Source src(f, game->packed, game->method);
          uint8_t head[128];
          const size_t head_n = src.Read(head, static_cast<size_t>(std::min<uint64_t>(sizeof(head), game->size)));
          const Rule rule = RuleFor(LowerExtension(game->name), head, head_n, game->size);
          if (src.Ok() && rule.skip == 0 && rule.order == Order::Plain)
          {
            *crc = game->crc;
            *size = game->size;
            ok = true;
          }
          else if (src.Ok())
            ok = HashContent(src, head, head_n, game->size, rule, crc, size);
        }
      }
    }
  }
  if (zip && !ok)
    LogF("netplay: %s: a zip whose game couldn't be read - the whole file's CRC32 instead", path.c_str());
  if (!ok && total > WHOLE)
  {
    // A disc image: its first and last 8 MiB, with its size (as before 01.05).
    std::vector<uint8_t> buf(1 << 20);
    uLong c = crc32(0L, Z_NULL, 0);
    const auto add = [&](uint64_t from, uint64_t n) {
      if (std::fseek(f, static_cast<long>(from), SEEK_SET) != 0)
        return false;
      while (n > 0)
      {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(n, buf.size()));
        const size_t got = std::fread(buf.data(), 1, want, f);
        if (got == 0)
          return false;
        c = crc32(c, buf.data(), static_cast<uInt>(got));
        n -= got;
      }
      return true;
    };
    ok = add(0, PART) && add(total - PART, PART);
    *crc = static_cast<uint32_t>(c);
    *size = total;
  }
  else if (!ok && std::fseek(f, 0, SEEK_SET) == 0)
  {
    Source src(f, total, 0);
    uint8_t head[128];
    const size_t head_n = src.Read(head, static_cast<size_t>(std::min<uint64_t>(sizeof(head), total)));
    if (src.Ok())
      ok = HashContent(src, head, head_n, total, RuleFor(LowerExtension(path), head, head_n, total), crc, size);
  }
  std::fclose(f);
  return ok;
}

// ---- (01.05) The standard versions ----
namespace
{
struct Standard
{
  uint32_t crc = 0, size = 0, name_at = 0;
  uint16_t name_n = 0;
  uint8_t console = 0;
};
bool StandardBefore(const Standard& x, const Standard& y)
{
  return x.console != y.console ? x.console < y.console : x.crc != y.crc ? x.crc < y.crc : x.size < y.size;
}
std::mutex s_std_mutex;
bool s_std_loaded = false;
std::vector<std::string> s_std_consoles;  // a console's key by its number (Standard::console)
std::string s_std_names;
std::vector<Standard> s_std;  // by console, CRC32, size

void LoadStandardLocked()
{
  if (s_std_loaded)
    return;
  s_std_loaded = true;
  std::string path = std::string(App::ASSETS_DIR) + "/xmb/netplay/standard.bin";
#ifndef __PS4__
  if (const char* test = std::getenv("EMUSUNI_STANDARD_DB"))
    path = test;
#endif
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
  {
    LogF("netplay: no list of standard versions (%s)", path.c_str());
    return;
  }
  std::vector<uint8_t> packed;
  uint8_t buf[65536];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    packed.insert(packed.end(), buf, buf + n);
  std::fclose(f);
  if (packed.size() < 4)
    return;
  const uLongf raw_size = packed[0] | (packed[1] << 8) | (packed[2] << 16) | (static_cast<uLongf>(packed[3]) << 24);
  if (raw_size > (16u << 20))
    return;
  std::string text(raw_size, '\0');
  uLongf out = raw_size;
  if (uncompress(reinterpret_cast<Bytef*>(&text[0]), &out, packed.data() + 4, static_cast<uLong>(packed.size() - 4)) !=
      Z_OK)
  {
    LogF("netplay: the list of standard versions %s is damaged", path.c_str());
    return;
  }
  text.resize(out);
  // "console<TAB>crc<TAB>size<TAB>name" lines ('#' a comment).
  std::map<std::string, uint8_t> numbers;
  s_std_names.reserve(text.size() / 2);
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const size_t a = text.find('\t', at);
    const size_t b = a < end ? text.find('\t', a + 1) : std::string::npos;
    const size_t c = b < end ? text.find('\t', b + 1) : std::string::npos;
    if (text[at] != '#' && c < end && numbers.size() < 255)
    {
      const std::string console = text.substr(at, a - at);
      auto it = numbers.find(console);
      if (it == numbers.end())
      {
        it = numbers.emplace(console, static_cast<uint8_t>(s_std_consoles.size())).first;
        s_std_consoles.push_back(console);
      }
      Standard s;
      s.console = it->second;
      s.crc = static_cast<uint32_t>(std::strtoul(text.c_str() + a + 1, nullptr, 16));
      s.size = static_cast<uint32_t>(std::strtoull(text.c_str() + b + 1, nullptr, 10));
      s.name_at = static_cast<uint32_t>(s_std_names.size());
      s.name_n = static_cast<uint16_t>(std::min<size_t>(end - c - 1, 65535));
      s_std_names.append(text, c + 1, s.name_n);
      s_std.push_back(s);
    }
    at = end + 1;
  }
  s_std_names.shrink_to_fit();
  std::sort(s_std.begin(), s_std.end(), StandardBefore);
  LogF("netplay: the list of standard versions: %zu entries of %zu consoles (%zu KiB)", s_std.size(),
       s_std_consoles.size(), (s_std.size() * sizeof(Standard) + s_std_names.size()) / 1024);
}

int StandardConsoleLocked(const std::string& console)
{
  for (size_t i = 0; i < s_std_consoles.size(); i++)
  {
    if (s_std_consoles[i] == console)
      return static_cast<int>(i);
  }
  return -1;
}
}  // namespace

std::string StandardName(const std::string& console, uint32_t crc, uint64_t size)
{
  std::lock_guard lock(s_std_mutex);
  LoadStandardLocked();
  // (The Game Boy's and the Game Boy Color's games are alike: a game of one may be in the other's list.)
  std::vector<std::string> keys = {console};
  if (console == "gb")
    keys.push_back("gbc");
  else if (console == "gbc")
    keys.push_back("gb");
  for (const std::string& key : keys)
  {
    const int number = StandardConsoleLocked(key);
    if (number < 0 || size > 0xFFFFFFFFull)
      continue;
    Standard want;
    want.console = static_cast<uint8_t>(number);
    want.crc = crc;
    want.size = static_cast<uint32_t>(size);
    const auto it = std::lower_bound(s_std.begin(), s_std.end(), want, StandardBefore);
    if (it != s_std.end() && it->console == want.console && it->crc == crc && (size == 0 || it->size == want.size))
      return s_std_names.substr(it->name_at, it->name_n);
  }
  return {};
}

bool Covered(const std::string& console)
{
  std::lock_guard lock(s_std_mutex);
  LoadStandardLocked();
  return StandardConsoleLocked(console) >= 0;
}

std::string StandardSource(const std::string& console)
{
  if (!Covered(console))
    return {};
  return console == "arcade" ? "FinalBurn Neo 1.0.0.03" : "No-Intro";
}
}  // namespace NetplayLibrary
