// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// emusuni-netd (00.23): online play's server. The main one is the rendezvous: consoles meet in a room by a 6-letter
// code or through its public list, learn each other's addresses (as it sees them, and as each console sees itself on
// its LAN) to punch a hole through their routers, and - when no hole can be punched, or a player hides their address -
// send their packets through a relay. Every server relays; others (--relay-only, in other regions) only relay, and the
// consoles of a room pick the one nearest both. The main server hands each member a ticket signed with the servers'
// shared secret (--secret-file), which the relays check: a relay passes packets only between members of one room,
// never for anyone else (no open relay).
//
// Abuse: a request bigger answers need a cookie first (Hello -> Cookie, never bigger than the Hello: a spoofed source
// gets nothing bigger than it sent); per-address rate limits; packets of at most NetProto::MAX_PACKET bytes; relayed
// data capped per member and per room; rooms expire. It keeps no game data and writes no addresses or codes anywhere:
// the log (stdout: journald under systemd) has counts and the consoles of the rooms.
//
// One UDP port (7777 by default), IPv4, one thread, no library beyond the C++17 standard one and Linux's sockets.
// About 2 KiB per room: thousands fit in a small VM. The packets are src/NetplayProtocol.h's.
//
//   g++ -std=c++17 -O2 -Wall -o emusuni-netd emusuni-netd.cpp
//   ./emusuni-netd [--port 7777] [--bind 0.0.0.0] [--secret-file FILE] [--relays FILE] [--name main]
//                  [--region EU] [--relay-only] [--max-rooms 5000] [--log-every 600] [--verbose]
// --relays FILE (the main server): one relay a line, "name region host:port" (# comments); the consoles get the list
// with ServerInfo, the main server first. Adding a region: a relay server with the same secret and a line here.
// --stats-dir DIR (the main server): the anonymous statistics, kept in DIR (counts and the country of an address, never
// the address; server/emusuni-netd-stats.h), --geoip FILE its country database (DB-IP's IP to Country Lite CSV).
//   ./emusuni-netd report [--stats-dir DIR] [--days 30] [--json]
//   curl -s https://api.github.com/repos/OWNER/REPO/releases?per_page=100 | ./emusuni-netd record-downloads [--stats-dir DIR]

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <tuple>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>  // (the statistics' store: the system's libsqlite3 - link with -lsqlite3)

#include "../src/NetplayProtocol.h"

namespace
{
using namespace NetProto;
using Clock = std::chrono::steady_clock;

constexpr double MEMBER_TIMEOUT = 30.0;  // s without a packet: the member is gone
constexpr double ROOM_LIFETIME = 12 * 3600.0;
constexpr double BINDING_TIMEOUT = 60.0;  // a relay's member silent this long is forgotten
constexpr uint32_t TICKET_LIFETIME = 24 * 3600;
// Per address.
constexpr double PACKETS_RATE = 400.0, PACKETS_BURST = 800.0;
constexpr double HELLOS_RATE = 5.0, HELLOS_BURST = 20.0;
constexpr double ROOMS_RATE = 20.0 / 60.0, ROOMS_BURST = 10.0;  // Create and Join: codes can't be guessed by trying
constexpr double LISTS_RATE = 1.0, LISTS_BURST = 6.0;
// Relayed bytes.
constexpr double RELAY_MEMBER_RATE = 384.0 * 1024, RELAY_MEMBER_BURST = 4.0 * 1024 * 1024;
constexpr double RELAY_ROOM_RATE = 768.0 * 1024, RELAY_ROOM_BURST = 8.0 * 1024 * 1024;
constexpr int ROOMS_PER_ADDRESS = 4;
constexpr double REPLY_MEMORY = 30.0;  // s: a Create or Join sent again (its answer lost) gets the same answer

bool s_verbose = false;
bool s_relay_only = false;
volatile std::sig_atomic_t s_stop = 0;

double Now()
{
  static const auto start = Clock::now();
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint64_t Random64()
{
  uint64_t value = 0;
  if (getrandom(&value, sizeof(value), 0) == static_cast<ssize_t>(sizeof(value)))
    return value;
  static std::mt19937_64 fallback(std::random_device{}());
  return fallback();
}

// ---- SHA-256 and HMAC (FIPS 180-4, RFC 2104): the cookies and the relay tickets ----
struct Sha256
{
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block[64] = {};
  size_t used = 0;
  uint64_t bits = 0;

  static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Compress()
  {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) |
             block[i * 4 + 3];
    for (int i = 16; i < 64; i++)
    {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++)
    {
      const uint32_t t1 = k + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      k = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
  }
  void Add(const void* data, size_t n)
  {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits += static_cast<uint64_t>(n) * 8;
    while (n > 0)
    {
      const size_t take = std::min(n, sizeof(block) - used);
      std::memcpy(block + used, p, take);
      used += take;
      p += take;
      n -= take;
      if (used == sizeof(block))
      {
        Compress();
        used = 0;
      }
    }
  }
  void Finish(uint8_t out[32])
  {
    const uint64_t total = bits;
    const uint8_t one = 0x80, zero = 0;
    Add(&one, 1);
    while (used != 56)
      Add(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; i++)
      length[i] = static_cast<uint8_t>(total >> (56 - i * 8));
    Add(length, 8);
    for (int i = 0; i < 8; i++)
    {
      out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
  }
};

std::vector<uint8_t> s_secret;

void Hmac(const void* data, size_t n, uint8_t out[32])
{
  uint8_t key[64] = {};
  if (s_secret.size() > 64)
  {
    Sha256 k;
    k.Add(s_secret.data(), s_secret.size());
    k.Finish(key);
  }
  else
  {
    std::memcpy(key, s_secret.data(), s_secret.size());
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++)
  {
    ipad[i] = key[i] ^ 0x36;
    opad[i] = key[i] ^ 0x5c;
  }
  uint8_t inner[32];
  Sha256 a;
  a.Add(ipad, 64);
  a.Add(data, n);
  a.Finish(inner);
  Sha256 b;
  b.Add(opad, 64);
  b.Add(inner, 32);
  b.Finish(out);
}

bool SameMac(const uint8_t* a, const uint8_t* b, size_t n)
{
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++)
    diff |= a[i] ^ b[i];
  return diff == 0;
}

// The cookie of an address for a minute: a request with this minute's or one of the two before it is from an address
// that got the Cookie (not a spoofed one).
void MakeCookie(const sockaddr_in& from, uint32_t minute, uint8_t out[COOKIE_SIZE])
{
  uint8_t data[16] = {'c', 'o', 'o', 'k'};
  std::memcpy(data + 4, &from.sin_addr.s_addr, 4);
  std::memcpy(data + 8, &from.sin_port, 2);
  std::memcpy(data + 10, &minute, 4);
  uint8_t mac[32];
  Hmac(data, 14, mac);
  std::memcpy(out, mac, COOKIE_SIZE);
}

uint32_t Minute()
{
  return static_cast<uint32_t>(std::time(nullptr) / 60);
}

bool CookieOk(const sockaddr_in& from, const uint8_t* cookie)
{
  if (!cookie)
    return false;
  const uint32_t now = Minute();
  for (uint32_t m = now; m + 3 > now; m--)
  {
    uint8_t expect[COOKIE_SIZE];
    MakeCookie(from, m, expect);
    if (SameMac(expect, cookie, COOKIE_SIZE))
      return true;
  }
  return false;
}

void SignTicket(Ticket* t)
{
  uint8_t data[24] = {'t', 'i', 'c', 'k'};
  std::memcpy(data + 4, &t->room, 8);
  data[12] = t->member;
  std::memcpy(data + 13, &t->expiry, 4);
  uint8_t mac[32];
  Hmac(data, 17, mac);
  std::memcpy(t->mac, mac, MAC_SIZE);
}

bool TicketOk(const Ticket& t)
{
  Ticket copy = t;
  SignTicket(&copy);
  return SameMac(copy.mac, t.mac, MAC_SIZE) && t.expiry >= static_cast<uint32_t>(std::time(nullptr));
}

// A token bucket: `rate` a second, at most `burst` saved.
struct Bucket
{
  double tokens = -1.0, last = 0.0;
  bool Take(double now, double rate, double burst, double amount = 1.0)
  {
    if (tokens < 0.0)
    {
      tokens = burst;
      last = now;
    }
    tokens = std::min(burst, tokens + (now - last) * rate);
    last = now;
    if (tokens < amount)
      return false;
    tokens -= amount;
    return true;
  }
};

struct AddressState
{
  Bucket packets, hellos, rooms, lists;
  Bucket stats, new_ids, profiles;  // (the anonymous statistics' heartbeats, and the installs new today they bring)
  double last_seen = 0.0;
};

struct Slot
{
  bool used = false;
  Member info;
  sockaddr_in addr{};
  uint64_t token = 0;
  double last_seen = 0.0;
  Bucket relay;
  uint16_t rtt_ms = 0;
};

struct Room
{
  std::string code;
  uint64_t key = 0, relay_id = 0;
  uint32_t list_id = 0;  // (00.85) its number in ListAll's list: neither its code nor its key
  GameInfo game;
  std::string app_version;
  uint8_t max_members = 2;
  bool is_public = false, locked = false;
  Slot members[MAX_MEMBERS];
  uint32_t seq = 0;
  double created = 0.0;
  uint32_t creator_ip = 0;
  std::unordered_set<uint32_t> kicked;  // addresses the host removed: they can't join again
  Bucket relay;
  int Count() const
  {
    int n = 0;
    for (const Slot& s : members)
      n += s.used ? 1 : 0;
    return n;
  }
};

// A relay's member of a room: where it is, the session it was given.
struct Binding
{
  sockaddr_in addr{};
  uint64_t session = 0;
  double last_seen = 0.0;
  Bucket relay;
};

struct Reply
{
  std::vector<uint8_t> packet;
  double at = 0.0;
};

struct RelayEntry
{
  std::string name, region;
  Address address;
};

int s_socket = -1;
std::unordered_map<std::string, Room> s_rooms;
std::unordered_map<uint32_t, AddressState> s_addresses;
std::map<std::pair<uint64_t, uint32_t>, Reply> s_replies;  // (address and port, nonce) -> the answer
std::map<std::pair<uint64_t, uint8_t>, Binding> s_bindings;  // (room's relay id, member) -> where it is
std::unordered_map<uint64_t, Bucket> s_room_relay;  // a relay's rooms' relayed bytes
std::vector<RelayEntry> s_relays;  // ServerInfo's list after this server
std::string s_name = "main", s_region = "";
size_t s_max_rooms = 5000;
struct Counts
{
  uint64_t packets = 0, dropped = 0, relayed = 0, relayed_bytes = 0, created = 0, joined = 0, refused = 0, closed = 0,
           kicked = 0, lists = 0;
} s_counts;

#include "emusuni-netd-stats.h"

Address ToAddress(const sockaddr_in& a)
{
  Address out;
  out.ip = ntohl(a.sin_addr.s_addr);
  out.port = ntohs(a.sin_port);
  return out;
}

uint64_t AddressKey(const sockaddr_in& a)
{
  return (static_cast<uint64_t>(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port);
}

bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
{
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void Send(const sockaddr_in& to, const uint8_t* data, size_t size)
{
  sendto(s_socket, data, size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Refused, written in the asker's version (its fields stay the same in every version): never bigger than the request.
void SendRefused(const sockaddr_in& to, uint8_t version, uint32_t nonce, Refusal reason, const std::string& detail,
                 size_t request_size)
{
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Refused, version);
  w.U32(nonce);
  w.U8(static_cast<uint8_t>(reason));
  w.Str(detail, 24);
  if (w.Size() <= request_size)
    Send(to, buf, w.Size());
  s_counts.refused++;
}

void SendMembers(const Room& room, const Slot& to)
{
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Members);
  WriteCode(w, room.code);
  w.U32(room.seq);
  w.U64(room.key);
  WriteGame(w, room.game);
  w.Str(room.app_version, MAX_APP_VERSION);
  w.U8(room.max_members);
  w.U8(room.is_public ? FLAG_PUBLIC : 0);
  w.U8(to.info.id);
  w.U8(static_cast<uint8_t>(room.Count()));
  for (const Slot& s : room.members)
  {
    if (!s.used)
      continue;
    // (A member that hides its addresses: the others get none, the relay carries everything.)
    const bool hide = (s.info.flags & FLAG_HIDDEN) && s.info.id != to.info.id;
    w.U8(s.info.id);
    w.Str(s.info.nick, MAX_NICK);
    WriteAddress(w, hide ? Address() : s.info.pub);
    WriteAddress(w, hide ? Address() : s.info.lan);
    w.U8(s.info.locals);
    w.U8(s.info.flags);
    w.Str(s.info.region, MAX_REGION);
  }
  if (w.Ok())
    Send(to.addr, buf, w.Size());
}

void SendMembersToAll(Room& room)
{
  room.seq++;
  for (const Slot& s : room.members)
  {
    if (s.used)
      SendMembers(room, s);
  }
}

void SendClosed(const Room& room, const Slot& to, CloseReason reason)
{
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Closed);
  WriteCode(w, room.code);
  w.U8(static_cast<uint8_t>(reason));
  Send(to.addr, buf, w.Size());
}

std::string NewCode()
{
  for (;;)
  {
    std::string code;
    uint64_t r = Random64();
    for (int i = 0; i < CODE_LENGTH; i++)
    {
      code += CODE_ALPHABET[r % (sizeof(CODE_ALPHABET) - 1)];
      r /= sizeof(CODE_ALPHABET) - 1;
    }
    if (!s_rooms.count(code))
      return code;
  }
}

int RoomsOf(uint32_t ip)
{
  int n = 0;
  for (const auto& [code, room] : s_rooms)
    n += room.creator_ip == ip ? 1 : 0;
  return n;
}

void CloseRoom(const std::string& code, CloseReason reason)
{
  auto it = s_rooms.find(code);
  if (it == s_rooms.end())
    return;
  for (const Slot& s : it->second.members)
  {
    if (s.used)
      SendClosed(it->second, s, reason);
  }
  s_counts.closed++;
  if (s_verbose)
    std::printf("room closed (%s, %s); %zu rooms\n", it->second.game.console.c_str(),
                reason == CloseReason::HostLeft ? "the host left" : "silent or too old", s_rooms.size() - 1);
  s_rooms.erase(it);
}

Ticket MakeTicket(const Room& room, uint8_t member)
{
  Ticket t;
  t.room = room.relay_id;
  t.member = member;
  t.expiry = static_cast<uint32_t>(std::time(nullptr)) + TICKET_LIFETIME;
  SignTicket(&t);
  return t;
}

// A member's own packet: its room, its place and its token (else nullptr: dropped). Its address is the one it sends
// from now (a router may change it).
Slot* Authenticate(Reader& r, const sockaddr_in& from, Room** room_out)
{
  const std::string code = ReadCode(r);
  const uint8_t id = r.U8();
  const uint64_t token = r.U64();
  if (!r.Ok() || id >= MAX_MEMBERS)
    return nullptr;
  auto it = s_rooms.find(code);
  if (it == s_rooms.end())
    return nullptr;
  Slot& slot = it->second.members[id];
  if (!slot.used || slot.token != token)
    return nullptr;
  slot.last_seen = Now();
  if (!SameAddress(from, slot.addr))
  {
    slot.info.pub = ToAddress(from);
    slot.addr = from;
    SendMembersToAll(it->second);  // (the others punch to the new address)
  }
  *room_out = &it->second;
  return &slot;
}

void OnHello(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  const uint32_t nonce = r.U32();
  if (!r.Ok() || size < HELLO_SIZE || !state.hellos.Take(Now(), HELLOS_RATE, HELLOS_BURST))
    return;
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Cookie);
  w.U32(nonce);
  uint8_t cookie[COOKIE_SIZE];
  MakeCookie(from, Minute(), cookie);
  w.Bytes(cookie, COOKIE_SIZE);
  WriteAddress(w, ToAddress(from));
  if (w.Size() <= size)
    Send(from, buf, w.Size());
}

// The cookie of a request (else it is refused with a small answer: Hello again).
bool CheckCookie(Reader& r, const sockaddr_in& from, size_t size, uint32_t* nonce)
{
  const uint8_t* cookie = r.Bytes(COOKIE_SIZE);
  *nonce = r.U32();
  if (!r.Ok())
    return false;
  if (CookieOk(from, cookie))
    return true;
  SendRefused(from, VERSION, *nonce, Refusal::BadCookie, "", size);
  return false;
}

void OnInfo(Reader& r, const sockaddr_in& from, size_t size)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::ServerInfo);
  w.U32(nonce);
  const int count = static_cast<int>(std::min<size_t>(s_relays.size() + 1, MAX_RELAYS));
  w.U8(static_cast<uint8_t>(count));
  // This server first (index 0, no address: the one asked).
  w.U8(0);
  w.Str(s_name, MAX_RELAY_NAME);
  w.Str(s_region, MAX_REGION);
  WriteAddress(w, Address());
  for (int i = 1; i < count; i++)
  {
    const RelayEntry& e = s_relays[static_cast<size_t>(i - 1)];
    w.U8(static_cast<uint8_t>(i));
    w.Str(e.name, MAX_RELAY_NAME);
    w.Str(e.region, MAX_REGION);
    WriteAddress(w, e.address);
  }
  if (w.Ok())
    Send(from, buf, w.Size());
}

// The anonymous statistics' heartbeat (the main server with --stats-dir): counted, answered with the players online.
void OnStats(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  if (!s_stats.on || size < STATS_SIZE || !state.stats.Take(Now(), STATS_RATE, STATS_BURST))
  {
    s_counts.dropped++;
    return;
  }
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const Heartbeat beat = ReadHeartbeat(r);
  if (!r.Ok())
    return;
  const ProfileReply profile = StatsRecord(beat, ntohl(from.sin_addr.s_addr), state);
  uint8_t buf[64];
  Writer w(buf, sizeof(buf));
  w.Header(Type::StatsAck);
  w.U32(nonce);
  w.U32(StatsOnline());
  w.U16(HEARTBEAT_EVERY);
  w.U8(static_cast<uint8_t>(profile.state));
  w.Str(profile.code, PROFILE_CODE_SIZE);
  if (w.Ok() && w.Size() <= size)
    Send(from, buf, w.Size());
}

void OnCreate(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const std::string app_version = r.Str(MAX_APP_VERSION);
  GameInfo game = ReadGame(r);
  game.title = CleanText(game.title, 80, "?");  // (strangers read it in the public list)
  Member me;
  me.nick = CleanNick(r.Str(MAX_NICK));
  me.lan = ReadAddress(r);
  me.locals = r.U8();
  const uint8_t max_members = r.U8();
  const uint8_t flags = r.U8();
  me.region = CleanText(r.Str(MAX_REGION), MAX_REGION, "");
  if (!r.Ok() || game.console.empty())
  {
    SendRefused(from, VERSION, nonce, Refusal::BadRequest, "", size);
    return;
  }
  const auto key = std::make_pair(AddressKey(from), nonce);
  if (auto it = s_replies.find(key); it != s_replies.end())
  {
    Send(from, it->second.packet.data(), it->second.packet.size());
    return;
  }
  const double now = Now();
  if (!state.rooms.Take(now, ROOMS_RATE, ROOMS_BURST))
  {
    SendRefused(from, VERSION, nonce, Refusal::TooMany, "", size);
    return;
  }
  const uint32_t ip = ntohl(from.sin_addr.s_addr);
  if (s_rooms.size() >= s_max_rooms || RoomsOf(ip) >= ROOMS_PER_ADDRESS)
  {
    SendRefused(from, VERSION, nonce, Refusal::ServerFull, "", size);
    return;
  }
  Room room;
  room.code = NewCode();
  room.key = Random64();
  room.relay_id = Random64();
  room.list_id = static_cast<uint32_t>(Random64() | 1);
  room.game = game;
  room.app_version = app_version;
  room.max_members = static_cast<uint8_t>(std::clamp<int>(max_members, 2, MAX_MEMBERS));
  room.is_public = (flags & FLAG_PUBLIC) != 0;
  room.created = now;
  room.creator_ip = ip;
  Slot& host = room.members[0];
  host.used = true;
  host.info = me;
  host.info.id = 0;
  host.info.flags = flags & FLAG_HIDDEN;
  host.info.locals = static_cast<uint8_t>(std::clamp<int>(me.locals, 1, MAX_PORTS));
  host.info.pub = ToAddress(from);
  host.addr = from;
  host.token = Random64();
  host.last_seen = now;

  uint8_t buf[128];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Created);
  w.U32(nonce);
  WriteCode(w, room.code);
  w.U8(0);
  w.U64(host.token);
  WriteAddress(w, host.info.pub);
  WriteTicket(w, MakeTicket(room, 0));
  s_replies[key] = {std::vector<uint8_t>(buf, buf + w.Size()), now};
  Send(from, buf, w.Size());
  const std::string code = room.code;
  s_rooms.emplace(code, std::move(room));
  SendMembersToAll(s_rooms[code]);
  s_counts.created++;
  if (s_verbose)
    std::printf("room created (%s, %s); %zu rooms\n", game.console.c_str(), (flags & FLAG_PUBLIC) ? "public" : "private",
                s_rooms.size());
}

void OnJoin(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const std::string code = NormalizeCode(ReadCode(r));
  const std::string app_version = r.Str(MAX_APP_VERSION);
  Member me;
  me.nick = CleanNick(r.Str(MAX_NICK));
  me.lan = ReadAddress(r);
  me.locals = r.U8();
  const uint8_t flags = r.U8();
  me.region = CleanText(r.Str(MAX_REGION), MAX_REGION, "");
  if (!r.Ok())
  {
    SendRefused(from, VERSION, nonce, Refusal::BadRequest, "", size);
    return;
  }
  const auto key = std::make_pair(AddressKey(from), nonce);
  if (auto it = s_replies.find(key); it != s_replies.end())
  {
    Send(from, it->second.packet.data(), it->second.packet.size());
    return;
  }
  const double now = Now();
  // (Every Join counts, a wrong code too: codes can't be tried one after another.)
  if (!state.rooms.Take(now, ROOMS_RATE, ROOMS_BURST))
  {
    SendRefused(from, VERSION, nonce, Refusal::TooMany, "", size);
    return;
  }
  auto it = s_rooms.find(code);
  if (it == s_rooms.end())
  {
    SendRefused(from, VERSION, nonce, Refusal::NoRoom, "", size);
    return;
  }
  Room& room = it->second;
  if (room.app_version != app_version)
  {
    SendRefused(from, VERSION, nonce, Refusal::Version, room.app_version, size);
    return;
  }
  if (room.kicked.count(ntohl(from.sin_addr.s_addr)))
  {
    SendRefused(from, VERSION, nonce, Refusal::Kicked, "", size);
    return;
  }
  if (room.locked)
  {
    SendRefused(from, VERSION, nonce, Refusal::Started, "", size);
    return;
  }
  int free_id = -1;
  for (int i = 0; i < MAX_MEMBERS && free_id < 0; i++)
  {
    if (!room.members[i].used)
      free_id = i;
  }
  if (room.Count() >= room.max_members || free_id < 0)
  {
    SendRefused(from, VERSION, nonce, Refusal::Full, "", size);
    return;
  }
  Slot& slot = room.members[free_id];
  slot = Slot();
  slot.used = true;
  slot.info = me;
  slot.info.id = static_cast<uint8_t>(free_id);
  slot.info.flags = flags & FLAG_HIDDEN;
  slot.info.locals = static_cast<uint8_t>(std::clamp<int>(me.locals, 1, MAX_PORTS));
  slot.info.pub = ToAddress(from);
  slot.addr = from;
  slot.token = Random64();
  slot.last_seen = now;

  uint8_t buf[128];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Joined);
  w.U32(nonce);
  w.U8(slot.info.id);
  w.U64(slot.token);
  WriteAddress(w, slot.info.pub);
  WriteTicket(w, MakeTicket(room, slot.info.id));
  s_replies[key] = {std::vector<uint8_t>(buf, buf + w.Size()), now};
  Send(from, buf, w.Size());
  SendMembersToAll(room);
  s_counts.joined++;
}

void OnList(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const std::string console = r.Str(MAX_CONSOLE);
  const int page = r.U8();
  if (!r.Ok() || !state.lists.Take(Now(), LISTS_RATE, LISTS_BURST))
    return;
  s_counts.lists++;
  // The public rooms waiting for players, newest first (a Choose Together room in every console's list).
  std::vector<const Room*> open;
  for (const auto& [code, room] : s_rooms)
  {
    if (room.is_public && !room.locked && room.Count() < room.max_members &&
        (console.empty() || room.game.console == console || room.game.console == TOGETHER))
      open.push_back(&room);
  }
  std::sort(open.begin(), open.end(), [](const Room* a, const Room* b) { return a->created > b->created; });
  const int pages = std::max(1, static_cast<int>((open.size() + ROOMS_PER_PAGE - 1) / ROOMS_PER_PAGE));
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Rooms);
  w.U32(nonce);
  w.U8(static_cast<uint8_t>(std::min(page, 255)));
  w.U8(static_cast<uint8_t>(std::min(pages, 255)));
  w.U16(static_cast<uint16_t>(std::min<size_t>(open.size(), 65535)));
  const size_t first = static_cast<size_t>(page) * ROOMS_PER_PAGE;
  const size_t count = first < open.size() ? std::min<size_t>(ROOMS_PER_PAGE, open.size() - first) : 0;
  w.U8(static_cast<uint8_t>(count));
  for (size_t i = first; i < first + count; i++)
  {
    const Room& room = *open[i];
    WriteCode(w, room.code);
    WriteGame(w, room.game);
    w.Str(room.members[0].info.nick, MAX_NICK);
    w.Str(room.members[0].info.region, MAX_REGION);
    w.U8(static_cast<uint8_t>(room.Count()));
    w.U8(room.max_members);
    w.U16(room.members[0].rtt_ms);
  }
  if (w.Ok())
    Send(from, buf, w.Size());
}

// (00.85) The Online tab's table: every room - public and private, waiting, full or playing -, the joinable ones first
// (the public rooms waiting for players, the private ones, the full ones, those playing; the newest first in each), as
// many as fit a datagram from the `first` asked for, with how many there are. A private room's code stays out (its
// host tells it to whom they like) and every address too: a stranger learns a game, a nickname, a time zone, a count.
// The cookie first, as List: the answer may be bigger than the request; each page counts in the lists' rate limit.
void OnListAll(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  const std::string console = r.Str(MAX_CONSOLE);
  const size_t first = r.U16();
  if (!r.Ok() || !state.lists.Take(Now(), LISTS_RATE, LISTS_BURST))
    return;
  s_counts.lists++;
  auto rank = [](const Room& room) {
    return room.locked ? 3 : room.Count() >= room.max_members ? 2 : room.is_public ? 0 : 1;
  };
  std::vector<const Room*> rooms;
  for (const auto& [code, room] : s_rooms)
  {
    if (console.empty() || room.game.console == console || room.game.console == TOGETHER)
      rooms.push_back(&room);
  }
  std::sort(rooms.begin(), rooms.end(), [&rank](const Room* a, const Room* b) {
    const int ra = rank(*a), rb = rank(*b);
    if (ra != rb)
      return ra < rb;
    return a->created != b->created ? a->created > b->created : a->list_id < b->list_id;
  });
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::AllRooms);
  w.U32(nonce);
  w.U16(static_cast<uint16_t>(std::min<size_t>(first, 65535)));
  w.U16(static_cast<uint16_t>(std::min<size_t>(rooms.size(), 65535)));
  const size_t count_at = w.Size();
  w.U8(0);
  size_t count = 0;
  for (size_t i = first; i < rooms.size() && count < 255; i++)
  {
    const Room& room = *rooms[i];
    RoomEntry e;
    e.id = room.list_id;
    e.is_public = room.is_public;
    e.playing = room.locked;
    e.code = room.is_public ? room.code : std::string();
    e.game = room.game;
    e.host = room.members[0].info.nick;
    e.region = room.members[0].info.region;
    e.players = static_cast<uint8_t>(room.Count());
    e.max_players = room.max_members;
    e.host_ms = room.members[0].rtt_ms;
    e.app_version = room.app_version;
    uint8_t entry[MAX_PACKET];
    Writer one(entry, sizeof(entry));
    WriteListedRoom(one, e);
    if (!one.Ok() || w.Size() + one.Size() > MAX_PACKET)
      break;  // (the next page: from here)
    w.Bytes(entry, one.Size());
    count++;
  }
  buf[count_at] = static_cast<uint8_t>(count);
  if (w.Ok())
    Send(from, buf, w.Size());
}

void OnKeepalive(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint16_t rtt = r.U16();
  if (!slot)
    return;
  if (r.Ok())
    slot->rtt_ms = rtt;
  SendMembers(*room, *slot);  // (its answer: the room as it is)
}

void OnLeave(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  if (!slot)
    return;
  if (slot->info.id == 0)
  {
    *slot = Slot();
    CloseRoom(room->code, CloseReason::HostLeft);
    return;
  }
  *slot = Slot();
  SendMembersToAll(*room);
}

void OnKick(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t target = r.U8();
  if (!slot || !r.Ok() || slot->info.id != 0 || target == 0 || target >= MAX_MEMBERS || !room->members[target].used)
    return;
  Slot& out = room->members[target];
  room->kicked.insert(ntohl(out.addr.sin_addr.s_addr));
  SendClosed(*room, out, CloseReason::Kicked);
  out = Slot();
  room->key = Random64();  // (a new key for the peer packets: the removed console knew the old one)
  SendMembersToAll(*room);
  s_counts.kicked++;
}

void OnLock(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t locked = r.U8();
  if (slot && r.Ok() && slot->info.id == 0)
    room->locked = locked != 0;
}

void OnRelay(Reader& r, const sockaddr_in& from, size_t packet_size)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t to = r.U8();
  if (!slot || !r.Ok() || to >= MAX_MEMBERS || to == slot->info.id || !room->members[to].used)
    return;
  const size_t size = r.Left();
  if (size < HEADER || size + 5 > MAX_PACKET)
    return;
  const double now = Now();
  if (!slot->relay.Take(now, RELAY_MEMBER_RATE, RELAY_MEMBER_BURST, static_cast<double>(packet_size)) ||
      !room->relay.Take(now, RELAY_ROOM_RATE, RELAY_ROOM_BURST, static_cast<double>(packet_size)))
  {
    s_counts.dropped++;
    return;
  }
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Relayed);
  w.U8(slot->info.id);
  w.Bytes(r.Bytes(size), size);
  if (!w.Ok())
    return;
  Send(room->members[to].addr, buf, w.Size());
  s_counts.relayed++;
  s_counts.relayed_bytes += w.Size();
}

// ---- The relay (every server) ----
void OnRelayPing(Reader& r, const sockaddr_in& from, size_t size)
{
  const uint32_t nonce = r.U32();
  if (!r.Ok() || size < RELAY_PING_SIZE)
    return;
  uint8_t buf[16];
  Writer w(buf, sizeof(buf));
  w.Header(Type::RelayPong);
  w.U32(nonce);
  Send(from, buf, w.Size());
}

void OnRelayOpen(Reader& r, const sockaddr_in& from, size_t size)
{
  const Ticket t = ReadTicket(r);
  if (!r.Ok() || size < RELAY_OPEN_SIZE || t.member >= MAX_MEMBERS || !TicketOk(t))
    return;
  Binding& b = s_bindings[{t.room, t.member}];
  if (b.session == 0 || !SameAddress(b.addr, from))
  {
    b.session = Random64();
    b.addr = from;
  }
  b.last_seen = Now();
  uint8_t buf[32];
  Writer w(buf, sizeof(buf));
  w.Header(Type::RelayOpened);
  w.U64(t.room);
  w.U8(t.member);
  w.U64(b.session);
  Send(from, buf, w.Size());
}

void OnRelayData(Reader& r, const sockaddr_in& from, size_t packet_size)
{
  const uint64_t room = r.U64();
  const uint8_t member = r.U8();
  const uint64_t session = r.U64();
  const uint8_t to = r.U8();
  if (!r.Ok() || to >= MAX_MEMBERS || to == member)
    return;
  auto me = s_bindings.find({room, member});
  if (me == s_bindings.end() || me->second.session != session || !SameAddress(me->second.addr, from))
    return;
  const double now = Now();
  me->second.last_seen = now;
  auto target = s_bindings.find({room, to});
  const size_t size = r.Left();
  if (target == s_bindings.end() || size < HEADER || size + 5 > MAX_PACKET)
    return;
  if (!me->second.relay.Take(now, RELAY_MEMBER_RATE, RELAY_MEMBER_BURST, static_cast<double>(packet_size)) ||
      !s_room_relay[room].Take(now, RELAY_ROOM_RATE, RELAY_ROOM_BURST, static_cast<double>(packet_size)))
  {
    s_counts.dropped++;
    return;
  }
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Relayed);
  w.U8(member);
  w.Bytes(r.Bytes(size), size);
  if (!w.Ok())
    return;
  Send(target->second.addr, buf, w.Size());
  s_counts.relayed++;
  s_counts.relayed_bytes += w.Size();
}

void OnPacket(const uint8_t* data, size_t size, const sockaddr_in& from)
{
  s_counts.packets++;
  uint8_t version = 0;
  if (size > MAX_PACKET || !IsOurs(data, size, &version))
  {
    s_counts.dropped++;
    return;
  }
  const uint32_t ip = ntohl(from.sin_addr.s_addr);
  const double now = Now();
  AddressState& state = s_addresses[ip];
  state.last_seen = now;
  if (!state.packets.Take(now, PACKETS_RATE, PACKETS_BURST))
  {
    s_counts.dropped++;
    return;
  }
  const Type type = static_cast<Type>(data[3]);
  if (version != VERSION)
  {
    // Another version's Hello: told so (its nonce is where every version keeps it; the answer is smaller).
    if (type == Type::Hello && size >= HEADER + 4 && state.hellos.Take(now, HELLOS_RATE, HELLOS_BURST))
    {
      Reader r(data + HEADER, size - HEADER);
      SendRefused(from, version, r.U32(), Refusal::Version, "protocol " + std::to_string(VERSION), size);
    }
    return;
  }
  Reader r(data + HEADER, size - HEADER);
  if (s_relay_only && type != Type::RelayPing && type != Type::RelayOpen && type != Type::RelayData)
  {
    s_counts.dropped++;
    return;
  }
  switch (type)
  {
  case Type::Hello:
    OnHello(r, from, size, state);
    break;
  case Type::Info:
    OnInfo(r, from, size);
    break;
  case Type::Create:
    OnCreate(r, from, size, state);
    break;
  case Type::Join:
    OnJoin(r, from, size, state);
    break;
  case Type::List:
    OnList(r, from, size, state);
    break;
  case Type::ListAll:
    OnListAll(r, from, size, state);
    break;
  case Type::Keepalive:
    OnKeepalive(r, from);
    break;
  case Type::Leave:
    OnLeave(r, from);
    break;
  case Type::Kick:
    OnKick(r, from);
    break;
  case Type::Lock:
    OnLock(r, from);
    break;
  case Type::Stats:
    OnStats(r, from, size, state);
    break;
  case Type::Relay:
    OnRelay(r, from, size);
    break;
  case Type::RelayPing:
    OnRelayPing(r, from, size);
    break;
  case Type::RelayOpen:
    OnRelayOpen(r, from, size);
    break;
  case Type::RelayData:
    OnRelayData(r, from, size);
    break;
  default:
    s_counts.dropped++;  // (a peer packet sent to the server by mistake, a server's own type)
    break;
  }
}

void Housekeeping()
{
  const double now = Now();
  std::vector<std::pair<std::string, CloseReason>> close;
  for (auto& [code, room] : s_rooms)
  {
    if (now - room.created > ROOM_LIFETIME)
    {
      close.push_back({code, CloseReason::Expired});
      continue;
    }
    bool changed = false;
    for (Slot& s : room.members)
    {
      if (s.used && now - s.last_seen > MEMBER_TIMEOUT)
      {
        if (s.info.id == 0)
        {
          close.push_back({code, CloseReason::Expired});
          break;
        }
        SendClosed(room, s, CloseReason::Removed);
        s = Slot();
        changed = true;
      }
    }
    if (changed)
      SendMembersToAll(room);
  }
  for (const auto& [code, reason] : close)
    CloseRoom(code, reason);
  for (auto it = s_bindings.begin(); it != s_bindings.end();)
    it = now - it->second.last_seen > BINDING_TIMEOUT ? s_bindings.erase(it) : std::next(it);
  for (auto it = s_room_relay.begin(); it != s_room_relay.end();)
    it = now - it->second.last > BINDING_TIMEOUT ? s_room_relay.erase(it) : std::next(it);
  for (auto it = s_addresses.begin(); it != s_addresses.end();)
    it = now - it->second.last_seen > 600.0 ? s_addresses.erase(it) : std::next(it);
  for (auto it = s_replies.begin(); it != s_replies.end();)
    it = now - it->second.at > REPLY_MEMORY ? s_replies.erase(it) : std::next(it);
}

void LogCounts()
{
  size_t members = 0, open = 0;
  std::map<std::string, int> consoles;
  for (const auto& [code, room] : s_rooms)
  {
    members += static_cast<size_t>(room.Count());
    open += room.is_public ? 1 : 0;
    consoles[room.game.console]++;
  }
  std::string by;
  for (const auto& [console, n] : consoles)
    by += " " + console + ":" + std::to_string(n);
  std::printf("counts: %zu rooms (%zu public), %zu consoles, %zu relay members;%s; since the last line %llu packets "
              "(%llu dropped), %llu relayed (%llu KiB), %llu rooms made, %llu joined, %llu refused, %llu closed, %llu "
              "kicked, %llu lists%s\n",
              s_rooms.size(), open, members, s_bindings.size(), by.c_str(),
              static_cast<unsigned long long>(s_counts.packets), static_cast<unsigned long long>(s_counts.dropped),
              static_cast<unsigned long long>(s_counts.relayed),
              static_cast<unsigned long long>(s_counts.relayed_bytes >> 10),
              static_cast<unsigned long long>(s_counts.created), static_cast<unsigned long long>(s_counts.joined),
              static_cast<unsigned long long>(s_counts.refused), static_cast<unsigned long long>(s_counts.closed),
              static_cast<unsigned long long>(s_counts.kicked), static_cast<unsigned long long>(s_counts.lists),
              StatsLogText().c_str());
  s_counts = Counts();
}

bool LoadSecret(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  if (text.size() < 16)
    return false;
  s_secret.assign(text.begin(), text.end());
  return true;
}

bool LoadRelays(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream words(line);
    RelayEntry e;
    std::string where;
    if (!(words >> e.name >> e.region >> where))
      continue;
    const size_t colon = where.rfind(':');
    const std::string host = colon == std::string::npos ? where : where.substr(0, colon);
    const int port = colon == std::string::npos ? DEFAULT_PORT : std::atoi(where.c_str() + colon + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
    {
      std::fprintf(stderr, "relay %s: %s can't be looked up\n", e.name.c_str(), host.c_str());
      continue;
    }
    e.address = ToAddress(*reinterpret_cast<const sockaddr_in*>(found->ai_addr));
    e.address.port = static_cast<uint16_t>(port);
    freeaddrinfo(found);
    s_relays.push_back(e);
  }
  return true;
}

void OnSignal(int)
{
  s_stop = 1;
}
}  // namespace

int main(int argc, char** argv)
{
  int port = DEFAULT_PORT;
  std::string bind_address = "0.0.0.0", secret_file, relays_file, stats_dir;
  double log_every = 600.0;
#ifdef NETD_TEST
  // (The PC test: the country database read, addresses looked up in it.)
  if (argc >= 4 && std::string(argv[1]) == "country")
  {
    if (!LoadGeo(argv[2]))
      return 1;
    std::printf("%zu ranges\n", s_geo.size());
    for (int i = 3; i < argc; i++)
    {
      uint32_t ip = 0;
      std::printf("%s %s\n", argv[i], ParseIp(argv[i], &ip) ? CountryOf(ip).c_str() : "not-an-address");
    }
    return 0;
  }
#endif
  // The game titles' list (update-titles.sh): build-titles OUT console:kind:DAT...
  if (argc >= 4 && std::string(argv[1]) == "build-titles")
    return BuildTitles(argv[2], std::vector<std::string>(argv + 3, argv + argc));
  // The public page's numbers (publish-stats.sh): public-json [--stats-dir DIR] [--titles FILE]; its profiles' files
  // and rankings: public-profiles --out DIR [...]; one profile: profile CODE [--delete] [...]
  if (argc >= 2 && (std::string(argv[1]) == "public-json" || std::string(argv[1]) == "public-profiles" ||
                    std::string(argv[1]) == "profile"))
  {
    const std::string command = argv[1];
    std::string dir = "/var/lib/emusuni-netd/stats", titles = "/var/lib/emusuni-netd/titles.txt", out, code;
    bool remove = false;
    for (int i = 2; i < argc; i++)
    {
      const std::string arg = argv[i];
      if (arg == "--delete")
        remove = true;
      else if (arg == "--stats-dir" && i + 1 < argc)
        dir = argv[++i];
      else if (arg == "--titles" && i + 1 < argc)
        titles = argv[++i];
      else if (arg == "--out" && i + 1 < argc)
        out = argv[++i];
#ifdef NETD_TEST
      else if (arg == "--clock-offset" && i + 1 < argc)
        s_clock_offset = std::atoll(argv[++i]);
#endif
      else if (command == "profile" && code.empty())
        code = arg;
    }
    if (command == "public-profiles")
      return out.empty() ? 2 : PublicProfiles(dir, titles, out);
    if (command == "profile")
      return ProfileCommand(dir, code, remove, titles);
    return PublicJson(dir, titles);
  }
  // The statistics' commands (on the server's machine): report, record-downloads.
  if (argc >= 2 && (std::string(argv[1]) == "report" || std::string(argv[1]) == "record-downloads"))
  {
    const std::string command = argv[1];
    std::string dir = "/var/lib/emusuni-netd/stats", titles = "/var/lib/emusuni-netd/titles.txt";
    int days = 30;
    bool json = false;
    for (int i = 2; i < argc; i++)
    {
      const std::string arg = argv[i];
      if (arg == "--stats-dir" && i + 1 < argc)
        dir = argv[++i];
      else if (arg == "--days" && i + 1 < argc)
        days = std::atoi(argv[++i]);
      else if (arg == "--json")
        json = true;
      else if (arg == "--titles" && i + 1 < argc)
        titles = argv[++i];
#ifdef NETD_TEST
      else if (arg == "--clock-offset" && i + 1 < argc)
        s_clock_offset = std::atoll(argv[++i]);
#endif
      else
      {
        std::fprintf(stderr, "usage: %s report [--stats-dir DIR] [--days 30] [--json] [--titles FILE]\n"
                             "       %s record-downloads [--stats-dir DIR] < GitHub's releases JSON\n",
                     argv[0], argv[0]);
        return 2;
      }
    }
    return command == "report" ? StatsReport(dir, days, json, titles) : RecordDownloads(dir);
  }
  for (int i = 1; i < argc; i++)
  {
    const std::string arg = argv[i];
    if (arg == "--stats-dir" && i + 1 < argc)
      stats_dir = argv[++i];
    else if (arg == "--geoip" && i + 1 < argc)
      s_geo_path = argv[++i];
#ifdef NETD_TEST
    else if (arg == "--clock-offset" && i + 1 < argc)
      s_clock_offset = std::atoll(argv[++i]);
    else if (arg == "--live-every" && i + 1 < argc)
      s_live_every = std::max(0.5, std::atof(argv[++i]));
    else if (arg == "--save-every" && i + 1 < argc)
      s_save_every = std::max(0.5, std::atof(argv[++i]));
    else if (arg == "--beat-gap" && i + 1 < argc)
      s_beat_min_gap = std::max(0.0, std::atof(argv[++i]));
#endif
    else if (arg == "--port" && i + 1 < argc)
      port = std::atoi(argv[++i]);
    else if (arg == "--bind" && i + 1 < argc)
      bind_address = argv[++i];
    else if (arg == "--secret-file" && i + 1 < argc)
      secret_file = argv[++i];
    else if (arg == "--relays" && i + 1 < argc)
      relays_file = argv[++i];
    else if (arg == "--name" && i + 1 < argc)
      s_name = argv[++i];
    else if (arg == "--region" && i + 1 < argc)
      s_region = argv[++i];
    else if (arg == "--relay-only")
      s_relay_only = true;
    else if (arg == "--max-rooms" && i + 1 < argc)
      s_max_rooms = static_cast<size_t>(std::max(1, std::atoi(argv[++i])));
    else if (arg == "--log-every" && i + 1 < argc)
      log_every = std::max(1.0, std::atof(argv[++i]));
    else if (arg == "--verbose")
      s_verbose = true;
    else
    {
      std::fprintf(stderr, "usage: %s [--port 7777] [--bind 0.0.0.0] [--secret-file FILE] [--relays FILE] [--name main] "
                           "[--region EU] [--relay-only] [--max-rooms 5000] [--log-every 600] [--verbose] "
                           "[--stats-dir DIR] [--geoip FILE]\n"
                           "       %s report [--stats-dir DIR] [--days 30] [--json]\n"
                           "       %s record-downloads [--stats-dir DIR] < GitHub's releases JSON\n",
                   argv[0], argv[0], argv[0]);
      return 2;
    }
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!secret_file.empty() && !LoadSecret(secret_file))
  {
    std::fprintf(stderr, "the secret file %s can't be read (16 characters at least)\n", secret_file.c_str());
    return 2;
  }
  if (s_secret.empty())
  {
    // (Without the shared secret this server's tickets fit only itself: other relays refuse them.)
    for (int i = 0; i < 4; i++)
    {
      const uint64_t r = Random64();
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&r);
      s_secret.insert(s_secret.end(), p, p + 8);
    }
    std::printf("emusuni-netd: no --secret-file: a random secret (only this server relays for its rooms)\n");
  }
  if (!relays_file.empty() && !LoadRelays(relays_file))
  {
    std::fprintf(stderr, "the relays file %s can't be read\n", relays_file.c_str());
    return 2;
  }
  if (!stats_dir.empty() && s_relay_only)
  {
    std::fprintf(stderr, "--stats-dir is the main server's (a relay-only server gets no heartbeats)\n");
    return 2;
  }
  if (!stats_dir.empty() && !StatsStart(stats_dir))
    return 2;
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_socket < 0)
  {
    std::perror("socket");
    return 1;
  }
  const int buffer = 4 << 20;
  setsockopt(s_socket, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
  setsockopt(s_socket, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1)
  {
    std::fprintf(stderr, "not an IPv4 address: %s\n", bind_address.c_str());
    return 2;
  }
  if (bind(s_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    std::perror("bind");
    return 1;
  }
  fcntl(s_socket, F_SETFL, fcntl(s_socket, F_GETFL, 0) | O_NONBLOCK);
  std::printf("emusuni-netd: protocol %d, UDP port %d, %s, %zu other relays listed, at most %zu rooms\n", VERSION, port,
              s_relay_only ? "relay only" : "main server and relay", s_relays.size(), s_max_rooms);

  double last_housekeeping = Now(), last_log = Now();
  uint8_t buf[2048];
  while (!s_stop)
  {
    pollfd p{s_socket, POLLIN, 0};
    poll(&p, 1, 100);
    for (int i = 0; i < 8192; i++)
    {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const ssize_t n = recvfrom(s_socket, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < 0)
        break;
      if (from.sin_family == AF_INET)
        OnPacket(buf, static_cast<size_t>(n), from);
    }
    const double now = Now();
    if (now - last_housekeeping >= 1.0)
    {
      last_housekeeping = now;
      Housekeeping();
      StatsTick(false);
    }
    if (now - last_log >= log_every)
    {
      last_log = now;
      LogCounts();
    }
  }
  StatsTick(true);  // (the day and its set saved: a restart goes on with them)
  for (auto& [code, room] : s_rooms)
  {
    for (const Slot& s : room.members)
    {
      if (s.used)
        SendClosed(room, s, CloseReason::Expired);
    }
  }
  std::printf("emusuni-netd: stopped\n");
  close(s_socket);
  return 0;
}
