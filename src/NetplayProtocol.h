// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Online play's packets (00.23): what the app (src/Netplay.cpp) and the servers (server/emusuni-netd.cpp) send each
// other over UDP, and what the consoles of a room send each other - directly once a hole is punched through their
// routers, else through a relay server. EmuSuNi's own protocol, not RetroArch's: there are no secrets in the app; the
// servers match consoles by a room code or a public list and pass packets only between the members of one room.
//
// Every datagram starts with 'E' 'N', the protocol version and its type, then the type's fields, little-endian. None
// is longer than MAX_PACKET (no IP fragments on any usual path). Header-only and C++17, no other library: the server
// builds it on any Linux. Nothing here throws (Reader and Writer count a short buffer as a fault).
//
// The main server (rendezvous, public list, and a relay itself):
//   Hello (padded to HELLO_SIZE) -> Cookie: a token for this address, never bigger than the Hello - a spoofed
//              address gets nothing bigger than what was sent (no amplification). Another protocol version's Hello
//              -> Refused(Version): "update EmuSuNi".
//   Info(cookie) -> ServerInfo: the relay servers (the main one first), by region.
//   Create(cookie: the game - console, title, CRC32 and size of its file -, the app's version, a nickname, the
//              console's LAN address, its local players, public or private, hide my address, its time zone)
//              -> Created (the code, the member's id, token and relay ticket)
//   Join(cookie, code, ...) -> Joined, or Refused (no such room, full, another app version, removed, too many tries)
//   List(cookie, console, page) -> Rooms: the public rooms waiting for players (the host's nickname, time zone, game,
//              its ping to the server)
//   ListAll(cookie, console, first) -> AllRooms (00.85: the Online tab's table): every room - public and private,
//              waiting for players, full or playing -, joinable ones first, as many as fit a datagram from the `first`
//              asked for, with how many there are: each its number in the list, public or private, playing or not,
//              its game, the host's nickname, time zone and ping to the server, its players and EmuSuNi version. A
//              public room's code, never a private one's (its host tells it), never an address. (As List: the cookie
//              proves the address, so the answer may be bigger than the request; the lists' rate limit counts it.)
//              A server before it drops it: the app asks it for List then (the public rooms waiting).
//   Members, whenever the room changes and in answer to every Keepalive (every 2 s): every member's nickname and
//              addresses (public as the server sees it, LAN as the console sees itself; none for one that hides them)
//   Leave, Kick (the host removes a member), Lock (the host: the match started, the room leaves the list)
//   Relay -> Relayed: a peer packet passed to another member of the same room (never an open relay)
//   Stats(cookie: a random install id, the app's version, the PS4's language, menu / playing a console / online, the
//              update checks and an update since the last one) -> StatsAck: the players with EmuSuNi open now.
//              The anonymous statistics (src/UsageStats.h; Settings -> Anonymous Statistics): every 5 minutes while
//              the app is open. The server keeps counts and the country of the address, never the address.
// Any relay server (the main one too):
//   RelayPing -> RelayPong (the console picks the relay nearest both consoles)
//   RelayOpen(ticket: room, member, expiry, signed by the main server with the servers' shared secret) -> RelayOpened
//   RelayData -> Relayed: as Relay, between the members of one room that opened on that relay
// Between the consoles (the peer packets: each carries the room's key and the sender's member id):
//   Punch / PunchAck   the hole punch, to each other's public and LAN addresses until one answers
//   Ping / Pong        the round trip (the lobby's and the game's "ping"), and whether the voice chat is on; (00.92) the
//                      ping also says what its console measured of the other (its ping, jitter, pings lost, how often
//                      it waited for its inputs, its game's speed, its delay), the pong whether it was busy (late)
//   Control (+ Ack)    sent again until acknowledged: the lobby's state, the relay chosen, start, ready, go, resync,
//                      quit, (00.86) the host's "come in" to a guest that asked to join
//   Bulk (+ BulkAck)   a big block in 1 KiB chunks, acknowledged by bitmap: the host's core options, its save state
//   Input              each console's local players' inputs, every frame, with the frames not acknowledged yet again
//                      (a lost packet costs nothing), its frame (time sync) and its last state checksum
//   Voice              a 20 ms Opus packet of the voice chat (src/Voice.h), never sent again

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace NetProto
{
constexpr uint8_t MAGIC0 = 'E', MAGIC1 = 'N';
// The protocol's version, in every datagram: a console of another version is told to update EmuSuNi.
constexpr uint8_t VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 7777;
constexpr size_t MAX_PACKET = 1400;
constexpr size_t HEADER = 4;
constexpr int MAX_MEMBERS = 4;  // consoles in a room (00.95: the host chooses 2 to 4; before, rooms of 2)
constexpr int MAX_PORTS = 4;    // the console's controller ports a match fills, every console's players together
constexpr int CODE_LENGTH = 6;
// The room codes' letters: no 0/O, 1/I/L - read aloud or from a screen, never mistaken (31 letters, 6 of them:
// ~887 million codes).
constexpr char CODE_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
constexpr size_t MAX_NICK = 32, NICK_LETTERS = 16, MAX_TITLE = 120, MAX_CONSOLE = 16, MAX_APP_VERSION = 16;
constexpr size_t MAX_REGION = 12, MAX_RELAY_NAME = 24;
constexpr size_t COOKIE_SIZE = 16;
constexpr size_t MAC_SIZE = 16;
constexpr size_t TICKET_SIZE = 8 + 1 + 4 + MAC_SIZE;  // room id, member, expiry, signature
// Requests padded to these sizes: their answers are never bigger (a spoofed address gets no amplification).
constexpr size_t HELLO_SIZE = 48;
constexpr size_t RELAY_PING_SIZE = 32;
constexpr size_t RELAY_OPEN_SIZE = 64;
constexpr size_t STATS_SIZE = 112;
// A Bulk chunk's data, and the chunks a BulkAck's bitmap covers past its base.
constexpr size_t BULK_CHUNK = 1024;
constexpr int BULK_WINDOW = 256;
constexpr int MAX_RELAYS = 16;
constexpr int ROOMS_PER_PAGE = 8;

enum class Type : uint8_t
{
  // console -> main server
  Hello = 1,
  Info = 2,
  Create = 3,
  Join = 4,
  List = 5,
  Keepalive = 6,
  Leave = 7,
  Relay = 8,
  Kick = 9,
  Lock = 10,
  Stats = 11,  // (the anonymous statistics' heartbeat)
  ListAll = 12,  // (00.85: every room, for the Online tab's table)
  // main server -> console
  Cookie = 20,
  ServerInfo = 21,
  Created = 22,
  Joined = 23,
  Refused = 24,
  Members = 25,
  Relayed = 26,  // (a relay server's too)
  Closed = 27,
  Rooms = 28,
  StatsAck = 29,
  // console <-> any relay server
  RelayPing = 30,
  RelayPong = 31,
  RelayOpen = 32,
  RelayOpened = 33,
  RelayData = 34,
  // main server -> console again (20-29 were taken)
  AllRooms = 35,  // (00.85: ListAll's answer)
  // console <-> console (directly, or inside Relay / RelayData and Relayed)
  Punch = 40,
  PunchAck = 41,
  Ping = 42,
  Pong = 43,
  Control = 44,
  ControlAck = 45,
  Bulk = 46,
  BulkAck = 47,
  Input = 48,
  Voice = 49,
};

// Why the server refused a request.
enum class Refusal : uint8_t
{
  NoRoom = 1,       // no room with that code (mistyped, or closed)
  Full = 2,         // the room has its players
  Version = 3,      // another version of EmuSuNi (detail: the room's, or the server's protocol)
  TooMany = 4,      // too many tries from this address: wait a minute
  ServerFull = 5,   // the server has too many rooms
  BadRequest = 6,   // a packet the server couldn't read
  Started = 7,      // the room's game started already
  BadCookie = 8,    // the cookie is old: Hello again
  Kicked = 9,       // the host removed this console from that room
};

// Why a room closed for a member (Closed).
enum class CloseReason : uint8_t
{
  HostLeft = 1,
  Expired = 2,
  Removed = 3,  // this member was dropped (silent too long)
  Kicked = 4,   // the host removed it
};

// Create's and Join's flags.
constexpr uint8_t FLAG_PUBLIC = 1;  // the room is in the public list (Create)
constexpr uint8_t FLAG_HIDDEN = 2;  // this member's addresses aren't given to the others: always through a relay

// The Control messages.
enum class ControlKind : uint8_t
{
  Lobby = 1,          // a guest's state in the lobby: its game (by its file or only its title), its relays' pings
  StartGame = 2,      // the host: everyone starts the game now
  Ready = 3,          // a guest: the host's state is loaded (its local players)
  Go = 4,             // the host: frame 0 now (input delay, rollback frames, every member's ports)
  ResyncRequest = 5,  // a guest: our states differ (the frame): the host sends its state again
  Quit = 6,           // leaving the match (why)
  RelayChoice = 7,    // the host: the relay server both consoles use (its index in ServerInfo)
  RelayReady = 8,     // a console opened on that relay: its packets may come through it
  Suggest = 9,        // a guest of a Choose Together room: the game it would like (the host sees it marked)
  EndMatch = 10,      // a Choose Together room's match ends: back to the room's list of common games
  // (00.86) The host lets a guest in. A console that joins asks to: until the host accepts it, it waits ("Waiting for
  // the host to accept you") and is in no match, list of games or voice chat. Rejected, it is told so by a Quit of
  // reason QUIT_REFUSED (and the server removes it: it can't ask again in that room); a request not answered in time,
  // by QUIT_NO_ANSWER (it leaves by itself). Only between consoles of one app version (the server's Join checks it):
  // the server knows nothing of it. (00.95) Its byte: every member the host accepted, a bit each (the host's too) -
  // sent to each of them whenever it accepts one, so that in a room of three or four the guests know who is in.
  Admit = 11,
  // (00.95) A Game Boy link cable room's (src/GbLink.h; its console NetProto::GBLINK): a guest's own game for Game Boy 2
  // (console, CRC32, size, title), to the host once it is accepted; the host's answer, one byte - a LobbyGame: Same (it
  // has that file too), Missing (it hasn't).
  LinkGame = 12,
};

// A Quit's reason (its one byte).
constexpr uint8_t QUIT_LEFT = 0;       // the player left
constexpr uint8_t QUIT_REMOVED = 1;    // the host removed this console
constexpr uint8_t QUIT_REFUSED = 2;    // (00.86) the host didn't accept this console's request to join
constexpr uint8_t QUIT_NO_ANSWER = 3;  // (00.86) the host didn't answer it in time

// A Choose Together room's "game" (Create's console): no game yet - the players choose one of the games they both have.
constexpr char TOGETHER[] = "*";
// (00.95) A Game Boy link cable room's console (src/GbLink.h): two Game Boys joined by the cable, the room's game Game
// Boy 1's (the host's: a Game Boy or Game Boy Color game), the guest's own Game Boy 2's (LinkGame).
constexpr char GBLINK[] = "gblink";

// What a Lobby control says of the guest's game.
enum class LobbyGame : uint8_t
{
  Looking = 0,
  Same = 1,       // the same file (CRC32 and size)
  Different = 2,  // another file of a game by that title: the guest chose to play anyway
  Missing = 3,    // not in the guest's games
  Asking = 4,     // another file by that title: the guest hasn't said yet
};

// What a Bulk carries.
enum class BulkKind : uint8_t
{
  Options = 1,  // the host's core options (before the game loads)
  State = 2,    // the host's save state, battery save and controller devices (the match's start)
  Resync = 3,   // the same during the match, at a frame
  Games = 4,    // a Choose Together room: a guest's games (console, CRC32, size of each), to the host
  Common = 5,   // ... and the host's answer: those both have
  LinkSave = 6,  // (00.95) a Game Boy link cable room's guest: its battery save (GbLink::PackSave's), to the host
};

// A game of a library as a Choose Together room compares them: its console and its file's CRC32 and size.
struct LibraryGame
{
  std::string console;
  uint32_t crc = 0;
  uint64_t size = 0;
  bool operator<(const LibraryGame& o) const
  {
    return console != o.console ? console < o.console : crc != o.crc ? crc < o.crc : size < o.size;
  }
  bool operator==(const LibraryGame& o) const { return console == o.console && crc == o.crc && size == o.size; }
};

// ---- Little-endian writing and reading, bounds-checked ----
class Writer
{
public:
  Writer(uint8_t* buffer, size_t capacity) : m_buf(buffer), m_cap(capacity) {}
  void U8(uint8_t v) { Put(&v, 1); }
  void U16(uint16_t v)
  {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    Put(b, 2);
  }
  void U32(uint32_t v)
  {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    Put(b, 4);
  }
  void U64(uint64_t v)
  {
    U32(static_cast<uint32_t>(v));
    U32(static_cast<uint32_t>(v >> 32));
  }
  void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
  // A string of at most `limit` bytes (longer: cut, not inside a UTF-8 letter), its length first (one byte).
  void Str(const std::string& s, size_t limit)
  {
    size_t n = std::min(s.size(), std::min(limit, size_t{255}));
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
      n--;
    U8(static_cast<uint8_t>(n));
    Put(s.data(), n);
  }
  void Bytes(const void* data, size_t n) { Put(data, n); }
  // Zeros up to `size` bytes in all (a request padded: its answer is never bigger).
  void PadTo(size_t size)
  {
    while (m_ok && m_len < size)
      U8(0);
  }
  // The header every datagram starts with.
  void Header(Type type, uint8_t version = VERSION)
  {
    U8(MAGIC0);
    U8(MAGIC1);
    U8(version);
    U8(static_cast<uint8_t>(type));
  }
  size_t Size() const { return m_len; }
  bool Ok() const { return m_ok; }

private:
  void Put(const void* data, size_t n)
  {
    if (!m_ok || n > m_cap - m_len)
    {
      m_ok = false;
      return;
    }
    if (n > 0)
      std::memcpy(m_buf + m_len, data, n);
    m_len += n;
  }
  uint8_t* m_buf;
  size_t m_cap;
  size_t m_len = 0;
  bool m_ok = true;
};

class Reader
{
public:
  Reader(const uint8_t* data, size_t size) : m_p(data), m_n(size) {}
  uint8_t U8()
  {
    uint8_t v = 0;
    Get(&v, 1);
    return v;
  }
  uint16_t U16()
  {
    uint8_t b[2] = {};
    Get(b, 2);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  }
  uint32_t U32()
  {
    uint8_t b[4] = {};
    Get(b, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
  }
  uint64_t U64()
  {
    const uint64_t lo = U32();
    return lo | (static_cast<uint64_t>(U32()) << 32);
  }
  int16_t I16() { return static_cast<int16_t>(U16()); }
  std::string Str(size_t limit)
  {
    const size_t n = U8();
    if (!m_ok || n > m_n - m_at || n > limit)
    {
      m_ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(m_p + m_at), n);
    m_at += n;
    return s;
  }
  // n bytes in place (nullptr: not that many left).
  const uint8_t* Bytes(size_t n)
  {
    if (!m_ok || n > m_n - m_at)
    {
      m_ok = false;
      return nullptr;
    }
    const uint8_t* p = m_p + m_at;
    m_at += n;
    return p;
  }
  size_t Left() const { return m_ok ? m_n - m_at : 0; }
  const uint8_t* Here() const { return m_p + m_at; }
  bool Ok() const { return m_ok; }

private:
  void Get(void* out, size_t n)
  {
    if (!m_ok || n > m_n - m_at)
    {
      m_ok = false;
      return;
    }
    std::memcpy(out, m_p + m_at, n);
    m_at += n;
  }
  const uint8_t* m_p;
  size_t m_n;
  size_t m_at = 0;
  bool m_ok = true;
};

// The datagram's type, when it is one of ours of this version (else false: dropped).
inline bool ReadHeader(Reader& r, Type* type)
{
  const uint8_t m0 = r.U8(), m1 = r.U8(), version = r.U8(), t = r.U8();
  if (!r.Ok() || m0 != MAGIC0 || m1 != MAGIC1 || version != VERSION)
    return false;
  *type = static_cast<Type>(t);
  return true;
}
// A datagram of ours, of any version (the server answers another version's Hello with Refused).
inline bool IsOurs(const uint8_t* data, size_t size, uint8_t* version)
{
  if (size < HEADER || data[0] != MAGIC0 || data[1] != MAGIC1)
    return false;
  *version = data[2];
  return true;
}

// ---- Room codes ----
// A code as typed: capitals, spaces and dashes left out.
inline std::string NormalizeCode(const std::string& typed)
{
  std::string code;
  for (const char c : typed)
  {
    if (c == ' ' || c == '-' || c == '_')
      continue;
    code += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  return code;
}
inline bool ValidCode(const std::string& code)
{
  if (code.size() != static_cast<size_t>(CODE_LENGTH))
    return false;
  for (const char c : code)
  {
    if (c == '\0' || std::strchr(CODE_ALPHABET, c) == nullptr)
      return false;
  }
  return true;
}
inline void WriteCode(Writer& w, const std::string& code)
{
  char c[CODE_LENGTH] = {};
  for (int i = 0; i < CODE_LENGTH && i < static_cast<int>(code.size()); i++)
    c[i] = code[static_cast<size_t>(i)];
  w.Bytes(c, CODE_LENGTH);
}
inline std::string ReadCode(Reader& r)
{
  const uint8_t* c = r.Bytes(CODE_LENGTH);
  return c ? std::string(reinterpret_cast<const char*>(c), CODE_LENGTH) : std::string();
}

// ---- Texts strangers see in the public list: nicknames, game titles, time zones ----
// Printable, at most `most` letters (UTF-8), the words of a short list (English and Spanish insults and slurs)
// starred; nothing left: `empty`. The app (its own) and the server (everyone's) apply it.
inline std::string CleanText(const std::string& in, size_t most, const char* empty)
{
  std::string out;
  size_t letters = 0;
  for (size_t i = 0; i < in.size() && letters < most;)
  {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || i + n > in.size())
      break;
    bool ok = n == 1 ? (c >= 0x20 && c != 0x7F) : true;
    for (size_t k = 1; k < n; k++)
      ok = ok && (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
    if (ok)
    {
      out.append(in, i, n);
      letters++;
    }
    i += n;
  }
  static const char* const WORDS[] = {"fuck",  "shit",   "cunt",  "nigger", "nigga",  "faggot", "bitch",
                                      "whore", "slut",   "retard", "nazi",  "hitler", "puta",   "puto",
                                      "mierda", "verga", "pendejo", "maricon", "cabron", "culero", "gilipollas",
                                      "zorra", "polla",  "joto"};
  std::string lower = out;
  for (char& c : lower)
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  for (const char* word : WORDS)
  {
    const size_t len = std::strlen(word);
    for (size_t at = lower.find(word); at != std::string::npos; at = lower.find(word, at + len))
    {
      for (size_t k = 0; k < len; k++)
        out[at + k] = lower[at + k] = '*';
    }
  }
  size_t first = out.find_first_not_of(' '), last = out.find_last_not_of(' ');
  if (first == std::string::npos)
    return empty;
  return out.substr(first, last - first + 1);
}
inline std::string CleanNick(const std::string& in)
{
  return CleanText(in, NICK_LETTERS, "Player");
}

// ---- An IPv4 address as the packets carry it: the address and the port in host order ----
struct Address
{
  uint32_t ip = 0;  // host order: 0x7F000001 is 127.0.0.1
  uint16_t port = 0;
  bool Valid() const { return ip != 0 && port != 0; }
  bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
  bool operator!=(const Address& o) const { return !(*this == o); }
};
inline void WriteAddress(Writer& w, const Address& a)
{
  w.U32(a.ip);
  w.U16(a.port);
}
inline Address ReadAddress(Reader& r)
{
  Address a;
  a.ip = r.U32();
  a.port = r.U16();
  return a;
}
inline std::string AddressText(const Address& a)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", (a.ip >> 24) & 0xFF, (a.ip >> 16) & 0xFF, (a.ip >> 8) & 0xFF,
                a.ip & 0xFF, a.port);
  return text;
}

// ---- A member as Members lists it ----
struct Member
{
  uint8_t id = 0;
  std::string nick;
  Address pub;  // as the server sees it (the router's side); none for a member that hides it
  Address lan;  // as the console sees itself
  uint8_t locals = 1;
  uint8_t flags = 0;  // FLAG_HIDDEN
  std::string region;
};

// ---- The game a room plays ----
struct GameInfo
{
  std::string console;  // the console's key ("snes", "md")
  std::string title;
  uint32_t crc = 0;     // CRC32 of the game's file (a file over 64 MiB: of its first and last 8 MiB)
  uint64_t size = 0;    // the file's size
};
inline void WriteGame(Writer& w, const GameInfo& g)
{
  w.Str(g.console, MAX_CONSOLE);
  w.Str(g.title, MAX_TITLE);
  w.U32(g.crc);
  w.U64(g.size);
}
inline GameInfo ReadGame(Reader& r)
{
  GameInfo g;
  g.console = r.Str(MAX_CONSOLE);
  g.title = r.Str(MAX_TITLE);
  g.crc = r.U32();
  g.size = r.U64();
  return g;
}

// ---- A public room as Rooms lists it (and any room as AllRooms does) ----
struct RoomEntry
{
  std::string code;     // (AllRooms: a public room's only)
  GameInfo game;
  std::string host;     // the host's nickname
  std::string region;   // its time zone ("UTC-5")
  uint8_t players = 1, max_players = 2;
  uint16_t host_ms = 0;  // the host's round trip to the server (with the asker's own: an estimate of the ping)
  // (00.85, AllRooms.) The room's number in the list - neither its code nor its key: the table keeps its selection by
  // it -, public or private, its match started (no one joins it then), the EmuSuNi version it was made with (a console
  // of another one can't join it). Rooms's rooms: public ones waiting, of any version (none said).
  uint32_t id = 0;
  bool is_public = true;
  bool playing = false;
  std::string app_version;
};

// AllRooms' rooms: their flags, then the code when it is public.
constexpr uint8_t ROOM_PUBLIC = 1;
constexpr uint8_t ROOM_PLAYING = 2;
inline void WriteListedRoom(Writer& w, const RoomEntry& e)
{
  w.U32(e.id);
  w.U8(static_cast<uint8_t>((e.is_public ? ROOM_PUBLIC : 0) | (e.playing ? ROOM_PLAYING : 0)));
  if (e.is_public)
    WriteCode(w, e.code);
  WriteGame(w, e.game);
  w.Str(e.host, MAX_NICK);
  w.Str(e.region, MAX_REGION);
  w.U8(e.players);
  w.U8(e.max_players);
  w.U16(e.host_ms);
  w.Str(e.app_version, MAX_APP_VERSION);
}
inline RoomEntry ReadListedRoom(Reader& r)
{
  RoomEntry e;
  e.id = r.U32();
  const uint8_t flags = r.U8();
  e.is_public = (flags & ROOM_PUBLIC) != 0;
  e.playing = (flags & ROOM_PLAYING) != 0;
  if (e.is_public)
    e.code = ReadCode(r);
  e.game = ReadGame(r);
  e.host = r.Str(MAX_NICK);
  e.region = r.Str(MAX_REGION);
  e.players = r.U8();
  e.max_players = r.U8();
  e.host_ms = r.U16();
  e.app_version = r.Str(MAX_APP_VERSION);
  return e;
}

// ---- A relay server as ServerInfo lists it ----
struct RelayInfo
{
  uint8_t index = 0;
  std::string name, region;
  Address address;
};

// ---- A relay ticket: the main server's word that a console is a member of a room, for the relays ----
struct Ticket
{
  uint64_t room = 0;  // the room's relay id (neither its code nor its key)
  uint8_t member = 0;
  uint32_t expiry = 0;  // Unix time
  uint8_t mac[MAC_SIZE] = {};
};
inline void WriteTicket(Writer& w, const Ticket& t)
{
  w.U64(t.room);
  w.U8(t.member);
  w.U32(t.expiry);
  w.Bytes(t.mac, MAC_SIZE);
}
inline Ticket ReadTicket(Reader& r)
{
  Ticket t;
  t.room = r.U64();
  t.member = r.U8();
  t.expiry = r.U32();
  if (const uint8_t* mac = r.Bytes(MAC_SIZE))
    std::memcpy(t.mac, mac, MAC_SIZE);
  return t;
}

// ---- One player's input of one frame, as the Input packets carry it ----
// The RetroPad's 16 buttons; the light gun's reload (a NAOMI's or Atomiswave's gun game, flags bit 0); the analog
// sticks (left x, y, right x, y) and the Dreamcast's analog triggers. A console without analog controls sends the
// buttons alone (2 bytes a player a frame).
struct PortInput
{
  uint16_t buttons = 0;
  uint16_t flags = 0;
  int16_t axes[4] = {};
  int16_t triggers[2] = {};
  bool operator==(const PortInput& o) const
  {
    return buttons == o.buttons && flags == o.flags && std::memcmp(axes, o.axes, sizeof(axes)) == 0 &&
           std::memcmp(triggers, o.triggers, sizeof(triggers)) == 0;
  }
  bool operator!=(const PortInput& o) const { return !(*this == o); }
};
constexpr size_t INPUT_BUTTONS = 2;  // bytes of a buttons-only input
constexpr size_t INPUT_FULL = 16;    // bytes of a full one
inline void WriteInput(Writer& w, const PortInput& in, bool full)
{
  w.U16(in.buttons);
  if (!full)
    return;
  w.U16(in.flags);
  for (const int16_t a : in.axes)
    w.I16(a);
  for (const int16_t t : in.triggers)
    w.I16(t);
}
inline PortInput ReadInput(Reader& r, bool full)
{
  PortInput in;
  in.buttons = r.U16();
  if (!full)
    return in;
  in.flags = r.U16();
  for (int16_t& a : in.axes)
    a = r.I16();
  for (int16_t& t : in.triggers)
    t = r.I16();
  return in;
}

// ---- The anonymous statistics' heartbeat (src/UsageStats.h): Stats, after the cookie and the nonce ----
constexpr size_t INSTALL_ID_SIZE = 16;
constexpr size_t MAX_LANGUAGE = 8;
constexpr uint8_t STATS_NEW_INSTALL = 1;  // the install's first heartbeat answered (made since the last one)
// The opt-in online profile (Settings -> Online Play -> Online Profile, off unless the player turns it on): with
// STATS_PROFILE the server keeps this install's history (minutes, sessions, online matches by game); STATS_PUBLIC puts
// it in the rankings under `nick`; STATS_DELETE deletes it (with STATS_NO_COUNT: a heartbeat that only asks that, sent
// even with the statistics off - it isn't counted).
constexpr uint8_t STATS_PROFILE = 2;
constexpr uint8_t STATS_PUBLIC = 4;
constexpr uint8_t STATS_DELETE = 8;
constexpr uint8_t STATS_NO_COUNT = 16;
// StatsAck's profile byte (after the players online and the next heartbeat's seconds), then the profile's code.
enum class ProfileState : uint8_t
{
  None = 0,     // no profile (or a server before them)
  Kept = 1,     // kept: its code follows ("XXXX-XXXX-XXXX-XXXX": the website shows it with that code)
  Deleted = 2,  // deleted, with every minute of it
};
constexpr size_t PROFILE_CODE_SIZE = 19;
enum class StatsState : uint8_t
{
  Menu = 0,           // the menus
  Playing = 1,        // a game (console)
  Room = 2,           // online play's room or its public list, no game yet
  PlayingOnline = 3,  // an online match (console)
};
struct Heartbeat
{
  uint8_t id[INSTALL_ID_SIZE] = {};  // random, made once by the app (stats.ini): never the PS4's or a user's
  uint8_t flags = 0;
  std::string app_version;   // "00.23"
  std::string language;      // the PS4's: "es", "en-US", ...
  StatsState state = StatsState::Menu;
  std::string console;       // the console's key while a game runs ("snes"), else ""
  uint8_t update_checks = 0;  // update checks since the last heartbeat answered
  std::string updated_from;   // the version this install ran before, once after an update ("": none)
  // The game being played, never its file's name: "c:1a2b3c4d" (a cartridge's CRC32, as No-Intro's lists have it),
  // "s:mslug" (an arcade set), "d:SLUS-00067" (a disc's code); "" when not known. The server keeps it only as a
  // count, and shows a title (from those lists, never from the app) only once 3 installs or more played it.
  std::string game;
  std::string nick;  // STATS_PUBLIC: the rankings' name (the online play nickname, cut and filtered again there)
};
constexpr size_t MAX_GAME_ID = 24;
inline void WriteHeartbeat(Writer& w, const Heartbeat& h)
{
  w.Bytes(h.id, INSTALL_ID_SIZE);
  w.U8(h.flags);
  w.Str(h.app_version, MAX_APP_VERSION);
  w.Str(h.language, MAX_LANGUAGE);
  w.U8(static_cast<uint8_t>(h.state));
  w.Str(h.console, MAX_CONSOLE);
  w.U8(h.update_checks);
  w.Str(h.updated_from, MAX_APP_VERSION);
  w.Str(h.game, MAX_GAME_ID);
  w.Str(h.nick, MAX_NICK);
}
inline Heartbeat ReadHeartbeat(Reader& r)
{
  Heartbeat h;
  if (const uint8_t* id = r.Bytes(INSTALL_ID_SIZE))
    std::memcpy(h.id, id, INSTALL_ID_SIZE);
  h.flags = r.U8();
  h.app_version = r.Str(MAX_APP_VERSION);
  h.language = r.Str(MAX_LANGUAGE);
  h.state = static_cast<StatsState>(r.U8());
  h.console = r.Str(MAX_CONSOLE);
  h.update_checks = r.U8();
  h.updated_from = r.Str(MAX_APP_VERSION);
  // (An app before the game's field: the padding's zeros read as "".)
  if (r.Left() > 0)
    h.game = r.Str(MAX_GAME_ID);
  if (r.Left() > 0)
    h.nick = r.Str(MAX_NICK);
  return h;
}
}  // namespace NetProto
