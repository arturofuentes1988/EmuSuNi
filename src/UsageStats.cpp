// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "UsageStats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <cctype>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#ifdef __PS4__
#include <orbis/SystemService.h>
#endif

#include "Log.h"
#include "Netplay.h"
#include "NetplayProtocol.h"

namespace UsageStats
{
namespace
{
using NetProto::Heartbeat;
using NetProto::Reader;
using NetProto::StatsState;
using NetProto::Type;
using NetProto::Writer;

constexpr double START_DELAY = 10.0;  // s after the menu's first start: the app's start isn't slowed
constexpr double EVERY = 300.0;       // between heartbeats (the server may say otherwise: 1-60 minutes)
constexpr double MIN_GAP = 60.0;      // a refresh or an update check: a heartbeat no sooner than this after the last
constexpr int ANSWER_MS = 2500;       // the Cookie's and StatsAck's wait (one try each)
constexpr double LOOKUP_AGAIN = 3600.0;

struct Shared
{
  std::mutex mutex;
  bool started = false;
  bool enabled = true;
  bool announced = false;  // the server answered this install once (no longer "new")
  std::string data_dir, version, language;
  uint8_t id[NetProto::INSTALL_ID_SIZE] = {};
  std::string updated_from;  // the version before an update, until a heartbeat with it is answered
  int update_checks = 0;
  bool playing = false, playing_online = false, in_room = false;
  std::string console;
  std::string game;      // NetProto::Heartbeat::game ("": not known)
  std::string psp_path;  // a PSP disc whose ID the thread reads
  bool refresh = false;
  int online = -1;
  // The opt-in online profile (stats.ini: profile, public, code, delete).
  bool profile = false, profile_public = false, profile_delete = false;
  std::string profile_code;  // the server's ("": none known)
} s;

// ---- The game's identity (SetGame) ----
// A PSP disc's ID ("ULES-01586"): UMD_DATA.BIN at the root of its ISO 9660 image (as src/CheatDownload.cpp reads it);
// "" for a compressed image or anything else.
std::string PspDiscId(const std::string& path)
{
  std::string extension = path.size() >= 4 ? path.substr(path.size() - 4) : "";
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".iso")
    return {};
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file)
    return {};
  auto read_at = [file](uint64_t offset, void* out, size_t size) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, file) == size;
  };
  auto le32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
  };
  std::string id;
  std::vector<uint8_t> volume(2048);
  if (read_at(16 * 2048, volume.data(), volume.size()) && volume[0] == 1 && std::memcmp(&volume[1], "CD001", 5) == 0)
  {
    const uint8_t* root = &volume[156];
    const uint32_t size = std::min<uint32_t>(le32(root + 10), 64 * 1024);
    std::vector<uint8_t> dir(size);
    if (size > 0 && read_at(static_cast<uint64_t>(le32(root + 2)) * 2048, dir.data(), size))
    {
      for (size_t at = 0; at + 34 <= size;)
      {
        const uint8_t length = dir[at];
        if (length == 0)
        {
          at = (at / 2048 + 1) * 2048;
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
  std::fclose(file);
  return id.size() == 10 && id[4] == '-' ? id : std::string();
}

// A disc's code as the lists write it: capitals, digits and '-' ("SCUS-94426"), else "".
std::string DiscCode(const std::string& code)
{
  std::string out;
  for (const char c : code)
  {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!std::isalnum(static_cast<unsigned char>(u)) && u != '-')
      return {};
    out += u;
  }
  return out.size() >= 4 && out.size() <= 12 ? out : std::string();
}

// A cartridge's CRC32 as No-Intro's lists have it: a Super Nintendo copier header (512 bytes) left out, a Nintendo 64
// image in big-endian order (.z64) whatever its file's (.v64 swaps each 2 bytes, .n64 each 4).
uint32_t CartridgeCrc(const std::string& console, const std::vector<uint8_t>& rom)
{
  const uint8_t* data = rom.data();
  size_t size = rom.size();
  if (console == "snes" && size % 1024 == 512 && size > 512)
  {
    data += 512;
    size -= 512;
  }
  int swap = 0;  // bytes swapped in each group of `swap`
  if (console == "n64" && size >= 4 && size % 4 == 0)
  {
    if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12)
      swap = 2;
    else if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80)
      swap = 4;
  }
  if (swap == 0)
    return static_cast<uint32_t>(crc32(0L, data, static_cast<uInt>(size)));
  uLong crc = crc32(0L, Z_NULL, 0);
  std::vector<uint8_t> chunk(64 * 1024);
  for (size_t at = 0; at < size; at += chunk.size())
  {
    const size_t n = std::min(chunk.size(), size - at);
    for (size_t i = 0; i < n; i += swap)
    {
      for (int k = 0; k < swap; k++)
        chunk[i + k] = data[at + i + swap - 1 - k];
    }
    crc = crc32(crc, chunk.data(), static_cast<uInt>(n));
  }
  return static_cast<uint32_t>(crc);
}

double Now()
{
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---- (01.04) The community online modes (CoreLine): atomics only - a core's thread writes, the heartbeat reads ----
enum Community
{
  PSP_ADHOC,
  PSP_INFRA,
  DCNET,
  WFC_WIIMMFI,
  WFC_WIILINK,
};
const char* const COMMUNITY[] = {"psp-adhoc", "psp-infra", "dcnet", "wfc-wiimmfi", "wfc-wiilink"};
constexpr double COMMUNITY_RECENT = 300.0;  // s: a sign this recent says the mode is connected
std::atomic<int> s_community{-1};           // the mode connected (Community), -1: none
std::atomic<int64_t> s_community_ms{0};     // its last sign (Now(), in ms)
std::atomic<int> s_ds_server{-1};           // SetDsWifi: the DS's mode, -1: none counted (off, another DNS)
constexpr unsigned long DS_PACKETS_IN = 10;  // packets in a DS's Wi-Fi line (~10 s) that say it is online (the DS
                                             // specialist: 1-4 stray ones - DHCP, ARP - with the Wi-Fi on, connected or
                                             // not; a connection test, a login, a match: tens)
constexpr double PSP_INFRA_KIB_IN = 1.0;     // a UDP-only game's servers: more than a DNS answer (under 0.5 KiB)

void CommunityOff(int mode)
{
  int was = mode;
  s_community.compare_exchange_strong(was, -1);
}

void CommunitySign(int mode)
{
  s_community_ms = static_cast<int64_t>(Now() * 1000.0);
  s_community = mode;
}

std::string CommunityNow()
{
  const int mode = s_community;
  if (mode < 0 || mode >= static_cast<int>(sizeof(COMMUNITY) / sizeof(COMMUNITY[0])) ||
      Now() * 1000.0 - static_cast<double>(s_community_ms.load()) > COMMUNITY_RECENT * 1000.0)
    return {};
  return COMMUNITY[mode];
}

#ifndef __PS4__
// The PC test: heartbeats every this many seconds (EMUSUNI_STATS_EVERY), from an address of the loopback of its own
// (EMUSUNI_STATS_BIND: the server's countries come from the test's country file).
double TestEvery()
{
  const char* every = std::getenv("EMUSUNI_STATS_EVERY");
  return every ? std::max(0.2, std::atof(every)) : 0.0;
}
#endif

std::string Trim(const std::string& text)
{
  const size_t a = text.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return {};
  const size_t b = text.find_last_not_of(" \t\r\n");
  return text.substr(a, b - a + 1);
}

std::string Hex(const uint8_t* p, size_t n)
{
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++)
  {
    out += digits[p[i] >> 4];
    out += digits[p[i] & 15];
  }
  return out;
}

bool FromHex(const std::string& text, uint8_t* out, size_t n)
{
  if (text.size() != n * 2)
    return false;
  for (size_t i = 0; i < n * 2; i++)
  {
    const char c = text[i];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0)
      return false;
    out[i / 2] = static_cast<uint8_t>(i % 2 == 0 ? v << 4 : out[i / 2] | v);
  }
  return true;
}

uint64_t Mix(uint64_t x)
{
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// A random install id: the CPU's cycle counter, the time to the nanosecond and where this process's memory is - no id
// of the PS4, its users or PSN.
void MakeId(uint8_t out[NetProto::INSTALL_ID_SIZE])
{
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t a = static_cast<uint64_t>(now.tv_nsec) ^ (static_cast<uint64_t>(now.tv_sec) << 30);
#if defined(__x86_64__)
  a ^= Mix(__builtin_ia32_rdtsc());
#endif
  void* heap = std::malloc(32);
  uint64_t b = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&now)) ^
               (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(heap)) << 7) ^ static_cast<uint64_t>(getpid());
  std::free(heap);
  for (int round = 0; round < 4; round++)
  {
    a = Mix(a ^ b);
#if defined(__x86_64__)
    b = Mix(b ^ a ^ __builtin_ia32_rdtsc());
#else
    b = Mix(b ^ a);
#endif
  }
  std::memcpy(out, &a, 8);
  std::memcpy(out + 8, &b, 8);
}

// The PS4's language (its system setting, not the app's).
std::string Language()
{
#ifdef __PS4__
  int32_t lang = -1;
  if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &lang) != 0)
    return "?";
  static const char* const CODES[] = {"ja", "en-US", "fr",    "es",    "de",    "it",     "nl", "pt-PT",
                                      "ru", "ko",    "zh-Hant", "zh-Hans", "fi",  "sv",     "da", "no",
                                      "pl", "pt-BR", "en-GB", "tr",    "es-419", "ar",   "fr-CA", "cs",
                                      "hu", "el",    "ro",    "th",    "vi",    "id"};
  if (lang >= 0 && lang < static_cast<int32_t>(sizeof(CODES) / sizeof(CODES[0])))
    return CODES[lang];
  return "lang" + std::to_string(lang);
#else
  const char* lang = std::getenv("EMUSUNI_STATS_LANG");
  return lang && *lang ? lang : "pc";
#endif
}

std::string IniPath()
{
  return s.data_dir + "/stats.ini";
}

void SaveLocked()
{
  std::string text = "# EmuSuNi's anonymous statistics (Settings -> Anonymous Statistics; README: Anonymous statistics).\n"
                     "# id: a random number this app made once - not the PS4's, a user's or PSN's.\n";
  text += std::string("enabled=") + (s.enabled ? "1" : "0") + "\n";
  text += "id=" + Hex(s.id, sizeof(s.id)) + "\n";
  text += std::string("announced=") + (s.announced ? "1" : "0") + "\n";
  text += "version=" + s.version + "\n";
  if (!s.updated_from.empty())
    text += "updated_from=" + s.updated_from + "\n";
  text += std::string("profile=") + (s.profile ? "1" : "0") + "\n";
  text += std::string("public=") + (s.profile_public ? "1" : "0") + "\n";
  if (!s.profile_code.empty())
    text += "code=" + s.profile_code + "\n";
  if (s.profile_delete)
    text += "delete=1\n";
  WriteTextFile(IniPath(), text);
}

// ---- One heartbeat: Hello -> Cookie, Stats -> StatsAck (a try each) ----
sockaddr_in ToSockaddr(uint32_t ip, uint16_t port)
{
  sockaddr_in a;
  std::memset(&a, 0, sizeof(a));
#ifdef __PS4__
  a.sin_len = sizeof(a);
#endif
  a.sin_family = AF_INET;
  a.sin_port = static_cast<uint16_t>((port >> 8) | (port << 8));
  a.sin_addr.s_addr = __builtin_bswap32(ip);
  return a;
}

// The answer of `type` with `nonce` (else nullptr after `ms`): its bytes after the nonce in *rest.
bool Answer(int fd, Type type, uint32_t nonce, int ms, std::vector<uint8_t>* rest, bool* refused)
{
  const double until = Now() + ms / 1000.0;
  uint8_t buf[NetProto::MAX_PACKET];
  while (true)
  {
    const int left = static_cast<int>((until - Now()) * 1000.0);
    if (left <= 0)
      return false;
    pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, left) <= 0)
      continue;
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    uint8_t version = 0;
    if (n < static_cast<ssize_t>(NetProto::HEADER + 4) || !NetProto::IsOurs(buf, static_cast<size_t>(n), &version))
      continue;
    Reader r(buf + NetProto::HEADER, static_cast<size_t>(n) - NetProto::HEADER);
    const Type got = static_cast<Type>(buf[3]);
    if (r.U32() != nonce)
      continue;
    if (got == Type::Refused)
    {
      *refused = true;
      return false;
    }
    if (got != type)
      continue;
    rest->assign(r.Here(), r.Here() + r.Left());
    return true;
  }
}

enum class Result
{
  Answered,
  NoServer,   // none known (netplay.ini, the GitHub list and SERVER name none) or not found
  NoAnswer,
  Refused,
};

// The answer's profile byte and code (a server before the profiles: none).
struct ProfileAnswer
{
  NetProto::ProfileState state = NetProto::ProfileState::None;
  std::string code;
};

Result Beat(const Heartbeat& beat, const std::string& data_dir, int* online, double* every, ProfileAnswer* profile)
{
  // Where: the main server, looked up again once an hour.
  static uint32_t ip = 0;
  static uint16_t port = 0;
  static double looked_up = -1e9;
  if (ip == 0 || Now() - looked_up > LOOKUP_AGAIN)
  {
    looked_up = Now();
    ip = 0;
    const std::vector<std::string> servers = Netplay::MainServers(data_dir, 12.0);
    if (servers.empty())
      return Result::NoServer;
    const std::string where = Trim(servers.front());
    const size_t colon = where.rfind(':');
    const std::string host = colon == std::string::npos ? where : where.substr(0, colon);
    const long p = colon == std::string::npos ? 0 : std::strtol(where.c_str() + colon + 1, nullptr, 10);
    port = p > 0 && p < 65536 ? static_cast<uint16_t>(p) : NetProto::DEFAULT_PORT;
    ip = host.empty() ? 0 : Netplay::LookUp(host);
    if (ip == 0)
      return Result::NoServer;
  }
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return Result::NoAnswer;
  uint32_t local = 0;
#ifndef __PS4__
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (const char* bind_to = std::getenv("EMUSUNI_STATS_BIND"))
  {
    if (std::sscanf(bind_to, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      local = (a << 24) | (b << 16) | (c << 8) | d;
  }
#endif
  const sockaddr_in any = ToSockaddr(local, 0);
  const sockaddr_in to = ToSockaddr(ip, port);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0)
  {
    close(fd);
    return Result::NoAnswer;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
  const uint32_t nonce = static_cast<uint32_t>(Mix(static_cast<uint64_t>(Now() * 1e6) ^ beat.id[0] ^ (beat.id[5] << 8)));
  Result result = Result::NoAnswer;
  uint8_t buf[NetProto::MAX_PACKET];
  Writer hello(buf, sizeof(buf));
  hello.Header(Type::Hello);
  hello.U32(nonce);
  hello.PadTo(NetProto::HELLO_SIZE);
  sendto(fd, buf, hello.Size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
  std::vector<uint8_t> rest;
  bool refused = false;
  if (Answer(fd, Type::Cookie, nonce, ANSWER_MS, &rest, &refused) && rest.size() >= NetProto::COOKIE_SIZE)
  {
    Writer w(buf, sizeof(buf));
    w.Header(Type::Stats);
    w.Bytes(rest.data(), NetProto::COOKIE_SIZE);
    w.U32(nonce);
    NetProto::WriteHeartbeat(w, beat);
    w.PadTo(NetProto::STATS_SIZE);
    if (w.Ok())
    {
      sendto(fd, buf, w.Size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      if (Answer(fd, Type::StatsAck, nonce, ANSWER_MS, &rest, &refused))
      {
        Reader r(rest.data(), rest.size());
        const uint32_t count = r.U32();
        const uint16_t seconds = r.U16();
        if (r.Ok())
        {
          *online = static_cast<int>(std::min<uint32_t>(count, 99999999));
          *every = std::clamp(static_cast<double>(seconds), 60.0, 3600.0);
          result = Result::Answered;
          if (r.Left() > 0)
          {
            const uint8_t state = r.U8();
            const std::string code = r.Str(NetProto::PROFILE_CODE_SIZE);
            if (r.Ok() && state <= 2)
            {
              profile->state = static_cast<NetProto::ProfileState>(state);
              profile->code = code;
            }
          }
        }
      }
    }
  }
  if (refused)
    result = Result::Refused;
  close(fd);
  return result;
}

void* Thread(void*)
{
  double every = EVERY, start_delay = START_DELAY, min_gap = MIN_GAP;
#ifndef __PS4__
  if (TestEvery() > 0.0)
  {
    every = TestEvery();
    start_delay = 0.2;
    min_gap = every;
  }
#endif
  const double first = Now() + start_delay;
  double next = first, last_sent = -1e9;
  Result last_result = Result::Answered;
  bool told = false;
  while (true)
  {
    usleep(every < 5.0 ? 50 * 1000 : 500 * 1000);
    Heartbeat beat;
    std::string data_dir, psp_path;
    {
      std::lock_guard lock(s.mutex);
      psp_path.swap(s.psp_path);
    }
    if (!psp_path.empty())
    {
      // (A PSP disc's ID read here, off the game's thread: a few sectors of its image.)
      const std::string id = PspDiscId(psp_path);
      std::lock_guard lock(s.mutex);
      if (s.playing && s.console == "psp" && s.game.empty() && !id.empty())
        s.game = "d:" + id;
    }
    {
      std::lock_guard lock(s.mutex);
      const double now = Now();
      const bool soon = (s.refresh || s.update_checks > 0 || s.profile_delete) && now - last_sent >= min_gap &&
                        now >= first;
      // (A profile's deletion is sent even with the statistics off: alone, not counted.)
      const bool delete_only = !s.enabled && s.profile_delete;
      if ((!s.enabled && !delete_only) || (now < next && !soon))
        continue;
      std::memcpy(beat.id, s.id, sizeof(beat.id));
      beat.flags = s.announced ? 0 : NetProto::STATS_NEW_INSTALL;
      if (s.profile_delete)
        beat.flags |= NetProto::STATS_DELETE | (delete_only ? NetProto::STATS_NO_COUNT : 0);
      else if (s.profile)
        beat.flags |= NetProto::STATS_PROFILE | (s.profile_public ? NetProto::STATS_PUBLIC : 0);
      beat.app_version = s.version;
      beat.language = s.language;
      beat.state = s.playing ? (s.playing_online ? StatsState::PlayingOnline : StatsState::Playing) :
                   s.in_room ? StatsState::Room :
                               StatsState::Menu;
      beat.console = s.playing ? s.console : std::string();
      beat.game = s.playing ? s.game : std::string();
      beat.community = s.playing ? CommunityNow() : std::string();  // (01.04)
      beat.update_checks = static_cast<uint8_t>(std::min(s.update_checks, 255));
      beat.updated_from = s.updated_from;
      s.refresh = false;
      data_dir = s.data_dir;
    }
    if (beat.flags & NetProto::STATS_PUBLIC)
      beat.nick = Netplay::GetNickname(data_dir);  // (netplay.ini read here, off the menu's thread)
    int online = -1;
    double server_every = every;
    ProfileAnswer profile;
    const Result result = Beat(beat, data_dir, &online, &server_every, &profile);
    {
      std::lock_guard lock(s.mutex);
      last_sent = Now();
#ifndef __PS4__
      if (TestEvery() > 0.0)
        server_every = every;
#endif
      next = last_sent + (result == Result::Answered ? server_every : every);
      if (result == Result::Answered)
      {
        s.online = s.enabled ? online : -1;
        s.update_checks = std::max(0, s.update_checks - beat.update_checks);
        bool save = false;
        if (!s.announced)
          save = s.announced = true;
        if (!beat.updated_from.empty() && s.updated_from == beat.updated_from)
        {
          s.updated_from.clear();
          save = true;
        }
        // The profile: its code kept; deleted: forgotten.
        if ((beat.flags & NetProto::STATS_DELETE) && profile.state == NetProto::ProfileState::Deleted)
        {
          s.profile_delete = false;
          s.profile_code.clear();
          save = true;
          LogF("stats: the online profile was deleted on the server");
        }
        else if ((beat.flags & NetProto::STATS_PROFILE) && profile.state == NetProto::ProfileState::Kept &&
                 !profile.code.empty() && profile.code != s.profile_code && s.profile)
        {
          s.profile_code = profile.code;
          save = true;
          LogF("stats: the online profile is on (its code is shown in the Online tab)");
        }
        if (save)
          SaveLocked();
      }
    }
    // The log: the first answer, and each change of how it goes (not every 5 minutes).
    if (result != last_result || !told)
    {
      told = true;
      last_result = result;
      if (result == Result::Answered)
        LogF("stats: heartbeat answered (%d players online now)", online);
      else if (result == Result::NoServer)
        LogF("stats: no online server known yet: nothing sent");
      else if (result == Result::Refused)
        LogF("stats: the server refused the heartbeat (another protocol version?)");
      else
        LogF("stats: the server didn't answer (sent again in %.0f minutes)", every / 60.0);
    }
  }
  return nullptr;
}
}  // namespace

void Start(const std::string& data_dir, const std::string& version)
{
  bool on = false;
  {
    std::lock_guard lock(s.mutex);
    if (s.started)
      return;
    s.started = true;
    s.data_dir = data_dir;
    s.version = version;
    s.language = Language();
    bool has_id = false;
    std::string before;
    const std::string text = ReadTextFile(IniPath());
    size_t at = 0;
    while (at < text.size())
    {
      const size_t end = std::min(text.find('\n', at), text.size());
      const std::string line = Trim(text.substr(at, end - at));
      at = end + 1;
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos)
        continue;
      const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
      if (key == "enabled")
        s.enabled = value != "0" && value != "off" && value != "no";
      else if (key == "id")
        has_id = FromHex(value, s.id, sizeof(s.id));
      else if (key == "announced")
        s.announced = value == "1";
      else if (key == "version")
        before = value;
      else if (key == "updated_from")
        s.updated_from = value;
      else if (key == "profile")
        s.profile = value == "1";
      else if (key == "public")
        s.profile_public = value == "1";
      else if (key == "code")
        s.profile_code = value.substr(0, NetProto::PROFILE_CODE_SIZE);
      else if (key == "delete")
        s.profile_delete = value == "1";
    }
    if (!has_id)
    {
      MakeId(s.id);
      s.announced = false;
    }
    // A version other than the last one run: updated (from it), told once.
    if (!before.empty() && before != version)
      s.updated_from = before;
    if (!has_id || before != version)
      SaveLocked();
    on = s.enabled;
    LogF("stats: anonymous statistics %s%s%s", on ? "on" : "off (nothing sent)", has_id ? "" : " (a new install id)",
         s.updated_from.empty() ? "" : (" - updated from " + s.updated_from).c_str());
  }
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attr, &Thread, nullptr) != 0)
    LogLine("stats: the thread couldn't start (nothing sent)");
  pthread_attr_destroy(&attr);
}

bool Enabled()
{
  std::lock_guard lock(s.mutex);
  return s.enabled;
}

void SetEnabled(bool on)
{
  std::lock_guard lock(s.mutex);
  if (s.enabled == on)
    return;
  s.enabled = on;
  s.online = -1;
  s.refresh = on;
  if (!on)
    s.profile = false;  // (no heartbeat, no profile: the one on the server stays until deleted)
  if (s.started)
    SaveLocked();
  LogF("stats: anonymous statistics %s", on ? "on" : "off (nothing sent)");
}

void SetPlaying(const char* console_key, bool online)
{
  std::lock_guard lock(s.mutex);
  s.playing = console_key != nullptr;
  s.playing_online = s.playing && online;
  s.console = console_key ? console_key : "";
  s.game.clear();  // (SetGame tells the new one)
  s.psp_path.clear();
  s_community = -1;  // (01.04: a new game, or none, connected to nothing yet)
}

void SetGame(const char* console_key, const std::string& path, const std::vector<uint8_t>& rom)
{
  const std::string console = console_key ? console_key : "";
  {
    std::lock_guard lock(s.mutex);
    if (!s.enabled || !s.playing || console != s.console)
      return;
  }
  std::string game;
  if (console == "arcade" || console == "naomi" || console == "atomiswave")
  {
    // The set's name: the file's, without its folder and extension ("mslug").
    const size_t slash = path.find_last_of('/');
    std::string set = path.substr(slash == std::string::npos ? 0 : slash + 1);
    set = set.substr(0, set.rfind('.'));
    std::transform(set.begin(), set.end(), set.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool clean = !set.empty() && set.size() <= 20 && std::all_of(set.begin(), set.end(), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'z') || c == '_';
    });
    if (clean)
      game = "s:" + set;
  }
  else if (console == "nes" || console == "snes" || console == "n64" || console == "gb" || console == "gbc" ||
           console == "gba" || console == "sg1000" || console == "sms" || console == "gg" || console == "md" ||
           console == "pce" || console == "atari2600" || console == "atari7800" || console == "atarijaguar")
  {
    if (!rom.empty() && rom.size() <= 64u * 1024 * 1024)
    {
      char text[16];
      std::snprintf(text, sizeof(text), "c:%08x", CartridgeCrc(console, rom));
      game = text;
    }
  }
  std::lock_guard lock(s.mutex);
  if (!s.playing || console != s.console)
    return;
  if (console == "psp")
    s.psp_path = path;
  else if (!game.empty())
    s.game = game;
}

void SetDiscCode(const std::string& code)
{
  const std::string clean = DiscCode(code);
  std::lock_guard lock(s.mutex);
  if (s.playing && s.console == "ps1" && !clean.empty())
    s.game = "d:" + clean;
}

void CoreLine(const char* line)
{
  if (!line)
    return;
  // Dreamcast: Flycast's DCNet lines (scripts/build-dc-core.sh): "connected with the modem in 80 ms", "connected for
  // 60 s: ..." (every minute while connected); "disconnected (...)", "can't connect: ...", "DCNet stopped: ...".
  if (const char* dcnet = std::strstr(line, "EmuSuNi: DCNet"))
  {
    if (std::strncmp(dcnet, "EmuSuNi: DCNet: connected", 25) == 0)
      CommunitySign(DCNET);
    else if (std::strncmp(dcnet, "EmuSuNi: DCNet: disconnected (", 30) == 0 ||
             std::strncmp(dcnet, "EmuSuNi: DCNet: can't connect", 29) == 0 ||
             std::strncmp(dcnet, "EmuSuNi: DCNet stopped: ", 24) == 0)  // (the DCNet thread's rescue: no "disconnected")
      CommunityOff(DCNET);
    return;
  }
  // The DS: melonDS DS's line every ~10 s while the game has the Wi-Fi on, traffic or not (scripts/build-nds-core.sh):
  // "nds: Wi-Fi: network 0.042 ms per frame (6882 calls, 6877 polls); 5 packets out, 3 in; ..." (01.04: more after
  // it). DS_PACKETS_IN or more in: online. The server: SetDsWifi's (none: another DNS, nds-dns.txt - not counted).
  // (01.04) Its login to the game's online service (GameSpy: once a minute at most, after Nintendo's login - a
  // connection test alone doesn't reach it): online at once.
  if (std::strstr(line, "nds: Wi-Fi: GameSpy login"))
  {
    if (const int server = s_ds_server; server >= 0)
      CommunitySign(server);
    return;
  }
  if (const char* wifi = std::strstr(line, "nds: Wi-Fi: network "))
  {
    const int server = s_ds_server;
    if (const char* counts = std::strstr(wifi, "); "); counts && server >= 0)
    {
      unsigned long out = 0, in = 0;
      if (std::sscanf(counts, "); %lu packets out, %lu in", &out, &in) == 2 && in >= DS_PACKETS_IN)
        CommunitySign(server);
    }
    return;
  }
  // The PSP: PPSSPP's lines (scripts/build-psp-core.sh; a category tag like "[SCENET] " may come before them).
  // Ad hoc: "EmuSuNi: logged in to the ad hoc server <server> as <nickname>" once a login; lost: "FriendFinder: Socket
  // Error (<n>) when sending OPCODE_PING"; failed: "DNS Error Resolving ...", "Socket error (<n>) when connecting to
  // AdhocServer". Every ~10 s while something went out or came in: "psp: online in 10.0 s: the games' own servers N
  // connections made, N failed, X KiB out, X KiB in; ad hoc (its server and relay) N made, N failed, X KiB out, X KiB
  // in" - ad hoc while its KiB out is above 0 (its server is pinged every 2 s), infrastructure with a connection made or
  // more than PSP_INFRA_KIB_IN in. "psp: online play off": neither. (Nothing of a line is kept: the server's name, the
  // nickname go nowhere.)
  if (std::strstr(line, "EmuSuNi: logged in to the ad hoc server "))
  {
    CommunitySign(PSP_ADHOC);
    return;
  }
  if ((std::strstr(line, "FriendFinder: Socket Error (") && std::strstr(line, "OPCODE_PING")) ||
      std::strstr(line, "DNS Error Resolving ") ||
      (std::strstr(line, "Socket error (") && std::strstr(line, "when connecting to AdhocServer")))
  {
    CommunityOff(PSP_ADHOC);
    return;
  }
  if (std::strstr(line, "psp: online play off"))
  {
    CommunityOff(PSP_ADHOC);
    CommunityOff(PSP_INFRA);
    return;
  }
  if (const char* online = std::strstr(line, "psp: online in "))
  {
    unsigned long made = 0, failed = 0, adhoc_made = 0, adhoc_failed = 0;
    double out = 0.0, in = 0.0, adhoc_out = 0.0, adhoc_in = 0.0;
    const char* games = std::strstr(online, "the games' own servers ");
    const char* adhoc = std::strstr(online, "ad hoc (its server and relay) ");
    const bool has_games = games && std::sscanf(games, "the games' own servers %lu connections made, %lu failed, %lf KiB "
                                                       "out, %lf KiB in", &made, &failed, &out, &in) == 4;
    const bool has_adhoc = adhoc && std::sscanf(adhoc, "ad hoc (its server and relay) %lu made, %lu failed, %lf KiB "
                                                       "out, %lf KiB in", &adhoc_made, &adhoc_failed, &adhoc_out,
                                                &adhoc_in) == 4;
    if (has_adhoc && adhoc_out > 0.0)
      CommunitySign(PSP_ADHOC);  // (both at once - rare -: ad hoc, the PSP's own way to play together)
    else if (has_games && (made > 0 || in > PSP_INFRA_KIB_IN))
      CommunitySign(PSP_INFRA);
  }
}

void SetDsWifi(const std::string& server)
{
  s_ds_server = server == "wiilink" ? WFC_WIILINK : server == "wiimmfi" ? WFC_WIIMMFI : -1;
}

void SetRoom(bool in_room)
{
  std::lock_guard lock(s.mutex);
  s.in_room = in_room;
}

void UpdateChecked()
{
  std::lock_guard lock(s.mutex);
  s.update_checks = std::min(s.update_checks + 1, 255);
}

void Refresh()
{
  std::lock_guard lock(s.mutex);
  s.refresh = true;
}

Profile GetProfile()
{
  std::lock_guard lock(s.mutex);
  Profile p;
  p.on = s.profile;
  p.is_public = s.profile_public;
  p.code = s.profile_code;
  p.deleting = s.profile_delete;
  return p;
}

void SetProfile(bool on)
{
  std::lock_guard lock(s.mutex);
  s.profile = on;
  if (on)
  {
    s.enabled = true;  // (a profile needs the heartbeats)
    s.profile_delete = false;
  }
  s.refresh = true;
  if (s.started)
    SaveLocked();
  LogF("stats: online profile %s", on ? "on" : "off");
}

void SetPublicProfile(bool on)
{
  std::lock_guard lock(s.mutex);
  s.profile_public = on;
  s.refresh = true;
  if (s.started)
    SaveLocked();
}

void DeleteProfile()
{
  std::lock_guard lock(s.mutex);
  s.profile = false;
  s.profile_public = false;
  s.profile_delete = true;
  s.refresh = true;
  if (s.started)
    SaveLocked();
  LogF("stats: the online profile's deletion asked");
}

int OnlineNow()
{
  std::lock_guard lock(s.mutex);
  return s.enabled ? s.online : -1;
}
}  // namespace UsageStats
