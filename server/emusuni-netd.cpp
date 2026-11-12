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
// the log (stdout: journald under systemd) has counts and the consoles of the rooms. (01.05) The others see each
// member's country (its address looked up in --geoip's database as it comes in, never the address; none for one that
// asks so - FLAG_NO_COUNTRY), kept only in the room's memory while it lasts. (01.06) Versions play together: a room's
// console's online level the same in both lets another version in (NetProto::PlayTogether; Join), each member gets the
// room's game by the CRC32 its app knows it by, and an app without "Show My Country" (v00.23's) shows none.
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
#include <deque>
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

// HMAC-SHA256 with a key of its own ((01.07) the world chat's salt: emusuni-netd-chat.h), and with the servers' secret.
void HmacKey(const uint8_t* secret, size_t secret_n, const void* data, size_t n, uint8_t out[32])
{
  uint8_t key[64] = {};
  if (secret_n > 64)
  {
    Sha256 k;
    k.Add(secret, secret_n);
    k.Finish(key);
  }
  else if (secret_n > 0)
  {
    std::memcpy(key, secret, secret_n);
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

void Hmac(const void* data, size_t n, uint8_t out[32])
{
  HmacKey(s_secret.data(), s_secret.size(), data, n, out);
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
  // (01.07) The seconds until `amount` is there (0: now), nothing taken.
  double Wait(double now, double rate, double burst, double amount = 1.0) const
  {
    const double have = tokens < 0.0 ? burst : std::min(burst, tokens + (now - last) * rate);
    return have >= amount || rate <= 0.0 ? 0.0 : (amount - have) / rate;
  }
};

struct AddressState
{
  Bucket packets, hellos, rooms, lists;
  Bucket stats, new_ids, profiles;  // (the anonymous statistics' heartbeats, and the installs new today they bring)
  // (01.07) The world chat's (emusuni-netd-chat.h): its questions, its lines (one every 3 s, 20 a minute - for one that
  // changes its chat key), its reports.
  Bucket world_asks, world_every, world_minute, world_reports;
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
  // (01.06) What its app does online (said at Create's or Join's end, else its version's: OldNetCaps); its version is
  // info.app_version.
  NetCaps caps;
  // (01.07) Its room chat's copies (RoomChatCopy: one every 2 s, 3 at once - the room's own chat's pace).
  Bucket chat;
  // (01.07) The players' ranking (emusuni-netd-players.h): its code when it is ranked ("": not - nothing counted), and
  // since when it plays the room's match (0: not playing).
  std::string rank_code;
  double match_from = 0.0;
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
  // (01.04) Its history, written when it closes (RoomLog: stats.db's room_log): the countries of those who were in it
  // (never their addresses), the most at once, the joins, the matches the host started (Lock) and how long they went,
  // the bytes this server relayed for it.
  std::set<std::string> countries;
  int peak = 1, joins = 0, matches = 0;
  double match_start = 0.0, match_seconds = 0.0, first_match = 0.0;
  uint64_t relayed = 0;
  // (01.04, the owner: why nobody plays) A public room's title (strangers read it in the list already); the guests who
  // left by themselves and those dropped (silent); the joins refused, by why (full, version, started, kicked); and what
  // the host tells (RoomNote): the games every player has (-1: not told), the requests it refused or didn't answer, a
  // Choose Together match's game (its Lock: "snes.c-...").
  std::string title, chosen;
  int left = 0, dropped = 0, refused_full = 0, refused_version = 0, refused_started = 0, refused_kicked = 0;
  int common = -1, host_refused = 0, host_no_answer = 0;
  // (01.05) Its spectators (member ids from FIRST_WATCHER: listed only to the host and to themselves), whether the host
  // lets them in (NOTE_WATCH), how many came (its history).
  Slot watchers[MAX_WATCHERS];
  bool watchable = false;
  int watcher_joins = 0;
  // (01.05) A player may join its match under way (NOTE_LATE), and how many did.
  bool late_ok = false;
  int late_joins = 0;
  // (01.05) The version of the host's file as its name says it ("USA, Rev 1"; Create's, cleaned): the list's.
  std::string version;
  // (01.06) The host's file's whole CRC32 and size (Create's end: GameInfo::file_crc; 0 not said), for the apps that
  // know a game by them (no CAP_CONTENT_ID).
  uint32_t file_crc = 0;
  uint64_t file_size = 0;
  bool Link() const { return game.console == GBLINK || game.console == GBALINK; }
  // (01.06) Every player's app joins a match under way (CAP_LATE): with one of v00.23's in it, none may join late (it
  // knows nothing of the pause for it, and would wait for inputs of a player it doesn't know).
  bool AllLate() const
  {
    for (const Slot& s : members)
    {
      if (s.used && !(s.caps.caps & CAP_LATE))
        return false;
    }
    return true;
  }
  // Its match under way takes a player now (NOTE_LATE, (01.06) AllLate).
  bool LateOpen() const { return locked && late_ok && !Link() && game.console != TOGETHER && AllLate(); }
  // The room's game as a member's app knows it (01.06): by its whole file for an app before 01.05, when the host said
  // it.
  GameInfo GameFor(const NetCaps& caps) const
  {
    GameInfo g = game;
    if (!(caps.caps & CAP_CONTENT_ID) && file_crc != 0)
    {
      g.crc = file_crc;
      g.size = file_size;
    }
    return g;
  }
  int Count() const
  {
    int n = 0;
    for (const Slot& s : members)
      n += s.used ? 1 : 0;
    return n;
  }
  int Watchers() const
  {
    int n = 0;
    for (const Slot& s : watchers)
      n += s.used ? 1 : 0;
    return n;
  }
  // A member's or a spectator's place by its id (nullptr: none such).
  Slot* SlotOf(int id)
  {
    if (id >= 0 && id < MAX_MEMBERS)
      return &members[id];
    if (id >= FIRST_WATCHER && id < MAX_SLOTS)
      return &watchers[id - FIRST_WATCHER];
    return nullptr;
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
std::unordered_map<uint64_t, uint64_t> s_room_relayed;  // (01.04) a room's bytes through RelayData, by its relay id
std::vector<RelayEntry> s_relays;  // ServerInfo's list after this server
std::string s_name = "main", s_region = "";
size_t s_max_rooms = 5000;
struct Counts
{
  uint64_t packets = 0, dropped = 0, relayed = 0, relayed_bytes = 0, created = 0, joined = 0, refused = 0, closed = 0,
           kicked = 0, lists = 0;
} s_counts;
Type s_request = Type::Hello;  // (01.04) the request being answered (OnPacket): a refusal's request (refusal_day)

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
  CountRefusal(s_request, reason);  // (01.04)
}

bool RoomChatStored();  // ((01.07) emusuni-netd-chat.h)
std::string ChatCode(const uint8_t* key);
// ((01.07) emusuni-netd-players.h: a ranked member's time in the room's match counted, its match_from back to 0; every
// member's.)
void PlayerTime(const Room& room, Slot& slot, double until = -1.0);
void PlayersTime(Room& room);

void SendMembers(const Room& room, const Slot& to)
{
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::Members);
  WriteCode(w, room.code);
  w.U32(room.seq);
  w.U64(room.key);
  WriteGame(w, room.GameFor(to.caps));  // ((01.06) by the CRC32 its app knows it by)
  w.Str(room.app_version, MAX_APP_VERSION);
  w.U8(room.max_members);
  w.U8(room.is_public ? FLAG_PUBLIC : 0);
  w.U8(to.info.id);
  // (01.05) The players to the players; the players and the spectators to the host; the host and itself to a
  // spectator.
  const bool to_watcher = to.info.id >= FIRST_WATCHER, to_host = to.info.id == 0;
  std::vector<const Slot*> listed;
  for (const Slot& s : room.members)
  {
    if (s.used && (!to_watcher || s.info.id == 0))
      listed.push_back(&s);
  }
  for (const Slot& s : room.watchers)
  {
    if (s.used && (to_host || &s == &to))
      listed.push_back(&s);
  }
  w.U8(static_cast<uint8_t>(listed.size()));
  for (const Slot* slot : listed)
  {
    const Slot& s = *slot;
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
  // (01.05) After them (an app before it reads none of it): how many watch - every member may know the number -, and
  // each listed member's country (none for one that doesn't show it).
  w.U8(EXTRA_VERSION);
  w.U8(static_cast<uint8_t>(room.Watchers()));
  for (const Slot* slot : listed)
    w.Str(slot->info.country, COUNTRY_SIZE);
  // (01.06) Then each one's app version and caps (an app of 01.05 reads to the countries' end): the others' chat and
  // late joining say what an older app can't.
  w.U8(EXTRA2_VERSION);
  for (const Slot* slot : listed)
  {
    w.Str(slot->info.app_version, MAX_APP_VERSION);
    w.U8(static_cast<uint8_t>(slot->caps.caps | (slot->caps.known ? CAPS_KNOWN : 0)));
  }
  // (01.07) Then whether the room's chat is kept (its copies: RoomChatCopy) - the room's notice says so.
  w.U8(EXTRA3_VERSION);
  w.U8(RoomChatStored() ? ROOM_CHAT_STORED : 0);
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
  for (const Slot& s : room.watchers)  // (01.05)
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

// (01.04) A room's history, when it closes (why: "host-left", "host-silent", "too-old", "server-stopped"): a row of
// stats.db's room_log - its times, console, game (its label: "snes.c-7655bb55"; the title the app gave isn't kept), kind,
// players, matches, the bytes relayed, the host's version and the countries of those who were in it. No address, no
// nickname, no code. Kept ROOM_LOG_DAYS days. Nothing when the statistics are off (no --stats-dir).
constexpr int ROOM_LOG_DAYS = 180;
// (01.05) When the games most played online were last counted (OnTopGames; a room logged with matches: again).
double s_top_at = -1e9;
void RoomLog(const Room& room, const char* why)
{
  if (!s_stats.on || !s_stats.db)
    return;
  if (room.matches > 0)
    s_top_at = -1e9;
  const double now = Now();
  const std::string& console = room.game.console;
  const bool together = console == TOGETHER, link = console == GBLINK || console == GBALINK;
  const char* kind = together ? "together" : link ? "link" : room.is_public ? "public" : "private";
  char crc[16];
  std::snprintf(crc, sizeof(crc), "c:%08x", room.game.crc);
  const std::string label = together ? room.chosen :
                            room.game.crc == 0 ? std::string() : GameLabel(Token(console, MAX_CONSOLE), crc);
  std::string countries;
  for (const std::string& c : room.countries)
    countries += (countries.empty() ? "" : ",") + c;
  double matched = room.match_seconds + (room.match_start > 0.0 ? now - room.match_start : 0.0);
  uint64_t relayed = room.relayed;
  if (auto r = s_room_relayed.find(room.relay_id); r != s_room_relayed.end())
    relayed += r->second;
  const int64_t wall = static_cast<int64_t>(std::time(nullptr));
  const auto at = [&](double t) { return t > 0.0 ? wall - static_cast<int64_t>(now - t) : int64_t{0}; };
  Stmt q(s_stats.db, "INSERT INTO room_log(created, closed, console, game, kind, max_members, peak, joins, matches,"
                     " match_seconds, first_match, relayed_kb, version, countries, reason, title, guests_left, dropped,"
                     " refused_full, refused_version, refused_started, refused_kicked, common, host_refused,"
                     " host_no_answer, watchers, late_joins) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
                     " ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  q.Int(1, at(room.created)).Int(2, wall).Text(3, together ? std::string("*") : Token(console, MAX_CONSOLE));
  q.Text(4, label).Text(5, kind).Int(6, room.max_members).Int(7, room.peak).Int(8, room.joins).Int(9, room.matches);
  q.Int(10, static_cast<int64_t>(matched)).Int(11, at(room.first_match));
  q.Int(12, static_cast<int64_t>((relayed + 1023) / 1024)).Text(13, Token(room.app_version, MAX_APP_VERSION));
  q.Text(14, countries).Text(15, why);
  q.Text(16, room.is_public && !together ? room.title : std::string()).Int(17, room.left).Int(18, room.dropped);
  q.Int(19, room.refused_full).Int(20, room.refused_version).Int(21, room.refused_started).Int(22, room.refused_kicked);
  q.Int(23, room.common).Int(24, room.host_refused).Int(25, room.host_no_answer);
  q.Int(26, room.watcher_joins).Int(27, room.late_joins);  // ((01.05) the spectators who came, the players late)
  q.Run();
  static double last_prune = 0.0;
  if (now - last_prune >= 3600.0)
  {
    last_prune = now;
    Stmt prune(s_stats.db, "DELETE FROM room_log WHERE created < ?");
    prune.Int(1, wall - int64_t{ROOM_LOG_DAYS} * 86400).Run();
  }
}

void CloseRoom(const std::string& code, CloseReason reason, const char* why = nullptr)
{
  auto it = s_rooms.find(code);
  if (it == s_rooms.end())
    return;
  PlayersTime(it->second);  // ((01.07) the ranking: its match's time so far)
  RoomLog(it->second, why ? why : reason == CloseReason::HostLeft ? "host-left" : "host-silent");
  s_room_relayed.erase(it->second.relay_id);
  for (const Slot& s : it->second.members)
  {
    if (s.used)
      SendClosed(it->second, s, reason);
  }
  for (const Slot& s : it->second.watchers)  // (01.05)
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
  if (!r.Ok() || id >= MAX_SLOTS)
    return nullptr;
  auto it = s_rooms.find(code);
  if (it == s_rooms.end())
    return nullptr;
  Slot& slot = *it->second.SlotOf(id);  // ((01.05) a spectator's too)
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

// (01.05, the owner: "juegos más jugados en línea") The games most played online: room_log's matches of the last
// days by game - its label, console and content CRC32 (a Choose Together match's: its game's) -, with a public room's
// title of it when there is one (strangers read those in the list already). No nickname, address or code is in it.
// Counted again every 10 minutes, or after a room with matches closed (an answer is the copy's).
struct TopEntry
{
  std::string console, title;
  uint32_t crc = 0;
  int64_t matches = 0;
};
std::vector<TopEntry> s_top;
int s_top_days = 0;

// (01.06) A game's whole file's CRC32 and its content's, as a host of 01.06 on says both (Create): the rooms of the
// apps that know a game by its whole file (FILE_ID_VERSIONS: v00.23's, most of them) count as its content's in the
// games most played - a .nes, a zip or an arcade set is in standard.bin by its content's only. Labels only
// ("nes.c-..."), the last seen kept; FILE_ID_DAYS days.
constexpr int FILE_ID_DAYS = 180;
void LearnFileId(const std::string& console, uint32_t file_crc, uint32_t content_crc)
{
  if (!s_stats.on || !s_stats.db)
    return;
  char file[16], content[16];
  std::snprintf(file, sizeof(file), "c:%08x", file_crc);
  std::snprintf(content, sizeof(content), "c:%08x", content_crc);
  const std::string old_label = GameLabel(Token(console, MAX_CONSOLE), file);
  const std::string label = GameLabel(Token(console, MAX_CONSOLE), content);
  if (old_label.empty() || label.empty())
    return;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  Stmt q(s_stats.db, "INSERT OR REPLACE INTO file_id(old, label, seen) VALUES (?, ?, ?)");
  q.Text(1, old_label).Text(2, label).Int(3, now).Run();
  s_top_at = -1e9;  // (counted again at the next question)
  static double last_prune = -1e9;
  if (Now() - last_prune >= 3600.0)
  {
    last_prune = Now();
    Stmt prune(s_stats.db, "DELETE FROM file_id WHERE seen < ?");
    prune.Int(1, now - int64_t{FILE_ID_DAYS} * 86400).Run();
  }
}

void CountTopGames(int days)
{
  s_top.clear();
  s_top_at = Now();
  s_top_days = days;
  if (!s_stats.on || !s_stats.db)
    return;
  // ((01.06) A room of an app that knew its game by its whole file: by its content's when a host of 01.06 said both.)
  std::string old_versions;
  for (const char* v : FILE_ID_VERSIONS)
    old_versions += std::string(old_versions.empty() ? "'" : ", '") + v + "'";
  Stmt q(s_stats.db, ("SELECT COALESCE(f.label, r.game) AS g, SUM(r.matches) AS m, MAX(r.title) FROM room_log r "
                      "LEFT JOIN file_id f ON f.old = r.game AND r.version IN (" + old_versions + ") "
                      "WHERE r.created >= ? AND r.matches > 0 AND r.game LIKE '%.c-%' GROUP BY g ORDER BY m DESC, g "
                      "LIMIT 40").c_str());
  q.Int(1, static_cast<int64_t>(std::time(nullptr)) - int64_t{days} * 86400);
  while (q.Row() && s_top.size() < static_cast<size_t>(TOP_GAMES))
  {
    const std::string label = q.ColText(0);
    const size_t at = label.find(".c-");
    if (at == std::string::npos || at == 0 || label.size() != at + 11)
      continue;
    TopEntry e;
    e.console = label.substr(0, at);
    e.crc = static_cast<uint32_t>(std::strtoul(label.c_str() + at + 3, nullptr, 16));
    e.matches = q.ColInt(1);
    e.title = CleanText(q.ColText(2), 40, "");
    s_top.push_back(std::move(e));
  }
}

void OnTopGames(Reader& r, const sockaddr_in& from, size_t size, AddressState& state)
{
  uint32_t nonce = 0;
  if (!CheckCookie(r, from, size, &nonce))
    return;
  int days = r.U8();
  if (!r.Ok() || !state.lists.Take(Now(), LISTS_RATE, LISTS_BURST))
    return;
  days = std::clamp(days == 0 ? TOP_DAYS : days, 1, 30);
  if (Now() - s_top_at > 600.0 || days != s_top_days)
    CountTopGames(days);
  uint8_t buf[MAX_PACKET];
  Writer w(buf, sizeof(buf));
  w.Header(Type::TopGamesList);
  w.U32(nonce);
  w.U8(static_cast<uint8_t>(days));
  const size_t count_at = w.Size();
  w.U8(0);
  size_t count = 0;
  for (const TopEntry& e : s_top)
  {
    uint8_t one[256];
    Writer o(one, sizeof(one));
    o.Str(e.console, MAX_CONSOLE);
    o.U32(e.crc);
    o.U32(static_cast<uint32_t>(std::min<int64_t>(e.matches, 0xFFFFFFFF)));
    o.Str(e.title, MAX_TOP_TITLE);
    if (!o.Ok() || w.Size() + o.Size() > MAX_PACKET)
      break;
    w.Bytes(one, o.Size());
    count++;
  }
  buf[count_at] = static_cast<uint8_t>(count);
  if (w.Ok())
    Send(from, buf, w.Size());
}

// (01.05) The country the others see of a member: its address's (CountryOf: the address goes no further), none when it
// asked so (FLAG_NO_COUNTRY) or when the database doesn't know it. (01.06) None of an app without "Show My Country"
// (CAP_COUNTRY: v00.23's) - its player never chose to show it.
std::string ShownCountry(const sockaddr_in& from, uint8_t flags, const NetCaps& caps)
{
  if ((flags & FLAG_NO_COUNTRY) || !(caps.caps & CAP_COUNTRY))
    return {};
  const std::string country = CountryOf(ntohl(from.sin_addr.s_addr));
  return country == "ZZ" ? std::string() : CleanCountry(country);
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
  // (01.05) After them, the version of the host's file (an app before it sends none; one that can't be read: none).
  std::string version;
  if (r.Left() > 0)
  {
    version = r.Str(MAX_GAME_VERSION);
    version = r.Ok() ? CleanText(version, GAME_VERSION_LETTERS, "") : std::string();
  }
  // (01.06) Then what the app does online and its file's whole CRC32 and size (an app before it: its version's).
  NetCaps caps;
  uint32_t file_crc = 0;
  uint64_t file_size = 0;
  std::string rank_code;  // ((01.07) then its player block: the ranking)
  if (r.Ok() && ReadNetCaps(r, &caps))
  {
    Reader x = r;
    file_crc = x.U32();
    file_size = x.U64();
    bool ranked = false;
    uint8_t player_key[WORLD_KEY_SIZE];
    if (x.Ok() && ReadPlayer(x, &ranked, player_key) && ranked)
      rank_code = ChatCode(player_key);
    if (!x.Ok() || file_size == 0)
    {
      file_crc = 0;
      file_size = 0;
    }
  }
  else
    caps = OldNetCaps(app_version);
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
  room.countries.insert(CountryOf(ip));  // (01.04: its history - the country only)
  if (room.is_public)
    room.title = game.title;  // (already cleaned: strangers read it in the public list)
  room.version = version;  // (01.05)
  // ((01.06) Its file's whole CRC32: for the apps that know a game by it - learnt for the games most played, whose
  // rooms of those apps the room_log keeps by it.)
  if ((caps.caps & CAP_CONTENT_ID) && file_crc != 0 && game.console != TOGETHER)
  {
    room.file_crc = file_crc;
    room.file_size = file_size;
    if (file_crc != game.crc)
      LearnFileId(game.console, file_crc, game.crc);
  }
  Slot& host = room.members[0];
  host.used = true;
  host.info = me;
  host.info.id = 0;
  host.info.flags = flags & FLAG_HIDDEN;
  host.info.app_version = Token(app_version, MAX_APP_VERSION);  // ((01.06) Members says it to the others)
  host.caps = caps;
  host.rank_code = rank_code;  // ((01.07))
  host.info.country = ShownCountry(from, flags, caps);  // (01.05)
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
  // (01.06) What the app does online, at the end (an app before it: its version's).
  NetCaps caps;
  const bool caps_said = ReadNetCaps(r, &caps);
  if (!caps_said)
    caps = OldNetCaps(app_version);
  me.app_version = Token(app_version, MAX_APP_VERSION);
  // ((01.07) Then its player block: the ranking.)
  std::string rank_code;
  bool ranked = false;
  uint8_t player_key[WORLD_KEY_SIZE];
  if (caps_said && ReadPlayer(r, &ranked, player_key) && ranked)
    rank_code = ChatCode(player_key);
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
  // (01.06) Another version plays the room's match when its console's online level is the same in both (PlayTogether:
  // never a Choose Together room's or a link cable's, nor an app not known) - before, the same version only.
  if (!PlayTogether(room.game.console, room.app_version, room.members[0].caps, app_version, caps))
  {
    room.refused_version++;  // (01.04: its history)
    // ((01.06) An app that says its levels learns the room's console too: it says who updates, and for what.)
    SendRefused(from, VERSION, nonce, Refusal::Version,
                caps_said ? VersionDetail(Token(room.app_version, MAX_APP_VERSION), room.game.console) :
                            room.app_version,
                size);
    return;
  }
  if (room.kicked.count(ntohl(from.sin_addr.s_addr)))
  {
    room.refused_kicked++;
    SendRefused(from, VERSION, nonce, Refusal::Kicked, "", size);
    return;
  }
  // (01.05) A spectator: in a room whose host lets them, while it waits or plays, MAX_WATCHERS at most.
  if (flags & FLAG_WATCH)
  {
    int free_watcher = -1;
    for (int i = 0; i < MAX_WATCHERS && free_watcher < 0; i++)
    {
      if (!room.watchers[i].used)
        free_watcher = i;
    }
    if (!room.watchable || free_watcher < 0)
    {
      SendRefused(from, VERSION, nonce, Refusal::NoWatch, room.watchable ? "full" : "", size);
      return;
    }
    Slot& slot = room.watchers[free_watcher];
    slot = Slot();
    slot.used = true;
    slot.info = me;
    slot.info.id = static_cast<uint8_t>(FIRST_WATCHER + free_watcher);
    slot.info.flags = static_cast<uint8_t>((flags & FLAG_HIDDEN) | FLAG_WATCH);
    slot.caps = caps;  // ((01.06))
    slot.info.country = ShownCountry(from, flags, caps);
    slot.info.locals = 0;
    slot.info.pub = ToAddress(from);
    slot.addr = from;
    slot.token = Random64();
    slot.last_seen = now;
    room.watcher_joins++;
    room.countries.insert(CountryOf(ntohl(from.sin_addr.s_addr)));
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
    return;
  }
  // ((01.05) A match under way takes a player more if its host lets it: not a link cable's or a Choose Together one's.
  // (01.06) Nor an app that doesn't join one under way (v00.23's: it would take the host's LateStart for nothing), nor
  // with such an app playing it - Room::LateOpen.)
  const bool late = room.LateOpen() && (caps.caps & CAP_LATE);
  if (room.locked && !late)
  {
    room.refused_started++;
    // ((01.05) "watch": a spectator may come in - an app before it ignores the detail.)
    SendRefused(from, VERSION, nonce, Refusal::Started,
                room.watchable && room.Watchers() < MAX_WATCHERS ? WATCH_DETAIL : "", size);
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
    room.refused_full++;
    SendRefused(from, VERSION, nonce, Refusal::Full, "", size);
    return;
  }
  Slot& slot = room.members[free_id];
  slot = Slot();
  slot.used = true;
  slot.info = me;
  slot.info.id = static_cast<uint8_t>(free_id);
  slot.info.flags = flags & FLAG_HIDDEN;
  slot.caps = caps;  // ((01.06))
  slot.rank_code = rank_code;  // ((01.07) the ranking: from now when it joins the match under way)
  slot.match_from = late ? now : 0.0;
  slot.info.country = ShownCountry(from, flags, caps);  // (01.05)
  slot.info.locals = static_cast<uint8_t>(std::clamp<int>(me.locals, 1, MAX_PORTS));
  slot.info.pub = ToAddress(from);
  slot.addr = from;
  slot.token = Random64();
  slot.last_seen = now;
  room.joins++;  // (01.04: its history)
  if (late)
    room.late_joins++;  // (01.05)
  room.peak = std::max(room.peak, room.Count());
  room.countries.insert(CountryOf(ntohl(from.sin_addr.s_addr)));

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
    WriteGame(w, room.GameFor(NetCaps()));  // ((01.06) the old list's askers: apps before ListAll)
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
  // (01.06) The asker's app (it says it at the end): one that doesn't gets each game by its whole file's CRC32.
  NetCaps asker;
  ReadNetCaps(r, &asker);
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
  // (01.05) After the rooms, each one's host's country and game's version (WriteRoomExtra): kept apart until the end,
  // the datagram's room counted for them too.
  uint8_t extra[MAX_PACKET];
  Writer extras(extra, sizeof(extra));
  extras.U8(EXTRA_VERSION);
  // ((01.06) And after those, each one's host's level of its console and caps - an app of 01.05 reads to the first
  // extras' end.)
  uint8_t extra2[MAX_PACKET];
  Writer extras2(extra2, sizeof(extra2));
  extras2.U8(EXTRA2_VERSION);
  for (size_t i = first; i < rooms.size() && count < 255; i++)
  {
    const Room& room = *rooms[i];
    RoomEntry e;
    e.id = room.list_id;
    e.is_public = room.is_public;
    e.playing = room.locked;
    e.code = room.is_public ? room.code : std::string();
    e.game = room.GameFor(asker);  // ((01.06))
    e.host = room.members[0].info.nick;
    e.region = room.members[0].info.region;
    e.players = static_cast<uint8_t>(room.Count());
    e.max_players = room.max_members;
    e.host_ms = room.members[0].rtt_ms;
    e.app_version = room.app_version;
    e.watchable = room.watchable;  // (01.05)
    e.watchers = static_cast<uint8_t>(room.Watchers());
    e.late = room.LateOpen() && room.Count() < room.max_members;  // ((01.06) LateOpen: no player of v00.23)
    e.country = room.members[0].info.country;  // (01.05)
    e.version = room.version;
    const NetCaps& host = room.members[0].caps;
    uint8_t entry[MAX_PACKET];
    Writer one(entry, sizeof(entry));
    WriteListedRoom(one, e);
    uint8_t more[MAX_PACKET];
    Writer its(more, sizeof(more));
    WriteRoomExtra(its, e);
    if (!one.Ok() || !its.Ok() || w.Size() + one.Size() + extras.Size() + its.Size() + extras2.Size() + 2 > MAX_PACKET)
      break;  // (the next page: from here)
    w.Bytes(entry, one.Size());
    extras.Bytes(more, its.Size());
    extras2.U8(static_cast<uint8_t>(host.known ? host.Level(room.game.console) : 0));
    extras2.U8(static_cast<uint8_t>(host.caps | (host.known ? CAPS_KNOWN : 0)));
    count++;
  }
  buf[count_at] = static_cast<uint8_t>(count);
  if (extras.Ok() && w.Size() + extras.Size() <= MAX_PACKET)
  {
    w.Bytes(extra, extras.Size());
    if (extras2.Ok() && w.Size() + extras2.Size() <= MAX_PACKET)
      w.Bytes(extra2, extras2.Size());
  }
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
    PlayerTime(*room, *slot);  // ((01.07) the ranking: the host's time before its place is emptied)
    *slot = Slot();
    CloseRoom(room->code, CloseReason::HostLeft);
    return;
  }
  const bool watcher = slot->info.id >= FIRST_WATCHER;
  PlayerTime(*room, *slot);  // ((01.07) the ranking)
  *slot = Slot();
  if (!watcher)
    room->left++;  // (01.04: a guest left by itself; (01.05) not a spectator)
  SendMembersToAll(*room);
}

void OnKick(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t target = r.U8();
  // ((01.05) A spectator too.)
  if (!slot || !r.Ok() || slot->info.id != 0 || target == 0 || target >= MAX_SLOTS || !room->SlotOf(target)->used)
    return;
  Slot& out = *room->SlotOf(target);
  room->kicked.insert(ntohl(out.addr.sin_addr.s_addr));
  SendClosed(*room, out, CloseReason::Kicked);
  PlayerTime(*room, out);  // ((01.07) the ranking)
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
  // ((01.04) A Choose Together match's game, after it: its console and CRC32 - an app before it sends nothing more.)
  std::string chosen_console;
  uint32_t chosen_crc = 0;
  if (locked && r.Left() > 0)
  {
    chosen_console = r.Str(MAX_CONSOLE);
    chosen_crc = r.U32();
  }
  if (slot && r.Ok() && slot->info.id == 0)
  {
    if (locked && room->game.console == TOGETHER && chosen_crc != 0)
    {
      char crc[16];
      std::snprintf(crc, sizeof(crc), "c:%08x", chosen_crc);
      const std::string label = GameLabel(Token(chosen_console, MAX_CONSOLE), crc);
      if (!label.empty())
        room->chosen = label;
    }
    // (01.04) The room's history: a match starts (Start: locked) and ends (back in the room).
    const double now = Now();
    if (locked && !room->locked)
    {
      room->matches++;
      room->match_start = now;
      if (room->first_match <= 0.0)
        room->first_match = now;
      for (Slot& s : room->members)  // ((01.07) the ranking: each player's time in it from now)
        s.match_from = s.used ? now : 0.0;
    }
    else if (!locked && room->locked && room->match_start > 0.0)
    {
      room->match_seconds += now - room->match_start;
      room->match_start = 0.0;
      PlayersTime(*room);
    }
    room->locked = locked != 0;
  }
}

// (01.04) The host's notes for the room's history: the games every player has, a request to join it ended.
void OnRoomNote(Reader& r, const sockaddr_in& from)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t kind = r.U8();
  if (!slot || !r.Ok() || slot->info.id != 0)
    return;
  if (kind == NOTE_COMMON)
  {
    const uint16_t common = r.U16();
    if (r.Ok())
      room->common = common;
  }
  else if (kind == NOTE_REFUSED)
  {
    const uint8_t why = r.U8();
    if (r.Ok() && why == QUIT_REFUSED)
      room->host_refused++;
    else if (r.Ok() && why == QUIT_NO_ANSWER)
      room->host_no_answer++;
  }
  else if (kind == NOTE_WATCH)  // (01.05) spectators let in or not (those in stay)
  {
    const uint8_t on = r.U8();
    if (r.Ok())
      room->watchable = on != 0;
  }
  else if (kind == NOTE_LATE)  // (01.05) a player may join the match under way, or not
  {
    const uint8_t on = r.U8();
    if (r.Ok())
      room->late_ok = on != 0;
  }
}

void OnRelay(Reader& r, const sockaddr_in& from, size_t packet_size)
{
  Room* room = nullptr;
  Slot* slot = Authenticate(r, from, &room);
  const uint8_t to = r.U8();
  // ((01.05) A spectator's packets only to and from the host.)
  if (!slot || !r.Ok() || to >= MAX_SLOTS || to == slot->info.id || !room->SlotOf(to)->used ||
      ((to >= FIRST_WATCHER || slot->info.id >= FIRST_WATCHER) && to != 0 && slot->info.id != 0))
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
  Send(room->SlotOf(to)->addr, buf, w.Size());
  s_counts.relayed++;
  s_counts.relayed_bytes += w.Size();
  room->relayed += w.Size();  // (01.04: its history)
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
  if (s_stats.on)
    s_room_relayed[room] += w.Size();  // (01.04: the room's history - this server's rooms only, CloseRoom forgets it)
}

#include "emusuni-netd-chat.h"     // ((01.07) the world chat)
#include "emusuni-netd-players.h"  // ((01.07) the players' ranking)

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
  s_request = type;  // (01.04: a refusal's request)
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
  case Type::TopGames:  // (01.05)
    OnTopGames(r, from, size, state);
    break;
  case Type::WorldChat:  // (01.07)
    OnWorldChat(r, from, size, state);
    break;
  case Type::WorldSay:
    OnWorldSay(r, from, size, state);
    break;
  case Type::WorldReport:
    OnWorldReport(r, from, size, state);
    break;
  case Type::RoomChatCopy:  // (01.07)
    OnRoomChatCopy(r, from);
    break;
  case Type::TopPlayers:
    OnTopPlayers(r, from, size, state);
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
  case Type::RoomNote:  // (01.04)
    OnRoomNote(r, from);
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
  std::vector<std::pair<std::string, CloseReason>> close, old;
  for (auto& [code, room] : s_rooms)
  {
    if (now - room.created > ROOM_LIFETIME)
    {
      old.push_back({code, CloseReason::Expired});
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
        room.dropped++;  // (01.04: silent - its connection lost)
        PlayerTime(room, s, s.last_seen);  // ((01.07) the ranking: until it was last heard)
        s = Slot();
        changed = true;
      }
    }
    for (Slot& s : room.watchers)  // (01.05) a spectator silent: gone
    {
      if (s.used && now - s.last_seen > MEMBER_TIMEOUT)
      {
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
  for (const auto& [code, reason] : old)
    CloseRoom(code, reason, "too-old");
  for (auto it = s_bindings.begin(); it != s_bindings.end();)
    it = now - it->second.last_seen > BINDING_TIMEOUT ? s_bindings.erase(it) : std::next(it);
  for (auto it = s_room_relay.begin(); it != s_room_relay.end();)
    it = now - it->second.last > BINDING_TIMEOUT ? s_room_relay.erase(it) : std::next(it);
  for (auto it = s_addresses.begin(); it != s_addresses.end();)
    it = now - it->second.last_seen > 600.0 ? s_addresses.erase(it) : std::next(it);
  for (auto it = s_replies.begin(); it != s_replies.end();)
    it = now - it->second.at > REPLY_MEMORY ? s_replies.erase(it) : std::next(it);
  ChatHousekeeping();  // ((01.07) the world chat's stored lines after 30 days, its salts)
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
              "kicked, %llu lists%s%s\n",
              s_rooms.size(), open, members, s_bindings.size(), by.c_str(),
              static_cast<unsigned long long>(s_counts.packets), static_cast<unsigned long long>(s_counts.dropped),
              static_cast<unsigned long long>(s_counts.relayed),
              static_cast<unsigned long long>(s_counts.relayed_bytes >> 10),
              static_cast<unsigned long long>(s_counts.created), static_cast<unsigned long long>(s_counts.joined),
              static_cast<unsigned long long>(s_counts.refused), static_cast<unsigned long long>(s_counts.closed),
              static_cast<unsigned long long>(s_counts.kicked), static_cast<unsigned long long>(s_counts.lists),
              StatsLogText().c_str(), WorldLogText().c_str());
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
    else if (arg == "--chat-salt-dir" && i + 1 < argc)  // ((01.07) the world chat: emusuni-netd-chat.h)
      s_chat_salt_dir = argv[++i];
    else if (arg == "--chat-blocks" && i + 1 < argc)
      s_chat_blocks_path = argv[++i];
#ifdef NETD_TEST
    else if (arg == "--clock-offset" && i + 1 < argc)
      s_clock_offset = std::atoll(argv[++i]);
    else if (arg == "--live-every" && i + 1 < argc)
      s_live_every = std::max(0.5, std::atof(argv[++i]));
    else if (arg == "--save-every" && i + 1 < argc)
      s_save_every = std::max(0.5, std::atof(argv[++i]));
    else if (arg == "--beat-gap" && i + 1 < argc)
      s_beat_min_gap = std::max(0.0, std::atof(argv[++i]));
    else if (arg == "--player-min" && i + 1 < argc)  // ((01.07) the ranking's minute, shorter)
      s_player_min_seconds = std::max(0.0, std::atof(argv[++i]));
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
                           "[--stats-dir DIR] [--geoip FILE] [--chat-salt-dir DIR] [--chat-blocks FILE]\n"
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
  // ((01.07) The world chat's salt, never in stats.db's folder: --chat-salt-dir, else beside the servers' secret. The
  // systemd unit's server can't write there - /etc/emusuni-netd, read only under its ProtectSystem=strict -: its env
  // gives /var/lib/emusuni-netd/chat-salt (scripts/server/deploy.sh). That default is for a server run by hand.)
  if (s_chat_salt_dir.empty() && !secret_file.empty())
  {
    const size_t slash = secret_file.rfind('/');
    s_chat_salt_dir = slash == std::string::npos ? "." : slash == 0 ? "/" : secret_file.substr(0, slash);
  }
  if (!s_relay_only)
    std::printf("emusuni-netd: the world chat %s\n",
                ChatStored() ? ("keeps its lines 30 days (salt in " + s_chat_salt_dir + ")").c_str() :
                               "keeps no line (no --stats-dir, or no salt's folder it can write: --chat-salt-dir)");
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
    RoomLog(room, "server-stopped");  // (01.04)
    for (const Slot& s : room.members)
    {
      if (s.used)
        SendClosed(room, s, CloseReason::Expired);
    }
    for (const Slot& s : room.watchers)  // (01.05)
    {
      if (s.used)
        SendClosed(room, s, CloseReason::Expired);
    }
  }
  std::printf("emusuni-netd: stopped\n");
  close(s_socket);
  return 0;
}
