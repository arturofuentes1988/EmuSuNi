// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// RetroAchievements (00.73): the achievements of retroachievements.org, as RetroArch has them, through rcheevos'
// rc_client (RetroAchievements' own library, MIT: /root/src/rcheevos, scripts/setup.sh), built into the app.
//
// - The account: logged in once with the user name and the password (Settings -> RetroAchievements, the app's
//   keyboard, 00.80); only the token the site gives back is kept, in data_dir/retroachievements.ini - never the
//   password.
//   Each start logs in again with it, in the background.
// - A game: identified by rcheevos' own hash of its file (rc_hash: each console's way, cartridges from their bytes,
//   discs from their files - .cue/.bin, .chd, .iso, .cso, .gdi... - read where the app reads them), its achievements
//   loaded, then checked every frame against the console's memory, which the core shows (its memory map,
//   RETRO_ENVIRONMENT_SET_MEMORY_MAPS, or retro_get_memory_data), on the emulation thread.
// - Unlocks: a popup with the achievement's picture (downloaded once, kept in data_dir/cache/ra) and its name; the
//   in-game menu lists the game's achievements, unlocked or not, with their points; leaderboards and rich presence
//   as rc_client runs them.
// - Casual only (00.78): RetroAchievements hasn't approved EmuSuNi (it would keep its Hardcore unlocks as Casual), so
//   there is no Hardcore - nor its rules: states, cheats and slow motion stay free - until it does. (00.73-00.77's
//   Hardcore: local/checkpoints/00.77-ra-hardcore-source.tar.gz.)
// - Nothing here ever stops a game: offline, a login that fails, a game RetroAchievements doesn't know - the game
//   plays as before, with a short note, and the log says each step ("ra: ...").
// - The session's state (00.75): connected or not, tried again by itself when the server didn't answer (a game's
//   achievements too, while it plays); the player's history of unlocks and games.
//
// The server calls, the hashing of a disc and the pictures' downloads run on one thread of this module's (256 KiB
// of stack, for the app's life); rc_client's own lock guards its state; nothing here blocks the emulation thread
// on the network.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Systems.h"

struct retro_memory_map;

namespace Achievements
{
// ---- The app ----
// At the app's start: the account from data_dir/retroachievements.ini (logs in again with its token, in the
// background).
void Init(const std::string& data_dir, const std::string& assets_dir);
// Every pass of the menus' loop: rc_client's waiting work (a retry of an unlock that didn't reach the server,
// the session's pings) while no game runs.
void Idle();

// ---- The account (Settings -> RetroAchievements) ----
enum class Login
{
  Out,       // not logged in (or logged out)
  Working,   // logging in now
  In,        // logged in
  Failed,    // the last try failed: Account::message says why
};
struct Account
{
  Login state = Login::Out;
  std::string user;          // the name shown (the site's display name once logged in)
  uint32_t points = 0;       // the account's hardcore points
  uint32_t casual_points = 0;
  std::string message;       // why the last login failed (plain words, English for PS4Lang::T)
  bool saved_token = false;  // a token is kept (the next start logs in with it)
};
Account GetAccount();
// Logs in with a user name and a password (in the background: GetAccount says how it went). The password is sent
// to RetroAchievements once and kept nowhere.
void LogIn(const std::string& user, const std::string& password);
// Forgets the token (the file keeps the user name for the next login).
void LogOut();

// ---- The session (00.75): Settings -> RetroAchievements, the in-game Achievements page ----
// Connected: logged in and the server answered the last call. Offline: no answer (the module tries again by itself:
// a login with the saved token, a game's achievements, after 15, 30, 60 then every 120 s). The unlocks made offline
// wait in rc_client and go when the server answers again (while EmuSuNi stays open).
enum class Session
{
  NotLoggedIn,
  Connecting,  // a login on its way
  Connected,
  Offline,
  Failed,      // the login was refused (Status::why)
};
struct Status
{
  Session session = Session::NotLoggedIn;
  std::string user;
  uint32_t points = 0, casual_points = 0;
  std::string why;          // Failed: why (English, for PS4Lang::T)
  int retry_seconds = -1;   // Offline: the seconds to the next try (0: trying now), -1: none planned
};
Status GetStatus();
// The status as one line (English, for PS4Lang::T): "Connected as tester (1234 points)", "No connection to
// RetroAchievements: trying again in 25 s"...
std::string StatusLine(const Status& status);
// The main menu while a RetroAchievements panel shows: the connection checked (a login with the saved token, which
// brings the account's points too) at most once a minute, when no game runs.
void CheckConnection();

// ---- The player's history (00.75): Settings -> RetroAchievements -> My Achievements ----
// The unlocks made in EmuSuNi and the games played in it are kept on the PS4 (data_dir/retroachievements-history.txt:
// shown offline too); the account's games with their progress come from retroachievements.org (its "allprogress" for
// each of the app's consoles, then the games' names: the token is enough), kept in data_dir/cache/ra.
struct HistoryUnlock
{
  int64_t time = 0;  // when (Unix time)
  uint32_t game_id = 0, achievement_id = 0, points = 0;
  std::string game, title, description, badge;  // badge: the achievement's picture address
};
struct HistoryGame
{
  uint32_t game_id = 0, console = 0;
  std::string title, badge;  // badge: the game's icon address
  uint32_t total = 0, unlocked = 0;  // (unlocked: Casual and Hardcore alike)
  int64_t played = 0;           // the last time it was played in EmuSuNi (0: only on the account)
};
struct History
{
  std::vector<HistoryUnlock> unlocks;  // the newest first
  std::vector<HistoryGame> games;      // those played in EmuSuNi first (the last one first), then the account's others
  bool syncing = false;                // the account's games are being updated now
  int64_t synced = 0;                  // when they came from retroachievements.org (0: never)
  std::string sync_error;              // the last update's failure (English, for PS4Lang::T)
};
History GetHistory();
// The account's games from retroachievements.org: when they are older than 10 minutes, or now (Update Now).
void UpdateHistory(bool now);
// RetroAchievements' name of a console ("Nintendo 64").
std::string ConsoleName(uint32_t console);

// ---- The achievements' texts in the app's language (00.76) ----
// RetroAchievements has its achievements' titles and descriptions (and leaderboards') in English only. With the
// setting on, they are machine-translated by MyMemory (api.mymemory.translated.net: free, no key; 500 bytes a text,
// 5000 characters a day for each PS4's address without an e-mail - counted as sent, the names taken out; the rest the
// next day, or when MyMemory says), one text per request as its terms ask, once: when a game's achievements load (in a
// game or in My Achievements). (00.79) Each achievement's title then its description; the achievements on screen first
// (TranslateFirst), then the game asked for last, then the others; an answer that can't be used is kept as English for
// good (never asked again). Kept in data_dir/cache/ra/translations-<language>.txt (offline afterwards). The game's name
// and other names (quoted, capitalised inside a sentence) are kept as they are. English while a translation isn't there.
void SetLanguage(const std::string& code);  // the app's language ("es"); none for English
bool TranslateSetting();                    // the setting (retroachievements.ini "translate"; unset: on in Spanish)
void SetTranslateSetting(bool on);
// The text in the app's language when the setting is on and its translation is known, else as it is.
std::string Translated(const std::string& english);
// Counts the translations that came (the pages are made again when it changes).
int TranslationsVersion();
// (00.79) The achievements on screen (the selected one first, then the rows around it): their texts go first.
void TranslateFirst(const std::vector<uint32_t>& ids);
// How the translations go for a game (its RetroAchievements name): its pages say it instead of English in silence.
struct TranslationState
{
  bool on = false;
  size_t waiting = 0, waiting_game = 0;  // texts waiting: in all, this game's
  int wait_minutes = -1;                 // paused for about this long (-1: going on)
  bool day_over = false;                 // the pause: MyMemory's free day is used (else: no answer, a minute)
};
TranslationState GetTranslationState(const std::string& game);
// Its line (English, for PS4Lang::T): "Translating this game's texts: 92 left"...; "" when nothing of the game waits.
std::string TranslationLine(const TranslationState& state);

// ---- The library's marks (00.77): which of the player's games RetroAchievements has achievements for ----
// RetroAchievements' games of each console in the library, with their files' hashes and their achievements' count,
// come from its "systemgames" (no account needed; one request a console, gzip), kept in data_dir/cache/ra/known-<c>.txt
// and asked for again after 7 days (or My Achievements' Update Now). The player's files are hashed as rc_client does
// (rc_hash: a cartridge's file, a disc's few sectors, an arcade set's name) on a thread of the module's own (low
// priority, 256 KiB, made once), one file after another, waiting while a game runs; each hash kept by the file's path,
// size and date in data_dir/cache/ra/library-hashes.txt (once per file).
struct LibraryFile
{
  std::string key, path;  // the menu's game key and its file (a multi-disc game's first disc)
  SystemId system = SystemId::SNES;
};
struct LibraryInfo
{
  enum class State
  {
    Unchecked,       // not hashed yet, or the console's list not there yet
    NotKnown,        // RetroAchievements doesn't know this file
    NoAchievements,  // it knows the game, which has none yet
    OtherVersion,    // a version RetroAchievements knows but doesn't support (another one has achievements)
    Known,           // this file has achievements
  } state = State::Unchecked;
  uint32_t game_id = 0, achievements = 0, points = 0;
  bool progress = false;  // the account's progress is known (logged in, its games fetched or played here)
  uint32_t unlocked = 0;
};
// The menu's games (after each scan): the work starts or goes on for the new list.
void SetLibrary(const std::vector<LibraryFile>& files);
// While a game runs, the hashing waits (between files).
void PauseLibrary(bool pause);
// Every game's state now, by key (one lock: the menu asks once a second at most, when LibraryVersion changed).
std::map<std::string, LibraryInfo> GetLibraryInfos();
int LibraryVersion();
// How a zipped cartridge's ROM is read (the app's Library::ReadRom); without one, zipped cartridges aren't hashed.
void SetRomReader(bool (*read)(const std::string& path, SystemId system, std::vector<uint8_t>* data,
                               std::string* error));
// The setting (Settings -> RetroAchievements -> Marks on the Games; retroachievements.ini "marks", on by default).
bool MarksSetting();
void SetMarksSetting(bool on);

// ---- The files RetroAchievements knows of a game (00.79): Triangle -> Achievements on a file it doesn't know ----
// (00.80) First the file itself, on the module's thread: its hash (the PS4's copy when its size and date are the same,
// else made now - a file the library's run hasn't reached, or the marks off), the console's list (the PS4's copy, else
// asked for), its state. Then for a file it doesn't take: RetroAchievements' game of that file (a version it knows but
// doesn't support), else of the same name on the console (systemgames' names: brackets, "the" and punctuation left out),
// and the files it supports for it - named when No-Intro's list in libretro's database (GitHub) has their hash:
// cartridges only, kept a month in data_dir/cache/ra/names-<console>.txt (RetroAchievements gives its files' names only
// to its web API's key or its site's pages) -, and the player's own files it supports for that game.
struct Versions
{
  enum class State
  {
    None,
    Loading,  // the file checked now (hashed, the console's list, the names: No-Intro's list once a month)
    Ready,
  } state = State::None;
  LibraryInfo::State file_state = LibraryInfo::State::Unchecked;  // the file's (Unchecked: the list didn't come)
  uint32_t game_id = 0;           // RetroAchievements' game (0: none of that name)
  std::string title;              // its name there
  uint32_t achievements = 0, points = 0;
  bool other_version = false;     // the file is a version it knows, whose achievements don't work with it
  std::string file_name;          // the file's name (No-Intro's when the list has it)
  std::vector<std::string> names; // the files it supports for the game, named (sorted)
  uint32_t unnamed = 0;           // those without a name known here
  std::vector<std::string> yours; // the player's own files it supports for the game
};
void OpenVersions(const LibraryFile& file, const std::string& title);
Versions GetVersions();

// ---- A game (Emulator.cpp) ----
struct Game
{
  SystemId system = SystemId::SNES;
  std::string path;              // the file rc_hash reads: the disc in the console, the arcade set's zip (as the core
                                 // gets it); a cartridge's file (its bytes below)
  const uint8_t* data = nullptr;  // a cartridge's ROM as the core gets it (the zip's file), else null
  size_t size = 0;
  std::string title;             // the app's title (the log)
  std::string library_name;      // retro_get_system_info's: rc_libretro's disallowed settings, the User-Agent
  std::string library_version;
};
// Before the core loads the game: hashes it (a cartridge now, from its bytes; a disc on the module's thread) and
// asks for its achievements. Nothing when no one is logged in.
void GameStarting(const Game& game);
// The core's functions that show its memory (retro_get_memory_data / size), for the regions it has no map of.
void SetCoreMemory(void* (*get_data)(unsigned), size_t (*get_size)(unsigned));
// RETRO_ENVIRONMENT_SET_MEMORY_MAPS: the core's map (copied: cores pass arrays of their stack).
void SetMemoryMap(const retro_memory_map* map);
// After each frame the core ran (every run: fast forward and skipped pictures too).
void DoFrame();
// While the in-game menu is open (the game paused): each pass.
void Paused();
// The game was reset (the in-game menu's Reset Game).
void Reset();
// A state: its achievements' progress beside it (<state>.ra), and that progress back when it is loaded (Casual; a
// state without one starts the achievements' progress afresh).
void StateSaved(const std::string& state_path);
void StateLoaded(const std::string& state_path);
// Another disc was put in (a game on several discs): identified as that disc.
void DiscChanged(const std::string& path);
// The game's end (before the core unloads it).
void GameEnded();

// ---- What the screen shows (AchievementsUi.cpp, IngameMenu.cpp) ----
enum class GameState
{
  None,         // no one logged in, or no game
  Waiting,      // identifying the game / loading its achievements
  Active,       // its achievements are running
  NoAchievements,  // RetroAchievements knows it but it has none yet
  Unknown,      // RetroAchievements doesn't know this file
  Unsupported,  // this console or file format can't be identified
  Offline,      // the server couldn't be reached
  Failed,       // another error (the note says it)
  NoMemory,     // the emulator showed no memory to read
};
struct Summary
{
  GameState state = GameState::None;
  std::string title;            // RetroAchievements' name of the game
  std::string badge;            // its picture's address
  uint32_t unlocked = 0, total = 0, unsupported = 0;
  uint32_t points = 0, total_points = 0;
  uint32_t leaderboards = 0;
  bool mastered = false;        // every achievement unlocked
  std::string rich_presence;    // what the game's doing ("Exploring Hyrule Field"), when it says
  std::string note;             // a short note when it isn't Active (English, for PS4Lang::T)
};
// rich_presence: also the rich presence's message - only on the emulation thread (making it can read the game's
// memory, which the core changes as it runs).
Summary GetSummary(bool rich_presence = false);

struct Entry
{
  uint32_t id = 0;
  std::string title, description;
  std::string badge;            // the picture's address (the locked one for a locked achievement)
  uint32_t points = 0;
  bool unlocked = false;
  std::string progress;         // "5/10" (measured), else ""
  float percent = 0.0f;
  std::string group;            // "Unlocked", "Locked", "Almost There"...: rc_client's buckets' labels
  int64_t unlock_time = 0;
  float rarity = 0.0f;          // % of players who have it (0: unknown)
};
// The game's achievements, by rc_client's groups (with their headers' labels in Entry::group).
std::vector<Entry> List();

// ---- A game's achievements outside a game (00.76): My Achievements -> a game ----
// Every achievement of the game, locked or not (Entry: its picture - the grey one while locked -, points, the progress
// of the last session in EmuSuNi, when it was unlocked, rarity): from data_dir/cache/ra/game-<id>.txt at once (the last
// session in EmuSuNi or the last look: shown offline too), then from retroachievements.org when connected (the game's
// sets and the account's unlocks: the token is enough).
struct GameDetail
{
  enum class State
  {
    None,
    Loading,   // asked for (what the PS4 kept shown meanwhile)
    Ready,     // as the server has it now
    Offline,   // as the PS4 kept it (no connection, or not logged in)
    Failed,    // the server refused (error)
  } state = State::None;
  uint32_t game_id = 0, console = 0;
  std::string title, badge;
  std::vector<Entry> entries;  // grouped: Entry::group "Locked", then "Unlocked" (rc_client's labels)
  uint32_t unlocked = 0, points = 0, total_points = 0;
  int64_t updated = 0;         // when the PS4's copy was made (0: none)
  std::string error;           // (English, for PS4Lang::T)
};
void OpenGame(uint32_t game_id);
GameDetail GetGameDetail();


// Things to show once, in order (the popups).
struct Notice
{
  enum class Kind
  {
    Placard,        // the game's achievements are loaded (or why not)
    Unlocked,       // an achievement
    Mastered,       // every achievement of the game (or of a subset): "Completed" (Casual)
    LeaderboardStarted,
    LeaderboardFailed,
    LeaderboardSubmitted,
    Disconnected,   // an unlock couldn't be sent: retried
    Reconnected,
    Message,        // anything else (a login that failed, a server error)
  };
  Kind kind = Kind::Message;
  std::string title, text;  // (English texts are translated where drawn; RetroAchievements' own aren't)
  std::string badge;
  uint32_t points = 0;
  float seconds = 4.0f;
};
std::vector<Notice> TakeNotices();
// What stays on the screen while it lasts: the challenge indicators (achievements one move away), the progress
// indicator (a measured achievement's "5/10"), the leaderboards' trackers.
struct Overlay
{
  std::vector<std::string> challenges;  // their pictures' addresses
  std::string progress_badge, progress;
  int progress_serial = 0;              // counts its updates (each shows it again)
  std::vector<std::string> trackers;    // their values
};
Overlay GetOverlay();

// An achievement's (or the game's) picture: the file once it is downloaded (data_dir/cache/ra/<name>.png), else ""
// (the download starts on the module's thread; the next call has it).
std::string PictureFile(const std::string& address);

// ---- For the PC tests ----
// The User-Agent sent now ("EmuSuNi/00.73 (PS4 9.00) snes9x_libretro/1.63 rcheevos/12.5").
std::string UserAgent();
// RetroAchievements' console id of one of the app's consoles (RC_CONSOLE_*), 0 for none.
uint32_t ConsoleId(SystemId system);
}  // namespace Achievements
