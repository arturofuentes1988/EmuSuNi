// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Netplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <mutex>
#include <new>
#include <random>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#ifdef __PS4__
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
// libSceNet's resolver, declared here as src/Net.cpp does (the 2022 OpenOrbis headers give it no prototypes).
extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetResolverCreate(const char* name, int memid, int flags);
int sceNetResolverDestroy(int rid);
int sceNetResolverStartNtoa(int rid, const char* hostname, uint32_t* addr, int timeout, int retry, int flags);
}
#include "Net.h"
#else
#include <netdb.h>
#endif

#include "Log.h"
#include "UsageStats.h"

// The voice chat (src/Voice.h, its own module): its packets go on this socket. Without it (a build before it, the PC
// test) nothing is sent.
#if __has_include("Voice.h") && !defined(EMUSUNI_NO_VOICE)
#include "Voice.h"
#else
namespace Voice
{
inline bool Start(int32_t) { return false; }
inline void Stop() {}
inline bool TakePacket(std::vector<uint8_t>*, uint16_t*) { return false; }
inline void GivePacket(int, uint16_t, const uint8_t*, size_t) {}
inline void ForgetPeer(int) {}
inline bool Talking(int) { return false; }
inline void SetPeerMuted(int, bool) {}
inline void SetSending(bool) {}
}  // namespace Voice
#endif

namespace Netplay
{
namespace
{
using Clock = std::chrono::steady_clock;
using NetProto::Address;
using NetProto::BulkKind;
using NetProto::ControlKind;
using NetProto::LobbyGame;
using NetProto::MAX_MEMBERS;
using NetProto::MAX_PORTS;
using NetProto::PortInput;
using NetProto::Reader;
using NetProto::Type;
using NetProto::Writer;

constexpr int HISTORY = 1024;  // frames of inputs kept (17 s at 60 FPS): a rollback's, a resync's
constexpr double PUNCH_EVERY = 0.15, PUNCH_AGAIN_EVERY = 1.0, PUNCH_FOR = 30.0;
constexpr double PING_EVERY = 0.5, KEEPALIVE_EVERY = 2.0, CONTROL_RESEND = 0.3, CONTROL_GIVE_UP = 60.0;
// (00.92) The ping: every 0.25 s in a match, every 0.1 s while its start waits (the delay is chosen from it); the
// median of the last RTT_SAMPLES round trips; a pong read after a pass of more than BUSY_GAP (this console busy: the
// game loading) or that says the other was, not counted - it came back late for that, not for the network. The start
// waits for FRESH_PINGS round trips measured once the guest runs its game (FRESH_WAIT s at most).
constexpr double PING_EVERY_MATCH = 0.25, PING_EVERY_START = 0.1, BUSY_GAP = 0.1, FRESH_WAIT = 1.5;
constexpr int RTT_SAMPLES = 16, FRESH_PINGS = 5;
// (00.92) The match's seconds counted one by one, the last WINDOW of them kept: the log's line every WINDOW seconds,
// what each console tells the others (its waits, its speed), the input delay's changes (each second).
constexpr int WINDOW = 10;
constexpr double ROOM_LOG_EVERY = 30.0;
constexpr double REQUEST_EVERY = 1.0;
constexpr int HELLO_TRIES = 4, REQUEST_TRIES = 6;
constexpr double SERVER_TIMEOUT = 25.0;  // in the lobby (a match goes on without the server when the path is direct)
constexpr double PEER_TIMEOUT_LOBBY = 30.0, PEER_TIMEOUT_START = 90.0, PEER_TIMEOUT_GAME = 12.0;
constexpr double DIRECT_LOST_AFTER = 3.0;  // s without a direct packet while the other's come through a relay
constexpr double RELAY_SILENT = 4.0;       // s without a packet through the chosen relay: the main server's again
constexpr double RELAY_REOPEN = 20.0;
// The most a peer packet may be: a Relay or RelayData around it still fits a datagram.
constexpr size_t PEER_MAX = NetProto::MAX_PACKET - 24;
constexpr int32_t NO_FRAME = INT32_MAX;
constexpr int BROWSE_PAGES = 4;  // pages of the list asked for (ListAll's: as many rooms as fit each, 6 to 20)
// (00.85) ListAll's tries before the app takes the server for one before it (it drops ListAll): its List instead.
constexpr int LIST_ALL_TRIES = 3;
// (00.86) A guest whose request expired still in the room this long after: the server removes it. The server's cookie
// lasts 2 to 3 minutes: the host's room asks for a new one before listing the rooms once its own is this old.
constexpr double ASK_GRACE = 10.0;
constexpr double COOKIE_FRESH = 100.0;

double Now()
{
  static const auto start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint32_t NowMs()
{
  return static_cast<uint32_t>(Now() * 1000.0);
}

uint64_t RandomNumber()
{
  static std::mt19937_64 rng(static_cast<uint64_t>(Clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ull);
  return rng();
}

// (00.86) How long a request to join waits for the host's answer (the PC test shortens it: EMUSUNI_NETPLAY_ASK=seconds).
double AskSeconds()
{
#ifndef __PS4__
  if (const char* seconds = std::getenv("EMUSUNI_NETPLAY_ASK"))
  {
    const double s = std::atof(seconds);
    if (s > 0.0)
      return s;
  }
#endif
  return ASK_SECONDS;
}

std::string Trim(const std::string& text)
{
  const size_t a = text.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return {};
  const size_t b = text.find_last_not_of(" \t\r\n");
  return text.substr(a, b - a + 1);
}

// ---- netplay.ini ----
struct Config
{
  std::string server;  // "host:port" the player set ("": none)
  std::string nickname;
  int delay = -1;     // frames; -1: from the ping
  int rollback = -1;  // frames; -1: the console's
  bool voice = true;
  bool hide_address = false;
  bool latency_bar = true;  // (00.92) the players' latency bar shown
};

bool SplitHostPort(const std::string& text, std::string* host, uint16_t* port)
{
  const std::string t = Trim(text);
  const size_t colon = t.rfind(':');
  *host = colon == std::string::npos ? t : t.substr(0, colon);
  *port = NetProto::DEFAULT_PORT;
  if (colon != std::string::npos)
  {
    const long p = std::strtol(t.c_str() + colon + 1, nullptr, 10);
    if (p > 0 && p < 65536)
      *port = static_cast<uint16_t>(p);
  }
  return !host->empty();
}

// The "key = value" lines of a text, in order.
std::vector<std::pair<std::string, std::string>> KeyValues(const std::string& text)
{
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = Trim(text.substr(at, end - at));
    at = end + 1;
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos)
      continue;
    out.push_back({Trim(line.substr(0, eq)), Trim(line.substr(eq + 1))});
  }
  return out;
}

bool IsOn(const std::string& value)
{
  return value == "on" || value == "1" || value == "yes" || value == "true";
}

Config LoadConfig(const std::string& data_dir)
{
  Config c;
  for (const auto& [key, value] : KeyValues(ReadTextFile(data_dir + "/netplay.ini")))
  {
    if (key == "server")
      c.server = value;
    else if (key == "nickname")
      c.nickname = value;
    else if (key == "delay")
      c.delay = std::clamp(static_cast<int>(std::strtol(value.c_str(), nullptr, 10)), 0, 12);
    else if (key == "rollback")
      c.rollback = std::clamp(static_cast<int>(std::strtol(value.c_str(), nullptr, 10)), 0, 12);
    else if (key == "voice")
      c.voice = value != "off" && value != "0" && value != "no";
    else if (key == "hide_address")
      c.hide_address = IsOn(value);
    else if (key == "latency_bar")
      c.latency_bar = value != "off" && value != "0" && value != "no";
  }
  return c;
}

// netplay.ini with one key set (the other lines as they are).
void SaveConfigKey(const std::string& data_dir, const std::string& key, const std::string& value)
{
  const std::string path = data_dir + "/netplay.ini";
  const std::string text = ReadTextFile(path);
  std::string out;
  bool done = false;
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find('\n', at), text.size());
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = line.find('=');
    if (eq != std::string::npos && Trim(line.substr(0, eq)) == key && line.find('#') != 0)
    {
      if (!done)
        out += key + "=" + value + "\n";
      done = true;
      continue;
    }
    out += line + "\n";
  }
  if (!done)
  {
    if (out.empty())
      out = "# EmuSuNi's online play (src/Netplay.h): server=host:port, nickname=, delay=, rollback=, voice=off, "
            "hide_address=on, latency_bar=off\n";
    out += key + "=" + value + "\n";
  }
  WriteTextFile(path, out);
}

// The player's name: netplay.ini's nickname, else the PS4 user's (cut and filtered as the server does).
std::string Nickname(const Config& c)
{
  if (!c.nickname.empty())
    return NetProto::CleanNick(c.nickname);
#ifdef __PS4__
  int32_t user = -1;
  char name[32] = {};
  if (sceUserServiceGetInitialUser(&user) == 0 && sceUserServiceGetUserName(user, name, 17) == 0 && name[0])
    return NetProto::CleanNick(name);
  return "Player";
#else
  const char* user = std::getenv("EMUSUNI_NICK");
  return NetProto::CleanNick(user && *user ? user : "PC");
#endif
}

// The players signed in on this console (its controllers), at most 4.
int SignedInPlayers()
{
#ifdef __PS4__
  OrbisUserServiceLoginUserIdList users;
  std::memset(&users, 0xFF, sizeof(users));
  if (sceUserServiceGetLoginUserIdList(&users) != 0)
    return 1;
  int n = 0;
  for (const int32_t user : users.userId)
    n += user >= 0 ? 1 : 0;
  return std::clamp(n, 1, MAX_PORTS);
#else
  const char* locals = std::getenv("EMUSUNI_LOCALS");
  return locals ? std::clamp(std::atoi(locals), 1, MAX_PORTS) : 1;
#endif
}

// The console's time zone, for the public list ("UTC-5", "UTC+5:30"): where in the world a player is, roughly.
std::string Region()
{
  int minutes = 0;
#ifdef __PS4__
  int32_t zone = 0, summer = 0;
  if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE, &zone) != 0)
    return "";
  if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME, &summer) != 0)
    summer = 0;
  minutes = zone + (summer ? 60 : 0);
#else
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_r(&now, &local);
  minutes = static_cast<int>(local.tm_gmtoff / 60);
#endif
  char text[16];
  if (minutes % 60 == 0)
    std::snprintf(text, sizeof(text), "UTC%+d", minutes / 60);
  else
    std::snprintf(text, sizeof(text), "UTC%+d:%02d", minutes / 60, std::abs(minutes % 60));
  return text;
}

// ---- The socket ----
// (The PS4's libc has the FreeBSD layout - sin_len - and its values, but MSG_DONTWAIT is Linux's: the socket is made
// non-blocking with fcntl instead, and poll() says whether a datagram waits. Its errno values aren't the kernel's:
// a failed call is just a failure.)
int s_fd = -1;

uint16_t Swap16(uint16_t v)
{
  return static_cast<uint16_t>((v >> 8) | (v << 8));
}

uint32_t Swap32(uint32_t v)
{
  return __builtin_bswap32(v);
}

sockaddr_in ToSockaddr(const Address& a)
{
  sockaddr_in s;
  std::memset(&s, 0, sizeof(s));
#ifdef __PS4__
  s.sin_len = sizeof(s);
#endif
  s.sin_family = AF_INET;
  s.sin_port = Swap16(a.port);
  s.sin_addr.s_addr = Swap32(a.ip);
  return s;
}

Address FromSockaddr(const sockaddr_in& s)
{
  Address a;
  a.ip = Swap32(s.sin_addr.s_addr);
  a.port = Swap16(s.sin_port);
  return a;
}

bool OpenSocket()
{
  if (s_fd >= 0)
    return true;
  s_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_fd < 0)
    return false;
  Address local;
#ifndef __PS4__
  // (The PC test: each console on an address of its own of the loopback, 127.0.x.y - the server's limits are per
  // address, as between real consoles.)
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (const char* bind_to = std::getenv("EMUSUNI_NETPLAY_BIND"))
  {
    if (std::sscanf(bind_to, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      local.ip = (a << 24) | (b << 16) | (c << 8) | d;
  }
#endif
  const sockaddr_in any = ToSockaddr(local);
  if (bind(s_fd, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0)
  {
    close(s_fd);
    s_fd = -1;
    return false;
  }
  const int flags = fcntl(s_fd, F_GETFL, 0);
  fcntl(s_fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
  int size = 1 << 20;
  setsockopt(s_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
  size = 512 << 10;
  setsockopt(s_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  return true;
}

void CloseSocket()
{
  if (s_fd >= 0)
    close(s_fd);
  s_fd = -1;
}

uint16_t BoundPort()
{
  sockaddr_in s;
  std::memset(&s, 0, sizeof(s));
  socklen_t len = sizeof(s);
  if (s_fd < 0 || getsockname(s_fd, reinterpret_cast<sockaddr*>(&s), &len) != 0)
    return 0;
  return FromSockaddr(s).port;
}

// The console's own address on its LAN: the one its route to the server leaves from (a socket "connected" to the
// server sends nothing).
uint32_t LanAddress(const Address& toward)
{
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return 0;
  uint32_t ip = 0;
  const sockaddr_in to = ToSockaddr(toward);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0)
  {
    sockaddr_in s;
    std::memset(&s, 0, sizeof(s));
    socklen_t len = sizeof(s);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&s), &len) == 0)
      ip = FromSockaddr(s).ip;
  }
  close(fd);
  return ip;
}

// ---- The PC test's network (EMUSUNI_NETSIM="delay=40,jitter=15,loss=0.03,block_direct=1,seed=7"): every datagram
// sent is delayed (ms, one way), some later still at random (jitter: they may arrive out of order), some lost;
// block_direct drops every datagram that isn't for a server (no hole can be punched: a relay must carry the match).
// (00.92) burst_every=8,burst_for=1.5,burst_ms=120: every 8 s, for 1.5 s, every datagram 120 ms later still (a busy
// Wi-Fi's moments). ----
#ifndef __PS4__
struct SimPacket
{
  double due = 0.0;
  Address to;
  std::vector<uint8_t> data;
};
struct Netsim
{
  bool on = false, block_direct = false;
  double delay = 0.0, jitter = 0.0, loss = 0.0;
  double burst_every = 0.0, burst_for = 0.0, burst_ms = 0.0;
  std::mt19937 rng{1};
  std::vector<SimPacket> queue;
} s_sim;
// (00.92) The datagrams on their way go out when due from a thread of their own (NetsimThread), as a network's would,
// also while the app doesn't pass (a game loading): before, they waited for its next pass and a pong answered just
// before a long one came a second late.
std::mutex s_sim_mutex;
void NetsimFlush(bool all);
void* NetsimThread(void*)
{
  for (;;)
  {
    usleep(1000);
    NetsimFlush(false);
  }
  return nullptr;
}

void LoadNetsim()
{
  const char* env = std::getenv("EMUSUNI_NETSIM");
  std::lock_guard lock(s_sim_mutex);
  s_sim = Netsim();
  if (!env || !*env)
    return;
  s_sim.on = true;
  static bool thread_started = false;
  if (!thread_started)
  {
    thread_started = true;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, NetsimThread, nullptr) == 0)
      pthread_detach(thread);
  }
  const std::string text = env;
  size_t at = 0;
  while (at < text.size())
  {
    const size_t end = std::min(text.find(',', at), text.size());
    const std::string item = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = item.substr(0, eq);
    const double value = std::atof(item.c_str() + eq + 1);
    if (key == "delay")
      s_sim.delay = value;
    else if (key == "jitter")
      s_sim.jitter = value;
    else if (key == "loss")
      s_sim.loss = value;
    else if (key == "block_direct")
      s_sim.block_direct = value != 0.0;
    else if (key == "seed")
      s_sim.rng.seed(static_cast<unsigned>(value));
    else if (key == "burst_every")
      s_sim.burst_every = value;
    else if (key == "burst_for")
      s_sim.burst_for = value;
    else if (key == "burst_ms")
      s_sim.burst_ms = value;
  }
  LogF("netplay: test network: %.0f ms one way, jitter %.0f ms, %.1f%% lost%s; bursts of %.0f ms more for %.1f s "
       "every %.0f s", s_sim.delay, s_sim.jitter, s_sim.loss * 100.0,
       s_sim.block_direct ? ", direct paths blocked" : "", s_sim.burst_ms, s_sim.burst_for, s_sim.burst_every);
}
#endif

// ---- The server's name looked up on a thread of its own (the PS4's resolver blocks up to 5 s) ----
struct Lookup
{
  std::mutex mutex;
  int generation = 0;
  bool done = false;
  uint32_t ip = 0;
  std::string error;
} s_lookup;

struct LookupJob
{
  int generation;
  std::string host;
};

bool StartThread(void* (*main)(void*), void* arg)
{
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  const bool ok = pthread_create(&thread, &attr, main, arg) == 0;
  pthread_attr_destroy(&attr);
  return ok;
}

uint32_t ResolveNow(const std::string& host, std::string* error)
{
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 &&
      d < 256)
    return (a << 24) | (b << 16) | (c << 8) | d;
#ifdef __PS4__
  static int pool = -1;
  {
    static std::mutex pool_mutex;  // (the lookup's thread and the anonymous statistics' may both get here first)
    std::lock_guard lock(pool_mutex);
    if (pool < 0)
    {
      sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
      sceNetInit();
      pool = sceNetPoolCreate("emusuni-netplay", 16 * 1024, 0);
    }
  }
  const int resolver = pool >= 0 ? sceNetResolverCreate("emusuni-netplay", pool, 0) : -1;
  uint32_t address = 0;
  const int result =
      resolver >= 0 ? sceNetResolverStartNtoa(resolver, host.c_str(), &address, 5 * 1000 * 1000, 2, 0) : -1;
  if (resolver >= 0)
    sceNetResolverDestroy(resolver);
  if (result < 0 || address == 0)
  {
    *error = "The online server couldn't be found: is the PS4 connected to the internet?";
    return 0;
  }
  return Swap32(address);  // (the resolver's address is in network order)
#else
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
  {
    *error = "The online server couldn't be found: is the PS4 connected to the internet?";
    return 0;
  }
  const uint32_t ip = FromSockaddr(*reinterpret_cast<const sockaddr_in*>(found->ai_addr)).ip;
  freeaddrinfo(found);
  return ip;
#endif
}

void* LookupThread(void* arg)
{
  LookupJob* job = static_cast<LookupJob*>(arg);
  std::string error;
  const uint32_t ip = ResolveNow(job->host, &error);
  {
    std::lock_guard lock(s_lookup.mutex);
    if (s_lookup.generation == job->generation)
    {
      s_lookup.done = true;
      s_lookup.ip = ip;
      s_lookup.error = error;
    }
  }
  delete job;
  return nullptr;
}

bool StartLookup(const std::string& host)
{
  LookupJob* job = nullptr;
  {
    std::lock_guard lock(s_lookup.mutex);
    s_lookup.generation++;
    s_lookup.done = false;
    s_lookup.ip = 0;
    s_lookup.error.clear();
    job = new (std::nothrow) LookupJob{s_lookup.generation, host};
  }
  if (!job)
    return false;
  if (StartThread(&LookupThread, job))
    return true;
  delete job;
  return false;
}

// ---- The main servers' list from EmuSuNi's GitHub repository (netplay/servers.txt), kept in data_dir/cache: read once
// a session on a thread of its own; the copy of before is used meanwhile ----
struct ServerList
{
  std::mutex mutex;
  bool started = false, done = false;
  std::string cache;  // the copy's path
} s_list;

void* ServerListThread(void*)
{
  std::string body;
#ifdef __PS4__
  Net::Request request;
  request.url = SERVERS_URL;
  request.timeout_seconds = 10;
  request.connect_seconds = 8;
  const Net::Response answer = Net::Fetch(request);
  if (answer.ok())
    body = answer.body;
  else
    LogF("netplay: the servers' list couldn't be read from GitHub (%s)", answer.error.c_str());
#endif
  std::string cache;
  {
    std::lock_guard lock(s_list.mutex);
    cache = s_list.cache;
  }
  if (body.find("server=") != std::string::npos && body.size() < 16384)
  {
    WriteTextFile(cache, body);
    LogF("netplay: the servers' list from GitHub kept (%zu bytes)", body.size());
  }
  std::lock_guard lock(s_list.mutex);
  s_list.done = true;
  return nullptr;
}

void StartServerList(const std::string& data_dir)
{
  std::lock_guard lock(s_list.mutex);
  if (s_list.started)
    return;
  s_list.started = true;
  MakeDirs(data_dir + "/cache");
  s_list.cache = data_dir + "/cache/netplay-servers.txt";
#ifdef __PS4__
  if (!StartThread(&ServerListThread, nullptr))
    s_list.done = true;
#else
  s_list.done = true;
#endif
}

// The main servers to try, in order: netplay.ini's, else the GitHub list's, else SERVER (left out while it is the
// placeholder).
std::vector<std::string> ServerCandidates(const Config& c, const std::string& data_dir)
{
  if (!c.server.empty())
    return {c.server};
  std::vector<std::string> out;
  for (const auto& [key, value] : KeyValues(ReadTextFile(data_dir + "/cache/netplay-servers.txt")))
  {
    if (key == "server" && !value.empty() && std::find(out.begin(), out.end(), value) == out.end())
      out.push_back(value);
  }
  if (std::string(SERVER).rfind("SERVER-NOT-SET", 0) != 0 && std::find(out.begin(), out.end(), SERVER) == out.end())
    out.push_back(SERVER);
  return out;
}

bool ServerListDone()
{
  std::lock_guard lock(s_list.mutex);
  return s_list.done;
}

// ---- A big block (core options, a save state) to one peer, in chunks the other acknowledges by bitmap ----
struct BulkOut
{
  bool active = false;
  uint16_t id = 0;
  BulkKind kind = BulkKind::Options;
  std::vector<uint8_t> data;  // [its size before compression u32][zlib's]
  int chunks = 0;
  std::vector<double> sent;   // when each was sent last (0: never)
  std::vector<uint8_t> acked;
  int base = 0, acked_count = 0;
  bool Done() const { return active && acked_count >= chunks; }
};

struct BulkIn
{
  bool active = false;
  uint16_t id = 0;
  BulkKind kind = BulkKind::Options;
  uint32_t total = 0;
  int chunks = 0;
  std::vector<uint8_t> data, got;
  int got_count = 0, base = 0;
  double last_ack = 0.0;
  bool ack_due = false;
  uint16_t done_id = 0;  // the last one completed (its chunks sent again are acknowledged, not taken)
  int done_chunks = -1;
};

// A Control message sent again until acknowledged.
struct ControlOut
{
  uint16_t seq = 0;
  std::vector<uint8_t> packet;
  double first = 0.0, last = 0.0;
};

// One member's inputs, frame by frame (its local players').
struct InputLog
{
  std::vector<PortInput> data = std::vector<PortInput>(static_cast<size_t>(HISTORY) * MAX_PORTS);
  int32_t latest = -1;  // every frame up to it is in
  PortInput& At(int32_t frame, int j)
  {
    return data[static_cast<size_t>(frame % HISTORY) * MAX_PORTS + static_cast<size_t>(j)];
  }
};

struct Peer
{
  bool present = false;
  NetProto::Member info;
  // The path: direct once a punch was answered; through a relay until then (and when it stops working).
  bool direct_ok = false;
  Address direct;
  uint32_t punch_nonce = 0;
  double punch_start = 0.0, last_punch = 0.0, last_direct = 0.0;
  double last_heard = 0.0, first_seen = 0.0, last_relayed = 0.0;
  bool relay_said = false;
  int relay_ready = -1;  // the chosen relay it opened on (RelayReady)
  // The round trip (00.92: the median of the last samples); its voice chat; muted by us.
  double rtt_ms = -1.0, last_ping = 0.0;
  bool voice_on = false, muted = false;
  // (00.92) The round trips (ms, newest last) and their jitter (AddRoundTrip); the pings sent (their time, answered)
  // for the losses; the round trips since the guest's game runs (the start's delay).
  std::deque<float> rtt_samples;
  double jitter_ms = -1.0;
  std::deque<std::pair<uint32_t, bool>> pings_out;
  std::vector<float> fresh;
  double ready_at = 0.0;
  // (00.92) What it says of the match, with its pings: its ping and jitter to us, its pings lost, how often it waited
  // for our inputs and how fast its game goes (per mille), its input delay. The worst margin of our inputs it had
  // (its newest of ours against its frame, as its Inputs say: under -max_rollback it waited for them), this second's
  // and the last one's.
  struct View
  {
    bool known = false;
    int ping_ms = -1, jitter_ms = -1, lost_pm = 0, waits_pm = 0, speed_pm = 1000, delay = 0;
  } says;
  int margin_now = INT32_MAX, margin_last = INT32_MAX;
  // The lobby: its game, its pings to the relays (index, ms; -1 not measured).
  LobbyGame game = LobbyGame::Looking;
  int relay_ms[NetProto::MAX_RELAYS];
  bool relay_ms_known = false;
  // Controls.
  uint16_t ctl_seq = 0;
  std::deque<ControlOut> ctl_out;
  std::deque<uint16_t> ctl_seen;
  // Bulks: one sent at a time, the others waiting their turn (the options, then the state).
  uint16_t bulk_seq = 0;
  BulkOut bulk_out;
  std::deque<std::pair<BulkKind, std::vector<uint8_t>>> bulk_queue;
  BulkIn bulk_in;
  // The match: its readiness, its frame and the inputs it acknowledged.
  bool ready = false;
  int ready_locals = 1;
  int32_t acked = -1;         // the newest of our frames it has every input of
  int32_t remote_frame = 0;   // its frame, as its last Input said
  int remote_adv = 0;         // its advantage over us, as it said
  double their_adv_avg = 0.0, my_adv_avg = 0.0;
  double last_input_sent = 0.0;
  std::map<int32_t, uint32_t> checks;  // its checksums
  int32_t resync_asked = -1;
  // (00.86) The host's view of a guest: accepted (until then it asked to join - no match, no list of games, no voice
  // chat), when it asked, its request's number, whether the request expired (the guest was told; still there
  // ASK_GRACE later, the server removes it). A guest's view of the host: accepted; (00.95) of another guest: accepted
  // once the host's Admit lists it (until then it asks to join: not one of the room's players).
  bool admitted = false;
  double asked_at = 0.0;
  uint32_t ask_serial = 0;
  bool ask_expired = false;
  // (00.95) The host's view of a guest in a Choose Together room: its games, once they came.
  std::vector<NetProto::LibraryGame> games;
  bool games_in = false;
  // (00.95) The host's view of a guest in a Game Boy link cable room: its game for Game Boy 2, its battery save.
  NetProto::LibraryGame link_game;
  std::string link_title;
  bool link_game_in = false;
  std::vector<uint8_t> link_save;
  bool link_save_in = false;
  Peer() { std::fill(std::begin(relay_ms), std::end(relay_ms), -1); }
};

// A relay server of ServerInfo's list (0: the main server itself).
struct RelayServer
{
  NetProto::RelayInfo info;
  Address address;
  uint32_t nonce = 0;
  int pings = 0;
  double ping_sent = 0.0;
  int rtt_ms = -1;
  bool opened = false;
  uint64_t session = 0;
  int opens = 0;
  double last_open = 0.0;
};

struct Saved
{
  int32_t frame = -1;
  size_t size = 0;
  std::vector<uint8_t> data;
  uint32_t memory_crc = 0;  // (Rules::check_memory: the memory's checksum as the state was saved, on a check's frame)
};

enum class Match
{
  None,
  Options,  // waiting for the host's options (a guest)
  Sync,     // the host's state on its way
  Running,
  Resync,   // a guest: the host's state on its way again
};

// What the connecting is for.
enum class Want
{
  Create,
  Join,
  List,
};

enum class Step
{
  Lookup,
  Hello,
  Request,
};

struct Session
{
  Phase phase = Phase::Off;
  bool host = false, is_public = false, hidden = false;
  std::string data_dir;
  Config config;
  std::string nick, region;
  int lobby_locals = 1;
  std::string code;
  // (00.95) The room's consoles at most (the host chooses 2 to MAX_MEMBERS; the server's Members says it to the guests).
  int max_members = 2;
  uint8_t my_id = 0;
  uint64_t token = 0, room_key = 0;
  NetProto::Ticket ticket;
  NetProto::GameInfo game;
  Address server, my_public, my_lan;
  // Connecting.
  Want want = Want::Create;
  Step step = Step::Lookup;
  std::vector<std::string> candidates;
  size_t candidate = 0;
  std::string server_name;
  uint16_t server_port = NetProto::DEFAULT_PORT;
  uint8_t cookie[NetProto::COOKIE_SIZE] = {};
  uint32_t nonce = 0, hello_nonce = 0;
  int tries = 0;
  double step_start = 0.0, last_request = 0.0, hello_sent = 0.0;
  double last_keepalive = 0.0, last_server_heard = 0.0, keepalive_sent = 0.0;
  int server_ms = 0;
  // Browsing. (00.85) Every room (ListAll: the Online tab's table), page after page into rooms_in, which becomes
  // `rooms` once complete (a list asked for again replaces the shown one whole); a server that never answers ListAll
  // is one before it: its List then (the public rooms waiting), for the rest of the session.
  std::string browse_console;
  std::vector<RoomView> rooms, rooms_in;
  int pages = 0, next_page = 0;
  int list_first = 0, rooms_total = 0;
  bool rooms_done = false;
  bool list_all_ok = false, list_old = false;
  size_t rooms_logged = SIZE_MAX;
  double last_list = 0.0;
  // The room.
  bool start_pending = false;
  Peer peers[MAX_MEMBERS];
  LobbyGame my_game = LobbyGame::Looking;
  std::vector<RelayServer> relays;
  bool info_asked = false;
  double last_info = 0.0;
  int relay_choice = 0;  // the relay every console uses (0: the main server)
  bool relay_pings_sent = false;
  std::string error;
  bool over = false;  // the match ended (error says why)
  bool voice_enabled = true, voice_running = false, voice_default_set = false;
  // The match.
  Match match = Match::None;
  Rules rules;
  double fps = 60.0;
  ApplyInputs apply = nullptr;
  const CoreApi* core = nullptr;
  // (The wrapped core - WrapCore - lives outside the session: s_wrapped.)
  unsigned devices[8] = {};
  bool device_set[8] = {};
  bool devices_locked = false;
  std::map<std::string, std::string> variables;
  bool variables_agreed = false;
  int32_t frame = 0;
  int delay = 2, max_rollback = 0;
  int first_port[MAX_MEMBERS] = {}, ports[MAX_MEMBERS] = {};
  int total_ports = 0;
  InputLog inputs[MAX_MEMBERS];
  std::vector<PortInput> used = std::vector<PortInput>(static_cast<size_t>(HISTORY) * MAX_PORTS);
  PortInput local_now[MAX_PORTS];
  int local_count = 1;
  int32_t local_next = 0;
  std::vector<Saved> ring;
  int32_t rollback_to = NO_FRAME;
  int32_t floor = 0;  // no rollback to a frame before it (a resync's frame)
  bool replaying = false;
  int32_t next_check = 0, check_floor = 0;
  std::map<int32_t, uint32_t> my_checks;
  std::pair<int32_t, uint32_t> last_check{-1, 0};
  std::vector<uint8_t> scratch;
  int skip_passes = 0;
  int32_t last_sync_frame = 0;
  double stalled_since = -1.0;
  // (00.92) The match's seconds (WINDOW of them, the newest last): frames run, passes that wanted frames, those that
  // waited for input (and for whose: the one with the oldest inputs) or for the time sync, rollbacks, frames run
  // again, the deepest, inputs that came while the pass waited a moment for them; each member's worst margin of its inputs here (its newest against our frame: under 0 the
  // frame ran with its input predicted). The input delay's next change: the seconds it could be lower.
  struct Second
  {
    int frames = 0, passes = 0, stalls = 0, waits = 0, rollbacks = 0, replayed = 0, deepest = 0, caught = 0;
    int waited_for[MAX_MEMBERS] = {};
    int margin[MAX_MEMBERS] = {INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX};
  };
  std::deque<Second> seconds;
  Second second;
  double second_start = 0.0;
  int seconds_logged = 0;
  int delay_lower = 0;
  // (00.92) The passes: the time since the one before (a long one: this console was busy, its pongs come late), the
  // core's time per frame (its average: how long a pass may wait for an input about to come).
  double last_pump = 0.0, pump_gap = 0.0;
  double core_ms = 0.0;
  double room_logged = 0.0;  // (00.92) the room's last line of its players' pings in the log
  double last_resync = -100.0;
  double resync_started = -1.0;
  // The resyncs so far (the host counts them; a guest takes the count with each state): a checksum made under
  // another count was made before that resync and isn't compared.
  uint32_t epoch = 0;
  // The blocks that came in whole, for whoever takes them (the options, the states).
  std::vector<std::pair<BulkKind, std::vector<uint8_t>>> blocks;
  Stats stats;
  // A Choose Together room (the room's game is NetProto::TOGETHER; `game` is the match's once the host picks one):
  // this console's games (complete once every file is hashed), a guest's sent to the host (00.95: each guest's in its
  // Peer), the host's answer - the games every player has -, a guest's suggestion; back to the room after a match
  // (to_room: the match ended so, not by a player leaving), the reason told (notice).
  NetProto::GameInfo room_game;
  bool together = false;
  std::vector<NetProto::LibraryGame> my_games, common;
  bool my_games_complete = false, games_sent = false, common_ready = false;
  NetProto::LibraryGame suggested;
  bool has_suggestion = false;
  int suggested_by = -1;  // (00.95) whose suggestion (a member id)
  // (00.95) A Game Boy link cable room: a guest's own game for Game Boy 2, its title and battery save (packed), whether
  // they went to the host; whether the host has that game (its own lookup: the host; what it said: a guest).
  NetProto::LibraryGame link_game;
  std::string link_title;
  std::vector<uint8_t> link_save;
  bool link_set = false, link_sent = false;
  LobbyGame link_found = LobbyGame::Looking;
  bool to_room = false, peer_left = false;
  std::string notice;
  // (00.86) A guest: the host accepted it (until then it waits - its games aren't sent, no match), since when it waits.
  // The host: its requests' count (each one a new number: the menu's notice). The host's room listing the rooms (the
  // Online tab while the room stays open): on the room's own socket, a new cookie first when cookie_at is old.
  bool admitted = false;
  double waiting_since = 0.0;
  uint32_t ask_serial = 0;
  // (00.95) A guest: the members the host accepted (a bit each, its last Admit's), the host's always.
  uint8_t admitted_mask = 1;
  bool room_list = false;
  double cookie_at = -1000.0;
} S;

// The core the emulator calls in a match (WrapCore: its controller devices kept) and the core itself - outside the
// session, which a room's end clears while the emulator may still call it (the game's unload).
CoreApi s_wrapped = {};
const CoreApi* s_real_core = nullptr;

// What the screen shows, for any thread.
std::mutex s_status_mutex;
Status s_status;

void ResetSession()
{
  const std::string error = S.error;
  S.~Session();
  new (&S) Session();
  S.error = error;
}

// ---- Sending ----
void SendRaw(const Address& to, const uint8_t* data, size_t size)
{
  if (s_fd < 0 || !to.Valid())
    return;
  S.stats.packets_out++;
#ifndef __PS4__
  if (s_sim.on)
  {
    bool server = to == S.server;
    for (const RelayServer& r : S.relays)
      server = server || to == r.address;
    if (s_sim.block_direct && !server)
      return;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    if (unit(s_sim.rng) < s_sim.loss)
      return;
    SimPacket p;
    p.due = Now() + (s_sim.delay + unit(s_sim.rng) * s_sim.jitter) / 1000.0;
    if (s_sim.burst_every > 0.0 && std::fmod(Now(), s_sim.burst_every) < s_sim.burst_for)
      p.due += s_sim.burst_ms / 1000.0;
    p.to = to;
    p.data.assign(data, data + size);
    std::lock_guard lock(s_sim_mutex);
    s_sim.queue.push_back(std::move(p));
    return;
  }
#endif
  const sockaddr_in addr = ToSockaddr(to);
  sendto(s_fd, data, size, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

#ifndef __PS4__
void NetsimFlush(bool all)
{
  std::lock_guard lock(s_sim_mutex);
  if (!s_sim.on || s_sim.queue.empty())
    return;
  const double now = Now();
  std::vector<SimPacket> later;
  for (SimPacket& p : s_sim.queue)
  {
    if (all || p.due <= now)
    {
      const sockaddr_in addr = ToSockaddr(p.to);
      sendto(s_fd, p.data.data(), p.data.size(), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }
    else
    {
      later.push_back(std::move(p));
    }
  }
  s_sim.queue.swap(later);
}
#endif

// A peer packet's header: the type, the room's key and our member id.
void PeerHeader(Writer& w, Type type)
{
  w.Header(type);
  w.U64(S.room_key);
  w.U8(S.my_id);
}

void ServerAuth(Writer& w, Type type)
{
  w.Header(type);
  NetProto::WriteCode(w, S.code);
  w.U8(S.my_id);
  w.U64(S.token);
}

// Is the address that of a server (the main one or a relay)? Its index (0: the main), else -1.
int ServerIndex(const Address& a)
{
  if (a == S.server)
    return 0;
  for (size_t i = 1; i < S.relays.size(); i++)
  {
    if (S.relays[i].address == a)
      return static_cast<int>(i);
  }
  return -1;
}

// Through the chosen relay when both consoles opened on it and it carries packets, else through the main server.
void SendRelayed(Peer& p, const uint8_t* data, size_t size)
{
  uint8_t buf[NetProto::MAX_PACKET];
  Writer w(buf, sizeof(buf));
  const int r = S.relay_choice;
  const bool external = r > 0 && r < static_cast<int>(S.relays.size()) && S.relays[static_cast<size_t>(r)].opened &&
                        p.relay_ready == r && Now() - p.last_heard < RELAY_SILENT;
  if (external)
  {
    w.Header(Type::RelayData);
    w.U64(S.ticket.room);
    w.U8(S.my_id);
    w.U64(S.relays[static_cast<size_t>(r)].session);
    w.U8(p.info.id);
    w.Bytes(data, size);
    if (w.Ok())
      SendRaw(S.relays[static_cast<size_t>(r)].address, buf, w.Size());
    return;
  }
  ServerAuth(w, Type::Relay);
  w.U8(p.info.id);
  w.Bytes(data, size);
  if (w.Ok())
    SendRaw(S.server, buf, w.Size());
}

void SendPeer(Peer& p, const uint8_t* data, size_t size)
{
  if (size > PEER_MAX)
    return;
  if (p.direct_ok)
    SendRaw(p.direct, data, size);
  else
    SendRelayed(p, data, size);
}

Peer* FindPeer(int id)
{
  return id >= 0 && id < MAX_MEMBERS && id != S.my_id && S.peers[id].present ? &S.peers[id] : nullptr;
}

void SendControl(Peer& p, ControlKind kind, const uint8_t* payload, size_t size)
{
  ControlOut c;
  c.seq = ++p.ctl_seq;
  uint8_t buf[NetProto::MAX_PACKET];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::Control);
  w.U16(c.seq);
  w.U8(static_cast<uint8_t>(kind));
  w.Bytes(payload, size);
  if (!w.Ok())
    return;
  c.packet.assign(buf, buf + w.Size());
  c.first = c.last = Now();
  SendPeer(p, c.packet.data(), c.packet.size());
  p.ctl_out.push_back(std::move(c));
}

void SendControlAll(ControlKind kind, const uint8_t* payload, size_t size)
{
  for (Peer& p : S.peers)
  {
    if (p.present)
      SendControl(p, kind, payload, size);
  }
}

// A Quit sent a few times without waiting for its acknowledgment (we are leaving, or the host removes it): its reason
// is NetProto's QUIT_LEFT, QUIT_REMOVED, (00.86) QUIT_REFUSED or QUIT_NO_ANSWER.
void SendQuitNow(Peer& p, uint8_t reason)
{
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::Control);
  w.U16(static_cast<uint16_t>(++p.ctl_seq));
  w.U8(static_cast<uint8_t>(ControlKind::Quit));
  w.U8(reason);
  for (int i = 0; i < 3; i++)
    SendPeer(p, buf, w.Size());
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& raw)
{
  uLongf bound = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> out(4 + bound);
  const uint32_t n = static_cast<uint32_t>(raw.size());
  out[0] = static_cast<uint8_t>(n);
  out[1] = static_cast<uint8_t>(n >> 8);
  out[2] = static_cast<uint8_t>(n >> 16);
  out[3] = static_cast<uint8_t>(n >> 24);
  if (compress2(out.data() + 4, &bound, raw.data(), static_cast<uLong>(raw.size()), 1) != Z_OK)
    return {};
  out.resize(4 + bound);
  return out;
}

bool Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>* raw)
{
  if (in.size() < 4)
    return false;
  const uint32_t n = in[0] | (in[1] << 8) | (in[2] << 16) | (static_cast<uint32_t>(in[3]) << 24);
  if (n > (512u << 20))
    return false;
  raw->resize(n);
  uLongf size = n;
  if (n > 0 && uncompress(raw->data(), &size, in.data() + 4, static_cast<uLong>(in.size() - 4)) != Z_OK)
    return false;
  return size == n;
}

void SendBulk(Peer& p, BulkKind kind, const std::vector<uint8_t>& raw)
{
  BulkOut& b = p.bulk_out;
  if (b.active && !b.Done())
  {
    p.bulk_queue.push_back({kind, raw});  // (after the one on its way)
    return;
  }
  b = BulkOut();
  b.data = Compress(raw);
  if (b.data.empty() || b.data.size() > static_cast<size_t>(65535) * NetProto::BULK_CHUNK)
  {
    LogF("netplay: a block of %zu bytes is too big to send", raw.size());
    return;
  }
  b.active = true;
  b.id = ++p.bulk_seq;
  b.kind = kind;
  b.chunks = static_cast<int>((b.data.size() + NetProto::BULK_CHUNK - 1) / NetProto::BULK_CHUNK);
  b.sent.assign(static_cast<size_t>(b.chunks), 0.0);
  b.acked.assign(static_cast<size_t>(b.chunks), 0);
  LogF("netplay: sending %s to %s: %zu bytes (%zu compressed, %d chunks)",
       kind == BulkKind::Options ? "the core options" : kind == BulkKind::State  ? "the start state" :
       kind == BulkKind::Resync  ? "a resync state"   : kind == BulkKind::Games ? "this console's games" :
                                                                               "the games both have",
       p.info.nick.c_str(), raw.size(), b.data.size(), b.chunks);
}

void SendBulkChunk(Peer& p, int index)
{
  BulkOut& b = p.bulk_out;
  const size_t offset = static_cast<size_t>(index) * NetProto::BULK_CHUNK;
  const size_t n = std::min(NetProto::BULK_CHUNK, b.data.size() - offset);
  uint8_t buf[NetProto::MAX_PACKET];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::Bulk);
  w.U16(b.id);
  w.U8(static_cast<uint8_t>(b.kind));
  w.U32(static_cast<uint32_t>(b.data.size()));
  w.U16(static_cast<uint16_t>(index));
  w.U16(static_cast<uint16_t>(b.chunks));
  w.Bytes(b.data.data() + offset, n);
  if (w.Ok())
    SendPeer(p, buf, w.Size());
  b.sent[static_cast<size_t>(index)] = Now();
}

// The chunks not acknowledged within the round trip again, then new ones, up to this pass's share (less through a
// relay: it allows each member 384 KiB a second).
void PumpBulk(Peer& p)
{
  BulkOut& b = p.bulk_out;
  if ((!b.active || b.Done()) && !p.bulk_queue.empty())
  {
    const std::pair<BulkKind, std::vector<uint8_t>> next = std::move(p.bulk_queue.front());
    p.bulk_queue.pop_front();
    SendBulk(p, next.first, next.second);
  }
  if (!b.active || b.Done())
    return;
  const double now = Now();
  const double rto = std::max(0.25, (p.rtt_ms > 0.0 ? p.rtt_ms * 2.0 : 500.0) / 1000.0);
  int budget = p.direct_ok ? 24 : 5;
  const int end = std::min(b.chunks, b.base + NetProto::BULK_WINDOW);
  for (int i = b.base; i < end && budget > 0; i++)
  {
    if (b.acked[static_cast<size_t>(i)])
      continue;
    const double sent = b.sent[static_cast<size_t>(i)];
    if (sent == 0.0 || now - sent > rto)
    {
      SendBulkChunk(p, i);
      budget--;
    }
  }
}

void SendBulkAck(Peer& p)
{
  BulkIn& b = p.bulk_in;
  uint8_t buf[128];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::BulkAck);
  uint8_t bits[NetProto::BULK_WINDOW / 8] = {};
  if (b.active)
  {
    w.U16(b.id);
    w.U16(static_cast<uint16_t>(b.base));
    for (int i = 0; i < NetProto::BULK_WINDOW && b.base + i < b.chunks; i++)
    {
      if (b.got[static_cast<size_t>(b.base + i)])
        bits[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    }
  }
  else
  {
    w.U16(b.done_id);
    w.U16(static_cast<uint16_t>(std::max(0, b.done_chunks)));
  }
  w.Bytes(bits, sizeof(bits));
  SendPeer(p, buf, w.Size());
  b.last_ack = Now();
  b.ack_due = false;
}

// ---- Errors and endings ----
void Fail(const std::string& why)
{
  if (S.error.empty())
    S.error = why;
  LogF("netplay: %s", why.c_str());
}

// The match is over: the game ends with `why`.
void EndMatch(const std::string& why)
{
  if (S.over)
    return;
  S.over = true;
  Fail(why);
}

std::string PeerName(const Peer& p)
{
  return p.info.nick.empty() ? std::string("The other player") : p.info.nick;
}

// ---- (00.92) The ping: its round trips, their jitter, the pings lost; what each console says of the match ----
double MedianOf(std::vector<float> values)
{
  if (values.empty())
    return -1.0;
  const size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<long>(mid), values.end());
  return values[mid];
}

// A round trip (its ping sent at t, NowMs's): the ping is the median of the last ones; the start's delay (the host)
// takes those of pings sent once the guest's game runs.
void AddRoundTrip(Peer& p, double sample, uint32_t t)
{
  p.rtt_samples.push_back(static_cast<float>(sample));
  while (p.rtt_samples.size() > static_cast<size_t>(RTT_SAMPLES))
    p.rtt_samples.pop_front();
  // The ping: their median. The jitter: how much slower the slow ones are - the 90th percentile over the median (one
  // late round trip among them changes neither).
  std::vector<float> sorted(p.rtt_samples.begin(), p.rtt_samples.end());
  std::sort(sorted.begin(), sorted.end());
  p.rtt_ms = sorted[sorted.size() / 2];
  p.jitter_ms = sorted.size() < 4 ? -1.0 : sorted[sorted.size() * 9 / 10] - p.rtt_ms;
  if (S.match == Match::Sync && p.ready && static_cast<double>(t) >= p.ready_at * 1000.0)
    p.fresh.push_back(static_cast<float>(sample));
}

// The pings lost (per mille) of those kept: sent over 2 s ago and never answered.
int LostPermille(const Peer& p)
{
  const uint32_t now = NowMs();
  int sent = 0, lost = 0;
  for (const auto& [t, answered] : p.pings_out)
  {
    if (now - t < 2000)
      continue;
    sent++;
    lost += answered ? 0 : 1;
  }
  return sent > 0 ? lost * 1000 / sent : 0;
}

// The last `count` seconds of the match added up (the one going on not among them).
Session::Second LastSeconds(int count)
{
  Session::Second sum;
  const int n = std::min(count, static_cast<int>(S.seconds.size()));
  for (int i = static_cast<int>(S.seconds.size()) - n; i < static_cast<int>(S.seconds.size()); i++)
  {
    const Session::Second& s = S.seconds[static_cast<size_t>(i)];
    sum.frames += s.frames;
    sum.passes += s.passes;
    sum.stalls += s.stalls;
    sum.waits += s.waits;
    sum.rollbacks += s.rollbacks;
    sum.replayed += s.replayed;
    sum.deepest = std::max(sum.deepest, s.deepest);
    sum.caught += s.caught;
    for (int m = 0; m < MAX_MEMBERS; m++)
    {
      sum.waited_for[m] += s.waited_for[m];
      sum.margin[m] = std::min(sum.margin[m], s.margin[m]);
    }
  }
  return sum;
}

// A ping: its time, whether our voice chat is on, and (00.92, flag 2) what this console says of the match to that
// player: its ping and jitter (ms, 0xFFFF: not known), the pings lost (per mille), how often it waited for that
// player's inputs and how fast its game went (per mille, the last 5 s), its input delay.
void SendPing(Peer& p)
{
  const Session::Second last = LastSeconds(5);
  const bool counted = S.match == Match::Running && last.passes > 0;
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::Ping);
  const uint32_t now = NowMs();
  w.U32(now);
  w.U8(static_cast<uint8_t>((S.voice_running ? 1 : 0) | 2));
  w.U16(p.rtt_ms < 0.0 ? 0xFFFF : static_cast<uint16_t>(std::min(std::lround(p.rtt_ms), 65534L)));
  w.U16(p.jitter_ms < 0.0 ? 0xFFFF : static_cast<uint16_t>(std::min(std::lround(p.jitter_ms), 65534L)));
  w.U16(static_cast<uint16_t>(LostPermille(p)));
  w.U16(static_cast<uint16_t>(counted ? last.waited_for[p.info.id] * 1000 / last.passes : 0));
  const int seconds = std::min(5, static_cast<int>(S.seconds.size()));
  w.U16(static_cast<uint16_t>(counted && seconds > 0 ?
                                  std::min(2000L, std::lround(last.frames * 1000.0 / (S.fps * seconds))) :
                                  1000));
  w.U8(static_cast<uint8_t>(S.match == Match::Running ? S.delay : 0));
  SendPeer(p, buf, w.Size());
  p.pings_out.emplace_back(now, false);
  while (p.pings_out.size() > 80)
    p.pings_out.pop_front();
}

// ---- The match's frames ----
int32_t RemoteLatest()
{
  int32_t latest = NO_FRAME;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    if (m != S.my_id && S.peers[m].present)
      latest = std::min(latest, S.inputs[m].latest);
  }
  return latest;
}

PortInput InputFor(int member, int32_t frame, int j)
{
  InputLog& log = S.inputs[member];
  if (frame <= log.latest)
    return log.At(frame, j);
  if (log.latest >= 0)
    return log.At(log.latest, j);  // predicted: the last one known
  return PortInput();
}

// Every port's input of the frame, given to the emulator (and kept: a later real input is compared with it).
void Apply(int32_t frame)
{
  PortInput ports[MAX_PORTS];
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    if (m != S.my_id && !S.peers[m].present)
      continue;
    for (int j = 0; j < S.ports[m]; j++)
    {
      const int port = S.first_port[m] + j;
      if (port < 0 || port >= MAX_PORTS)
        continue;
      ports[port] = InputFor(m, frame, j);
      S.used[static_cast<size_t>(frame % HISTORY) * MAX_PORTS + static_cast<size_t>(port)] = ports[port];
    }
  }
  if (S.apply)
    S.apply(ports, std::max(S.total_ports, 1));
}

void RunCore()
{
  if (!S.core)
    return;
  const double start = Now();
  S.core->run();
  const double ms = (Now() - start) * 1000.0;
  S.core_ms = S.core_ms <= 0.0 ? ms : S.core_ms * 0.95 + ms * 0.05;
}

bool SerializeTo(std::vector<uint8_t>* out, size_t* size)
{
  if (!S.core)
    return false;
  const size_t n = S.core->serialize_size();
  if (n == 0)
    return false;
  if (out->size() < n)
    out->resize(n);
  if (!S.core->serialize(out->data(), n))
    return false;
  *size = n;
  return true;
}

// The checksum of the console's memory as it is now (Rules::check_memory).
uint32_t MemoryCrc()
{
  uLong c = crc32(0L, Z_NULL, 0);
  if (!S.core || !S.core->get_memory_data || !S.core->get_memory_size)
    return 0;
  for (const unsigned id : {RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_SAVE_RAM, RETRO_MEMORY_VIDEO_RAM})
  {
    const void* data = S.core->get_memory_data(id);
    const size_t size = S.core->get_memory_size(id);
    if (data && size > 0)
      c = crc32(c, static_cast<const Bytef*>(data), static_cast<uInt>(size));
  }
  return static_cast<uint32_t>(c);
}

bool SaveRing(int32_t frame)
{
  if (S.ring.empty())
    return true;
  Saved& s = S.ring[static_cast<size_t>(frame) % S.ring.size()];
  s.frame = -1;
  if (!SerializeTo(&s.data, &s.size))
    return false;
  if (S.rules.check_memory && frame % std::max(S.rules.check_every, 1) == 0)
    s.memory_crc = MemoryCrc();
  s.frame = frame;
  return true;
}

bool LoadRing(int32_t frame)
{
  if (S.ring.empty())
    return false;
  Saved& s = S.ring[static_cast<size_t>(frame) % S.ring.size()];
  return s.frame == frame && S.core && S.core->unserialize(s.data.data(), s.size);
}

// A frame's start (Rules::reload_each_frame): its state, just saved in the ring (or now), loaded again.
void Reload(int32_t frame)
{
  if (!S.rules.reload_each_frame || !S.core)
    return;
  if (!S.ring.empty())
  {
    LoadRing(frame);
    return;
  }
  size_t size = 0;
  if (SerializeTo(&S.scratch, &size))
    S.core->unserialize(S.scratch.data(), size);
}

void OnDesync(int32_t frame);

// The frames since `rollback_to` again, with the inputs known now.
void Rollback()
{
  const int32_t from = std::max(S.rollback_to, S.floor);
  S.rollback_to = NO_FRAME;
  if (from >= S.frame)
    return;
  if (!LoadRing(from))
  {
    // (Never expected: the ring holds every frame a rollback can reach. The states are made the same again.)
    LogF("netplay: no saved state for frame %d (at %d): resync", from, S.frame);
    OnDesync(from);
    return;
  }
  S.replaying = true;
  for (int32_t f = from; f < S.frame; f++)
  {
    if (f != from)
    {
      SaveRing(f);
      Reload(f);
    }
    Apply(f);
    RunCore();
  }
  S.replaying = false;
  S.stats.rollbacks++;
  S.stats.replayed += S.frame - from;
  S.second.rollbacks++;
  S.second.replayed += S.frame - from;
  S.second.deepest = std::max(S.second.deepest, static_cast<int>(S.frame - from));
}

void CompareCheck(int32_t frame, uint32_t mine, Peer& p, uint32_t theirs)
{
  if (mine == theirs)
    return;
  S.stats.mismatches++;
  LogF("netplay: frame %d differs from %s's (%08x, theirs %08x): desync", frame, p.info.nick.c_str(), mine, theirs);
  OnDesync(frame);
}

// The state checksums of the frames that became final (every input before them known), each compared with the
// others' when theirs is in.
void DoChecks()
{
  const int32_t latest = RemoteLatest();
  const int32_t final_frame = latest == NO_FRAME ? S.frame : std::min(latest + 1, S.frame);
  while (S.next_check <= final_frame)
  {
    const int32_t f = S.next_check;
    S.next_check += S.rules.check_every;
    if (f < S.check_floor)
      continue;
    uint32_t crc = 0;
    if (!S.ring.empty())
    {
      const Saved& s = S.ring[static_cast<size_t>(f) % S.ring.size()];
      if (s.frame != f)
        continue;
      crc = S.rules.check_memory ? s.memory_crc : static_cast<uint32_t>(crc32(0L, s.data.data(), static_cast<uInt>(s.size)));
    }
    else if (S.rules.check_memory)
    {
      if (f != S.frame)
        continue;
      crc = MemoryCrc();
    }
    else
    {
      size_t size = 0;
      if (f != S.frame || !SerializeTo(&S.scratch, &size))
        continue;
      crc = static_cast<uint32_t>(crc32(0L, S.scratch.data(), static_cast<uInt>(size)));
    }
    S.my_checks[f] = crc;
    while (S.my_checks.size() > 64)
      S.my_checks.erase(S.my_checks.begin());
    S.last_check = {f, crc};
    S.stats.checks++;
    S.stats.check_log.push_back({f, crc});
    for (Peer& p : S.peers)
    {
      if (!p.present)
        continue;
      auto it = p.checks.find(f);
      if (it != p.checks.end())
        CompareCheck(f, crc, p, it->second);
    }
  }
}

void OnRemoteCheck(Peer& p, int32_t frame, uint32_t crc, uint32_t epoch)
{
  if (frame < 0 || frame < S.check_floor || epoch != S.epoch || p.checks.count(frame))
    return;  // (an old one, from before a resync, or one compared already: the same report comes many times)
  p.checks[frame] = crc;
  while (p.checks.size() > 32)
    p.checks.erase(p.checks.begin());
  auto mine = S.my_checks.find(frame);
  if (mine != S.my_checks.end())
    CompareCheck(frame, mine->second, p, crc);
}

// Our local players' inputs up to `frame + delay`.
void AddLocalInputs()
{
  InputLog& log = S.inputs[S.my_id];
  while (S.local_next <= S.frame + S.delay)
  {
    for (int j = 0; j < S.ports[S.my_id] && j < MAX_PORTS; j++)
      log.At(S.local_next, j) = j < S.local_count ? S.local_now[j] : PortInput();
    log.latest = S.local_next;
    S.local_next++;
  }
}

// Our frame's advantage over the peer: our frame against where it is now (its last frame plus half the ping).
double Advantage(const Peer& p)
{
  const double frame_ms = 1000.0 / std::max(S.fps, 1.0);
  const double one_way = p.rtt_ms > 0.0 ? p.rtt_ms * 0.5 / frame_ms : 0.0;
  return static_cast<double>(S.frame) - (static_cast<double>(p.remote_frame) + one_way);
}

void SendInputs(Peer& p)
{
  InputLog& mine = S.inputs[S.my_id];
  const int ports = std::max(0, S.ports[S.my_id]);
  const bool full = S.rules.full_input;
  const size_t per_frame = static_cast<size_t>(ports) * (full ? NetProto::INPUT_FULL : NetProto::INPUT_BUTTONS);
  constexpr size_t FIXED = 4 + 9 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1 + 1 + 1;
  const int most = per_frame > 0 ? static_cast<int>(std::min<size_t>(64, (PEER_MAX - FIXED) / per_frame)) : 64;
  const int32_t first = std::max(p.acked + 1, mine.latest - HISTORY + 2);
  const int count = mine.latest >= first ? std::min(most, static_cast<int>(mine.latest - first + 1)) : 0;
  uint8_t buf[NetProto::MAX_PACKET];
  Writer w(buf, sizeof(buf));
  PeerHeader(w, Type::Input);
  w.U32(static_cast<uint32_t>(S.frame));
  w.U8(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(Advantage(p))), -127, 127))));
  w.U32(static_cast<uint32_t>(S.inputs[p.info.id].latest));
  w.U32(static_cast<uint32_t>(S.last_check.first));
  w.U32(S.last_check.second);
  w.U32(S.epoch);
  w.U32(static_cast<uint32_t>(first));
  w.U8(static_cast<uint8_t>(count));
  w.U8(static_cast<uint8_t>(ports));
  w.U8(full ? 1 : 0);
  for (int i = 0; i < count; i++)
  {
    for (int j = 0; j < ports; j++)
      NetProto::WriteInput(w, mine.At(first + i, j), full);
  }
  if (w.Ok())
    SendPeer(p, buf, w.Size());
  p.last_input_sent = Now();
}

void SendInputsAll()
{
  for (Peer& p : S.peers)
  {
    if (p.present)
      SendInputs(p);
  }
}

void OnInput(Peer& p, Reader& r)
{
  const int32_t their_frame = static_cast<int32_t>(r.U32());
  const int adv = static_cast<int8_t>(r.U8());
  const int32_t ack = static_cast<int32_t>(r.U32());
  const int32_t check_frame = static_cast<int32_t>(r.U32());
  const uint32_t check_crc = r.U32();
  const uint32_t epoch = r.U32();
  const int32_t first = static_cast<int32_t>(r.U32());
  const int count = r.U8();
  const int ports = r.U8();
  const bool full = r.U8() != 0;
  if (!r.Ok() || (S.match != Match::Running && S.match != Match::Resync))
    return;
  p.acked = std::max(p.acked, ack);
  p.remote_frame = std::max(p.remote_frame, their_frame);
  p.remote_adv = adv;
  // (00.92: how far ahead of its frame our inputs reach it - under -max_rollback it waits for them.)
  if (S.match == Match::Running && ack >= 0 && their_frame > 0)
    p.margin_now = std::min(p.margin_now, ack - their_frame);
  const int member = p.info.id;
  InputLog& log = S.inputs[member];
  for (int i = 0; i < count; i++)
  {
    const int32_t f = first + i;
    PortInput in[MAX_PORTS];
    for (int j = 0; j < ports; j++)
    {
      const PortInput value = NetProto::ReadInput(r, full);
      if (j < MAX_PORTS)
        in[j] = value;
    }
    if (!r.Ok())
      return;
    if (f != log.latest + 1 || f > S.frame + HISTORY / 2)
      continue;  // (one we have, or past a gap: it comes again)
    for (int j = 0; j < S.ports[member] && j < MAX_PORTS; j++)
      log.At(f, j) = j < ports ? in[j] : PortInput();
    log.latest = f;
    // Run already with a prediction: a different input means a rollback to it.
    if (f < S.frame && f >= S.floor && S.rollback_to > f)
    {
      for (int j = 0; j < S.ports[member]; j++)
      {
        const int port = S.first_port[member] + j;
        if (port >= 0 && port < MAX_PORTS &&
            S.used[static_cast<size_t>(f % HISTORY) * MAX_PORTS + static_cast<size_t>(port)] != log.At(f, j))
        {
          S.rollback_to = f;
          break;
        }
      }
    }
  }
  if (check_frame >= 0 && check_frame != NO_FRAME)
    OnRemoteCheck(p, check_frame, check_crc, epoch);
}

// ---- The start and resync states ----
void PutBytes(std::vector<uint8_t>& out, const void* data, size_t size)
{
  const uint8_t* p = static_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + size);
}

void PutU32(std::vector<uint8_t>& out, uint32_t v)
{
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  PutBytes(out, b, 4);
}

// [frame][resync count][battery save][state][controller devices]
void MakeStatePacket(int32_t frame, const uint8_t* state, size_t state_size, bool with_sram, std::vector<uint8_t>* out)
{
  out->clear();
  PutU32(*out, static_cast<uint32_t>(frame));
  PutU32(*out, S.epoch);
  void* sram = with_sram && S.core && S.core->get_memory_data ? S.core->get_memory_data(RETRO_MEMORY_SAVE_RAM) : nullptr;
  const size_t sram_size = sram && S.core->get_memory_size ? S.core->get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;
  PutU32(*out, sram ? static_cast<uint32_t>(sram_size) : 0xFFFFFFFFu);
  if (sram && sram_size)
    PutBytes(*out, sram, sram_size);
  PutU32(*out, static_cast<uint32_t>(state_size));
  PutBytes(*out, state, state_size);
  uint8_t count = 0;
  for (const bool set : S.device_set)
    count += set ? 1 : 0;
  out->push_back(count);
  for (int port = 0; port < 8; port++)
  {
    if (!S.device_set[port])
      continue;
    out->push_back(static_cast<uint8_t>(port));
    PutU32(*out, S.devices[port]);
  }
}

// The host's state taken: its battery save, its devices, the state; *frame its frame, *epoch its resync count.
bool TakeStatePacket(const std::vector<uint8_t>& raw, int32_t* frame, uint32_t* epoch, bool devices)
{
  Reader r(raw.data(), raw.size());
  *frame = static_cast<int32_t>(r.U32());
  *epoch = r.U32();
  const uint32_t sram_size = r.U32();
  const uint8_t* sram = sram_size != 0xFFFFFFFFu ? r.Bytes(sram_size) : nullptr;
  const uint32_t state_size = r.U32();
  const uint8_t* state = r.Bytes(state_size);
  if (!r.Ok() || !S.core)
    return false;
  if (devices)
  {
    const int count = r.U8();
    for (int i = 0; i < count && r.Ok(); i++)
    {
      const int port = r.U8();
      const unsigned device = r.U32();
      if (!r.Ok() || port >= 8)
        break;
      S.devices[port] = device;
      S.device_set[port] = true;
      if (S.core->set_controller_port_device)
        S.core->set_controller_port_device(static_cast<unsigned>(port), device);
      LogF("netplay: port %d: the host's controller device %#x", port + 1, device);
    }
    S.devices_locked = true;
  }
  if (sram && S.core->get_memory_data && S.core->get_memory_size)
  {
    void* mine = S.core->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    const size_t mine_size = S.core->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (mine && mine_size == sram_size)
      std::memcpy(mine, sram, sram_size);
    else if (sram_size > 0)
      LogF("netplay: the host's battery save is %u bytes, ours %zu: not taken", sram_size, mine_size);
  }
  if (!S.core->unserialize(state, state_size))
  {
    LogF("netplay: the host's state (%u bytes) couldn't be loaded", state_size);
    return false;
  }
  return true;
}

// The states differ at `frame`. The host sends its state again (StartResync); a guest asks it to.
void StartResync();

void OnDesync(int32_t frame)
{
  if (S.host)
  {
    StartResync();
    return;
  }
  for (Peer& p : S.peers)
  {
    if (!p.present || p.info.id != 0 || p.resync_asked == static_cast<int32_t>(S.epoch))
      continue;
    p.resync_asked = static_cast<int32_t>(S.epoch);  // (once per resync count: the host's answer raises it)
    uint8_t payload[8];
    Writer w(payload, sizeof(payload));
    w.U32(static_cast<uint32_t>(frame));
    w.U32(S.epoch);
    SendControl(p, ControlKind::ResyncRequest, payload, w.Size());
    LogF("netplay: the host is asked for its state (frame %d differs)", frame);
  }
}

// The host: its state at the newest frame whose inputs every console has (the others can load it and run on to where
// they are), with the resync count raised: every checksum made before it is left out.
void StartResync()
{
  if (!S.host || S.match != Match::Running)
    return;
  const double now = Now();
  for (const Peer& p : S.peers)
  {
    if (p.present && ((p.bulk_out.active && !p.bulk_out.Done() && p.bulk_out.kind == BulkKind::Resync) ||
                      !p.bulk_queue.empty()))
      return;  // (one at a time)
  }
  if (now - S.last_resync < 1.0)
    return;
  if (S.rollback_to < S.frame)
    Rollback();
  const int32_t latest = RemoteLatest();
  int32_t at = latest == NO_FRAME ? S.frame : std::max(S.floor, std::min(latest + 1, S.frame));
  const uint8_t* state = nullptr;
  size_t size = 0;
  if (at < S.frame && !S.ring.empty())
  {
    const Saved& s = S.ring[static_cast<size_t>(at) % S.ring.size()];
    if (s.frame == at)
    {
      state = s.data.data();
      size = s.size;
    }
  }
  if (!state)
  {
    at = S.frame;
    if (!SerializeTo(&S.scratch, &size))
    {
      LogLine("netplay: resync: the state couldn't be made");
      return;
    }
    state = S.scratch.data();
  }
  S.epoch++;
  std::vector<uint8_t> raw;
  MakeStatePacket(at, state, size, at == S.frame, &raw);
  S.last_resync = now;
  S.check_floor = std::max(S.check_floor, at);
  for (Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    p.checks.clear();
    SendBulk(p, BulkKind::Resync, raw);
  }
  S.stats.resyncs++;
  LogF("netplay: resync %u: the state of frame %d sent (the host at frame %d)", S.epoch, at, S.frame);
}

// A guest: the host's resync state loaded, and the frames from it to ours run again.
void ApplyResync(const std::vector<uint8_t>& raw)
{
  int32_t at = 0;
  uint32_t epoch = 0;
  if (!TakeStatePacket(raw, &at, &epoch, false))
  {
    S.match = Match::Running;
    LogLine("netplay: the resync state couldn't be loaded");
    return;
  }
  S.epoch = epoch;
  const int32_t was = S.frame;
  if (at < S.frame)
  {
    S.replaying = true;
    for (int32_t f = at; f < S.frame; f++)
    {
      SaveRing(f);
      Reload(f);
      Apply(f);
      RunCore();
    }
    S.replaying = false;
  }
  else
  {
    S.frame = at;
  }
  S.rollback_to = NO_FRAME;
  S.floor = at;
  S.check_floor = at;
  S.my_checks.clear();
  S.last_check = {-1, 0};
  S.next_check = (at + S.rules.check_every - 1) / S.rules.check_every * S.rules.check_every;
  for (Peer& p : S.peers)
    p.checks.clear();
  S.match = Match::Running;
  S.stats.resyncs++;
  LogF("netplay: resync %u: the host's state of frame %d loaded (we were at %d, %.0f ms after it began)", epoch, at,
       was, (Now() - S.resync_started) * 1000.0);
}

// The voice chat on or off as the player wants it, while the match runs.
void ApplyVoice()
{
  const bool want = S.voice_enabled && S.phase == Phase::Playing && !S.over;
  if (want && !S.voice_running)
  {
#ifdef __PS4__
    int32_t user = -1;
    sceUserServiceGetInitialUser(&user);
#else
    const int32_t user = 0;
#endif
    S.voice_running = Voice::Start(user);
    Voice::SetSending(true);
    for (const Peer& p : S.peers)
    {
      if (p.present && p.muted)
        Voice::SetPeerMuted(p.info.id, true);
    }
    LogF("netplay: voice chat %s", S.voice_running ? "on" : "not started (no microphone, or Voice Chat off)");
    if (!S.voice_running)
      S.voice_enabled = false;
  }
  else if (!want && S.voice_running)
  {
    Voice::Stop();
    S.voice_running = false;
    LogLine("netplay: voice chat off");
  }
}

// Frame 0: every member's first `delay` frames of input are neutral (no one pressed anything before the match).
void StartFrames()
{
  S.frame = 0;
  S.total_ports = 0;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    S.inputs[m].latest = -1;
    if (m != S.my_id && !S.peers[m].present)
      continue;
    for (int32_t f = 0; f < S.delay; f++)
    {
      for (int j = 0; j < MAX_PORTS; j++)
        S.inputs[m].At(f, j) = PortInput();
    }
    S.inputs[m].latest = S.delay - 1;
    S.total_ports = std::max(S.total_ports, S.first_port[m] + S.ports[m]);
  }
  std::fill(S.used.begin(), S.used.end(), PortInput());
  S.local_next = S.delay;
  S.rollback_to = NO_FRAME;
  S.floor = 0;
  S.next_check = 0;
  S.check_floor = 0;
  S.my_checks.clear();
  S.last_check = {-1, 0};
  S.skip_passes = 0;
  S.last_sync_frame = 0;
  S.stalled_since = -1.0;
  S.ring.clear();
  if (S.max_rollback > 0)
  {
    S.ring.resize(static_cast<size_t>(S.max_rollback) + 2);
    const size_t size = S.core ? S.core->serialize_size() : 0;
    for (Saved& s : S.ring)
      s.data.resize(size);
  }
  for (Peer& p : S.peers)
  {
    p.acked = S.delay - 1;
    p.remote_frame = 0;
    p.remote_adv = 0;
    p.my_adv_avg = p.their_adv_avg = 0.0;
    p.checks.clear();
    p.margin_now = p.margin_last = INT32_MAX;
  }
  S.seconds.clear();
  S.second = Session::Second();
  S.second_start = Now();
  S.seconds_logged = 0;
  S.delay_lower = 0;
  S.match = Match::Running;
  S.phase = Phase::Playing;
  S.stats.delay = S.delay;
  S.stats.max_rollback = S.max_rollback;
  std::string ports;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    if ((m == S.my_id || S.peers[m].present) && S.ports[m] > 0)
    {
      char part[96];
      std::snprintf(part, sizeof(part), "%s%s %d-%d", ports.empty() ? "" : ", ",
                    m == S.my_id ? "us" : S.peers[m].info.nick.c_str(), S.first_port[m] + 1,
                    S.first_port[m] + S.ports[m]);
      ports += part;
    }
  }
  LogF("netplay: the match starts: input delay %d frames, %s, checksum every %d frames; ports %s", S.delay,
       S.max_rollback > 0 ? ("rollback up to " + std::to_string(S.max_rollback) + " frames").c_str() : "delay-based",
       S.rules.check_every, ports.c_str());
  ApplyVoice();
}

// (00.92) How far ahead of the others' inputs this console runs: max_rollback, less when its core is slow - a rollback
// that far and the frame itself in about two refreshes (00.91's log, the PS4: CPS2 and Snes9x 4-4.6 ms a frame, 6; a
// board twice as slow, 3). Each console its own (the ring keeps max_rollback's states).
int RollbackReach()
{
  if (S.max_rollback <= 0 || S.core_ms <= 0.0)
    return S.max_rollback;
  const int fits = static_cast<int>(2000.0 / std::max(S.fps, 1.0) / S.core_ms) - 1;
  return std::clamp(fits, std::min(2, S.max_rollback), S.max_rollback);
}

// The input delay for the ping: about half the one-way trip played late on both consoles, the rest predicted -
// never more than a rollback can undo. Delay-based: the whole trip, and a little for the jitter.
int ChooseDelay(double rtt_ms)
{
  if (S.config.delay >= 0)
    return S.config.delay;
  const double frame_ms = 1000.0 / std::max(S.fps, 1.0);
  const double one_way = rtt_ms > 0.0 ? rtt_ms * 0.5 : 50.0;
  const int trip = static_cast<int>(std::ceil(one_way / frame_ms));
  if (S.max_rollback == 0)
    return std::clamp(static_cast<int>(std::ceil((one_way + 12.0) / frame_ms)) + 1, 1, 12);
  int delay = std::clamp((trip + 1) / 2, 1, 3);
  delay = std::max(delay, trip - RollbackReach() + 2);
  return std::clamp(delay, 1, 8);
}

// (00.92) The input delay in the match, each second - each console its own: it is when our inputs are played, which
// both consoles know with each input (netplay.ini's delay= keeps its own). What the ping and its jitter call for now
// (ChooseDelay, as at the start, with the slow round trips: the 90th percentile); a frame more at once when another
// console waited for our inputs (they reached it later than its rollback covers); one less after 5 seconds in a row
// that ask for less (a busy Wi-Fi's moments come back). Not in the match's first 2 seconds (one console starts a trip after the other). A frame more
// plays the next input twice; one less leaves one out.
void AdaptDelay()
{
  if (S.config.delay >= 0 || S.match != Match::Running || S.seconds.size() < 2)
    return;
  double rtt = -1.0, jitter = 0.0;
  bool waited = false;
  for (const Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    if (p.rtt_ms > 0.0)
    {
      rtt = std::max(rtt, p.rtt_ms);
      jitter = std::max(jitter, p.jitter_ms);
    }
    waited = waited || std::min(p.margin_now, p.margin_last) < -RollbackReach();
  }
  if (rtt <= 0.0)
    return;
  const int most = S.max_rollback > 0 ? 8 : 12;
  int want = ChooseDelay(rtt + jitter);
  if (waited)
    want = std::max(want, S.delay + 1);
  want = std::clamp(want, 1, most);
  int next = S.delay;
  if (want > S.delay)
  {
    next = S.delay + 1;
    S.delay_lower = 0;
  }
  else if (want < S.delay && ++S.delay_lower >= 5)
  {
    next = S.delay - 1;
    S.delay_lower = 0;
  }
  else if (want >= S.delay)
  {
    S.delay_lower = 0;
  }
  if (next == S.delay)
    return;
  LogF("netplay: input delay %d -> %d frames (ping %.0f ms, jitter %.0f ms%s)", S.delay, next, rtt, jitter,
       waited ? "; the other console waited for our inputs" : "");
  S.delay = next;
  S.stats.delay = next;
  for (Peer& p : S.peers)
    p.margin_now = p.margin_last = INT32_MAX;  // (what the new delay does, measured again)
}

// (00.92) Every WINDOW seconds of a match, a line in the log: the frames run (the game's speed), the delay, the waits
// for input and for the time sync, the rollbacks; then each other player's: its ping, jitter and pings lost, how far
// ahead its inputs came (under 0: predicted; under -max_rollback: waited for), and what it says - its ping, how often
// it waited for ours, its game's speed, its delay.
void LogWindow()
{
  const Session::Second w = LastSeconds(WINDOW);
  const int seconds = std::min(WINDOW, static_cast<int>(S.seconds.size()));
  if (seconds <= 0)
    return;
  const double speed = w.frames * 100.0 / (std::max(S.fps, 1.0) * seconds);
  LogF("netplay: %d s at frame %d: %d frames (%.0f%% of the game's speed), input delay %d, rollback up to %d; waited "
       "for input in %d passes of %d (%.1f%%; %d inputs came as the pass waited a moment), for the time sync in %d; "
       "%d rollbacks (%d frames again, the deepest %d)",
       seconds, S.frame, w.frames, speed, S.delay, RollbackReach(), w.stalls, w.passes,
       w.passes > 0 ? w.stalls * 100.0 / w.passes : 0.0, w.caught, w.waits, w.rollbacks, w.replayed, w.deepest);
  for (const Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    const int m = p.info.id;
    std::string says = "-";
    if (p.says.known)
    {
      char text[160];
      std::snprintf(text, sizeof(text), "ping %d ms, waited for our inputs %.1f%%, its game at %d%%, its delay %d",
                    p.says.ping_ms, p.says.waits_pm / 10.0, (p.says.speed_pm + 5) / 10, p.says.delay);
      says = text;
    }
    const int margin = w.margin[m] == INT32_MAX ? 0 : w.margin[m];
    const int theirs = std::min(p.margin_now, p.margin_last);
    LogF("netplay:   %s: ping %.0f ms, jitter %.0f ms, %.1f%% of pings lost%s; its inputs here %+d frames at worst, "
         "waited for in %d passes; ours there %+d at worst; it says: %s",
         p.info.nick.c_str(), p.rtt_ms, std::max(p.jitter_ms, 0.0), LostPermille(p) / 10.0,
         p.direct_ok ? "" : " (relay)", margin, w.waited_for[m], theirs == INT32_MAX ? 0 : theirs, says.c_str());
  }
}

// The match's clock: each second ends (its counts kept, the delay looked at), the log's line every WINDOW.
void TickSecond()
{
  const double now = Now();
  if (now - S.second_start < 1.0)
    return;
  S.second_start = now - S.second_start < 2.0 ? S.second_start + 1.0 : now;
  S.seconds.push_back(S.second);
  while (S.seconds.size() > static_cast<size_t>(WINDOW))
    S.seconds.pop_front();
  S.second = Session::Second();
  AdaptDelay();
  for (Peer& p : S.peers)
  {
    p.margin_last = p.margin_now;
    p.margin_now = INT32_MAX;
  }
  if (++S.seconds_logged >= WINDOW)
  {
    S.seconds_logged = 0;
    LogWindow();
  }
}

// The host: once every guest has loaded the state, the match's settings to all and frame 0.
void TryGo()
{
  if (!S.host || S.match != Match::Sync)
    return;
  double rtt = 0.0;
  int guests = 0, fresh = INT32_MAX;
  const double now = Now();
  for (const Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    if (!p.ready || !p.bulk_out.Done() || !p.bulk_queue.empty())
      return;
    // (00.92) Its ping measured once its game runs (a few round trips first, FRESH_WAIT s at most): those of before
    // came back late while a console loaded its game (00.91's log: 732 ms for two consoles 150 ms from the server
    // each, an input delay of 8 frames).
    if (static_cast<int>(p.fresh.size()) < FRESH_PINGS && now - p.ready_at < FRESH_WAIT)
      return;
    rtt = std::max(rtt, p.fresh.empty() ? p.rtt_ms : MedianOf(p.fresh));
    fresh = std::min(fresh, static_cast<int>(p.fresh.size()));
    guests++;
  }
  if (guests == 0)
    return;
  S.delay = ChooseDelay(rtt);
  // The ports: the host's players first, then each guest's, while there are ports.
  int next = 0;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    const bool member = m == S.my_id || S.peers[m].present;
    const int want = m == S.my_id ? std::clamp(S.local_count, 1, MAX_PORTS) : S.peers[m].ready_locals;
    S.first_port[m] = next;
    S.ports[m] = member ? std::clamp(want, 0, MAX_PORTS - next) : 0;
    next += S.ports[m];
  }
  uint8_t payload[64];
  Writer w(payload, sizeof(payload));
  w.U8(static_cast<uint8_t>(S.delay));
  w.U8(static_cast<uint8_t>(S.max_rollback));
  w.U16(static_cast<uint16_t>(S.rules.check_every));
  int count = 0;
  for (int m = 0; m < MAX_MEMBERS; m++)
    count += (m == S.my_id || S.peers[m].present) ? 1 : 0;
  w.U8(static_cast<uint8_t>(count));
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    if (m != S.my_id && !S.peers[m].present)
      continue;
    w.U8(static_cast<uint8_t>(m));
    w.U8(static_cast<uint8_t>(S.first_port[m]));
    w.U8(static_cast<uint8_t>(S.ports[m]));
  }
  SendControlAll(ControlKind::Go, payload, w.Size());
  LogF("netplay: everyone has the state (ping %.0f ms, the median of %d round trips since the games run): go", rtt,
       fresh);
  StartFrames();
}

void OnGo(Reader& r)
{
  const int delay = r.U8();
  const int rollback = r.U8();
  const int check_every = r.U16();
  const int count = r.U8();
  int first[MAX_MEMBERS] = {}, ports[MAX_MEMBERS] = {};
  for (int i = 0; i < count; i++)
  {
    const int m = r.U8();
    const int f = r.U8();
    const int n = r.U8();
    if (m < MAX_MEMBERS)
    {
      first[m] = f;
      ports[m] = n;
    }
  }
  if (!r.Ok() || S.host || S.match != Match::Sync)
    return;
  S.delay = std::clamp(delay, 0, 12);
  S.max_rollback = std::clamp(rollback, 0, 12);
  S.rules.check_every = std::max(1, check_every);
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    S.first_port[m] = first[m];
    S.ports[m] = std::clamp(ports[m], 0, MAX_PORTS);
  }
  StartFrames();
}

// ---- The room ----
// A guest's lobby state to the host: its game and its pings to the relays.
void SendLobbyState()
{
  if (S.host)
    return;
  uint8_t payload[2 + NetProto::MAX_RELAYS * 3];
  Writer w(payload, sizeof(payload));
  w.U8(static_cast<uint8_t>(S.my_game));
  int known = 0;
  for (const RelayServer& r : S.relays)
    known += r.rtt_ms >= 0 ? 1 : 0;
  w.U8(static_cast<uint8_t>(known));
  for (const RelayServer& r : S.relays)
  {
    if (r.rtt_ms < 0)
      continue;
    w.U8(r.info.index);
    w.U16(static_cast<uint16_t>(std::min(r.rtt_ms, 65535)));
  }
  for (Peer& p : S.peers)
  {
    if (p.present && p.info.id == 0)
      SendControl(p, ControlKind::Lobby, payload, w.Size());
  }
  // (00.95) A Game Boy link cable room: our game for Game Boy 2 and our battery save, once accepted (once).
  if (LinkRoom() && S.link_set && !S.link_sent && S.admitted)
  {
    if (Peer* host = FindPeer(0))
    {
      uint8_t link[NetProto::MAX_CONSOLE + NetProto::MAX_TITLE + 32];
      Writer lw(link, sizeof(link));
      lw.Str(S.link_game.console, NetProto::MAX_CONSOLE);
      lw.U32(S.link_game.crc);
      lw.U64(S.link_game.size);
      lw.Str(S.link_title, NetProto::MAX_TITLE);
      SendControl(*host, ControlKind::LinkGame, link, lw.Size());
      SendBulk(*host, BulkKind::LinkSave, S.link_save);
      S.link_sent = true;
      LogF("netplay: our game for Game Boy 2 (%s, %08x) and our battery save (%zu bytes) to the host",
           S.link_game.console.c_str(), S.link_game.crc, S.link_save.size());
    }
  }
}

// ---- Choose Together rooms ----
// A list of games as a block: each console's name, then its games' CRC32 and size.
std::vector<uint8_t> GamesBlock(const std::vector<NetProto::LibraryGame>& games)
{
  std::map<std::string, std::vector<const NetProto::LibraryGame*>> by_console;
  for (const NetProto::LibraryGame& g : games)
    by_console[g.console].push_back(&g);
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(std::min<size_t>(by_console.size(), 255)));
  for (const auto& [console, list] : by_console)
  {
    const size_t n = std::min(console.size(), NetProto::MAX_CONSOLE);
    out.push_back(static_cast<uint8_t>(n));
    PutBytes(out, console.data(), n);
    PutU32(out, static_cast<uint32_t>(list.size()));
    for (const NetProto::LibraryGame* g : list)
    {
      PutU32(out, g->crc);
      PutU32(out, static_cast<uint32_t>(g->size));
      PutU32(out, static_cast<uint32_t>(g->size >> 32));
    }
  }
  return out;
}

bool ReadGamesBlock(const std::vector<uint8_t>& raw, std::vector<NetProto::LibraryGame>* games)
{
  games->clear();
  Reader r(raw.data(), raw.size());
  const int consoles = r.U8();
  for (int c = 0; c < consoles && r.Ok(); c++)
  {
    const std::string console = r.Str(NetProto::MAX_CONSOLE);
    const uint32_t count = r.U32();
    if (!r.Ok() || count > 1000000 || static_cast<size_t>(count) * 12 > r.Left())
      return false;
    for (uint32_t i = 0; i < count; i++)
    {
      NetProto::LibraryGame g;
      g.console = console;
      g.crc = r.U32();
      g.size = r.U64();
      games->push_back(g);
    }
  }
  return r.Ok();
}

// The host: every guest it accepted has sent its games (and there is one).
bool GuestGamesIn()
{
  int guests = 0;
  for (const Peer& p : S.peers)
  {
    if (!p.present || !p.admitted)
      continue;
    if (!p.games_in)
      return false;
    guests++;
  }
  return guests > 0;
}

// The host: the games every console has (the same file), to the guests - once every list is complete, again when one
// changes (00.95: a player more or less too).
void ShareCommon()
{
  if (!S.host || !S.together || !S.my_games_complete || !GuestGamesIn())
    return;
  std::vector<NetProto::LibraryGame> common = S.my_games;
  std::sort(common.begin(), common.end());
  common.erase(std::unique(common.begin(), common.end()), common.end());
  std::string theirs;
  for (const Peer& p : S.peers)
  {
    if (!p.present || !p.admitted)
      continue;
    std::vector<NetProto::LibraryGame> games = p.games;
    std::sort(games.begin(), games.end());
    std::vector<NetProto::LibraryGame> both;
    std::set_intersection(common.begin(), common.end(), games.begin(), games.end(), std::back_inserter(both));
    common.swap(both);
    theirs += ", " + p.info.nick + "'s " + std::to_string(p.games.size());
  }
  S.common = common;
  S.common_ready = true;
  const std::vector<uint8_t> block = GamesBlock(common);
  for (Peer& p : S.peers)
  {
    if (p.present && p.admitted)  // (00.86: not to a player still asking to join)
      SendBulk(p, BulkKind::Common, block);
  }
  LogF("netplay: %zu games every player has (ours %zu%s)", common.size(), S.my_games.size(), theirs.c_str());
}

// (00.95) The host: a Choose Together room's games again without that guest (it left): those of the others, its
// suggestion forgotten - none to show until every list is in.
void ForgetGuestGames(Peer& p)
{
  if (!S.together)
    return;
  p.games.clear();
  p.games_in = false;
  S.common.clear();
  S.common_ready = false;
  if (S.suggested_by == p.info.id)
    S.has_suggestion = false;
}

// A guest: its games to the host, once complete (the room known).
void SendGames()
{
  // (00.86: once the host accepted us.)
  if (S.host || !S.together || S.phase != Phase::Room || !S.my_games_complete || S.games_sent || !S.admitted)
    return;
  Peer* host = FindPeer(0);
  if (!host || S.room_key == 0)
    return;
  SendBulk(*host, BulkKind::Games, GamesBlock(S.my_games));
  S.games_sent = true;
}

void UpdateStatus();

// A Choose Together match over: back in the room (the socket, the room and the other player as they were), the
// match's state forgotten, the room open to pick another game.
void BackToRoom()
{
  LogF("netplay: the match ended at frame %d (%lld rollbacks, %lld resyncs): back to the room", S.frame,
       static_cast<long long>(S.stats.rollbacks), static_cast<long long>(S.stats.resyncs));
  S.phase = Phase::Room;
  S.match = Match::None;
  S.over = false;
  S.to_room = false;
  S.peer_left = false;
  S.error.clear();
  S.start_pending = false;
  S.core = nullptr;
  S.apply = nullptr;
  S.devices_locked = false;
  std::fill(std::begin(S.device_set), std::end(S.device_set), false);
  S.variables.clear();
  S.variables_agreed = false;
  S.frame = 0;
  S.ring.clear();
  S.blocks.clear();
  S.rollback_to = NO_FRAME;
  S.floor = S.check_floor = S.next_check = 0;
  S.my_checks.clear();
  S.last_check = {-1, 0};
  S.epoch = 0;
  S.skip_passes = 0;
  S.stalled_since = -1.0;
  S.replaying = false;
  S.stats = Stats();
  for (InputLog& log : S.inputs)
    log.latest = -1;
  for (Peer& p : S.peers)
  {
    p.ready = false;
    p.acked = -1;
    p.remote_frame = 0;
    p.checks.clear();
    p.resync_asked = -1;
    p.bulk_out = BulkOut();
    p.bulk_queue.clear();
  }
  S.game = S.room_game;
  S.has_suggestion = false;
  ApplyVoice();  // (off between matches)
  if (S.host && s_fd >= 0)
  {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    ServerAuth(w, Type::Lock);
    w.U8(0);
    SendRaw(S.server, buf, w.Size());
  }
  UpdateStatus();
}

// Out of the room: the server told, the socket closed, everything forgotten but the error.
void CloseAll(const std::string& why)
{
  if (!why.empty())
    Fail(why);
  if (S.phase != Phase::Off && S.phase != Phase::Connecting && S.phase != Phase::Browsing && s_fd >= 0)
  {
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    ServerAuth(w, Type::Leave);
    for (int i = 0; i < 2; i++)
      SendRaw(S.server, buf, w.Size());
  }
#ifndef __PS4__
  NetsimFlush(true);  // (the test network's datagrams on their way go now)
#endif
  if (S.voice_running)
    Voice::Stop();
  CloseSocket();
  {
    std::lock_guard lock(s_lookup.mutex);
    s_lookup.generation++;  // (a lookup still running is forgotten)
  }
  ResetSession();
  UsageStats::SetRoom(false);
  std::lock_guard lock(s_status_mutex);
  s_status = Status();
}

void PeerGone(Peer& p, const std::string& why)
{
  if (!p.present)
    return;
  Voice::ForgetPeer(p.info.id);
  // (00.95) The others still in the room, that one left out (the host among them for a guest).
  int others = 0;
  for (const Peer& q : S.peers)
    others += q.present && &q != &p && (S.host || q.admitted) ? 1 : 0;
  if (S.phase == Phase::Starting || S.phase == Phase::Playing)
  {
    // (00.95) A Choose Together match of three or four that a guest left: the others back in the room, without it.
    if (S.together && p.info.id != 0 && others > 0 && (!S.over || S.to_room))
    {
      if (!S.over)
      {
        S.to_room = true;
        S.over = true;
        S.notice = why;
      }
      LogF("netplay: %s: the match ends, back to the room", why.c_str());
      if (S.host)
        ForgetGuestGames(p);
      p = Peer();
      if (S.host)
        ShareCommon();
      return;
    }
    S.peer_left = true;  // (no room to go back to)
    EndMatch(why);
    return;
  }
  if (S.host)
  {
    LogF("netplay: %s (the room stays open)", why.c_str());
    // (00.86) A Choose Together room's lists were that player's: the next one's come with it, once accepted. (00.95:
    // the others' are still there.)
    if (p.admitted)
      ForgetGuestGames(p);
    p = Peer();
    S.link_found = LobbyGame::Looking;  // (00.95: a link cable room's next guest says its game again)
    ShareCommon();
    return;
  }
  if (p.info.id != 0)
  {
    // (00.95) Another guest left (or no longer asks to join): the room goes on with the host.
    LogF("netplay: %s", why.c_str());
    S.admitted_mask = static_cast<uint8_t>(S.admitted_mask & ~(1u << p.info.id));
    p = Peer();
    return;
  }
  CloseAll(why);
}

// The host: a member out of the room - told why (a Quit of `reason`), then the server removes it (it can't join that
// room again; the room's key changes). (00.86: the Quit first - through the server's relay it can't reach a member
// the server removed already; the server's Closed says it too.)
void RemoveMember(Peer& p, uint8_t reason, const std::string& why)
{
  SendQuitNow(p, reason);
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  ServerAuth(w, Type::Kick);
  w.U8(p.info.id);
  SendRaw(S.server, buf, w.Size());
  PeerGone(p, why);
}

void NewOrUpdatePeer(const NetProto::Member& m)
{
  if (m.id >= MAX_MEMBERS || m.id == S.my_id)
    return;
  Peer& p = S.peers[m.id];
  const double now = Now();
  if (!p.present)
  {
    p = Peer();
    p.present = true;
    p.info = m;
    p.first_seen = p.last_heard = p.punch_start = now;
    p.punch_nonce = static_cast<uint32_t>(RandomNumber());
    // (00.86) The host: a guest that came asks to join - accepted or not by the host (Admit, Refuse; expired after
    // AskSeconds). A guest: the host is in already; (00.95) another guest, once the host's Admit lists it.
    p.admitted = !S.host && (m.id == 0 || ((S.admitted_mask >> m.id) & 1) != 0);
    p.asked_at = now;
    if (S.host)
      p.ask_serial = ++S.ask_serial;
    LogF("netplay: %s (%s) %s (%d player%s on that console%s)", m.nick.c_str(), m.region.c_str(),
         S.host                         ? "asks to join the room" :
         p.admitted                     ? "is in the room" :
                                          "asks the host to join the room",
         m.locals, m.locals == 1 ? "" : "s", (m.flags & NetProto::FLAG_HIDDEN) ? ", its address hidden" : "");
    SendLobbyState();
    return;
  }
  if ((m.pub != p.info.pub || m.lan != p.info.lan) && !p.direct_ok)
    p.punch_start = now;  // (new addresses: punched again)
  p.info = m;
}

std::string RefusalText(NetProto::Refusal reason, const std::string& detail)
{
  using NetProto::Refusal;
  switch (reason)
  {
  case Refusal::NoRoom:
    return "There is no room with that code (check it, or ask for a new one)";
  case Refusal::Full:
    return "That room already has its players";
  case Refusal::Version:
    return detail.rfind("protocol", 0) == 0 ? std::string("The online server needs another version of EmuSuNi: "
                                                          "update EmuSuNi") :
                                              "The room's console has EmuSuNi " + detail +
                                                  ": both need the same version (update EmuSuNi)";
  case Refusal::TooMany:
    return "Too many tries: wait a minute and try again";
  case Refusal::ServerFull:
    return "The online server is full: try again later";
  case Refusal::Started:
    return "That room's game has already started";
  case Refusal::Kicked:
    return "The host removed you from that room";
  default:
    return "The online server refused the request";
  }
}

// ---- The relay servers: each one's ping, the best for every console chosen by the host, opened with the ticket ----
void OpenRelay(int index)
{
  if (index <= 0 || index >= static_cast<int>(S.relays.size()))
    return;
  RelayServer& r = S.relays[static_cast<size_t>(index)];
  uint8_t buf[NetProto::RELAY_OPEN_SIZE + 8];
  Writer w(buf, sizeof(buf));
  w.Header(Type::RelayOpen);
  NetProto::WriteTicket(w, S.ticket);
  w.PadTo(NetProto::RELAY_OPEN_SIZE);
  SendRaw(r.address, buf, w.Size());
  r.last_open = Now();
  r.opens++;
}

void ChooseRelay()
{
  if (!S.host || S.relays.empty())
    return;
  // (00.95) Every guest's pings that the host accepted, each one's once known (an accepted guest sends them again).
  std::vector<const Peer*> guests;
  for (const Peer& p : S.peers)
  {
    if (p.present && p.admitted && p.relay_ms_known)
      guests.push_back(&p);
  }
  if (guests.empty())
    return;
  int best = 0;
  double best_score = 1e9;
  for (size_t i = 0; i < S.relays.size(); i++)
  {
    const int mine = S.relays[i].rtt_ms;
    int slowest = mine, sum = mine;
    for (const Peer* g : guests)
    {
      slowest = g->relay_ms[i] < 0 ? -1 : std::max(slowest, g->relay_ms[i]);
      sum += g->relay_ms[i];
      if (slowest < 0)
        break;
    }
    if (mine < 0 || slowest < 0)
      continue;
    // The slowest leg decides; the main server unless another is clearly better (10 ms).
    const double score = slowest + 0.1 * sum - (i == 0 ? 10.0 : 0.0);
    if (score < best_score)
    {
      best_score = score;
      best = static_cast<int>(i);
    }
  }
#ifndef __PS4__
  // (The PC test: a relay of the list forced, whatever the pings.)
  if (const char* forced = std::getenv("EMUSUNI_NETPLAY_RELAY"))
    best = std::clamp(std::atoi(forced), 0, static_cast<int>(S.relays.size()) - 1);
#endif
  if (best == S.relay_choice)
    return;
  S.relay_choice = best;
  const uint8_t payload[1] = {static_cast<uint8_t>(best)};
  SendControlAll(ControlKind::RelayChoice, payload, 1);
  std::string theirs;
  for (const Peer* g : guests)
    theirs += ", " + std::to_string(g->relay_ms[best]) + " from " + g->info.nick;
  LogF("netplay: relay chosen: %s (%s), %d ms from us%s", S.relays[static_cast<size_t>(best)].info.name.c_str(),
       S.relays[static_cast<size_t>(best)].info.region.c_str(), S.relays[static_cast<size_t>(best)].rtt_ms,
       theirs.c_str());
  OpenRelay(best);
}

void PumpRelays(double now)
{
  if (S.phase != Phase::Room && S.phase != Phase::Starting && S.phase != Phase::Playing)
    return;
  if (S.relays.empty())
  {
    if (now - S.last_info >= 1.0 && now - S.step_start < 30.0)
    {
      // The relays' list (the cookie is still good a few minutes after the room was made or joined).
      S.last_info = now;
      uint8_t buf[64];
      Writer w(buf, sizeof(buf));
      w.Header(Type::Info);
      w.Bytes(S.cookie, NetProto::COOKIE_SIZE);
      w.U32(S.nonce);
      SendRaw(S.server, buf, w.Size());
    }
    return;
  }
  bool all_measured = true;
  for (RelayServer& r : S.relays)
  {
    if (r.pings < 3 && now - r.ping_sent >= 0.3)
    {
      r.pings++;
      r.ping_sent = now;
      r.nonce = static_cast<uint32_t>(RandomNumber());
      uint8_t buf[NetProto::RELAY_PING_SIZE + 8];
      Writer w(buf, sizeof(buf));
      w.Header(Type::RelayPing);
      w.U32(r.nonce);
      w.PadTo(NetProto::RELAY_PING_SIZE);
      SendRaw(r.address, buf, w.Size());
    }
    all_measured = all_measured && (r.pings >= 3 && now - r.ping_sent > 0.5);
  }
  if (all_measured && !S.relay_pings_sent)
  {
    S.relay_pings_sent = true;
    std::string pings;
    for (const RelayServer& r : S.relays)
      pings += " " + r.info.name + ":" + (r.rtt_ms < 0 ? std::string("-") : std::to_string(r.rtt_ms) + "ms");
    LogF("netplay: relays' pings:%s", pings.c_str());
    SendLobbyState();
    ChooseRelay();
  }
  // The chosen relay opened (again a few times if it doesn't answer; kept open while the room lasts).
  const int c = S.relay_choice;
  if (c > 0 && c < static_cast<int>(S.relays.size()))
  {
    RelayServer& r = S.relays[static_cast<size_t>(c)];
    if ((!r.opened && r.opens < 5 && now - r.last_open >= 1.0) || (r.opened && now - r.last_open >= RELAY_REOPEN))
      OpenRelay(c);
  }
}

// ---- Packets from the servers ----
void OnPeerPacket(const uint8_t* data, size_t size, const Address& from, bool via_relay, int relay_from);
void SendRequest();

void OnMembers(Reader& r)
{
  const std::string code = NetProto::ReadCode(r);
  r.U32();  // (its count of changes)
  const uint64_t key = r.U64();
  const NetProto::GameInfo game = NetProto::ReadGame(r);
  r.Str(NetProto::MAX_APP_VERSION);
  const int max_members = r.U8();
  const uint8_t room_flags = r.U8();
  const uint8_t you = r.U8();
  const int count = r.U8();
  std::vector<NetProto::Member> members;
  for (int i = 0; i < count; i++)
  {
    NetProto::Member m;
    m.id = r.U8();
    m.nick = r.Str(NetProto::MAX_NICK);
    m.pub = NetProto::ReadAddress(r);
    m.lan = NetProto::ReadAddress(r);
    m.locals = r.U8();
    m.flags = r.U8();
    m.region = r.Str(NetProto::MAX_REGION);
    members.push_back(m);
  }
  if (!r.Ok() || S.phase == Phase::Connecting || S.phase == Phase::Browsing || code != S.code || you != S.my_id)
    return;
  const double now = Now();
  S.last_server_heard = now;
  if (S.keepalive_sent > 0.0)
  {
    S.server_ms = static_cast<int>((now - S.keepalive_sent) * 1000.0);
    S.keepalive_sent = 0.0;
  }
  S.room_key = key;
  S.room_game = game;
  S.max_members = std::clamp(max_members, 2, MAX_MEMBERS);
  S.together = game.console == NetProto::TOGETHER;
  if (!S.together || S.phase == Phase::Room)
    S.game = game;  // (a Choose Together match's own game - the host's pick - stays while it lasts)
  S.is_public = (room_flags & NetProto::FLAG_PUBLIC) != 0;
  if (!S.voice_default_set)
  {
    // The voice chat starts on in a private room (friends), off in a public one (each player turns theirs on).
    S.voice_default_set = true;
    S.voice_enabled = S.config.voice && !S.is_public;
  }
  bool listed[MAX_MEMBERS] = {};
  for (const NetProto::Member& m : members)
  {
    if (m.id >= MAX_MEMBERS)
      continue;
    listed[m.id] = true;
    if (m.id == S.my_id)
      S.my_public = m.pub;
    else
      NewOrUpdatePeer(m);
  }
  for (Peer& p : S.peers)
  {
    if (!p.present || listed[p.info.id])
      continue;
    // (In a match the server's list doesn't end it: the direct path may still work - the peer's own timeout does.)
    if (S.phase == Phase::Room)
      PeerGone(p, PeerName(p) + (p.info.id == 0 ? " closed the room" :
                                 p.admitted     ? " left the room" :
                                                  " no longer asks to join"));  // (00.86)
  }
}

// A list of rooms is being asked for: the Online tab's (Browsing), or (00.86) the host's room's while it stays open.
bool Listing()
{
  return S.phase == Phase::Browsing || (S.host && S.phase == Phase::Room && S.room_list);
}

// The list asked for came whole (every page of it): shown, replacing the one before at once; logged when its count
// changes (the Online tab asks again every few seconds).
void RoomsDone(int total)
{
  S.rooms = std::move(S.rooms_in);
  S.rooms_in.clear();
  S.rooms_total = std::max(total, static_cast<int>(S.rooms.size()));
  S.rooms_done = true;
  if (S.rooms.size() == S.rooms_logged)
    return;
  S.rooms_logged = S.rooms.size();
  LogF("netplay: the %s list: %zu rooms (%d on the server)%s", S.list_old ? "public" : "rooms'", S.rooms.size(),
       S.rooms_total, S.browse_console.empty() ? "" : (" (" + S.browse_console + ")").c_str());
}

void OnRooms(Reader& r)
{
  const uint32_t nonce = r.U32();
  const int page = r.U8();
  const int pages = r.U8();
  r.U16();
  const int count = r.U8();
  std::vector<RoomView> rooms;
  for (int i = 0; i < count; i++)
  {
    RoomView v;
    v.room.code = NetProto::ReadCode(r);
    v.room.game = NetProto::ReadGame(r);
    v.room.host = r.Str(NetProto::MAX_NICK);
    v.room.region = r.Str(NetProto::MAX_REGION);
    v.room.players = r.U8();
    v.room.max_players = r.U8();
    v.room.host_ms = r.U16();
    v.ping_ms = v.room.host_ms > 0 ? v.room.host_ms + S.server_ms : -1;
    rooms.push_back(v);
  }
  if (!r.Ok() || !Listing() || nonce != S.nonce || page != S.next_page)
    return;
  if (page == 0)
    S.rooms_in.clear();
  S.rooms_in.insert(S.rooms_in.end(), rooms.begin(), rooms.end());
  S.pages = pages;
  S.next_page = page + 1;
  S.tries = 0;
  if (S.next_page >= pages || S.next_page >= BROWSE_PAGES)
    RoomsDone(static_cast<int>(S.rooms_in.size()));
  else
    SendRequest();
}

// (00.85) A page of every room (ListAll's answer): the rooms from `first` on, each once (a room made or closed between
// two pages moves the others by one: one seen twice is left out), then the next page until every room came or
// BROWSE_PAGES did.
void OnAllRooms(Reader& r)
{
  const uint32_t nonce = r.U32();
  const int first = r.U16();
  const int total = r.U16();
  const int count = r.U8();
  std::vector<NetProto::RoomEntry> rooms;
  for (int i = 0; i < count && r.Ok(); i++)
    rooms.push_back(NetProto::ReadListedRoom(r));
  if (!r.Ok() || !Listing() || S.list_old || nonce != S.nonce || first != S.list_first)
    return;
  if (!S.list_all_ok)
    LogLine("netplay: the server lists every room (public and private)");
  S.list_all_ok = true;
  if (first == 0)
  {
    S.rooms_in.clear();
    if (S.tries == 1)
      S.server_ms = static_cast<int>((Now() - S.last_request) * 1000.0);  // (the ping estimates: today's round trip)
  }
  for (NetProto::RoomEntry& room : rooms)
  {
    const bool seen = std::any_of(S.rooms_in.begin(), S.rooms_in.end(),
                                  [&room](const RoomView& o) { return o.room.id == room.id; });
    if (seen)
      continue;
    RoomView v;
    v.ping_ms = room.host_ms > 0 ? room.host_ms + S.server_ms : -1;
    v.room = std::move(room);
    S.rooms_in.push_back(std::move(v));
  }
  S.list_first = first + count;
  S.next_page++;
  S.tries = 0;
  if (count == 0 || S.list_first >= total || S.next_page >= BROWSE_PAGES)
    RoomsDone(total);
  else
    SendRequest();
}

void OnServerPacket(int index, Type type, Reader& r)
{
  const double now = Now();
  switch (type)
  {
  case Type::Cookie:
  {
    const uint32_t nonce = r.U32();
    const uint8_t* cookie = r.Bytes(NetProto::COOKIE_SIZE);
    const Address seen = NetProto::ReadAddress(r);
    if (!r.Ok() || index != 0 || S.step != Step::Hello || nonce != S.hello_nonce || !cookie)
      return;
    std::memcpy(S.cookie, cookie, NetProto::COOKIE_SIZE);
    S.cookie_at = now;
    S.my_public = seen;
    S.server_ms = static_cast<int>((now - S.hello_sent) * 1000.0);
    S.step = Step::Request;
    S.tries = 0;
    S.last_request = 0.0;
    // (00.86: in the host's room, a new cookie for its list of rooms - said once already.)
    if (S.phase == Phase::Connecting || S.phase == Phase::Browsing)
      LogF("netplay: the server answered (%d ms); we are %s to it", S.server_ms, NetProto::AddressText(seen).c_str());
    SendRequest();
    return;
  }
  case Type::ServerInfo:
  {
    const uint32_t nonce = r.U32();
    const int count = r.U8();
    std::vector<RelayServer> relays;
    for (int i = 0; i < count && i < NetProto::MAX_RELAYS; i++)
    {
      RelayServer s;
      s.info.index = r.U8();
      s.info.name = r.Str(NetProto::MAX_RELAY_NAME);
      s.info.region = r.Str(NetProto::MAX_REGION);
      s.info.address = NetProto::ReadAddress(r);
      s.address = i == 0 ? S.server : s.info.address;
      if (s.info.index == i && (i == 0 || s.address.Valid()))
        relays.push_back(s);
    }
    if (!r.Ok() || index != 0 || nonce != S.nonce || !S.relays.empty() || relays.empty())
      return;
    S.relays = relays;
    LogF("netplay: %zu relay server%s", relays.size(), relays.size() == 1 ? "" : "s");
    return;
  }
  case Type::Created:
  {
    const uint32_t nonce = r.U32();
    const std::string code = NetProto::ReadCode(r);
    const uint8_t id = r.U8();
    const uint64_t token = r.U64();
    const Address pub = NetProto::ReadAddress(r);
    const NetProto::Ticket ticket = NetProto::ReadTicket(r);
    if (!r.Ok() || index != 0 || !S.host || S.phase != Phase::Connecting || nonce != S.nonce)
      return;
    S.code = code;
    S.my_id = id;
    S.token = token;
    S.my_public = pub;
    S.ticket = ticket;
    S.phase = Phase::Room;
    S.step_start = S.last_server_heard = S.last_keepalive = now;
    LogF("netplay: %s room made for %s (%s); we are at %s, %s on the LAN", S.is_public ? "public" : "private",
         S.game.title.c_str(), S.game.console.c_str(), NetProto::AddressText(pub).c_str(),
         NetProto::AddressText(S.my_lan).c_str());
    return;
  }
  case Type::Joined:
  {
    const uint32_t nonce = r.U32();
    const uint8_t id = r.U8();
    const uint64_t token = r.U64();
    const Address pub = NetProto::ReadAddress(r);
    const NetProto::Ticket ticket = NetProto::ReadTicket(r);
    if (!r.Ok() || index != 0 || S.host || S.phase != Phase::Connecting || nonce != S.nonce)
      return;
    S.my_id = id;
    S.token = token;
    S.my_public = pub;
    S.ticket = ticket;
    S.phase = Phase::Room;
    S.step_start = S.last_server_heard = S.last_keepalive = now;
    S.waiting_since = now;  // (00.86: until the host accepts us)
    LogF("netplay: joined the room as member %d; we are at %s, %s on the LAN; waiting for the host to accept us", id,
         NetProto::AddressText(pub).c_str(), NetProto::AddressText(S.my_lan).c_str());
    return;
  }
  case Type::Refused:
  {
    const uint32_t nonce = r.U32();
    const auto reason = static_cast<NetProto::Refusal>(r.U8());
    const std::string detail = r.Str(64);
    // (00.86) The host's room's list of rooms: its cookie got old, a new one; the room stays whatever else is refused.
    const bool room_list = S.host && S.phase == Phase::Room && S.room_list;
    if (!r.Ok() || index != 0 || (S.phase != Phase::Connecting && S.phase != Phase::Browsing && !room_list) ||
        nonce != S.nonce)
      return;
    if (reason == NetProto::Refusal::BadCookie)
    {
      S.step = Step::Hello;  // (the cookie got old: a new one)
      S.tries = 0;
      S.hello_sent = 0.0;
      return;
    }
    if (room_list)
      return;
    CloseAll(RefusalText(reason, detail));
    return;
  }
  case Type::Members:
    if (index == 0)
      OnMembers(r);
    return;
  case Type::Rooms:
    if (index == 0)
      OnRooms(r);
    return;
  case Type::AllRooms:
    if (index == 0)
      OnAllRooms(r);
    return;
  case Type::Closed:
  {
    const std::string code = NetProto::ReadCode(r);
    const auto reason = static_cast<NetProto::CloseReason>(r.U8());
    if (!r.Ok() || index != 0 || code != S.code || S.phase == Phase::Connecting || S.phase == Phase::Browsing)
      return;
    // (00.86: removed before the host accepted us - its Quit may come after this, or not at all - is its "no".)
    const bool refused = reason == NetProto::CloseReason::Kicked && !S.host && !S.admitted;
    const std::string why = reason == NetProto::CloseReason::HostLeft ? "The host closed the room" :
                            refused                                   ? "The host did not accept you" :
                            reason == NetProto::CloseReason::Kicked   ? "The host removed you from the room" :
                            reason == NetProto::CloseReason::Removed  ? "The online server lost this console" :
                                                                        "The room closed";
    if ((S.phase == Phase::Starting || S.phase == Phase::Playing) && reason != NetProto::CloseReason::Kicked)
    {
      // A match whose consoles reach each other directly goes on without the server.
      bool all_direct = true;
      for (const Peer& p : S.peers)
        all_direct = all_direct && (!p.present || p.direct_ok);
      LogF("netplay: the server: %s%s", why.c_str(), all_direct ? " (the match goes on: direct path)" : "");
      if (!all_direct)
        EndMatch(why);
      return;
    }
    if (S.phase == Phase::Starting || S.phase == Phase::Playing)
      EndMatch(why);
    else
      CloseAll(why);
    return;
  }
  case Type::Relayed:
  {
    const int from = r.U8();
    if (!r.Ok() || r.Left() == 0)
      return;
    const size_t n = r.Left();
    if (Peer* p = FindPeer(from))
      p->last_relayed = now;
    OnPeerPacket(r.Here(), n, Address(), true, from);
    return;
  }
  case Type::RelayPong:
  {
    const uint32_t nonce = r.U32();
    if (!r.Ok() || index < 0 || index >= static_cast<int>(S.relays.size()))
      return;
    RelayServer& relay = S.relays[static_cast<size_t>(index)];
    if (nonce != relay.nonce)
      return;
    const int ms = static_cast<int>((now - relay.ping_sent) * 1000.0);
    relay.rtt_ms = relay.rtt_ms < 0 ? ms : std::min(relay.rtt_ms, ms);
    return;
  }
  case Type::RelayOpened:
  {
    const uint64_t room = r.U64();
    const uint8_t member = r.U8();
    const uint64_t session = r.U64();
    if (!r.Ok() || index <= 0 || index >= static_cast<int>(S.relays.size()) || room != S.ticket.room ||
        member != S.my_id)
      return;
    RelayServer& relay = S.relays[static_cast<size_t>(index)];
    const bool was = relay.opened;
    relay.opened = true;
    relay.session = session;
    if (!was && index == S.relay_choice)
    {
      LogF("netplay: opened on the relay %s", relay.info.name.c_str());
      const uint8_t payload[1] = {static_cast<uint8_t>(index)};
      SendControlAll(ControlKind::RelayReady, payload, 1);
    }
    return;
  }
  default:
    return;
  }
}

// ---- Packets from the other consoles ----
void OnControl(Peer& p, Reader& r)
{
  const uint16_t seq = r.U16();
  const auto kind = static_cast<ControlKind>(r.U8());
  if (!r.Ok())
    return;
  {
    uint8_t buf[32];
    Writer w(buf, sizeof(buf));
    PeerHeader(w, Type::ControlAck);
    w.U16(seq);
    SendPeer(p, buf, w.Size());
  }
  if (std::find(p.ctl_seen.begin(), p.ctl_seen.end(), seq) != p.ctl_seen.end())
    return;
  p.ctl_seen.push_back(seq);
  while (p.ctl_seen.size() > 128)
    p.ctl_seen.pop_front();
  switch (kind)
  {
  case ControlKind::Lobby:
  {
    const auto game = static_cast<LobbyGame>(r.U8());
    const int count = r.U8();
    int ms[NetProto::MAX_RELAYS];
    std::fill(std::begin(ms), std::end(ms), -1);
    for (int i = 0; i < count; i++)
    {
      const int index = r.U8();
      const int value = r.U16();
      if (index < NetProto::MAX_RELAYS)
        ms[index] = value;
    }
    if (!r.Ok() || !S.host)
      return;
    if (p.game != game)
      LogF("netplay: %s: %s", p.info.nick.c_str(),
           game == LobbyGame::Same      ? "has the game" :
           game == LobbyGame::Different ? "plays with another file of the game" :
           game == LobbyGame::Missing   ? "hasn't got the game" :
           game == LobbyGame::Asking    ? "has another file of the game, choosing" :
                                          "is looking for the game");
    p.game = game;
    if (count > 0)
    {
      std::copy(std::begin(ms), std::end(ms), std::begin(p.relay_ms));
      p.relay_ms_known = true;
      if (S.relay_pings_sent)
        ChooseRelay();
    }
    return;
  }
  case ControlKind::RelayChoice:
  {
    const int index = r.U8();
    if (!r.Ok() || S.host || index >= static_cast<int>(S.relays.size()))
      return;
    S.relay_choice = index;
    LogF("netplay: the host chose the relay %s", S.relays[static_cast<size_t>(index)].info.name.c_str());
    if (index > 0)
    {
      S.relays[static_cast<size_t>(index)].opens = 0;
      OpenRelay(index);
    }
    return;
  }
  case ControlKind::RelayReady:
  {
    const int index = r.U8();
    if (r.Ok())
      p.relay_ready = index;
    return;
  }
  case ControlKind::StartGame:
    if (!S.host && S.phase == Phase::Room)
    {
      // (A Choose Together room: the game the host picked comes with it.)
      if (r.Left() > 0)
      {
        const NetProto::GameInfo game = NetProto::ReadGame(r);
        if (!r.Ok())
          return;
        S.game = game;
      }
      S.phase = Phase::Starting;
      S.start_pending = true;
      S.notice.clear();
      S.admitted = true;  // (00.86: the host starts only with players it accepted - its Admit may still be on its way)
      for (Peer& q : S.peers)
        q.admitted = q.admitted || q.present;  // (00.95: the others too)
      LogF("netplay: %s started the game (%s)", p.info.nick.c_str(), S.game.title.c_str());
    }
    return;
  case ControlKind::Admit:
  {
    // (00.86) The host accepted us: the room as before - the games' list (Choose Together), the host's Start. (00.95)
    // Its byte: every member it accepted (a bit each) - sent to all of them whenever it accepts one.
    const uint8_t mask =
        static_cast<uint8_t>((r.Left() > 0 ? r.U8() : static_cast<uint8_t>(1u << S.my_id)) | 1u);
    if (S.host || p.info.id != 0 || S.phase != Phase::Room)
      return;
    S.admitted_mask = mask;
    for (Peer& q : S.peers)
    {
      if (!q.present || q.info.id == 0)
        continue;
      const bool in = ((mask >> q.info.id) & 1) != 0;
      if (in && !q.admitted && S.admitted)
        LogF("netplay: %s is in the room (the host accepted it)", q.info.nick.c_str());
      q.admitted = in;
    }
    if (!S.admitted && ((mask >> S.my_id) & 1) != 0)
    {
      S.admitted = true;
      LogF("netplay: %s accepted us in the room (after %.0f s)", p.info.nick.c_str(), Now() - S.waiting_since);
      SendLobbyState();
    }
    else if (S.admitted && S.together)
    {
      // (00.95) A player more: the games every player has come again from the host, with that one's.
      S.common.clear();
      S.common_ready = false;
    }
    return;
  }
  case ControlKind::LinkGame:
  {
    // (00.95) A Game Boy link cable room. The host: a guest's game for Game Boy 2. A guest: whether the host has it.
    if (S.host)
    {
      NetProto::LibraryGame g;
      g.console = r.Str(NetProto::MAX_CONSOLE);
      g.crc = r.U32();
      g.size = r.U64();
      const std::string title = r.Str(NetProto::MAX_TITLE);
      if (!r.Ok() || !LinkRoom() || !p.admitted || (g.console != "gb" && g.console != "gbc"))
        return;
      p.link_game = g;
      p.link_title = title;
      p.link_game_in = true;
      S.link_found = LobbyGame::Looking;  // (the menu looks for it: SetLinkGameFound)
      LogF("netplay: %s's game for Game Boy 2: %s (%s, %08x, %llu bytes)", p.info.nick.c_str(), title.c_str(),
           g.console.c_str(), g.crc, static_cast<unsigned long long>(g.size));
    }
    else if (p.info.id == 0)
    {
      const auto found = static_cast<LobbyGame>(r.U8());
      if (!r.Ok() || !LinkRoom())
        return;
      S.link_found = found;
      LogF("netplay: the host %s our game for Game Boy 2", found == LobbyGame::Same ? "has" : "hasn't got");
    }
    return;
  }
  case ControlKind::Suggest:
  {
    NetProto::LibraryGame g;
    g.console = r.Str(NetProto::MAX_CONSOLE);
    g.crc = r.U32();
    g.size = r.U64();
    if (r.Ok() && S.host && S.together && p.admitted)
    {
      S.suggested = g;
      S.has_suggestion = true;
      S.suggested_by = p.info.id;
      LogF("netplay: %s suggests a game (%s, %08x)", p.info.nick.c_str(), g.console.c_str(), g.crc);
    }
    return;
  }
  case ControlKind::EndMatch:
    if (S.together && (S.phase == Phase::Starting || S.phase == Phase::Playing))
    {
      // Back to the room for both (no error: the menu shows the room's games again).
      S.to_room = true;
      S.over = true;
      S.notice = PeerName(p) + " ended the match";
      LogF("netplay: %s ended the match: back to the room", p.info.nick.c_str());
    }
    return;
  case ControlKind::Ready:
  {
    const int locals = r.U8();
    if (r.Ok() && S.host)
    {
      if (!p.ready)
      {
        // (00.92: the round trips from now on choose the delay - before, its game was loading.)
        p.ready_at = Now();
        p.fresh.clear();
      }
      p.ready = true;
      p.ready_locals = std::clamp(locals, 1, MAX_PORTS);
      LogF("netplay: %s has loaded the state (%d player%s)", p.info.nick.c_str(), p.ready_locals,
           p.ready_locals == 1 ? "" : "s");
    }
    return;
  }
  case ControlKind::Go:
    OnGo(r);
    return;
  case ControlKind::ResyncRequest:
  {
    const int32_t frame = static_cast<int32_t>(r.U32());
    const uint32_t epoch = r.U32();
    if (r.Ok() && S.host && epoch == S.epoch)
    {
      LogF("netplay: %s says frame %d differs: resync", p.info.nick.c_str(), frame);
      S.stats.mismatches++;
      StartResync();
    }
    return;
  }
  case ControlKind::Quit:
  {
    const int reason = r.Ok() ? r.U8() : NetProto::QUIT_LEFT;
    const bool from_host = p.info.id == 0 && !S.host;
    // (00.86) The host's "no" to our request to join, or no answer in time.
    PeerGone(p, from_host && reason == NetProto::QUIT_REMOVED   ? std::string("The host removed you from the room") :
                from_host && reason == NetProto::QUIT_REFUSED   ? std::string("The host did not accept you") :
                from_host && reason == NetProto::QUIT_NO_ANSWER ? std::string("The host didn't answer: try again later") :
                p.info.id == 0 && S.phase == Phase::Room        ? PeerName(p) + " closed the room" :
                S.phase == Phase::Room                          ? PeerName(p) + " left the room" :  // (00.95)
                                                                  PeerName(p) + " left the game");
    return;
  }
  default:
    return;
  }
}

void OnBulk(Peer& p, Reader& r)
{
  const uint16_t id = r.U16();
  const auto kind = static_cast<BulkKind>(r.U8());
  const uint32_t total = r.U32();
  const int index = r.U16();
  const int chunks = r.U16();
  if (!r.Ok() || chunks == 0 || total > static_cast<uint32_t>(chunks) * NetProto::BULK_CHUNK)
    return;
  BulkIn& b = p.bulk_in;
  if (!b.active && b.done_chunks >= 0 && id == b.done_id)
  {
    b.ack_due = true;  // (a chunk of the one finished: acknowledged again, its sender missed that)
    return;
  }
  if (!b.active || id != b.id)
  {
    if (b.active && static_cast<int16_t>(id - b.id) < 0)
      return;
    if (!b.active && b.done_chunks >= 0 && static_cast<int16_t>(id - b.done_id) <= 0)
      return;
    b.active = true;
    b.id = id;
    b.kind = kind;
    b.total = total;
    b.chunks = chunks;
    b.data.assign(total, 0);
    b.got.assign(static_cast<size_t>(chunks), 0);
    b.got_count = 0;
    b.base = 0;
    if (kind == BulkKind::Resync && !S.host && S.match == Match::Running)
    {
      S.match = Match::Resync;  // (no frame runs until it is loaded)
      S.resync_started = Now();
      LogF("netplay: resync: the host's state is coming (%u bytes compressed)", total);
    }
  }
  if (index >= b.chunks || total != b.total)
    return;
  const size_t offset = static_cast<size_t>(index) * NetProto::BULK_CHUNK;
  const size_t n = std::min(NetProto::BULK_CHUNK, static_cast<size_t>(total) - offset);
  const uint8_t* data = r.Bytes(n);
  if (!data)
    return;
  if (!b.got[static_cast<size_t>(index)])
  {
    std::memcpy(b.data.data() + offset, data, n);
    b.got[static_cast<size_t>(index)] = 1;
    b.got_count++;
    while (b.base < b.chunks && b.got[static_cast<size_t>(b.base)])
      b.base++;
  }
  b.ack_due = true;
  if (b.got_count == b.chunks)
  {
    std::vector<uint8_t> raw;
    const bool ok = Decompress(b.data, &raw);
    b.active = false;
    b.done_id = b.id;
    b.done_chunks = b.chunks;
    SendBulkAck(p);
    std::vector<uint8_t>().swap(b.data);
    if (!ok)
    {
      LogLine("netplay: a block came damaged: dropped");
      return;
    }
    // (A Choose Together room's lists: taken at once - the guest's games by the host, the games both have by the
    // guest.)
    if (b.kind == BulkKind::Games && S.host)
    {
      if (!p.admitted)
        return;  // (00.86: a guest sends them once accepted - not before)
      p.games_in = ReadGamesBlock(raw, &p.games);
      LogF("netplay: %s's games: %zu", p.info.nick.c_str(), p.games.size());
      ShareCommon();
      return;
    }
    // (00.95) A Game Boy link cable room's guest's battery save (as it packed it: GbLink::UnpackSave reads it).
    if (b.kind == BulkKind::LinkSave && S.host)
    {
      if (!p.admitted || !LinkRoom())
        return;
      p.link_save = std::move(raw);
      p.link_save_in = true;
      LogF("netplay: %s's battery save for Game Boy 2: %zu bytes", p.info.nick.c_str(), p.link_save.size());
      return;
    }
    if (b.kind == BulkKind::Common && !S.host)
    {
      if (p.info.id != 0)
        return;
      S.common_ready = ReadGamesBlock(raw, &S.common);
      LogF("netplay: %zu games every player has", S.common.size());
      return;
    }
    S.blocks.push_back({b.kind, std::move(raw)});
  }
}

void OnBulkAck(Peer& p, Reader& r)
{
  const uint16_t id = r.U16();
  const int base = r.U16();
  const uint8_t* bits = r.Bytes(NetProto::BULK_WINDOW / 8);
  BulkOut& b = p.bulk_out;
  if (!bits || !b.active || id != b.id)
    return;
  auto ack = [&b](int i) {
    if (i >= 0 && i < b.chunks && !b.acked[static_cast<size_t>(i)])
    {
      b.acked[static_cast<size_t>(i)] = 1;
      b.acked_count++;
    }
  };
  for (int i = 0; i < std::min(base, b.chunks); i++)
    ack(i);
  for (int i = 0; i < NetProto::BULK_WINDOW; i++)
  {
    if (bits[i / 8] & (1 << (i % 8)))
      ack(base + i);
  }
  while (b.base < b.chunks && b.acked[static_cast<size_t>(b.base)])
    b.base++;
}

void OnPeerPacket(const uint8_t* data, size_t size, const Address& from, bool via_relay, int relay_from)
{
  Reader r(data, size);
  Type type;
  if (!NetProto::ReadHeader(r, &type))
    return;
  const uint64_t key = r.U64();
  const int id = r.U8();
  if (!r.Ok() || S.room_key == 0 || key != S.room_key || (via_relay && id != relay_from))
    return;
  Peer* found = FindPeer(id);
  if (!found)
    return;
  Peer& p = *found;
  const double now = Now();
  p.last_heard = now;
  if (!via_relay && p.direct_ok && from == p.direct)
    p.last_direct = now;
  switch (type)
  {
  case Type::Punch:
  {
    const uint32_t nonce = r.U32();
    if (via_relay || !r.Ok() || S.hidden)
      return;  // (hiding our address: never answered, so it never learns it)
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    PeerHeader(w, Type::PunchAck);
    w.U32(nonce);
    SendRaw(from, buf, w.Size());
    // (Our own punch to where theirs came from: the way back through our router.)
    if (!p.direct_ok)
    {
      Writer punch(buf, sizeof(buf));
      PeerHeader(punch, Type::Punch);
      punch.U32(p.punch_nonce);
      NetProto::WriteAddress(punch, from);
      SendRaw(from, buf, punch.Size());
    }
    return;
  }
  case Type::PunchAck:
  {
    const uint32_t nonce = r.U32();
    if (via_relay || !r.Ok() || nonce != p.punch_nonce || S.hidden)
      return;
    if (!p.direct_ok)
    {
      p.direct_ok = true;
      p.direct = from;
      LogF("netplay: direct path to %s (%s), after %.1f s", p.info.nick.c_str(),
           from == p.info.lan && p.info.lan != p.info.pub ? "on the same network" : "through the routers",
           now - p.punch_start);
    }
    p.last_direct = now;
    return;
  }
  case Type::Ping:
  {
    const uint32_t t = r.U32();
    const uint8_t flags = r.U8();
    if (!r.Ok())
      return;
    p.voice_on = (flags & 1) != 0;
    // (00.92) What it says of the match (see SendPing).
    const int ping = r.U16(), jitter = r.U16(), lost = r.U16(), waits = r.U16(), speed = r.U16();
    const int delay = r.U8();
    if (r.Ok() && (flags & 2))
    {
      p.says.known = true;
      p.says.ping_ms = ping == 0xFFFF ? -1 : ping;
      p.says.jitter_ms = jitter == 0xFFFF ? -1 : jitter;
      p.says.lost_pm = std::min(lost, 1000);
      p.says.waits_pm = std::min(waits, 1000);
      p.says.speed_pm = std::min(speed, 2000);
      p.says.delay = delay;
    }
    // (00.92: the pong says when this console was busy as the ping came - its round trip isn't the network's.)
    uint8_t buf[64];
    Writer w(buf, sizeof(buf));
    PeerHeader(w, Type::Pong);
    w.U32(t);
    w.U8(S.pump_gap > BUSY_GAP ? 1 : 0);
    SendPeer(p, buf, w.Size());
    return;
  }
  case Type::Pong:
  {
    const uint32_t t = r.U32();
    const double sample = static_cast<double>(NowMs() - t);
    if (!r.Ok() || sample < 0.0 || sample > 10000.0)
      return;
    const uint8_t flags = r.U8();
    const bool busy = (r.Ok() && (flags & 1)) || S.pump_gap > BUSY_GAP;
    for (auto it = p.pings_out.rbegin(); it != p.pings_out.rend(); ++it)
    {
      if (it->first == t)
      {
        it->second = true;
        break;
      }
    }
    if (busy)
      return;
    AddRoundTrip(p, sample, t);
    return;
  }
  case Type::Control:
    OnControl(p, r);
    return;
  case Type::ControlAck:
  {
    const uint16_t seq = r.U16();
    if (!r.Ok())
      return;
    for (auto it = p.ctl_out.begin(); it != p.ctl_out.end(); ++it)
    {
      if (it->seq == seq)
      {
        p.ctl_out.erase(it);
        break;
      }
    }
    return;
  }
  case Type::Bulk:
    OnBulk(p, r);
    return;
  case Type::BulkAck:
    OnBulkAck(p, r);
    return;
  case Type::Input:
    OnInput(p, r);
    return;
  case Type::Voice:
  {
    const uint16_t seq = r.U16();
    const size_t n = r.Left();
    if (r.Ok() && n > 0 && S.voice_running && !p.muted)
      Voice::GivePacket(p.info.id, seq, r.Here(), n);
    return;
  }
  default:
    return;
  }
}

void Receive()
{
  uint8_t buf[2048];
  for (int i = 0; i < 2048 && s_fd >= 0; i++)
  {
    pollfd pfd;
    pfd.fd = s_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
      break;
    sockaddr_in from;
    std::memset(&from, 0, sizeof(from));
    socklen_t len = sizeof(from);
    const ssize_t n = recvfrom(s_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0)
      break;
    if (n < static_cast<ssize_t>(NetProto::HEADER) || static_cast<size_t>(n) > NetProto::MAX_PACKET)
      continue;
    S.stats.packets_in++;
    const Address a = FromSockaddr(from);
    const int server = ServerIndex(a);
    if (server >= 0)
    {
      Reader r(buf, static_cast<size_t>(n));
      Type type;
      uint8_t version = 0;
      if (NetProto::ReadHeader(r, &type))
        OnServerPacket(server, type, r);
      else if (server == 0 && NetProto::IsOurs(buf, static_cast<size_t>(n), &version) &&
               buf[3] == static_cast<uint8_t>(Type::Refused) &&
               (S.phase == Phase::Connecting || S.phase == Phase::Browsing))
        CloseAll("The online server runs another version: update EmuSuNi");
    }
    else
    {
      OnPeerPacket(buf, static_cast<size_t>(n), a, false, -1);
    }
    if (S.phase == Phase::Off)
      return;
  }
}

// The request the connecting is for (Create, Join or List), with the cookie.
void SendRequest()
{
  S.last_request = Now();
  S.tries++;
  uint8_t buf[NetProto::MAX_PACKET];
  Writer w(buf, sizeof(buf));
  const uint8_t flags = static_cast<uint8_t>((S.is_public ? NetProto::FLAG_PUBLIC : 0) |
                                             (S.hidden ? NetProto::FLAG_HIDDEN : 0));
  // (Hiding the address: no LAN address for the others either.)
  const Address lan = S.hidden ? Address() : S.my_lan;
  // (00.86: once in a room - the host's, open while the Online tab lists the rooms - the request is the list.)
  switch (S.phase == Phase::Connecting || S.phase == Phase::Browsing ? S.want : Want::List)
  {
  case Want::Create:
    w.Header(Type::Create);
    w.Bytes(S.cookie, NetProto::COOKIE_SIZE);
    w.U32(S.nonce);
    w.Str(App::VERSION, NetProto::MAX_APP_VERSION);
    NetProto::WriteGame(w, S.game);
    w.Str(S.nick, NetProto::MAX_NICK);
    NetProto::WriteAddress(w, lan);
    w.U8(static_cast<uint8_t>(S.lobby_locals));
    w.U8(static_cast<uint8_t>(S.max_members));  // (00.95: 2 to 4 consoles, the host's choice)
    w.U8(flags);
    w.Str(S.region, NetProto::MAX_REGION);
    break;
  case Want::Join:
    w.Header(Type::Join);
    w.Bytes(S.cookie, NetProto::COOKIE_SIZE);
    w.U32(S.nonce);
    NetProto::WriteCode(w, S.code);
    w.Str(App::VERSION, NetProto::MAX_APP_VERSION);
    w.Str(S.nick, NetProto::MAX_NICK);
    NetProto::WriteAddress(w, lan);
    w.U8(static_cast<uint8_t>(S.lobby_locals));
    w.U8(flags & NetProto::FLAG_HIDDEN);
    w.Str(S.region, NetProto::MAX_REGION);
    break;
  case Want::List:
    // (00.85) Every room, from the next one not come yet; a server before ListAll: its public list, page by page.
    w.Header(S.list_old ? Type::List : Type::ListAll);
    w.Bytes(S.cookie, NetProto::COOKIE_SIZE);
    w.U32(S.nonce);
    w.Str(S.browse_console, NetProto::MAX_CONSOLE);
    if (S.list_old)
      w.U8(static_cast<uint8_t>(S.next_page));
    else
      w.U16(static_cast<uint16_t>(S.list_first));
    break;
  }
  if (w.Ok())
    SendRaw(S.server, buf, w.Size());
}

// A Hello for a cookie (padded: its answer is never bigger).
void SendHello(double now)
{
  S.hello_sent = now;
  S.hello_nonce = static_cast<uint32_t>(RandomNumber());
  uint8_t buf[NetProto::HELLO_SIZE + 8];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Hello);
  w.U32(S.hello_nonce);
  w.PadTo(NetProto::HELLO_SIZE);
  SendRaw(S.server, buf, w.Size());
}

// The main server looked up (each of the candidates in turn), the socket opened, the Hello for a cookie, then the
// request every second until it is answered.
void ConnectStep(double now)
{
  if (S.step == Step::Lookup)
  {
    if (S.candidates.empty())
    {
      // (No server known yet: the GitHub list may still come.)
      if (!ServerListDone() && now - S.step_start < 12.0)
        return;
      S.candidates = ServerCandidates(S.config, S.data_dir);
      if (S.candidates.empty())
      {
        CloseAll("No online server is known yet: try again later (or set one in netplay.ini: server=host:port)");
        return;
      }
      S.candidate = 0;
      SplitHostPort(S.candidates[0], &S.server_name, &S.server_port);
      if (!StartLookup(S.server_name))
      {
        CloseAll("The network couldn't be used");
        return;
      }
      S.step_start = now;
      return;
    }
    bool done = false;
    uint32_t ip = 0;
    std::string error;
    {
      std::lock_guard lock(s_lookup.mutex);
      done = s_lookup.done;
      ip = s_lookup.ip;
      error = s_lookup.error;
    }
    if (!done && now - S.step_start < 12.0)
      return;
    if (ip == 0)
    {
      if (S.candidate + 1 < S.candidates.size())
      {
        LogF("netplay: %s: %s; the next server", S.server_name.c_str(), error.empty() ? "no answer" : error.c_str());
        S.candidate++;
        SplitHostPort(S.candidates[S.candidate], &S.server_name, &S.server_port);
        StartLookup(S.server_name);
        S.step_start = now;
        return;
      }
      CloseAll(error.empty() ? std::string("The online server couldn't be found") : error);
      return;
    }
    if (!OpenSocket())
    {
      CloseAll("The network couldn't be used (no socket)");
      return;
    }
    S.server.ip = ip;
    S.server.port = S.server_port;
    S.my_lan.ip = LanAddress(S.server);
    S.my_lan.port = BoundPort();
#ifndef __PS4__
    unsigned a = 0, b = 0, c = 0, d = 0;
    const char* bind_to = std::getenv("EMUSUNI_NETPLAY_BIND");
    if (bind_to && std::sscanf(bind_to, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      S.my_lan.ip = (a << 24) | (b << 16) | (c << 8) | d;
#endif
    S.step = Step::Hello;
    S.tries = 0;
    S.hello_sent = 0.0;
    LogF("netplay: server %s:%u at %s; our socket %s", S.server_name.c_str(), S.server_port,
         NetProto::AddressText(S.server).c_str(), NetProto::AddressText(S.my_lan).c_str());
  }
  if (S.step == Step::Hello)
  {
    if (now - S.hello_sent < REQUEST_EVERY)
      return;
    if (S.tries >= HELLO_TRIES)
    {
      if (S.candidate + 1 < S.candidates.size())
      {
        LogF("netplay: %s didn't answer; the next server", S.server_name.c_str());
        S.candidate++;
        SplitHostPort(S.candidates[S.candidate], &S.server_name, &S.server_port);
        StartLookup(S.server_name);
        S.step = Step::Lookup;
        S.step_start = now;
        return;
      }
      CloseAll("The online server didn't answer: try again later");
      return;
    }
    S.tries++;
    SendHello(now);
    return;
  }
  // (The request: Cookie sent the first; again every second.)
  if (now - S.last_request < REQUEST_EVERY)
    return;
  if (S.want == Want::List && !S.list_old && !S.list_all_ok && S.tries >= LIST_ALL_TRIES)
  {
    // (00.85) It answered the Hello, never ListAll: a server before it. Its public list (the rooms waiting) instead.
    LogLine("netplay: the server lists no private rooms (an older one): its public rooms");
    S.list_old = true;
    S.tries = 0;
    S.next_page = 0;
    S.rooms_in.clear();
  }
  if (S.tries >= REQUEST_TRIES)
  {
    CloseAll("The online server didn't answer: try again later");
    return;
  }
  SendRequest();
}

// (00.86) The host's room listing the rooms (the Online tab while the room stays open): a Hello for a new cookie when
// the room's is old, then the list's pages, each asked again every second a few times. A list that doesn't come is let
// go (the next RefreshRooms asks again) - never the room.
void PumpRoomList(double now)
{
  if (!S.room_list || S.rooms_done || !S.host || S.phase != Phase::Room)
    return;
  if (S.step == Step::Hello)
  {
    if (now - S.hello_sent < REQUEST_EVERY)
      return;
    if (S.tries >= HELLO_TRIES)
    {
      S.room_list = false;
      LogLine("netplay: the server didn't answer for the list of rooms (the room stays)");
      return;
    }
    S.tries++;
    SendHello(now);
    return;
  }
  if (now - S.last_request < REQUEST_EVERY)
    return;
  if (!S.list_old && !S.list_all_ok && S.tries >= LIST_ALL_TRIES)
  {
    LogLine("netplay: the server lists no private rooms (an older one): its public rooms");
    S.list_old = true;
    S.tries = 0;
    S.next_page = 0;
    S.rooms_in.clear();
  }
  if (S.tries >= REQUEST_TRIES)
  {
    S.room_list = false;
    LogLine("netplay: the list of rooms didn't come (the room stays)");
    return;
  }
  SendRequest();
}

// (00.86) The requests to join: the host's not answered in time expire (the guest is told, and leaves; still there a
// while after, the server removes it); a guest left waiting longer than that (the host's word lost) leaves by itself.
void PumpRequests(double now)
{
  if (S.phase != Phase::Room)
    return;
  if (!S.host)
  {
    if (!S.admitted && now - S.waiting_since > AskSeconds() + 5.0)
      CloseAll("The host didn't answer: try again later");
    return;
  }
  for (Peer& p : S.peers)
  {
    if (!p.present || p.admitted)
      continue;
    const double asked = now - p.asked_at;
    if (!p.ask_expired && asked > AskSeconds())
    {
      p.ask_expired = true;
      LogF("netplay: %s's request to join expired (no answer in %.0f s)", p.info.nick.c_str(), AskSeconds());
      SendQuitNow(p, NetProto::QUIT_NO_ANSWER);
    }
    else if (p.ask_expired && asked > AskSeconds() + ASK_GRACE)
    {
      RemoveMember(p, NetProto::QUIT_NO_ANSWER, PeerName(p) + "'s request expired: removed");
    }
  }
}

void PumpPeer(Peer& p, double now)
{
  // The hole punch: to its public and LAN addresses, often in the first 5 s, then every second for a while. (Not when
  // either hides its address: the relay carries everything.)
  const bool can_punch = !S.hidden && !(p.info.flags & NetProto::FLAG_HIDDEN) && p.info.pub.Valid();
  if (can_punch && !p.direct_ok && now - p.punch_start < PUNCH_FOR)
  {
    const double every = now - p.punch_start < 5.0 ? PUNCH_EVERY : PUNCH_AGAIN_EVERY;
    if (now - p.last_punch >= every)
    {
      p.last_punch = now;
      Address targets[2] = {p.info.pub, p.info.lan};
      for (int i = 0; i < 2; i++)
      {
        if (!targets[i].Valid() || (i == 1 && targets[1] == targets[0]))
          continue;
        uint8_t buf[64];
        Writer w(buf, sizeof(buf));
        PeerHeader(w, Type::Punch);
        w.U32(p.punch_nonce);
        NetProto::WriteAddress(w, targets[i]);
        SendRaw(targets[i], buf, w.Size());
      }
    }
  }
  if (!p.direct_ok && !p.relay_said && now - p.first_seen > 3.0)
  {
    p.relay_said = true;
    LogF("netplay: %s: the packets go through a relay", can_punch ? "no direct path yet" : "an address hidden");
  }
  if (p.direct_ok && now - p.last_direct > DIRECT_LOST_AFTER && now - p.last_heard < 1.0)
  {
    p.direct_ok = false;
    p.punch_start = now;
    p.relay_said = false;
    LogF("netplay: the direct path to %s stopped: through a relay again", p.info.nick.c_str());
  }
  const double ping_every = S.match == Match::Sync    ? PING_EVERY_START :
                            S.match == Match::Running ? PING_EVERY_MATCH :
                                                        PING_EVERY;
  if (now - p.last_ping >= ping_every)
  {
    p.last_ping = now;
    SendPing(p);
  }
  for (ControlOut& c : p.ctl_out)
  {
    if (now - c.first > CONTROL_GIVE_UP)
    {
      PeerGone(p, "The connection to " + PeerName(p) + " was lost");
      return;
    }
    if (now - c.last >= CONTROL_RESEND)
    {
      c.last = now;
      SendPeer(p, c.packet.data(), c.packet.size());
    }
  }
  PumpBulk(p);
  if (p.bulk_in.ack_due && now - p.bulk_in.last_ack >= 0.03)
    SendBulkAck(p);
  const double timeout = S.phase == Phase::Playing  ? PEER_TIMEOUT_GAME :
                         S.phase == Phase::Starting ? PEER_TIMEOUT_START :
                                                      PEER_TIMEOUT_LOBBY;
  if (now - p.last_heard > timeout)
    PeerGone(p, "The connection to " + PeerName(p) + " was lost");
}

void PumpVoice()
{
  if (!S.voice_running)
    return;
  std::vector<uint8_t> packet;
  uint16_t seq = 0;
  for (int i = 0; i < 16 && Voice::TakePacket(&packet, &seq); i++)
  {
    if (packet.empty() || packet.size() > PEER_MAX - 32)
      continue;
    uint8_t buf[NetProto::MAX_PACKET];
    Writer w(buf, sizeof(buf));
    PeerHeader(w, Type::Voice);
    w.U16(seq);
    w.Bytes(packet.data(), packet.size());
    if (!w.Ok())
      continue;
    for (Peer& p : S.peers)
    {
      if (p.present && p.voice_on)
        SendPeer(p, buf, w.Size());  // (only to those who have theirs on)
    }
  }
}

// (00.92) A connection's quality, the latency bar's colour: by its ping (green under 80 ms, amber under 150, red above;
// as the game's corner showed it) - worse with a jittery ping, pings lost or waits for that player's inputs.
int QualityOf(int ping_ms, int jitter_ms, int lost_pm, int waits_pm)
{
  if (ping_ms < 0)
    return -1;
  int q = ping_ms < 80 ? 0 : ping_ms < 150 ? 1 : 2;
  if (jitter_ms >= 15 || lost_pm >= 20 || waits_pm >= 30)
    q = std::max(q, 1);
  if (jitter_ms >= 40 || lost_pm >= 60 || waits_pm >= 100)
    q = 2;
  return q;
}

std::vector<PlayerView> Players()
{
  std::vector<PlayerView> out;
  // (00.92) The waits for each one's inputs: the match's last 5 s. Mine: the worst of what the others say of me.
  const Session::Second last = LastSeconds(5);
  const bool counted = S.match == Match::Running && last.passes > 0;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    PlayerView v;
    v.id = m;
    if (m == S.my_id)
    {
      if (S.phase == Phase::Off || S.phase == Phase::Connecting || S.phase == Phase::Browsing)
        continue;
      v.nick = S.nick;
      v.region = S.region;
      v.me = true;
      v.linked = true;
      v.locals = S.lobby_locals;
      v.voice_on = S.voice_running;
      v.talking = S.voice_running && Voice::Talking(-1);
      v.game = S.host ? LobbyGame::Same : S.my_game;
      // (00.92) The others' connections to us, as they say (the ping: ours to them - the same path).
      for (const Peer& p : S.peers)
      {
        if (!p.present || p.rtt_ms < 0.0)
          continue;
        v.ping_ms = std::max(v.ping_ms, static_cast<int>(std::lround(p.rtt_ms)));
        if (p.says.known)
        {
          v.jitter_ms = std::max(v.jitter_ms, p.says.jitter_ms);
          v.lost_pm = std::max(v.lost_pm, p.says.lost_pm);
          v.waits_pm = std::max(v.waits_pm, S.match == Match::Running ? p.says.waits_pm : 0);
        }
      }
    }
    else
    {
      const Peer& p = S.peers[m];
      if (!p.present || (!S.host && !p.admitted))
        continue;  // (00.95: a guest doesn't see one still asking the host to join)
      v.nick = p.info.nick;
      v.region = p.info.region;
      v.ping_ms = p.rtt_ms < 0.0 ? -1 : static_cast<int>(std::lround(p.rtt_ms));
      v.linked = p.rtt_ms >= 0.0 && Now() - p.last_heard < 3.0;
      v.relay = !p.direct_ok;
      v.locals = p.info.locals;
      v.voice_on = p.voice_on;
      v.muted = p.muted;
      v.talking = S.voice_running && !p.muted && Voice::Talking(m);
      v.game = m == 0 ? LobbyGame::Same : p.game;
      v.asking = S.host && !p.admitted;  // (00.86)
      v.jitter_ms = p.jitter_ms < 0.0 ? -1 : static_cast<int>(std::lround(p.jitter_ms));
      v.lost_pm = LostPermille(p);
      v.waits_pm = counted ? last.waited_for[m] * 1000 / last.passes : 0;
    }
    v.quality = QualityOf(v.ping_ms, v.jitter_ms, v.lost_pm, v.waits_pm);
    if (S.phase == Phase::Playing)
    {
      v.first_port = S.first_port[m];
      v.ports = S.ports[m];
    }
    out.push_back(v);
  }
  return out;
}

void UpdateStatus()
{
  Status st;
  st.active = S.phase == Phase::Starting || S.phase == Phase::Playing;
  st.host = S.host;
  st.voice_on = S.voice_running;
  st.together = S.together;
  st.players = Players();
  st.delay = S.match == Match::Running ? S.delay : 0;
  st.max_rollback = RollbackReach();
  st.latency_bar = S.config.latency_bar;
  std::string other;
  for (const PlayerView& v : st.players)
  {
    if (v.me)
      continue;
    st.ping_ms = std::max(st.ping_ms, v.ping_ms);
    st.relay = st.relay || v.relay;
    if (other.empty())
      other = v.nick;
  }
  if (other.empty())
    other = "the other player";
  // (00.95) Of three or four: the one waited for - the host's guest that hasn't loaded the state yet, the host for a
  // guest, the one whose inputs are the oldest in the match.
  if (S.match == Match::Sync)
  {
    for (const Peer& p : S.peers)
    {
      if (p.present && (S.host ? !p.ready : p.info.id == 0))
      {
        other = p.info.nick;
        break;
      }
    }
  }
  else if (S.match == Match::Running)
  {
    int whose = -1;
    for (int m = 0; m < MAX_MEMBERS; m++)
    {
      if (m != S.my_id && S.peers[m].present && (whose < 0 || S.inputs[m].latest < S.inputs[whose].latest))
        whose = m;
    }
    if (whose >= 0)
      other = S.peers[whose].info.nick;
  }
  if (!S.over)
  {
    if (S.match == Match::Options)
      st.line = "Getting the host's settings...";
    else if (S.match == Match::Sync && S.host)
      st.line = "Waiting for " + other + "...";
    else if (S.match == Match::Sync)
    {
      int percent = 0;
      for (const Peer& p : S.peers)
      {
        if (p.present && p.bulk_in.active && p.bulk_in.chunks > 0)
          percent = p.bulk_in.got_count * 100 / p.bulk_in.chunks;
      }
      st.line = percent > 0 ? "Receiving the game from " + other + "... " + std::to_string(percent) + "%" :
                              "Connecting to " + other + "...";
    }
    else if (S.match == Match::Resync)
      st.line = "Resyncing...";
    else if (S.match == Match::Running && S.stalled_since >= 0.0 && Now() - S.stalled_since > 0.5)
      st.line = "Waiting for " + other + "...";
  }
  std::lock_guard lock(s_status_mutex);
  s_status = st;
}

void Pump()
{
  if (S.phase == Phase::Off)
    return;
  const double now = Now();
  // (00.92: a long time since the last pass - this console was busy, loading a game -: its pongs read now came late.)
  S.pump_gap = S.last_pump > 0.0 ? now - S.last_pump : 0.0;
  S.last_pump = now;
  if (S.phase == Phase::Connecting || (S.phase == Phase::Browsing && !S.rooms_done))
    ConnectStep(now);
  if (S.phase == Phase::Off)
    return;
  Receive();
  if (S.phase == Phase::Off)
    return;
#ifndef __PS4__
  NetsimFlush(false);
#endif
  if (S.phase == Phase::Room || S.phase == Phase::Starting || S.phase == Phase::Playing)
  {
    if (now - S.last_keepalive >= KEEPALIVE_EVERY)
    {
      S.last_keepalive = now;
      S.keepalive_sent = now;
      uint8_t buf[64];
      Writer w(buf, sizeof(buf));
      ServerAuth(w, Type::Keepalive);
      w.U16(static_cast<uint16_t>(std::clamp(S.server_ms, 0, 65535)));
      SendRaw(S.server, buf, w.Size());
    }
    if (S.phase == Phase::Room && now - S.last_server_heard > SERVER_TIMEOUT)
    {
      CloseAll("The connection to the online server was lost");
      return;
    }
    PumpRelays(now);
  }
  for (Peer& p : S.peers)
  {
    if (p.present)
      PumpPeer(p, now);
    if (S.phase == Phase::Off)
      return;
  }
  // (00.86) The requests to join; the host's room's list of rooms.
  PumpRequests(now);
  if (S.phase == Phase::Off)
    return;
  PumpRoomList(now);
  // (00.92) In the room, every ROOM_LOG_EVERY s: each other player's ping, jitter and pings lost (the match's are
  // LogWindow's).
  if (S.phase == Phase::Room && now - S.room_logged >= ROOM_LOG_EVERY)
  {
    S.room_logged = now;
    for (const Peer& p : S.peers)
    {
      if (p.present && p.rtt_ms > 0.0 && p.admitted)
        LogF("netplay: in the room: %s: ping %.0f ms, jitter %.0f ms, %.1f%% of pings lost%s", p.info.nick.c_str(),
             p.rtt_ms, std::max(p.jitter_ms, 0.0), LostPermille(p) / 10.0, p.direct_ok ? "" : " (relay)");
    }
  }
  if (S.match == Match::Running || S.match == Match::Resync)
  {
    for (Peer& p : S.peers)
    {
      if (p.present && now - p.last_input_sent >= 0.015)
        SendInputs(p);
    }
  }
  SendGames();
  PumpVoice();
  TryGo();
  UpdateStatus();
}

// A guest's blocks from the host, taken in the game: the start state (the devices, the battery save, frame 0; then
// Ready), a resync state.
void TakeBlocks()
{
  for (size_t i = 0; i < S.blocks.size();)
  {
    const BulkKind kind = S.blocks[i].first;
    if (kind == BulkKind::State && S.match == Match::Sync && !S.host)
    {
      std::vector<uint8_t> raw = std::move(S.blocks[i].second);
      S.blocks.erase(S.blocks.begin() + static_cast<long>(i));
      int32_t frame = 0;
      uint32_t epoch = 0;
      if (!TakeStatePacket(raw, &frame, &epoch, true))
      {
        EndMatch("The host's game state couldn't be loaded (is it the same game?)");
        return;
      }
      S.epoch = epoch;
      uint8_t payload[1] = {static_cast<uint8_t>(std::clamp(S.local_count, 1, MAX_PORTS))};
      for (Peer& p : S.peers)
      {
        if (p.present && p.info.id == 0)
          SendControl(p, ControlKind::Ready, payload, 1);
      }
      LogF("netplay: the host's state loaded (%zu bytes): ready", raw.size());
      continue;
    }
    if (kind == BulkKind::Resync && !S.host && (S.match == Match::Running || S.match == Match::Resync))
    {
      std::vector<uint8_t> raw = std::move(S.blocks[i].second);
      S.blocks.erase(S.blocks.begin() + static_cast<long>(i));
      ApplyResync(raw);
      continue;
    }
    i++;
  }
}

// The controller devices' hook (WrapCore): each kept; in the match the session's stay.
void PortDevice(unsigned port, unsigned device)
{
  if (S.devices_locked)
  {
    LogF("netplay: port %u's device %#x not changed in the match", port + 1, device);
    return;
  }
  if (port < 8)
  {
    S.devices[port] = device;
    S.device_set[port] = true;
  }
  if (s_real_core && s_real_core->set_controller_port_device)
    s_real_core->set_controller_port_device(port, device);
}

// The common start of Host, Join and Browse.
void Begin(Want want, const std::string& data_dir)
{
  CloseAll("");
  S.error.clear();
#ifndef __PS4__
  LoadNetsim();
#endif
  S.data_dir = data_dir;
  S.config = LoadConfig(data_dir);
  S.nick = Nickname(S.config);
  S.region = Region();
  S.lobby_locals = SignedInPlayers();
  S.hidden = S.config.hide_address;
  S.voice_enabled = S.config.voice;
  S.want = want;
  S.step = Step::Lookup;
  S.step_start = Now();
  S.nonce = static_cast<uint32_t>(RandomNumber());
  StartServerList(data_dir);
  UsageStats::SetRoom(true);  // (00.23: the anonymous statistics' state: online)
}
}  // namespace

// ---------------------------------------------------------------------------------------------

std::vector<std::string> MainServers(const std::string& data_dir, double wait_seconds)
{
  StartServerList(data_dir);
  const double until = Now() + wait_seconds;
  while (!ServerListDone() && Now() < until)
    usleep(200 * 1000);
  return ServerCandidates(LoadConfig(data_dir), data_dir);
}

uint32_t LookUp(const std::string& host)
{
  std::string error;
  return ResolveNow(host, &error);
}

Rules RulesFor(const std::string& key)
{
  Rules r;
  // The light consoles roll back: a frame, its state saved and loaded, costs little. The limits come from the PC's
  // measurements (local/herramientas/prueba-netplay: nptool measure, busy test programs) against the PS4's CPU, about
  // 3.5 times slower: a rollback of max_rollback frames and the frame itself fit in one refresh. On the PC, a frame
  // and its state saved: FCEUmm 0.22 ms (state 13 KiB), Snes9x 0.42 (804 KiB), Genesis Plus GX 0.16 (Mega Drive) and
  // 0.08 (Master System; 1012 KiB), Beetle PCE Fast 0.13 (78 KiB), FinalBurn Neo 0.10-0.29 (the free boards tested;
  // the Neo Geo's and CPS's ROMs aren't free to test: a few times more). Every one gave the same state from the same
  // inputs, also without drawing.
  if (key == NetProto::GBLINK)
  {
    // (00.95) SameBoy's two Game Boys joined by the cable (src/GbLink.h): a frame of both and its state saved 1.7-1.8 ms
    // on the PC (local/herramientas/prueba-gblink: Pokemon Red, Gold, Crystal and Yellow), about 6 on the PS4 - four
    // frames back fit in two refreshes (RollbackReach takes fewer if the PS4 measures it slower). Its state (247 KiB) is
    // the same bytes on both consoles, the link cable's bits on their way included (scripts/build-gblink-core.sh).
    r.max_rollback = 4;
  }
  else if (key == "nes" || key == "sg1000" || key == "sms" || key == "gg" || key == "md" || key == "pce")
  {
    r.max_rollback = 8;  // (the PS4: ~7 ms for 8 frames and the frame)
    // (Genesis Plus GX's state holds its Z80's interrupt callback, a pointer of each console's own: its memory is
    // compared instead. FCEUmm's and Beetle's states are the same bytes on both. Its Mega Drive's loaded state goes on
    // otherwise than the one it was saved from: every frame starts from its state loaded again.)
    r.check_memory = key != "nes" && key != "pce";
    r.reload_each_frame = key == "md";
  }
  else if (key == "snes")
    r.max_rollback = 6;  // (~11 ms; a Super FX or SA-1 game's frames cost two or three times more)
  else if (key == "atari2600")
  {
    // (01.03) Stella: a frame and its state saved 0.67-0.74 ms on the PC, 1.0-1.1 with a cartridge's ARM (Draconian,
    // Lady Bug Arcade); 6 frames back and the frame 4.6-5.2 ms (6.8-8.0) - the PS4's is about 7 times slower (Jr.
    // Pac-Man 5.4 ms a frame): RollbackReach takes ~5 there, ~3 with an ARM (local/herramientas/prueba-atari:
    // HARNESS_NETPLAY). Its state (4-37 KiB) is the same bytes on both consoles once the guest has the host's (its
    // random seed, from the clock at its start, is in it; after it the paddles', the driving controller's and the
    // input window's own: scripts/build-atari-cores.sh - without the window the first frame after a load read the
    // input from before it). The sticks too: Stella's paddles follow the left stick. Every frame from its state loaded
    // again (~0.02 ms): the first frame after a load leaves one byte of its state otherwise than a frame without one -
    // so the checksums of both consoles' states always agree.
    r.max_rollback = 6;
    r.full_input = true;
    r.reload_each_frame = true;
  }
  else if (key == "atari7800")
  {
    // (01.03) ProSystem: a frame and its state 0.16-0.25 ms on the PC, 8 frames back and the frame 1.4-2.2 (the PS4's
    // Mario Bros. 1.8 ms a frame). Its "fast" state (82 KiB) - Emulator.cpp says so in a 7800 match - with the RIOT's
    // timer (scripts/build-atari-cores.sh) is the same bytes on both: without them 1942 (Extended) went on otherwise.
    r.max_rollback = 8;
  }
  else if (key == "arcade")
  {
    // (00.92: 6 as the SNES - 00.91's log, the PS4: CPS2's Street Fighter Alpha 3 and X-Men vs. Street Fighter 4.0-4.6
    // ms a frame, Snes9x's Killer Instinct 4.6; a heavier board rolls back less, RollbackReach. With 3, a ping of 100 ms
    // and a busy Wi-Fi's moments made the match wait or play 7 frames late.)
    r.max_rollback = 6;
    r.full_input = true;  // (a wheel, a paddle or a dial: FinalBurn Neo reads the sticks)
    r.check_memory = true;  // (a board's state may hold bytes that differ from one save to the next: Robby Roto's)
  }
  else
    return r;  // (not offered)
  r.online = true;
  return r;
}

bool GameCrc(const std::string& path, uint32_t* crc, uint64_t* size)
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
  constexpr uint64_t WHOLE = 64ull << 20, PART = 8ull << 20;
  std::vector<uint8_t> buf(1 << 20);
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

void Host(const NetProto::GameInfo& game, bool is_public, const std::string& data_dir, int max_players)
{
  Begin(Want::Create, data_dir);
  S.host = true;
  S.is_public = is_public;
  S.max_members = std::clamp(max_players, 2, MAX_MEMBERS);
  if (game.console == NetProto::GBLINK)
    S.max_members = 2;  // (00.95: the link cable joins two Game Boys)
  S.game = game;
  S.game.title = game.title.substr(0, NetProto::MAX_TITLE);
  S.phase = Phase::Connecting;
  if (game.console == NetProto::TOGETHER)
    LogF("netplay: making a %s Choose Together room (no game yet) for %d players as %s (%s)%s",
         is_public ? "public" : "private", S.max_members, S.nick.c_str(), S.region.c_str(),
         S.hidden ? ", the address hidden" : "");
  else
    LogF("netplay: making a %s room for %s (%s, CRC32 %08x, %llu bytes) for %d players as %s (%s)%s",
         is_public ? "public" : "private", game.title.c_str(), game.console.c_str(), game.crc,
         static_cast<unsigned long long>(game.size), S.max_members, S.nick.c_str(), S.region.c_str(),
         S.hidden ? ", the address hidden" : "");
}

void Join(const std::string& typed, const std::string& data_dir)
{
  const std::string code = NetProto::NormalizeCode(typed);
  // From the public list: the server, its cookie and the socket already there.
  const bool from_list = S.phase == Phase::Browsing && S.step == Step::Request;
  if (from_list)
  {
    S.want = Want::Join;
    S.phase = Phase::Connecting;
    S.code = code;
    S.host = false;
    S.nonce = static_cast<uint32_t>(RandomNumber());
    S.tries = 0;
    S.rooms.clear();
    LogF("netplay: joining room %s from the public list as %s", code.c_str(), S.nick.c_str());
    SendRequest();
    return;
  }
  Begin(Want::Join, data_dir);
  S.host = false;
  S.code = code;
  if (!NetProto::ValidCode(S.code))
  {
    Fail("That isn't a room code: 6 letters and numbers");
    return;
  }
  S.phase = Phase::Connecting;
  LogF("netplay: joining room %s as %s (%s)%s", S.code.c_str(), S.nick.c_str(), S.region.c_str(),
       S.hidden ? ", the address hidden" : "");
}

void Browse(const std::string& console_key, const std::string& data_dir)
{
  // (00.86) The host's room open: the list on its connection - never a new session, which would close the room.
  if (Hosting())
  {
    S.browse_console = console_key;
    RefreshRooms();
    return;
  }
  Begin(Want::List, data_dir);
  S.browse_console = console_key;
  S.phase = Phase::Browsing;
  S.next_page = 0;
  S.list_first = 0;
  S.rooms_done = false;
  S.last_list = Now();
  LogF("netplay: the list of rooms (%s)", console_key.empty() ? "every console" : console_key.c_str());
}

void RefreshRooms()
{
  // (00.86) The host's room open (not while it is being made): the list asked for on the room's socket - with a new
  // cookie first when the room's is old (PumpRoomList) -, one list at a time.
  if (S.host && S.phase == Phase::Room)
  {
    const double now = Now();
    if ((S.room_list && !S.rooms_done) || now - S.last_list < 2.0)
      return;
    S.room_list = true;
    S.last_list = now;
    S.rooms_done = false;
    S.next_page = 0;
    S.list_first = 0;
    S.rooms_in.clear();
    S.tries = 0;
    S.nonce = static_cast<uint32_t>(RandomNumber());
    if (now - S.cookie_at > COOKIE_FRESH)
    {
      S.step = Step::Hello;
      S.hello_sent = 0.0;
      return;
    }
    S.step = Step::Request;
    SendRequest();
    return;
  }
  if (S.phase != Phase::Browsing || !S.rooms_done || Now() - S.last_list < 2.0)
    return;
  S.last_list = Now();
  S.rooms_done = false;
  S.next_page = 0;
  S.list_first = 0;
  S.rooms_in.clear();
  S.tries = 0;
  S.nonce = static_cast<uint32_t>(RandomNumber());
  if (S.step == Step::Request)
    SendRequest();
}

void Update()
{
  Pump();
}

View GetView()
{
  View v;
  v.phase = S.phase;
  v.host = S.host;
  v.is_public = S.is_public;
  v.hidden = S.hidden;
  v.code = S.code;
  v.error = S.error;
  v.game = S.game;
  v.players = Players();
  v.voice_on = S.voice_enabled;
  v.rooms = S.rooms;
  v.rooms_done = S.rooms_done;
  v.rooms_total = S.rooms_total;
  const int c = S.relay_choice;
  bool relayed = false;
  for (const Peer& p : S.peers)
    relayed = relayed || (p.present && !p.direct_ok);
  if (relayed && c >= 0 && c < static_cast<int>(S.relays.size()))
    v.relay = S.relays[static_cast<size_t>(c)].info.name;
  int guests = 0;
  bool ready = true;
  for (const Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    guests++;
    // (A Choose Together room: the game is one both have, picked from their common list. 00.86: every player
    // accepted - one still asking waits for the host's answer.)
    ready = ready && p.admitted && p.rtt_ms >= 0.0 && Now() - p.last_heard < 3.0 &&
            (S.together || p.game == LobbyGame::Same || p.game == LobbyGame::Different);
    // (00.95) A Game Boy link cable room: the guest's game for Game Boy 2 here too, and its battery save in.
    if (LinkRoom())
    {
      ready = ready && p.link_game_in && p.link_save_in && S.link_found == LobbyGame::Same;
      v.link_game_in = p.link_game_in;
      v.link_game = p.link_game;
      v.link_title = p.link_title;
      v.link_save_in = p.link_save_in;
    }
  }
  v.link = LinkRoom();
  v.link_found = S.link_found;
  if (v.link && !S.host)
  {
    v.link_game_in = S.link_set;
    v.link_game = S.link_game;
    v.link_title = S.link_title;
  }
  v.can_start = S.host && S.phase == Phase::Room && guests > 0 && ready;
  // (00.86) A player asking to join (the host's), or waiting for the host's answer (a guest's).
  const JoinRequest ask = GetJoinRequest();
  v.asking = ask.member;
  v.asking_nick = ask.nick;
  v.asking_region = ask.region;
  v.asking_seconds = ask.seconds_left;
  v.asking_serial = ask.serial;
  v.waiting_host = !S.host && S.phase == Phase::Room && !S.admitted;
  if (v.waiting_host)
    v.waiting_seconds = std::max(0, static_cast<int>(std::ceil(AskSeconds() - (Now() - S.waiting_since))));
  v.max_players = S.max_members;
  v.together = S.together;
  v.common = S.common;
  v.common_ready = S.common_ready;
  v.games_sent = S.games_sent;
  v.peer_games_in = GuestGamesIn();
  v.has_suggestion = S.has_suggestion;
  v.suggested = S.suggested;
  v.notice = S.notice;
  return v;
}

void SetLobbyGame(NetProto::LobbyGame game)
{
  if (S.my_game == game)
    return;
  S.my_game = game;
  SendLobbyState();
}

bool StartMatch()
{
  if (!GetView().can_start)
    return false;
  // (A Choose Together room: the game picked goes with it.)
  uint8_t payload[NetProto::MAX_PACKET / 2];
  Writer w_game(payload, sizeof(payload));
  if (S.together)
    NetProto::WriteGame(w_game, S.game);
  SendControlAll(ControlKind::StartGame, payload, w_game.Size());
  S.notice.clear();
  // The room leaves the public list and takes no one else.
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  ServerAuth(w, Type::Lock);
  w.U8(1);
  SendRaw(S.server, buf, w.Size());
  S.phase = Phase::Starting;
  S.start_pending = true;
  LogLine("netplay: the host starts the game");
  return true;
}

bool TakeStart()
{
  if (!S.start_pending)
    return false;
  S.start_pending = false;
  return true;
}

void HostTogether(bool is_public, const std::string& data_dir, int max_players)
{
  NetProto::GameInfo game;
  game.console = NetProto::TOGETHER;
  Host(game, is_public, data_dir, max_players);
  S.together = true;
  S.room_game = game;
}

void SetLibrary(const std::vector<NetProto::LibraryGame>& games, bool complete)
{
  const bool changed = complete != S.my_games_complete || games.size() != S.my_games.size();
  S.my_games = games;
  S.my_games_complete = complete;
  if (!changed || !complete)
    return;
  LogF("netplay: this console's games that play online: %zu", games.size());
  S.games_sent = false;  // (a guest sends them - again, when they changed)
  ShareCommon();         // (the host: the common list again)
}

void Suggest(const NetProto::LibraryGame& game)
{
  if (S.host || !S.together || S.phase != Phase::Room)
    return;
  uint8_t payload[64];
  Writer w(payload, sizeof(payload));
  w.Str(game.console, NetProto::MAX_CONSOLE);
  w.U32(game.crc);
  w.U64(game.size);
  if (Peer* host = FindPeer(0))
    SendControl(*host, ControlKind::Suggest, payload, w.Size());
}

bool PickGame(const NetProto::GameInfo& game)
{
  if (!S.host || !S.together || S.phase != Phase::Room)
    return false;
  NetProto::LibraryGame g;
  g.console = game.console;
  g.crc = game.crc;
  g.size = game.size;
  if (!std::binary_search(S.common.begin(), S.common.end(), g))
    return false;  // (only a game both have)
  const NetProto::GameInfo before = S.game;
  S.game = game;
  if (StartMatch())
    return true;
  S.game = before;
  return false;
}

void EndMatchToRoom()
{
  if (!S.together || !Active() || S.over)
    return;
  S.to_room = true;
  SendControlAll(ControlKind::EndMatch, nullptr, 0);
  LogLine("netplay: the match ends here: back to the room for both");
}

bool Together()
{
  return S.together;
}

// ---- (00.95) The Game Boy link cable online ----
bool LinkRoom()
{
  return S.game.console == NetProto::GBLINK || S.room_game.console == NetProto::GBLINK;
}

void SetLinkGame(const NetProto::LibraryGame& game, const std::string& title, const std::vector<uint8_t>& packed_save)
{
  if (S.host || !LinkRoom())
    return;
  S.link_game = game;
  S.link_title = title.substr(0, NetProto::MAX_TITLE);
  S.link_save = packed_save;
  S.link_set = true;
  S.link_sent = false;
  S.link_found = LobbyGame::Looking;
  SendLobbyState();
}

void SetLinkGameFound(bool found)
{
  if (!S.host || !LinkRoom())
    return;
  const LobbyGame now = found ? LobbyGame::Same : LobbyGame::Missing;
  if (S.link_found == now)
    return;
  S.link_found = now;
  for (Peer& p : S.peers)
  {
    if (!p.present || !p.link_game_in)
      continue;
    uint8_t payload[1] = {static_cast<uint8_t>(now)};
    SendControl(p, ControlKind::LinkGame, payload, sizeof(payload));
  }
  LogF("netplay: the guest's game for Game Boy 2: %s here", found ? "the same file" : "not");
}

std::vector<uint8_t> LinkGuestSave()
{
  for (const Peer& p : S.peers)
  {
    if (p.present && p.link_save_in)
      return p.link_save;
  }
  return {};
}

void Kick(int member)
{
  Peer* p = FindPeer(member);
  if (!S.host || !p)
    return;
  LogF("netplay: %s removed from the room", p->info.nick.c_str());
  RemoveMember(*p, NetProto::QUIT_REMOVED, "You removed " + PeerName(*p) + " from the room");
}

bool Hosting()
{
  return S.host && (S.phase == Phase::Connecting || S.phase == Phase::Room);
}

void Admit(int member)
{
  Peer* p = FindPeer(member);
  if (!S.host || S.phase != Phase::Room || !p || p->admitted || p->ask_expired)
    return;
  p->admitted = true;
  // (00.95) Every member accepted, a bit each (ours too), to each of them: the other guests know who is in the room.
  uint8_t mask = 1;
  for (const Peer& q : S.peers)
    mask = static_cast<uint8_t>(mask | (q.present && q.admitted ? 1u << q.info.id : 0u));
  for (Peer& q : S.peers)
  {
    if (q.present && q.admitted)
      SendControl(q, ControlKind::Admit, &mask, 1);
  }
  LogF("netplay: %s accepted in the room (after %.0f s)", p->info.nick.c_str(), Now() - p->asked_at);
  if (S.together)
  {
    // (00.95) The games every player has: again once its games come.
    S.common.clear();
    S.common_ready = false;
  }
  ShareCommon();  // (a Choose Together room: once its games come - after this)
}

void Refuse(int member)
{
  Peer* p = FindPeer(member);
  if (!S.host || !p || p->admitted)
    return;
  LogF("netplay: %s not accepted (it can't ask again in this room)", p->info.nick.c_str());
  RemoveMember(*p, NetProto::QUIT_REFUSED, PeerName(*p) + " was not accepted");
}

JoinRequest GetJoinRequest()
{
  JoinRequest r;
  if (!S.host || S.phase != Phase::Room)
    return r;
  for (const Peer& p : S.peers)
  {
    if (!p.present || p.admitted || p.ask_expired)
      continue;
    r.member = p.info.id;
    r.nick = p.info.nick;
    r.region = p.info.region;
    r.seconds_left = std::max(0, static_cast<int>(std::ceil(AskSeconds() - (Now() - p.asked_at))));
    r.serial = p.ask_serial;
    break;
  }
  return r;
}

void Leave()
{
  if (S.phase == Phase::Off)
    return;
  LogLine(S.phase == Phase::Browsing ? "netplay: the list of rooms closed" :
          S.host                     ? "netplay: closing the room" :
                                       "netplay: leaving the room");
  for (Peer& p : S.peers)
  {
    if (p.present)
      SendQuitNow(p, 0);
  }
  CloseAll("");
  S.error.clear();
}

std::string TakeError()
{
  std::string error;
  error.swap(S.error);
  return error;
}

bool HideAddress(const std::string& data_dir)
{
  return LoadConfig(data_dir).hide_address;
}

void SetHideAddress(const std::string& data_dir, bool hide)
{
  SaveConfigKey(data_dir, "hide_address", hide ? "on" : "off");
  LogF("netplay: hide my address %s", hide ? "on" : "off");
}

bool LatencyBar(const std::string& data_dir)
{
  return LoadConfig(data_dir).latency_bar;
}

void SetLatencyBar(const std::string& data_dir, bool on)
{
  SaveConfigKey(data_dir, "latency_bar", on ? "on" : "off");
  S.config.latency_bar = on;  // (the room's and the match's at once)
  UpdateStatus();
  LogF("netplay: the latency bar %s", on ? "shown" : "hidden");
}

std::string GetNickname(const std::string& data_dir)
{
  return Nickname(LoadConfig(data_dir));
}

void SetNickname(const std::string& data_dir, const std::string& nickname)
{
  // (Kept as the server would show it: an empty or all-filtered one is the PS4 user's name again.)
  const std::string trimmed = Trim(nickname);
  const std::string clean = trimmed.empty() ? std::string() : NetProto::CleanNick(trimmed);
  SaveConfigKey(data_dir, "nickname", clean == "Player" && trimmed != "Player" ? std::string() : clean);
  LogF("netplay: nickname %s", clean.empty() ? "the PS4 user's" : ("\"" + clean + "\"").c_str());
}

bool Active()
{
  return S.phase == Phase::Starting || S.phase == Phase::Playing;
}

bool IsHost()
{
  return S.host;
}

const CoreApi* WrapCore(const CoreApi* core)
{
  S.core = core;
  s_real_core = core;
  s_wrapped = *core;
  s_wrapped.set_controller_port_device = &PortDevice;
  std::fill(std::begin(S.device_set), std::end(S.device_set), false);
  S.devices_locked = false;
  return &s_wrapped;
}

Wait AgreeOptions(std::map<std::string, std::string>* variables)
{
  Pump();
  if (S.over || !Active())
    return Wait::Failed;
  if (S.host)
  {
    S.variables = *variables;
    S.variables_agreed = true;
    std::vector<uint8_t> raw;
    PutU32(raw, static_cast<uint32_t>(variables->size()));
    for (const auto& [key, value] : *variables)
    {
      PutU32(raw, static_cast<uint32_t>(key.size()));
      PutBytes(raw, key.data(), key.size());
      PutU32(raw, static_cast<uint32_t>(value.size()));
      PutBytes(raw, value.data(), value.size());
    }
    for (Peer& p : S.peers)
    {
      if (p.present)
        SendBulk(p, BulkKind::Options, raw);
    }
    S.match = Match::Sync;
    return Wait::Done;
  }
  S.match = Match::Options;
  for (size_t i = 0; i < S.blocks.size(); i++)
  {
    if (S.blocks[i].first != BulkKind::Options)
      continue;
    const std::vector<uint8_t> raw = std::move(S.blocks[i].second);
    S.blocks.erase(S.blocks.begin() + static_cast<long>(i));
    Reader r(raw.data(), raw.size());
    const uint32_t count = r.U32();
    std::map<std::string, std::string> host;
    for (uint32_t k = 0; k < count && r.Ok(); k++)
    {
      const uint32_t key_size = r.U32();
      const uint8_t* key = r.Bytes(key_size);
      const uint32_t value_size = r.U32();
      const uint8_t* value = r.Bytes(value_size);
      if (key && value)
        host[std::string(reinterpret_cast<const char*>(key), key_size)] =
            std::string(reinterpret_cast<const char*>(value), value_size);
    }
    if (!r.Ok())
    {
      EndMatch("The host's settings came damaged");
      return Wait::Failed;
    }
    int differ = 0;
    for (const auto& [key, value] : host)
    {
      auto it = variables->find(key);
      if (it == variables->end() || it->second != value)
        differ++;
    }
    LogF("netplay: the host's core options taken (%zu, %d differ from ours)", host.size(), differ);
    *variables = host;
    S.variables = host;
    S.variables_agreed = true;
    S.match = Match::Sync;
    return Wait::Done;
  }
  return Wait::Waiting;
}

void KeepVariables(std::map<std::string, std::string>* variables, bool* changed)
{
  if (!Active() || !S.variables_agreed || *variables == S.variables)
    return;
  *variables = S.variables;
  *changed = true;
}

void GameLoaded(const std::string& console_key, double fps, ApplyInputs apply)
{
  if (!Active())
    return;
  S.rules = RulesFor(console_key);
  S.max_rollback = S.config.rollback >= 0 ? S.config.rollback : S.rules.max_rollback;
  S.fps = fps > 1.0 ? fps : 60.0;
  S.core_ms = 0.0;  // (00.92: this game's core measured anew - RollbackReach)
  S.apply = apply;
  S.devices_locked = true;  // (the devices the game started with are the match's)
  if (!S.host)
  {
    S.match = Match::Sync;
    LogLine("netplay: the game loaded: waiting for the host's state");
    return;
  }
  size_t size = 0;
  if (!SerializeTo(&S.scratch, &size))
  {
    EndMatch("This game's state can't be saved: it can't be played online");
    return;
  }
  std::vector<uint8_t> raw;
  MakeStatePacket(0, S.scratch.data(), size, true, &raw);
  for (Peer& p : S.peers)
  {
    if (p.present)
      SendBulk(p, BulkKind::State, raw);
  }
  S.match = Match::Sync;
}

void SetLocalInputs(const NetProto::PortInput* inputs, int count)
{
  S.local_count = std::clamp(count, 0, MAX_PORTS);
  if (LinkRoom())
    S.local_count = std::min(S.local_count, 1);  // (00.95: a Game Boy each - the host's port 0, the guest's port 1)
  for (int j = 0; j < MAX_PORTS; j++)
    S.local_now[j] = j < S.local_count ? inputs[j] : PortInput();
}

int FramesThisPass(int frames)
{
  Pump();
  TakeBlocks();
  if (S.over || S.match != Match::Running)
    return 0;
  TickSecond();
  if (frames > 0)
    S.second.passes++;
  // Time sync: a console ahead of the other waits a frame now and then (every 60 frames it decides).
  double my_adv = 0.0, their_adv = 0.0;
  bool any = false;
  for (Peer& p : S.peers)
  {
    if (!p.present)
      continue;
    p.my_adv_avg = p.my_adv_avg * 0.9 + Advantage(p) * 0.1;
    p.their_adv_avg = p.their_adv_avg * 0.9 + p.remote_adv * 0.1;
    if (!any || p.my_adv_avg - p.their_adv_avg > my_adv - their_adv)
    {
      my_adv = p.my_adv_avg;
      their_adv = p.their_adv_avg;
    }
    any = true;
  }
  if (S.skip_passes > 0)
  {
    S.skip_passes--;
    S.stats.waits++;
    S.second.waits++;
    return 0;
  }
  if (any && S.frame - S.last_sync_frame >= 60)
  {
    S.last_sync_frame = S.frame;
    const double wait = (my_adv - their_adv) * 0.5;
    if (wait >= 1.0)
    {
      S.skip_passes = std::min(static_cast<int>(std::lround(wait)), 4) - 1;
      S.stats.waits++;
      S.second.waits++;
      return 0;
    }
  }
  // The frames the other consoles' inputs allow: up to max_rollback past the newest one in.
  int32_t latest = RemoteLatest();
  const int reach = RollbackReach();
  auto allowed = [&] {
    return latest == NO_FRAME ? frames : static_cast<int>(latest + reach + 1 - S.frame);
  };
  int can = allowed();
  // (00.92) An input about to come: the pass waits for it a moment - what the core leaves of the refresh, 6 ms at
  // most - instead of a whole refresh: its frame runs now, not a pass later (each of those is a frame the match lost).
  if (frames > 0 && can <= 0 && latest != NO_FRAME && s_fd >= 0)
  {
    const double until = Now() + std::clamp(10.0 - 1.5 * S.core_ms, 0.0, 6.0) / 1000.0;
    for (double left = until - Now(); can <= 0 && left > 0.0; left = until - Now())
    {
      pollfd pfd;
      pfd.fd = s_fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      poll(&pfd, 1, std::max(1, static_cast<int>(left * 1000.0)));
      Receive();
#ifndef __PS4__
      NetsimFlush(false);
#endif
      latest = RemoteLatest();
      can = allowed();
    }
    S.second.caught += can > 0 ? 1 : 0;
  }
  const int run = std::clamp(std::min(frames, can), 0, 8);
  if (frames > 0 && run == 0)
  {
    if (S.stalled_since < 0.0)
      S.stalled_since = Now();
    S.stats.stalls++;
    S.second.stalls++;
    // (Whose input: the member with the oldest.)
    int whose = -1;
    for (int m = 0; m < MAX_MEMBERS; m++)
    {
      if (m != S.my_id && S.peers[m].present && (whose < 0 || S.inputs[m].latest < S.inputs[whose].latest))
        whose = m;
    }
    if (whose >= 0)
      S.second.waited_for[whose]++;
  }
  else if (run > 0)
  {
    S.stalled_since = -1.0;
  }
  return run;
}

void RunFrame()
{
  if (S.match != Match::Running)
    return;
  S.second.frames++;
  for (int m = 0; m < MAX_MEMBERS; m++)
  {
    if (m != S.my_id && S.peers[m].present)
      S.second.margin[m] = std::min(S.second.margin[m], S.inputs[m].latest - S.frame);
  }
  AddLocalInputs();
  SendInputsAll();
  if (S.rollback_to < S.frame)
    Rollback();
  if (S.max_rollback > 0)
    SaveRing(S.frame);
  DoChecks();
  Reload(S.frame);
  Apply(S.frame);
  RunCore();
  S.frame++;
  S.stats.frame = S.frame;
}

bool Replaying()
{
  return S.replaying;
}

bool SkipVideo()
{
  return S.replaying && !S.rules.replay_video;
}

bool Waiting()
{
  return Active() && S.match != Match::Running;
}

bool Ended(std::string* why)
{
  if (!S.over)
    return false;
  *why = S.to_room ? std::string() : S.error;
  return true;
}

void GameEnded()
{
  if (S.phase == Phase::Off)
    return;
  // A Choose Together match ended so (End Match here or there), the other still in the room: back to the room.
  bool other = false;
  for (const Peer& p : S.peers)
    other = other || p.present;
  if (S.together && S.to_room && other && !S.peer_left)
  {
    BackToRoom();
    return;
  }
  const std::string error = S.error;
  LogF("netplay: the match ended at frame %d: %lld rollbacks (%lld frames run again), %lld waits for input, %lld for "
       "time sync, %lld checksums (%lld differed, %lld resyncs), %lld packets in, %lld out",
       S.frame, static_cast<long long>(S.stats.rollbacks), static_cast<long long>(S.stats.replayed),
       static_cast<long long>(S.stats.stalls), static_cast<long long>(S.stats.waits),
       static_cast<long long>(S.stats.checks), static_cast<long long>(S.stats.mismatches),
       static_cast<long long>(S.stats.resyncs), static_cast<long long>(S.stats.packets_in),
       static_cast<long long>(S.stats.packets_out));
  if (!S.over)
  {
    for (Peer& p : S.peers)
    {
      if (p.present)
        SendQuitNow(p, 0);
    }
  }
  CloseAll("");
  S.error = error;
}

bool KeepOwnSaves()
{
  return Active() && !S.host;
}

void SetVoiceOn(bool on)
{
  S.voice_enabled = on;
  ApplyVoice();
  UpdateStatus();
}

void SetPeerMuted(int member, bool muted)
{
  Peer* p = FindPeer(member);
  if (!p || p->muted == muted)
    return;
  p->muted = muted;
  Voice::SetPeerMuted(member, muted);
  LogF("netplay: %s %s", p->info.nick.c_str(), muted ? "muted" : "heard again");
  UpdateStatus();
}

Status GetStatus()
{
  std::lock_guard lock(s_status_mutex);
  return s_status;
}

Stats GetStats()
{
  Stats st = S.stats;
  st.frame = S.frame;
  st.relay_index = S.relay_choice;
  for (const Peer& p : S.peers)
    st.relay = st.relay || (p.present && !p.direct_ok);
  return st;
}
}  // namespace Netplay
