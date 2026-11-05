// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "NetplayLibrary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
  uint64_t size = 0;  // the file's, as hashed (and its CRC32's size)
  int64_t mtime = 0;
  uint32_t crc = 0;
  bool ok = false;    // hashed (false: it couldn't be read)
};

struct Job
{
  std::string path;
  uint64_t size = 0;
  int64_t mtime = 0;
};

std::mutex s_mutex;
std::unordered_map<std::string, Known> s_known;  // by path
std::deque<Job> s_jobs;
std::string s_cache;       // data_dir/cache/netplay-crcs.txt
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

// The copy: "crc<TAB>size<TAB>mtime<TAB>path" lines (a file that couldn't be read isn't kept).
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
    unsigned long long size = 0;
    long long mtime = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%x\t%llu\t%lld\t%n", &crc, &size, &mtime, &used) != 3 || used <= 0 ||
        static_cast<size_t>(used) >= line.size())
      continue;
    Known k;
    k.crc = crc;
    k.size = size;
    k.mtime = mtime;
    k.ok = true;
    s_known[line.substr(static_cast<size_t>(used))] = k;
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
    char head[64];
    std::snprintf(head, sizeof(head), "%08x\t%llu\t%lld\t", k.crc, static_cast<unsigned long long>(k.size),
                  static_cast<long long>(k.mtime));
    text += head;
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
    {
      std::lock_guard lock(s_mutex);
      const auto it = s_known.find(job.path);
      if (it != s_known.end() && it->second.size == job.size && it->second.mtime == job.mtime)
      {
        s_done++;
        continue;
      }
    }
    uint32_t crc = 0;
    uint64_t size = 0;
    const bool ok = Netplay::GameCrc(job.path, &crc, &size);
    std::lock_guard lock(s_mutex);
    Known& k = s_known[job.path];
    k.ok = ok;
    k.crc = crc;
    k.size = job.size;
    k.mtime = job.mtime;
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
  s_cache = data_dir + "/cache/netplay-crcs.txt";
  LoadLocked();
  s_jobs.clear();
  s_done = 0;
  s_total = static_cast<int>(files.size());
  for (const File& file : files)
  {
    Job job;
    job.path = file.path;
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
}  // namespace NetplayLibrary
