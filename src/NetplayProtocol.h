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
//              -> Created (the code, the member's id, token and relay ticket). (01.05) After them, the version of the
//              host's file as its name says ("USA, Rev 1"), for the list: a server before it reads up to the time zone.
//              (01.06) Then what the app plays online and how (WriteNetCaps) and its file's whole CRC32 and size (an
//              app before 01.05 knows a game by them: GameInfo::file_crc).
//   Join(cookie, code, ...) -> Joined, or Refused (no such room, full, another app version, removed, too many tries)
//              (01.06) Then WriteNetCaps: two versions play a room's match together when its console's online level is
//              the same in both (PlayTogether) - a server before it asks for the same version.
//   List(cookie, console, page) -> Rooms: the public rooms waiting for players (the host's nickname, time zone, game,
//              its ping to the server)
//   ListAll(cookie, console, first) -> AllRooms (00.85: the Online tab's table): every room - public and private,
//              waiting for players, full or playing -, joinable ones first, as many as fit a datagram from the `first`
//              asked for, with how many there are: each its number in the list, public or private, playing or not,
//              its game, the host's nickname, time zone and ping to the server, its players and EmuSuNi version. A
//              public room's code, never a private one's (its host tells it), never an address. (As List: the cookie
//              proves the address, so the answer may be bigger than the request; the lists' rate limit counts it.)
//              A server before it drops it: the app asks it for List then (the public rooms waiting). (01.05) After
//              the rooms, each host's country and its game's version (EXTRA_VERSION). (01.06) The asker says
//              WriteNetCaps after `first`: one that doesn't gets each game by its whole file's CRC32 (the rooms of a
//              host of 01.06 on); after the rooms' extras, each host's level of its console and caps (EXTRA2_VERSION).
//   (01.05) TopGames(cookie, days) -> TopGamesList: the games most played online those days (counts by game only).
//   Members, whenever the room changes and in answer to every Keepalive (every 2 s): every member's nickname and
//              addresses (public as the server sees it, LAN as the console sees itself; none for one that hides them).
//              (01.05) After them, how many watch and each member's country (EXTRA_VERSION): the country of the
//              address, looked up by the server - never the address -, none for one that doesn't show it. (01.06)
//              Then each member's app version and caps (EXTRA2_VERSION); the room's game by the CRC32 that member's
//              app knows it by (an app before 01.05: its whole file's, when the host said it).
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
#include <utility>
#include <vector>

namespace NetProto
{
constexpr uint8_t MAGIC0 = 'E', MAGIC1 = 'N';
// The protocol's version, in every datagram: a console of another version is told to update EmuSuNi.
constexpr uint8_t VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 7777;
constexpr size_t MAX_PACKET = 1400;
constexpr size_t HEADER = 4;
constexpr int MAX_MEMBERS = 4;  // consoles in a room (00.95: the host chooses 2 to 4; before, rooms of 2)
// (01.05) Spectators of a room ("Mirar": they watch its match, play nothing), their member ids from FIRST_WATCHER. The
// server lists them only to the host and to themselves (the players never see one: an app before it skips the ids
// past MAX_MEMBERS anyway); only a host that lets them (NOTE_WATCH) gets any.
constexpr int MAX_WATCHERS = 4;
constexpr int FIRST_WATCHER = MAX_MEMBERS;
constexpr int MAX_SLOTS = MAX_MEMBERS + MAX_WATCHERS;
constexpr int MAX_PORTS = 4;    // the console's controller ports a match fills, every console's players together
constexpr int CODE_LENGTH = 6;
// The room codes' letters: no 0/O, 1/I/L - read aloud or from a screen, never mistaken (31 letters, 6 of them:
// ~887 million codes).
constexpr char CODE_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
constexpr size_t MAX_NICK = 32, NICK_LETTERS = 16, MAX_TITLE = 120, MAX_CONSOLE = 16, MAX_APP_VERSION = 16;
constexpr size_t MAX_REGION = 12, MAX_RELAY_NAME = 24;
// (01.05) A country as the others see it: two capitals (ISO 3166-1, "CL"). A game's version as its file's name says it
// ("USA, Rev 1"): at most MAX_GAME_VERSION bytes, GAME_VERSION_LETTERS letters.
constexpr size_t COUNTRY_SIZE = 2, MAX_GAME_VERSION = 48, GAME_VERSION_LETTERS = 28;
// (01.05) The games most played online (TopGamesList): how many, their titles' bytes at most, the days counted.
constexpr int TOP_GAMES = 12, TOP_DAYS = 7;
constexpr size_t MAX_TOP_TITLE = 48;
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
  // (01.04) The host tells the server what only the consoles know, for the room's history (server/emusuni-netd.cpp's
  // room_log; never shown to anyone): the server's auth (code, member, token), a NOTE_* and its number. No answer.
  RoomNote = 13,
  // (01.05, the owner) The games most played online (cookie, nonce, U8 days) -> TopGamesList: the main server's
  // room_log of those days, by game - counts only (no nickname, no address, no code). A server before it drops it.
  TopGames = 14,
  // (01.07, the owner: "un chat mundial") The world chat (WORLD CHAT below): its lines since one, a line said, a line
  // reported. A server before it drops them.
  WorldChat = 15,
  WorldSay = 16,
  WorldReport = 17,
  // (01.07, the owner: "no quiero que se hablen cosas sensibles o ilegales") A copy of a line of the room's chat, from
  // the member that said it (the server's auth - code, member, token -, U8 flags - WORLD_NOTICE_SEEN -, the chat key,
  // U8 the phrase's number then Str its parameter, or 0 then Str the text), kept as the world chat's lines are
  // (WORLD CHAT): only a member's, only once the room's notice was shown. No answer. The line itself goes from console
  // to console as ever.
  RoomChatCopy = 18,
  // (01.07, the owner) The players' ranking: TopPlayers(cookie, nonce, U8 days: TOP_DAYS, or 0 since ever) ->
  // TopPlayersList(U32 nonce, U8 days, U8 count, each: Str nickname, Str country ("" not shown), U32 seconds in
  // matches, Str the console played most) - TOP_PLAYERS at most, the most first. The server counts each ranked
  // player's time in matches (WritePlayer at Create's and Join's end: PLAYER_RANKED), by its code (the chat's), never
  // an address. A server before it drops it.
  TopPlayers = 19,
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
  // (01.05) TopGames' answer: U32 nonce, U8 days, U8 count, then each game - Str console (MAX_CONSOLE), U32 its
  // content's CRC32 (the rooms' GameInfo::crc), U32 matches started, Str a public room's title of it (MAX_TOP_TITLE;
  // "" none) - the most played first, TOP_GAMES at most.
  TopGamesList = 36,
  // (01.07) WorldChat's and WorldSay's answers, TopPlayers'.
  WorldLines = 37,
  WorldSaid = 38,
  TopPlayersList = 39,
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
  // (01.05) A match watched (MAX_WATCHERS): the host's final inputs - every port's, frame by frame, with its newest
  // checksum - to a spectator; the spectator's answer, the newest frame it has every input of. Only between a host and
  // its spectators (an app before it never lets one in).
  Watch = 50,
  WatchAck = 51,
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
  // (01.05) A spectator's Join (FLAG_WATCH): the host doesn't let spectators watch (detail "full": MAX_WATCHERS watch
  // already). Only a spectator's Join is refused so.
  NoWatch = 10,
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
// (01.05) Join: as a spectator, in a room whose host lets them (also while its match plays). A server before it would
// take it as a player: the app asks so only of a room listed as watchable (ROOM_WATCHABLE), or one a Started refusal
// says is ("watch": WATCH_DETAIL).
constexpr uint8_t FLAG_WATCH = 4;
constexpr char WATCH_DETAIL[] = "watch";
// (01.05) Create and Join: this member's country isn't shown to the others (Members and AllRooms say none for it).
// A server before it reads only the first three.
constexpr uint8_t FLAG_NO_COUNTRY = 8;

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
  // A Choose Together room's match ends: back to the room's list of common games. (01.23, the owner: "salir del juego
  // sin salir de la sala") Any room's (not a link cable's), when every player's app has CAP_ROOM: the match ends for
  // every player, all back in the room; the host's to its spectators (CAP_ROOM's: back in it too, the next match
  // theirs to watch).
  EndMatch = 10,
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
  // (01.05) The room's chat, from a member the host accepted to every other one: a quick phrase (CHAT_PHRASE, U8 its
  // number: ChatPhrase - each console shows it in its own language) or a line of text (CHAT_TEXT, Str of CHAT_BYTES
  // at most: CleanChat again where it arrives). Nothing of it goes to the server (a relay passes it on, unread and
  // unkept - but the packets between consoles aren't encrypted). An app before it acknowledges it and ignores it.
  Chat = 13,
  // (01.05) A spectator to the host: its game loaded (or its state found different), the host's state please - the
  // host sends it (BulkKind::Resync) at a frame whose inputs are final, and its inputs from there (Type::Watch).
  WatchState = 14,
  // (01.05) A player joining the match under way (NOTE_LATE). The host to it: the match is on - start the game
  // (LateStart; then the options); it to the host: its game loaded (LateReady, U8 its players). The host to the
  // players: stop at a frame (JoinPause: U32 frame, U8 member, Str nickname) - a console past it goes back to it; to
  // the newcomer, the ports and the match's rules from that frame (LateGo: U32 frame, U8 delay, U8 rollback, U16
  // checksum frames, U8 count, then member, first port, ports each - itself too) and its state of it (BulkKind::State);
  // the newcomer's Ready; then to the players: on from that frame with it (JoinResume: U32 frame, U8 member - JOIN_NONE
  // none came in -, U8 first port, U8 ports).
  LateStart = 15,
  LateReady = 16,
  JoinPause = 17,
  LateGo = 18,
  JoinResume = 19,
  // (01.05) A player's file of the room's game, to every other player: U8 a LobbyGame (the host's: Same), Str its
  // version as its name says ("USA, Rev 1": MAX_GAME_VERSION; "" none or not found) - each card of the room says it.
  GameVersion = 20,
  // (01.14, the owner: Gabriel's "reiniciar en partida online") The host to the players: the game is reset at a frame
  // (U32), past every console's (as JoinPause's). Each player stops there (one past it goes back to it), and once every
  // input before it is in - the state of that frame the same on every console - resets the core and goes on. Only when
  // every player's app has CAP_RESET. The spectators get the host's state again after it.
  ResetAt = 21,
};
constexpr uint8_t JOIN_NONE = 0xFF;

constexpr uint8_t CHAT_PHRASE = 1, CHAT_TEXT = 2;
constexpr size_t CHAT_LETTERS = 100, CHAT_BYTES = 250;
// The quick phrases' English (PS4Lang translates them where they are shown); their numbers never change - a new one
// goes at the end. "" for 0 and a number past the end (a newer app's: not shown).
// (01.07, the owner) 13-20 more. PHRASE_FROM's "{}" is its sayer's country - never said by the sayer: the others fill
// it with the country the server shows of it (Members' for the room's chat, the server's own for the world chat's);
// none shown, the phrase isn't shown (nor said).
// (01.15, the owner) 21, a laugh. An app before it (01.05-01.14) drops the number as a newer app's: its player sees
// nothing of that line - the room's chat and the world chat alike (Netplay.cpp's ReceiveChat, the world's lines); the
// server takes numbers up to 64 (MAX_WORLD_PHRASE).
inline const char* ChatPhrase(int id)
{
  static const char* const PHRASES[] = {"",           "Hello!",       "Ready?",     "Ready!",  "Good game!",
                                        "One moment, please",         "Another game?",       "You choose",
                                        "Thanks!",    "Nice!",        "There's lag", "I have to go", "See you!",
                                        "I'm fine",   "How are you?", "Where are you from?", "I'm from {}",
                                        "Yes",        "No",           "Good luck!", "Let's play!", "Hahaha!"};
  return id > 0 && id < static_cast<int>(sizeof(PHRASES) / sizeof(PHRASES[0])) ? PHRASES[id] : "";
}
constexpr int CHAT_PHRASES = 21;
constexpr int PHRASE_FROM = 16;

// A Quit's reason (its one byte).
constexpr uint8_t QUIT_LEFT = 0;       // the player left
constexpr uint8_t QUIT_REMOVED = 1;    // the host removed this console
constexpr uint8_t QUIT_REFUSED = 2;    // (00.86) the host didn't accept this console's request to join
constexpr uint8_t QUIT_NO_ANSWER = 3;  // (00.86) the host didn't answer it in time
constexpr uint8_t QUIT_NO_PORT = 4;    // (01.05) the match under way has no controller port left for it

// (01.04) RoomNote's kinds (the host's; numbers only - never a title or a nickname).
constexpr uint8_t NOTE_COMMON = 1;   // a Choose Together room's games every player has: U16 how many (0: none)
constexpr uint8_t NOTE_REFUSED = 2;  // a request to join ended by the host: U8 QUIT_REFUSED or QUIT_NO_ANSWER
// (01.05) Spectators: U8 1 the host lets them watch (MAX_WATCHERS), 0 no more (those in stay till they leave). A server
// before it ignores it.
constexpr uint8_t NOTE_WATCH = 3;
// (01.05) A player may join the match under way (a room with a place left: the host sends it its state, everyone
// pauses a moment, it plays from there): U8 1 yes, 0 no. Never a link cable room's or a Choose Together one's. A server
// before it ignores it (and refuses those joins as ever).
constexpr uint8_t NOTE_LATE = 4;

// A Choose Together room's "game" (Create's console): no game yet - the players choose one of the games they both have.
constexpr char TOGETHER[] = "*";
// (00.95) A Game Boy link cable room's console (src/GbLink.h): two Game Boys joined by the cable, the room's game Game
// Boy 1's (the host's: a Game Boy or Game Boy Color game), the guest's own Game Boy 2's (LinkGame).
constexpr char GBLINK[] = "gblink";
// (01.04) A Game Boy Advance link cable room's: the same, two GBA games (GBA 1 the host's, GBA 2 the guest's).
constexpr char GBALINK[] = "gbalink";

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
// starred (`words` false: not - the chat's); nothing left: `empty`. The app (its own) and the server (everyone's)
// apply it.
inline std::string CleanText(const std::string& in, size_t most, const char* empty, bool words = true)
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
    if (!words)
      break;
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
// (01.05, the owner: "sin filtro de palabras") A line of the room's chat: printable, CHAT_LETTERS at most, its words as
// they were typed. (The nicknames keep their filter.)
inline std::string CleanChat(const std::string& in)
{
  return CleanText(in, CHAT_LETTERS, "", false);
}
// (01.05) A country as the packets carry it: two capitals, else "" (none shown).
inline std::string CleanCountry(const std::string& in)
{
  return in.size() == COUNTRY_SIZE && in[0] >= 'A' && in[0] <= 'Z' && in[1] >= 'A' && in[1] <= 'Z' ? in : std::string();
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
  std::string country;  // (01.05) "CL" ("": not shown - FLAG_NO_COUNTRY, not known, a server before it)
  // (01.06) Its EmuSuNi version and what it does online (CAP_*; caps_known false: a server before it, or an app it
  // doesn't know).
  std::string app_version;
  uint8_t caps = 0;
  bool caps_known = false;
};

// ---- The game a room plays ----
struct GameInfo
{
  std::string console;  // the console's key ("snes", "md")
  std::string title;
  uint32_t crc = 0;     // CRC32 of the game's file (a file over 64 MiB: of its first and last 8 MiB)
  uint64_t size = 0;    // the file's size
  // (01.06) The file's own CRC32 and size, as an app before 01.05 knew a game (its whole file as it is: a header, a
  // zip and all - crc and size above are its content's, NetplayLibrary::ContentId): a host says them at Create's end,
  // the server gives them to such an app. Never written by WriteGame. 0: not known.
  uint32_t file_crc = 0;
  uint64_t file_size = 0;
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
  // (01.05) Its host lets spectators watch it (FLAG_WATCH), and how many do now: a server before it says neither.
  bool watchable = false;
  uint8_t watchers = 0;
  // (01.05) Its match under way takes a player more (NOTE_LATE, a place left).
  bool late = false;
  // (01.05, AllRooms' extra) Its host's country ("CL"; "" not shown) and its game's version as the host's file's name
  // says it ("USA, Rev 1"; "" not said): a server before it says neither.
  std::string country, version;
  // (01.06, AllRooms' second extra) Its host's online level of its console (NetLevel; 0: not known) and caps
  // (CAP_*, CAPS_KNOWN with them known): a server before it says neither - the room plays with its own version only.
  uint8_t level = 0, host_caps = 0;
};

// (01.05) What follows Members' members and AllRooms' rooms - an app before it reads neither (a datagram's end is never
// checked): EXTRA_VERSION, then for Members how many watch (U8) and each member's country (Str, COUNTRY_SIZE) in the
// list's order; for AllRooms each room's WriteRoomExtra, in the rooms' order. A server before it sends none.
constexpr uint8_t EXTRA_VERSION = 1;
inline void WriteRoomExtra(Writer& w, const RoomEntry& e)
{
  w.Str(e.country, COUNTRY_SIZE);
  w.Str(e.version, MAX_GAME_VERSION);
}
inline void ReadRoomExtra(Reader& r, RoomEntry* e)
{
  e->country = CleanCountry(r.Str(COUNTRY_SIZE));
  e->version = CleanText(r.Str(MAX_GAME_VERSION), GAME_VERSION_LETTERS, "");
}

// ---- (01.06, the owner: "jugar entre versiones") Online play between versions of EmuSuNi ----
// What an app does online beside a match. An app before 01.06 says none of it: its version does (OldNetCaps).
constexpr uint8_t CAP_CHAT = 1;         // the room's chat (ControlKind::Chat): an app without it never shows a line
constexpr uint8_t CAP_WATCH = 2;        // spectators (FLAG_WATCH, Type::Watch)
constexpr uint8_t CAP_LATE = 4;         // joining a match under way (NOTE_LATE: LateStart, JoinPause...)
constexpr uint8_t CAP_COUNTRY = 8;      // "Show My Country" (FLAG_NO_COUNTRY): an app without it shows none
constexpr uint8_t CAP_CONTENT_ID = 16;  // a game known by its content (GameInfo::crc), not by its whole file
constexpr uint8_t CAP_RESET = 32;       // (01.14) the match's Reset (ControlKind::ResetAt): the host resets it when all have it
constexpr uint8_t CAP_ROOM = 64;        // (01.23) back to the room after a match, any room's (ControlKind::EndMatch)
constexpr uint8_t NET_CAPS = CAP_CHAT | CAP_WATCH | CAP_LATE | CAP_COUNTRY | CAP_CONTENT_ID | CAP_RESET | CAP_ROOM;
constexpr uint8_t CAPS_KNOWN = 0x80;    // (a caps byte of EXTRA2's: what follows is known)

// A console's online level: its match as online play has it - the rules (src/Netplay.cpp's RulesFor), its core's
// build, its inputs' and states' form. Two apps play that console's matches together when their levels of it are the
// same, whatever their versions (local/herramientas/prueba-netplay/cruce.sh: v00.23 against 01.06). RAISED BY ONE
// whenever any of that changes for a console - a core built from other code or with other options, a rule -: a level
// kept is a promise that both consoles' matches go on alike (the Atari's cores of another build went out of step from
// the first frame). 0: not played online.
//  - 1: v00.23's: NES, SNES, Mega Drive, Master System, Game Gear, SG-1000, PC Engine, Atari 7800, the Arcade; (01.04)
//    the GBA link cable's first.
//  - 2 (01.04): the Atari 2600 (Stella's paddles moved by the stick or the mouse); the Game Boy link cable (its own
//    rules: rollback 4 - v00.23 played it with the Game Boy's).
inline int NetLevel(const std::string& console)
{
  if (console == "atari2600" || console == GBLINK)
    return 2;
  if (console == "nes" || console == "snes" || console == "md" || console == "sms" || console == "gg" ||
      console == "sg1000" || console == "pce" || console == "atari7800" || console == "arcade" || console == GBALINK)
    return 1;
  return 0;
}
// The consoles an app says its level of (NetLevel's).
constexpr const char* NET_CONSOLES[] = {"nes",       "snes",      "md",     "sms",  "gg",   "sg1000",
                                        "pce",       "atari2600", "atari7800", "arcade", GBLINK, GBALINK};
constexpr size_t MAX_NET_CONSOLES = 32;

// An app as online play knows it: its caps and its consoles' levels.
struct NetCaps
{
  bool known = false;  // said by the app (01.06 on) or by OldNetCaps' table; not known: plays with its own version only
  uint8_t caps = 0;
  std::vector<std::pair<std::string, uint8_t>> levels;
  int Level(const std::string& console) const
  {
    for (const auto& level : levels)
    {
      if (level.first == console)
        return level.second;
    }
    return 0;
  }
};
inline NetCaps ThisNetCaps()
{
  NetCaps c;
  c.known = true;
  c.caps = NET_CAPS;
  for (const char* console : NET_CONSOLES)
    c.levels.emplace_back(console, static_cast<uint8_t>(NetLevel(console)));
  return c;
}
// The apps before 01.06, by their version (they say nothing more). "00.23" is the published one ("01.03" its test
// builds'); 01.04 and 01.05 were test builds. Any other version not known (before 00.23: none played online).
inline NetCaps OldNetCaps(const std::string& version)
{
  NetCaps c;
  const bool v0023 = version == "00.23" || version == "01.03";
  const bool v0105 = version == "01.05";
  if (!v0023 && !v0105 && version != "01.04")
    return c;
  c.known = true;
  c.caps = v0105 ? static_cast<uint8_t>(CAP_CHAT | CAP_WATCH | CAP_LATE | CAP_COUNTRY | CAP_CONTENT_ID) : 0;
  for (const char* console : {"nes", "snes", "md", "sms", "gg", "sg1000", "pce", "atari7800", "arcade"})
    c.levels.emplace_back(console, 1);
  c.levels.emplace_back("atari2600", v0023 ? 1 : 2);
  c.levels.emplace_back(GBLINK, v0023 ? 1 : 2);
  if (!v0023)
    c.levels.emplace_back(GBALINK, 1);
  return c;
}
// The apps whose games are known by their whole file (OldNetCaps without CAP_CONTENT_ID): the server's room_log keeps
// their rooms' games so (emusuni-netd.cpp's CountTopGames).
constexpr const char* FILE_ID_VERSIONS[] = {"00.23", "01.03", "01.04"};

// What an app says of itself at Create's, Join's and ListAll's end: NETCAPS_VERSION, its caps, its consoles' levels
// (count, then each one's console and level). A server before it reads none of it.
constexpr uint8_t NETCAPS_VERSION = 1;
inline void WriteNetCaps(Writer& w, const NetCaps& c)
{
  w.U8(NETCAPS_VERSION);
  w.U8(static_cast<uint8_t>(c.caps & ~CAPS_KNOWN));
  const size_t n = std::min(c.levels.size(), MAX_NET_CONSOLES);
  w.U8(static_cast<uint8_t>(n));
  for (size_t i = 0; i < n; i++)
  {
    w.Str(c.levels[i].first, MAX_CONSOLE);
    w.U8(c.levels[i].second);
  }
}
// Read when there (false: none, or not whole - r as it was).
inline bool ReadNetCaps(Reader& r, NetCaps* c)
{
  if (r.Left() == 0)
    return false;
  Reader x = r;
  if (x.U8() < NETCAPS_VERSION)
    return false;
  NetCaps got;
  got.caps = static_cast<uint8_t>(x.U8() & ~CAPS_KNOWN);
  const size_t n = x.U8();
  for (size_t i = 0; i < n && i < MAX_NET_CONSOLES && x.Ok(); i++)
  {
    std::string console = x.Str(MAX_CONSOLE);
    const uint8_t level = x.U8();
    got.levels.emplace_back(std::move(console), level);
  }
  if (!x.Ok() || n > MAX_NET_CONSOLES)
    return false;
  got.known = true;
  *c = std::move(got);
  r = x;
  return true;
}

// Whether consoles of the app versions a and b play a room's match of that console together: the same version always;
// else both known, and that console's level the same in both - never a Choose Together room (its players' libraries
// are compared by their own CRC32s) nor a link cable's (its own messages, LinkGame and LinkSave).
inline bool PlayTogether(const std::string& console, const std::string& a, const NetCaps& ca, const std::string& b,
                         const NetCaps& cb)
{
  if (a == b)
    return true;
  if (console == TOGETHER || console == GBLINK || console == GBALINK || !ca.known || !cb.known)
    return false;
  const int level = ca.Level(console);
  return level > 0 && level == cb.Level(console);
}

// "00.23" before "01.06", by their numbers (one that isn't "NN.NN": never before).
inline bool VersionBefore(const std::string& a, const std::string& b)
{
  int am = 0, an = 0, bm = 0, bn = 0;
  if (std::sscanf(a.c_str(), "%d.%d", &am, &an) != 2 || std::sscanf(b.c_str(), "%d.%d", &bm, &bn) != 2)
    return false;
  return am != bm ? am < bm : an < bn;
}

// A Version refusal's detail for an app that says its levels (01.06 on): the room's version and console ("00.23
// atari2600"), so that it says who updates and for what; an app before it gets the room's version alone, as ever.
inline std::string VersionDetail(const std::string& version, const std::string& console)
{
  return version + " " + console;
}
inline void ReadVersionDetail(const std::string& detail, std::string* version, std::string* console)
{
  const size_t space = detail.find(' ');
  *version = detail.substr(0, space);
  *console = space == std::string::npos ? std::string() : detail.substr(space + 1);
}

// (01.06) What follows EXTRA_VERSION's extra in Members and AllRooms (an app of 01.05 reads to that one's end, one
// before it none): EXTRA2_VERSION, then for Members each listed member's app version (Str, MAX_APP_VERSION) and caps
// (U8: CAPS_KNOWN with them known), in the list's order; for AllRooms each room's host's level of its console (U8; 0
// not known) and caps (U8), in the rooms' order.
constexpr uint8_t EXTRA2_VERSION = 2;
// (01.07) And after Members' second extra: EXTRA3_VERSION, then U8 the server's ROOM_* flags (an app of 01.06 reads to
// the second's end).
constexpr uint8_t EXTRA3_VERSION = 3;
constexpr uint8_t ROOM_CHAT_STORED = 1;  // the server keeps the room's chat (RoomChatCopy) 30 days: its notice shows

// ---- WORLD CHAT (01.07, the owner: "un chat mundial (general)") ----
// Every console on the Online tab, through the main server: no session - the console asks for the lines since the last
// it has (every 2 s while the tab's Chat shows, every 10 s else: how many new), with its cookie.
//   WorldChat(cookie, nonce, U32 since) -> WorldLines(U32 nonce, U32 the newest line's number, U8 WORLD_* flags, Str
//              the asker's country as the lines show it ("" not known: no PHRASE_FROM), U8 count, each line
//              WriteWorldLine - the oldest first, as many as fit: asked again from the last one). since 0: the last
//              ones.
//   WorldSay(cookie, nonce, Str app version, Str nickname, U8 flags - FLAG_NO_COUNTRY, WORLD_NOTICE_SEEN -, the chat
//              key (WORLD_KEY_SIZE: random, made once by the app - netplay.ini's chat_key -, never the PS4's or a
//              user's, nor the statistics' id), U8 the phrase's number (ChatPhrase; 0: text) then Str its parameter
//              (MAX_PHRASE_PARAM; none so far: PHRASE_FROM's is the server's) or Str the text (CHAT_BYTES)) ->
//              WorldSaid(U32 nonce, U8
//              WorldResult, U32 the line's number, U16 seconds to wait / left blocked (0xFFFF: always)).
//   WorldReport(cookie, nonce, U32 a line's number): the line marked for the server's owner. No answer.
// The others see a player by its code (PLAYER_CODE_SIZE letters of CODE_ALPHABET: the server's HMAC of its chat key),
// never by an address. The server shows each line's nickname filtered (CleanNick), its text as typed (CleanChat: the
// owner's "sin filtro de palabras"), its country only as the rooms do; it keeps the last WORLD_KEPT in memory and -
// WORLD_STORED - every line 30 days for moderation, with a keyed hash of the address (never the address). A line is
// taken only from an app that showed the player that notice (WORLD_NOTICE_SEEN).
constexpr size_t WORLD_KEY_SIZE = 16, PLAYER_CODE_SIZE = 8, MAX_PHRASE_PARAM = 8;
constexpr int WORLD_KEPT = 50;
constexpr uint8_t WORLD_NOTICE_SEEN = 32;  // (WorldSay's flags: the notice under the chat was shown)
constexpr uint8_t WORLD_STORED = 1;        // (WorldLines' flags: the server keeps the lines 30 days)
enum class WorldResult : uint8_t
{
  Sent = 0,
  TooSoon = 1,   // one line every 3 seconds
  TooMany = 2,   // 20 a minute
  Busy = 3,      // the whole server's (10 lines a second)
  Blocked = 4,   // the server's owner blocked this player (seconds: how long, 0xFFFF always)
  NoNotice = 5,  // the notice wasn't shown
  Bad = 6,       // nothing to send, or a packet the server couldn't read
};
struct WorldLine
{
  uint32_t serial = 0;  // its number: each line one more
  uint16_t age = 0;     // seconds since it was said (the server's clock)
  std::string code;     // its player's code
  std::string nick, country;  // ("" no country shown)
  uint8_t phrase = 0;   // a quick phrase (ChatPhrase), 0: text
  std::string param, text;
};
inline void WriteWorldLine(Writer& w, const WorldLine& l)
{
  w.U32(l.serial);
  w.U16(l.age);
  w.Str(l.code, PLAYER_CODE_SIZE);
  w.Str(l.nick, MAX_NICK);
  w.Str(l.country, COUNTRY_SIZE);
  w.U8(l.phrase);
  w.Str(l.phrase ? l.param : l.text, l.phrase ? MAX_PHRASE_PARAM : CHAT_BYTES);
}
inline WorldLine ReadWorldLine(Reader& r)
{
  WorldLine l;
  l.serial = r.U32();
  l.age = r.U16();
  l.code = r.Str(PLAYER_CODE_SIZE);
  l.nick = CleanNick(r.Str(MAX_NICK));
  l.country = CleanCountry(r.Str(COUNTRY_SIZE));
  l.phrase = r.U8();
  if (l.phrase)
    l.param = CleanText(r.Str(MAX_PHRASE_PARAM), MAX_PHRASE_PARAM, "");
  else
    l.text = CleanChat(r.Str(CHAT_BYTES));
  return l;
}
// (01.07, the owner) The players' ranking: after Create's and Join's NetCaps (and Create's file id) a player block -
// PLAYER_VERSION, U8 flags (PLAYER_RANKED: "Appear in the Ranking: Yes"), the chat key (WORLD_KEY_SIZE: its code, the
// chat's). A server before it reads none; without PLAYER_RANKED the server counts nothing of it.
constexpr uint8_t PLAYER_VERSION = 1;
constexpr uint8_t PLAYER_RANKED = 1;
constexpr int TOP_PLAYERS = 20;
inline void WritePlayer(Writer& w, bool ranked, const uint8_t key[WORLD_KEY_SIZE])
{
  w.U8(PLAYER_VERSION);
  w.U8(ranked ? PLAYER_RANKED : 0);
  w.Bytes(key, WORLD_KEY_SIZE);
}
// Read when there (false: none, or not whole - r as it was); key WORLD_KEY_SIZE bytes.
inline bool ReadPlayer(Reader& r, bool* ranked, uint8_t* key)
{
  if (r.Left() == 0)
    return false;
  Reader x = r;
  if (x.U8() < PLAYER_VERSION)
    return false;
  const uint8_t flags = x.U8();
  const uint8_t* k = x.Bytes(WORLD_KEY_SIZE);
  if (!x.Ok() || !k)
    return false;
  *ranked = (flags & PLAYER_RANKED) != 0;
  std::memcpy(key, k, WORLD_KEY_SIZE);
  r = x;
  return true;
}

// A player's code from a keyed hash of its chat key (32 bytes).
inline std::string PlayerCode(const uint8_t mac[32])
{
  std::string code;
  for (size_t i = 0; i < PLAYER_CODE_SIZE; i++)
    code += CODE_ALPHABET[mac[i] % (sizeof(CODE_ALPHABET) - 1)];
  return code;
}

// AllRooms' rooms: their flags, then the code when it is public. (01.05) Watchable, and its spectators' count in the
// bits above (an app before it reads only the first two).
constexpr uint8_t ROOM_PUBLIC = 1;
constexpr uint8_t ROOM_PLAYING = 2;
constexpr uint8_t ROOM_WATCHABLE = 4;
constexpr int ROOM_WATCHERS_SHIFT = 3;  // 3 bits: 0 to MAX_WATCHERS
constexpr uint8_t ROOM_LATE = 64;       // (01.05) its match takes a player more
inline void WriteListedRoom(Writer& w, const RoomEntry& e)
{
  w.U32(e.id);
  w.U8(static_cast<uint8_t>((e.is_public ? ROOM_PUBLIC : 0) | (e.playing ? ROOM_PLAYING : 0) |
                            (e.watchable ? ROOM_WATCHABLE : 0) |
                            (std::min<int>(e.watchers, MAX_WATCHERS) << ROOM_WATCHERS_SHIFT) | (e.late ? ROOM_LATE : 0)));
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
  e.watchable = (flags & ROOM_WATCHABLE) != 0;
  e.watchers = static_cast<uint8_t>((flags >> ROOM_WATCHERS_SHIFT) & 7);
  e.late = (flags & ROOM_LATE) != 0;
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
  // (01.04) The community online mode the game is connected to while it plays - "psp-adhoc", "psp-infra", "dcnet",
  // "wfc-wiimmfi", "wfc-wiilink" (servers that aren't EmuSuNi's: src/UsageStats.cpp's CoreLine sees it in the cores'
  // log lines) -, "" when none: only the mode's name, never an address. The server drops any other.
  std::string community;
};
constexpr size_t MAX_GAME_ID = 24;
constexpr size_t MAX_COMMUNITY = 16;
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
  w.Str(h.community, MAX_COMMUNITY);  // (01.04)
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
  if (r.Left() > 0)
    h.community = r.Str(MAX_COMMUNITY);  // ((01.04) an app before it: "")
  return h;
}
}  // namespace NetProto
