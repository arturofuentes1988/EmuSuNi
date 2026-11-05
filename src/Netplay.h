// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Online play (00.23): PS4s anywhere in the world play one game as if every controller were on one console. A player
// picks a game (Triangle -> Play Online) and makes a room - private (a 6-letter code to tell a friend) or public
// (anyone joins it) - on EmuSuNi's room server (server/emusuni-netd.cpp). The Online tab's table shows every room
// (00.85: the private ones too, without their code). The other player types the code or picks the room (a private
// one: with its code), the app finds the same game in their library (by its file's CRC32 and size, else by its
// title: "another version, play anyway?"), and the host presses Start. The packets are src/NetplayProtocol.h's.
//
// How the consoles stay in step (GGPO's way):
//  - The host's core options go to the guest before the game loads, the host's save state (with its battery save and
//    its controller devices) once it has loaded: both start from the same frame 0 with the same settings.
//  - Every frame each console sends its local players' input, with the frames the other hasn't acknowledged yet again
//    (a lost packet costs nothing). An input is played `delay` frames after it is pressed, on both consoles. (00.92)
//    The host chooses the first delay from the ping measured once both games run (before, a console loading its game
//    answers late: 00.91 took 732 ms for 150 and played 8 frames late); then each console adapts its own every second
//    to the ping, its jitter and whether the other waited for its inputs.
//  - Rollback (the light consoles): a frame whose remote input hasn't come yet is run with the input predicted (the
//    last one known). When the real one differs, the state saved before that frame is loaded and the frames since run
//    again with it - their sound not played, their picture not shown - up to `max_rollback` frames back; further ahead
//    the console waits. Delay-based (max_rollback 0): a frame runs only once every input for it is in.
//  - Time sync: a console running ahead of the other (its frame against the other's, the ping counted) waits a frame
//    now and then, so neither drifts ahead.
//  - Every `check_every` frames both hash their state (CRC32 of the save state) and compare: a difference (a desync)
//    has the host send its state again, which the guest loads and runs on from (resync).
// The path: directly once a hole is punched through both routers (on the same network, its LAN address), else through
// a relay server - the one with the best ping for both, from the main server's list. "Hide my address" sends
// everything through the relay: the other player never learns it.
// With strangers: the host can remove a player (it can't join that room again); voice chat starts off in public rooms
// (each player turns theirs on) and on in private ones; each player can be muted; nicknames are cut to 16 letters and
// filtered (the server does it too).
// While online: no save or load state, cheats, reset, fast forward or slow motion; RetroAchievements is paused (no
// frame of a match is checked: re-run frames and predicted ones would count); the in-game menu doesn't pause the match.
//
// The main server: /data/EmuSuNi/netplay.ini's `server=host:port` (a player's own), else the list in EmuSuNi's GitHub
// repository (netplay/servers.txt, cached in data_dir/cache: servers can move without an update), else SERVER below.
// netplay.ini may also set `nickname=`, `delay=` (frames; kept the whole match), `rollback=` (frames; 0: delay-based),
// `voice=off`, `hide_address=on`, `latency_bar=off`.
// (00.92) Every 10 s of a match the log has a line of it: the speed, the delay, the waits for input and the rollbacks,
// and each player's ping, jitter, pings lost, how late its inputs came and what its console says of ours.
// (00.86) A room stays open while its host goes elsewhere in the menus or plays a game alone; a console that joins asks
// to, and the host accepts it or not (Admit, Refuse) - see Hosting() below.
//
// Everything is called from one thread - the menu's, then the game's emulation thread - except GetStatus(), which any
// thread may read (the drawing beside the emulation). Nothing here throws.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Cores.h"
#include "NetplayProtocol.h"

namespace Netplay
{
// The main server when neither netplay.ini nor the GitHub list names one. (The owner sets it once the server is
// hosted: "host:port".)
constexpr const char* SERVER = "132.145.164.206:7777";
// The list of main servers in the repository (one "server=host:port" a line, tried in order).
constexpr const char* SERVERS_URL = "https://raw.githubusercontent.com/arturofuentes1988/EmuSuNi/main/netplay/servers.txt";

// ---- The anonymous statistics' way to the main server (src/UsageStats.cpp: their thread, never the menu's) ----
// The main servers, in order: netplay.ini's server, else the GitHub list's (read once a session: waited for up to
// `wait_seconds`, else the copy of before), else SERVER (none while it is the placeholder).
std::vector<std::string> MainServers(const std::string& data_dir, double wait_seconds);
// A server's name looked up now: its IPv4 address in host order, 0 when not found. Blocks up to 5 s.
uint32_t LookUp(const std::string& host);

// ---- Which consoles play online, and how ----
struct Rules
{
  bool online = false;        // offered (Triangle -> Play Online)
  bool experimental = false;  // offered with a warning (delay-based on a heavy console)
  int max_rollback = 0;       // frames a console may run ahead of the other's input (0: delay-based)
  bool full_input = false;    // the analog sticks and triggers too (else the buttons alone)
  bool replay_video = true;   // re-run frames draw their picture (false: the core is told not to, which is faster)
  int check_every = 60;       // frames between state checksums
  // The checksum of the console's memory (work RAM, battery save, video RAM) instead of the whole state: a core whose
  // state carries a pointer of its own process (Genesis Plus GX's Z80 interrupt callback) saves the same moment as
  // different bytes on two consoles.
  bool check_memory = false;
  // Every frame starts from its state saved and loaded again: a core whose loaded state goes on otherwise than the one
  // it was saved from (Genesis Plus GX's Mega Drive: something of its timing isn't in the state) - the host runs on
  // from its state, the guest from it loaded, and a rollback from it loaded - stays in step only so.
  bool reload_each_frame = false;
};
Rules RulesFor(const std::string& console_key);

// The game's identity a room is made for: CRC32 and size of its file (a file over 64 MiB: CRC32 of its first and last
// 8 MiB, with its size: a disc image is read in seconds, not minutes). False: the file can't be read.
bool GameCrc(const std::string& path, uint32_t* crc, uint64_t* size);

// ---- The lobby (the menu) ----
enum class Phase
{
  Off,         // no room
  Browsing,    // the public list asked for and shown
  Connecting,  // the server looked up and asked for a room
  Room,        // in a room: the code is known (the host), or the room's game (a guest)
  Starting,    // the host pressed Start: the game loads on every console, the state goes to the guests
  Playing,     // frames run
};

struct PlayerView
{
  int id = 0;            // member id (0: the host)
  std::string nick;
  std::string region;    // its time zone ("UTC-5")
  bool me = false;
  int ping_ms = -1;      // -1: not known yet
  bool linked = false;   // packets come and go (directly or through a relay)
  bool relay = false;    // through a relay (no direct path, or an address hidden)
  bool voice_on = false;  // its voice chat is on (mine: on, theirs: as their pings say)
  bool talking = false;
  bool muted = false;    // muted by me
  bool asking = false;   // (00.86, the host's view) asked to join, not answered yet: not in the room's players
  int locals = 1;
  NetProto::LobbyGame game = NetProto::LobbyGame::Looking;  // a guest's game, as it said
  int first_port = -1, ports = 0;  // in the match
  // (00.92) The latency bar: the ping's jitter, the pings lost (per mille), how often this console waited for that
  // player's inputs in the match (per mille, the last 5 s), and all of it as one - 0 good (green), 1 fair (amber), 2 bad
  // (red), -1 not known yet. Mine: the others' connection to me as they say it (the ping: mine to them, the same path).
  int jitter_ms = -1;
  int lost_pm = 0;
  int waits_pm = 0;
  int quality = -1;
};

struct RoomView
{
  NetProto::RoomEntry room;
  int ping_ms = -1;  // estimated: the host's round trip to the server and ours
};

struct View
{
  Phase phase = Phase::Off;
  bool host = false, is_public = false, hidden = false;
  std::string code;
  std::string error;  // the room was refused or lost, a match ended: why (English, for PS4Lang::T)
  NetProto::GameInfo game;  // the room's game
  std::vector<PlayerView> players;
  int max_players = 2;     // (00.95) the room's consoles at most (2 to NetProto::MAX_MEMBERS: the host's choice)
  bool can_start = false;  // the host: every guest is linked and has the game
  bool voice_on = false;
  std::string relay;       // the relay server in use ("" while none)
  // Browsing: the rooms (00.85: every room, public and private, waiting or playing - a server before that lists only
  // the public ones waiting), whether the list asked for came whole (asked again, the last one stays till then), and
  // how many rooms the server has (the list holds the first pages' of them).
  std::vector<RoomView> rooms;
  bool rooms_done = false;
  int rooms_total = 0;
  // A Choose Together room: the games both have (once both lists are in), whether the other's list came (the host),
  // the guest's suggestion (the host's view), what the other said as the last match ended. (00.95: of three or four
  // players, the games every one has, once every guest's list came; the last suggestion.)
  bool together = false;
  std::vector<NetProto::LibraryGame> common;
  bool common_ready = false, games_sent = false, peer_games_in = false;
  bool has_suggestion = false;
  NetProto::LibraryGame suggested;
  std::string notice;
  // (00.86) The host: the player asking to join (its member id, -1: none), the seconds left to answer, a number that
  // changes with every new request. A guest: waiting for the host to accept it, the seconds it still waits.
  int asking = -1;
  std::string asking_nick, asking_region;
  int asking_seconds = 0;
  uint32_t asking_serial = 0;
  bool waiting_host = false;
  // (00.95) A Game Boy link cable room (LinkRoom): the guest's own game for Game Boy 2 (the host's view: once it came; a
  // guest's: the one it chose), its title, whether the host has its file too (Looking: not said yet), whether the
  // guest's battery save came (the host's view).
  bool link = false;
  bool link_game_in = false;
  NetProto::LibraryGame link_game;
  std::string link_title;
  NetProto::LobbyGame link_found = NetProto::LobbyGame::Looking;
  bool link_save_in = false;
  int waiting_seconds = 0;
};

// Make a room for the game / join one by its code / list the rooms (console_key "": every console's; 00.85: every
// room, the Online tab's table). The answer comes with Update(): GetView().phase, .error, .rooms. Join while the list
// shows uses its server and cookie (no new Hello). RefreshRooms: the list asked for again (2 s apart at most).
// (00.86) While the host's room is open (Hosting), both ask for the list on the room's own connection - a new cookie
// first when the room's is old - and the room stays whatever the list does.
// (00.95) A room for `max_players` consoles (2 to NetProto::MAX_MEMBERS): the server takes no more. The match is
// played by those in it when the host presses Start (two at least), the ports given in order - the host's players
// first - while the console has ports (NetProto::MAX_PORTS).
void Host(const NetProto::GameInfo& game, bool is_public, const std::string& data_dir, int max_players = 2);
void Join(const std::string& code, const std::string& data_dir);
void Browse(const std::string& console_key, const std::string& data_dir);
void RefreshRooms();
// The network, every frame while in a room (the menu's; in a game FramesThisPass does it).
void Update();
View GetView();
// A guest: what it found of the room's game in its library (the host shows it and starts only when it has the game).
void SetLobbyGame(NetProto::LobbyGame game);
// The host: everyone starts the game (false: not every guest is ready). The room leaves the public list.
bool StartMatch();
// The match starts now (once: the menu then starts the game).
bool TakeStart();
// The host: a player out of the room (it can't join again).
void Kick(int member);
// Leave the room (and a match: the others are told).
void Leave();

// ---- (00.86) The host's room while the host is elsewhere, and who comes in ----
// The host's room stays open while the host goes anywhere in the menus or plays a game alone (Update() goes on: the
// menu's every frame, Emulator::Run's every pass): it closes with Close Room, Leave, the app's end, or the server.
// Hosting(): a room of ours, being made or open, no match in it.
bool Hosting();
// A console that joins asks to: the host accepts it (Admit) or not (Refuse: the server removes it, it can't ask again
// in that room); a request not answered in ASK_SECONDS expires (the guest is told and leaves). Until accepted, a guest
// is in no match, no list of games, no voice chat; the host's Start waits for it.
constexpr double ASK_SECONDS = 60.0;
void Admit(int member);
void Refuse(int member);
// The request waiting for the host's answer (member -1: none) - cheaper than GetView(), for the game's pass.
struct JoinRequest
{
  int member = -1;
  std::string nick, region;
  int seconds_left = 0;
  uint32_t serial = 0;  // a new number for every request
};
JoinRequest GetJoinRequest();
// The last error, taken (the menu shows it once).
std::string TakeError();

// ---- Choose Together rooms (a room without a game) ----
// Once the other player is in, both see the games they both have - the same file (CRC32 and size: the same dump,
// region and revision), on a console that plays online (the same app version is the same cores) -; the host picks one
// (the guest may suggest one); after its match both are back in the room to pick another, without joining again.
// (00.95) A room of three or four: the games every player has; a guest who leaves a match ends it, the others are
// back in the room (the host who leaves closes it).
void HostTogether(bool is_public, const std::string& data_dir, int max_players = 2);
// This console's games that play online (the menu's, as NetplayLibrary hashes them): complete once every file is
// hashed - the guest's go to the host then, the host answers with the games both have.
void SetLibrary(const std::vector<NetProto::LibraryGame>& games, bool complete);
// A guest: the game it would like (the host sees it marked).
void Suggest(const NetProto::LibraryGame& game);
// The host: the match starts with this game (one both have).
bool PickGame(const NetProto::GameInfo& game);
// In a Choose Together match: it ends for both, who are back in the room (Leave Room ends it and leaves).
void EndMatchToRoom();
bool Together();
// "Hide my address" (netplay.ini's hide_address): everything through a relay. Kept for the next rooms.
bool HideAddress(const std::string& data_dir);
void SetHideAddress(const std::string& data_dir, bool hide);
// The nickname the others see (Settings -> Online Play -> Nickname; netplay.ini's nickname): cut to 16 letters and
// filtered as the server does; "" set: the PS4 user's name again. The next room uses it.
std::string GetNickname(const std::string& data_dir);
void SetNickname(const std::string& data_dir, const std::string& nickname);
// (00.92) The latency bar - each player's ping and the quality of its connection, in the room and over the game
// (Online Settings, the in-game menu's online page; netplay.ini's latency_bar=off hides it): at once, and kept.
bool LatencyBar(const std::string& data_dir);
void SetLatencyBar(const std::string& data_dir, bool on);

// ---- (00.95) The Game Boy link cable online (src/GbLink.h): a room whose console is NetProto::GBLINK ----
// The room's game is Game Boy 1's, the host's (a Game Boy or Game Boy Color game: GameInfo's CRC32 and size its file's):
// a guest finds it among its Game Boy and Game Boy Color games, as for any room (SetLobbyGame), and chooses its own for
// Game Boy 2 (SetLinkGame, with its battery save: GbLink::PackSave(GbLink::ReadSave(...))), sent once the host accepted
// it. The host finds that game among its own (it needs the file too: a game is never sent) and says whether it has it
// (SetLinkGameFound). Start waits for both and for the save; a room of two, one controller on each console (the host's
// is Game Boy 1's, port 0; the guest's Game Boy 2's, port 1). Before Emulator::Run, the menu sets GbLink::Set.
bool LinkRoom();
void SetLinkGame(const NetProto::LibraryGame& game, const std::string& title, const std::vector<uint8_t>& packed_save);
void SetLinkGameFound(bool found);
// The host: the guest's battery save as it came (GbLink::UnpackSave reads it; empty: none yet).
std::vector<uint8_t> LinkGuestSave();

// ---- The match (src/Emulator.cpp) ----
// A match: the game Emulator::Run starts plays online.
bool Active();
bool IsHost();
// The core with its controller devices kept: the host's are the guest's too, and neither changes them in the match.
const CoreApi* WrapCore(const CoreApi* core);
// The core options: the host's are sent (Done at once); a guest waits for them (Waiting) and gets them. Failed: the
// match ended meanwhile (Ended()).
enum class Wait
{
  Done,
  Waiting,
  Failed
};
Wait AgreeOptions(std::map<std::string, std::string>* variables);
// After every build of the options (BuildVariables): the match's stay.
void KeepVariables(std::map<std::string, std::string>* variables, bool* changed);
// The emulator's side: puts a frame's inputs on the console's ports (count of them).
using ApplyInputs = void (*)(const NetProto::PortInput* ports, int count);
// The game loaded (and Continue's state, if any): the host sends its state; the guest waits for it.
void GameLoaded(const std::string& console_key, double fps, ApplyInputs apply);
// This pass's local players' inputs (the connected controllers', in order; neutral while the in-game menu is open).
void SetLocalInputs(const NetProto::PortInput* inputs, int count);
// The network, then the frames this pass may run of those the pacing wants (0 while waiting for the other console).
int FramesThisPass(int frames);
// One frame: the inputs, any rollback, the core's run.
void RunFrame();
// Frames being run again (a rollback): their sound isn't played, their picture not shown.
bool Replaying();
// ... and the core is told not to draw them (Rules::replay_video false).
bool SkipVideo();
// No frame runs now: the start, a resync, waiting for the other console's input.
bool Waiting();
// The match ended (the other left, the connection was lost): why. The game ends with it. (A Choose Together match the
// other ended: *why empty - back to the room, no error.)
bool Ended(std::string* why);
// The game's end: the room is left, the others told (a Choose Together match ended so: back to the room instead).
void GameEnded();
// A guest keeps its own saves untouched: its battery save and Continue's state aren't written in a match.
bool KeepOwnSaves();

// ---- The voice chat (src/Voice.h) ----
void SetVoiceOn(bool on);       // this console's microphone and speaker
void SetPeerMuted(int member, bool muted);

// ---- What the screen shows (any thread) ----
struct Status
{
  bool active = false;
  bool host = false;
  std::string line;  // "Waiting for Ana...", "Resyncing..." ("": nothing to say)
  int ping_ms = -1;
  bool relay = false;
  std::vector<PlayerView> players;
  bool voice_on = false;
  bool together = false;  // (a Choose Together room: the in-game menu's End Match)
  int delay = 0, max_rollback = 0;  // (00.92) the match's input delay (this console's, it changes) and rollback
  bool latency_bar = true;          // (00.92) the latency bar shown
};
Status GetStatus();

// ---- For the log and the PC test ----
struct Stats
{
  int32_t frame = 0;
  int delay = 0, max_rollback = 0;
  int64_t rollbacks = 0, replayed = 0;  // rollbacks, frames run again
  int64_t stalls = 0, waits = 0;        // passes waiting for input / for time sync
  int64_t checks = 0, mismatches = 0, resyncs = 0;
  int64_t packets_in = 0, packets_out = 0;
  bool relay = false;
  int relay_index = -1;
  std::vector<std::pair<int32_t, uint32_t>> check_log;  // (frame, CRC32) of every final checksum, in order
};
Stats GetStats();
}  // namespace Netplay
