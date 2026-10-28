// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

#include "Achievements.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>

#include <libretro.h>

#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_libretro.h"

#include "AchievementsDisc.h"
#include "Log.h"
#include "Net.h"

#ifdef __PS4__
#include <orbis/libkernel.h>
// libkernel's affinity call as it really is (OpenOrbis declares it without parameters): bit n of the mask is core n.
extern "C" int emusuni_ra_set_affinity(OrbisPthread thread, uint64_t mask) __asm__("scePthreadSetaffinity");
#endif

namespace Achievements
{
namespace
{
using Clock = std::chrono::steady_clock;

// ---- The module's thread: the server calls, a disc's hashing, the pictures' downloads ----
// One thread for the app's life (256 KiB of stack: libthr keeps a stack once made, the start watchdog's and the
// battery saves' threads are that size too). The server's calls go first: an unlock never waits behind pictures.
class Worker
{
public:
  void Post(std::function<void()> job, bool picture = false)
  {
    {
      std::lock_guard lock(m_mutex);
      if (!m_started && !m_failed)
      {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        m_started = pthread_create(&m_thread, &attr, &Worker::Main, this) == 0;
        pthread_attr_destroy(&attr);
        m_failed = !m_started;
        if (m_failed)
          LogLine("ra: the thread for RetroAchievements couldn't start: its calls run where they're made");
      }
      if (m_started)
      {
        (picture ? m_pictures : m_jobs).push_back(std::move(job));
        m_wake.notify_one();
        return;
      }
    }
    if (!picture)
      job();  // (no thread: never seen on the PS4)
  }

private:
  static void* Main(void* self)
  {
#ifdef __PS4__
    // Off the emulation's core 2 and the N64's RDP thread's core 3, at the PS4's usual priority: it mostly waits for
    // the network.
    emusuni_ra_set_affinity(scePthreadSelf(), (1ull << 0) | (1ull << 1) | (1ull << 4) | (1ull << 5));
    scePthreadSetprio(scePthreadSelf(), 700);
#endif
    static_cast<Worker*>(self)->Loop();
    return nullptr;
  }
  void Loop()
  {
    std::unique_lock lock(m_mutex);
    for (;;)
    {
      m_wake.wait(lock, [this] { return !m_jobs.empty() || !m_pictures.empty(); });
      std::deque<std::function<void()>>& queue = !m_jobs.empty() ? m_jobs : m_pictures;
      std::function<void()> job = std::move(queue.front());
      queue.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
  }
  std::mutex m_mutex;
  std::condition_variable m_wake;
  std::deque<std::function<void()>> m_jobs, m_pictures;
  bool m_started = false, m_failed = false;
  pthread_t m_thread{};
};

// (Made once and never destroyed: the thread waits on it for the app's whole life, and a condition variable
// destroyed under a waiting thread blocks the process's exit - seen on the PC.)
Worker& s_worker = *new Worker;
// (The thread's own: one connection to each host kept open, no new TLS handshake each time.)
Net::Session& s_api_session = *new Net::Session;
Net::Session& s_media_session = *new Net::Session;

// ---- The state (s_mutex guards it; never held while calling into rc_client, which has its own lock and calls back
// into this module) ----
std::mutex s_mutex;
rc_client_t* s_client = nullptr;
std::string s_ini, s_cache, s_ca_file;
// The account.
std::string s_user, s_token, s_display;
Login s_login = Login::Out;
std::string s_login_message;
bool s_login_offline = false;  // the last login failed for want of an answer (not a refusal)
uint32_t s_points = 0, s_casual_points = 0;
// The User-Agent: "EmuSuNi/00.73 (PS4 9.00)", the core's part while a game runs, rcheevos' own part.
std::string s_ua_base, s_ua_core, s_ua_clause;
// The game.
int s_generation = 0;       // each game's start counts: an answer for a game that ended is left out
bool s_game = false;        // a game runs (GameStarting .. GameEnded)
SystemId s_system = SystemId::SNES;
uint32_t s_console = 0;     // its RC_CONSOLE_*
std::string s_title;        // the app's title (the log)
std::string s_library;      // its core's name, as rc_libretro's lists know it
GameState s_state = GameState::None;
std::string s_note;
// What the screen shows.
std::deque<Notice> s_notices;
Overlay s_overlay;
std::vector<std::pair<uint32_t, std::string>> s_challenges;  // achievement id, picture
std::vector<std::pair<uint32_t, std::string>> s_trackers;    // leaderboard id, value
// The pictures asked for (downloading or done) and those that failed (asked again after a minute).
std::set<std::string> s_pictures_asked;
std::vector<std::pair<std::string, Clock::time_point>> s_pictures_failed;
// rc_client's log lines about leaderboards starting and stopping: a few per game (some games start them all the time).
int s_leaderboard_lines = 0;
// The connection (00.75): the server's last answer, the next try without one, the login that checks it.
bool s_net_down = false;
Clock::time_point s_retry_at{};  // ({}: none planned)
int s_retry_step = 0;
Clock::time_point s_checked_at{};
bool s_checking = false;       // a login with the token that checks the connection is on its way (the state shown stays)
std::string s_game_hash;       // the game's hash (its achievements asked for again with it)
bool s_load_retried = false;   // the game's achievements were asked for again while it played
// The history (00.75): data_dir/retroachievements-history.txt, and the account's games from the server
// (data_dir/cache/ra/account-games.txt).
std::string s_history_file, s_account_file;
std::vector<std::pair<std::string, HistoryUnlock>> s_unlocks;  // (user, unlock), the oldest first
std::vector<std::pair<std::string, HistoryGame>> s_played;     // (user, game), the last played last
std::string s_account_user;
std::vector<HistoryGame> s_account_games;
int64_t s_synced = 0;
bool s_syncing = false;
std::string s_sync_error;
std::vector<HistoryGame> s_sync_found;  // (an update's own, on the module's thread)
size_t s_sync_console = 0, s_sync_title = 0;
// The game opened in My Achievements (00.76).
GameDetail s_detail;
int s_detail_serial = 0;
// The achievements' texts in the app's language (00.76): MyMemory's translations, kept in
// cache/ra/translations-<language>.txt; the texts waiting for theirs, one request at a time on the module's thread (its
// low-priority queue, after the server's calls), within the day's characters. (00.79) An achievement's title and
// description one after the other; the achievements on screen first (TranslateFirst), then the game asked for last
// (the one played, the one opened in My Achievements), then the others.
std::string s_language = "en";
int s_translate = -1;  // retroachievements.ini's "translate": -1 unset (on in Spanish), 0, 1
std::string s_tr_file, s_tr_usage_file;
std::map<std::string, std::string> s_tr;                   // English (Clean) -> translation (the English itself: kept so)
struct Pending
{
  std::string english, game;  // (the game's name: kept as it is)
  bool sentence = false;       // a description (its names inside it kept too), not a title
  uint32_t id = 0;             // its achievement
  bool single = false;         // Google lost its names: MyMemory's, alone
};
std::deque<Pending> s_tr_pending;
std::set<std::string> s_tr_failed;     // refused by MyMemory this session (an error answer): not asked again till the next
std::vector<uint32_t> s_tr_first;      // the achievements on screen, the last TranslateFirst's
bool s_tr_running = false;
std::string s_tr_day;          // "2026-10-23": the day the characters below count for
size_t s_tr_chars = 0;         // characters sent to MyMemory that day
Clock::time_point s_tr_paused_until{};
enum class Pause
{
  None,
  Polite,    // a second between two of Google's requests
  NoAnswer,  // MyMemory didn't answer: a minute
  DayOver,   // neither can now: Google left for the day (or after an error), MyMemory's day used
} s_tr_pause = Pause::None;
Clock::time_point s_tr_reset_at{};  // MyMemory's next day as it said (the day's count starts again then)
Clock::time_point s_tr_mm_until{};  // MyMemory's free day used: till then
std::string s_tr_google_off;        // the day Google refused (429, its "unusual traffic" page) or changed its answer
Clock::time_point s_tr_google_retry{};  // after Google's error (no answer): MyMemory till then
int s_tr_google_errors = 0;          // Google's errors in a row (its pause: 1, 5, 15, 60 minutes)
std::vector<std::string> s_tr_inflight;  // the games of the texts on their way now
int s_tr_mm_count = 0;               // MyMemory's translations this session (the log)
int s_tr_version = 0;          // counts the translations that came
int s_tr_failures = 0;         // texts that came back unusable this session (logged a few)
Net::Session& s_tr_session = *new Net::Session;
// The library's marks (00.77): RetroAchievements' known hashes of the library's consoles, the player's files' hashes,
// the thread that makes them (its own, made once, low priority).
bool s_marks = true;  // the setting
struct KnownHash
{
  uint32_t game_id = 0;
  bool supported = false;
};
struct KnownGame
{
  uint32_t achievements = 0, points = 0;
  uint32_t console = 0;
  std::string title;  // (00.79: a game with achievements' name)
};
std::unordered_map<std::string, KnownHash> s_known_hashes;
std::unordered_map<uint32_t, KnownGame> s_known_games;
std::map<uint32_t, int64_t> s_known_fetched;  // console -> when its list came (loaded from the PS4 or the server)
struct HashedFile
{
  uint64_t size = 0;
  int64_t mtime = 0;
  uint32_t console = 0;
  std::string hash;  // "" when it couldn't be made
};
std::unordered_map<std::string, HashedFile> s_lib_hashes;  // by path
std::vector<LibraryFile> s_lib_files;
bool s_lib_work = false, s_lib_paused = false, s_lib_force = false, s_lib_started = false;
int s_lib_version = 0;
std::condition_variable& s_lib_wake = *new std::condition_variable;  // (never destroyed: its thread waits on it)
bool (*s_rom_reader)(const std::string&, SystemId, std::vector<uint8_t>*, std::string*) = nullptr;
Net::Session& s_lib_session = *new Net::Session;

// ---- The core's memory (the emulation thread's only: Environment, DoFrame) ----
std::vector<retro_memory_descriptor> s_descriptors;
std::vector<std::string> s_addrspaces;
bool s_has_map = false;
void* (*s_get_data)(unsigned) = nullptr;
size_t (*s_get_size)(unsigned) = nullptr;
bool s_gpgx = false;  // the game is Genesis Plus GX's (its save RAM's size: CoreMemoryInfo)
rc_libretro_memory_regions_t s_regions{};
bool s_regions_ready = false;
int s_memory_waits = 0;     // frames run since the game's achievements came without memory to read
uint64_t s_frames = 0;      // frames since the regions were made (checked again now and then)
size_t s_regions_total = 0;

void Push(Notice notice)
{
  std::lock_guard lock(s_mutex);
  if (s_notices.size() >= 24)
    s_notices.pop_front();
  s_notices.push_back(std::move(notice));
}

// "r=login2&u=..." -> "login2" (the log names the call, never its body: the password, the token).
std::string ApiName(const char* text)
{
  if (!text)
    return "?";
  const char* r = std::strstr(text, "r=");
  while (r && r != text && r[-1] != '&' && r[-1] != '?')
    r = std::strstr(r + 1, "r=");
  if (!r)
    return "?";
  r += 2;
  const char* end = r;
  while (*end && *end != '&')
    end++;
  return std::string(r, end);
}

std::string Version()
{
  // "00.73" (the test builds) or "00.21" (a public release): numbers that only grow, as RetroAchievements asks.
  return App::VERSION;
}

// The PS4's system software: "(PS4 9.00)", "(PS4)" when it doesn't say.
std::string SystemPart()
{
#ifdef __PS4__
  OrbisKernelSwVersion version = {};
  const uint64_t size = sizeof(version);
  std::memcpy(version.padding, &size, sizeof(size));
  if (sceKernelGetSystemSwVersion(&version) == 0 && version.i_version != 0)
  {
    char text[32];
    std::snprintf(text, sizeof(text), "(PS4 %x.%02x)", version.i_version >> 24, (version.i_version >> 16) & 0xFF);
    return text;
  }
  return "(PS4)";
#else
  return "(PS4; PC test)";
#endif
}

// The core's part of the User-Agent, named as the libretro cores' files are ("snes9x_libretro/1.63"), as RetroArch
// sends it: RetroAchievements tells cores apart by it.
std::string CorePart(SystemId system, const std::string& library_version)
{
  const char* name = nullptr;
  switch (system)
  {
  case SystemId::SNES: name = "snes9x_libretro"; break;
  case SystemId::NES: name = "fceumm_libretro"; break;
  case SystemId::N64: name = "mupen64plus_next_libretro"; break;
  case SystemId::GB:
  case SystemId::GBC:
  case SystemId::GBA: name = "mgba_libretro"; break;
  case SystemId::PS1: name = "swanstation_libretro"; break;
  case SystemId::NDS: name = "melondsds_libretro"; break;
  case SystemId::PSP: name = "ppsspp_libretro"; break;
  case SystemId::DC:
  case SystemId::NAOMI:
  case SystemId::ATOMISWAVE: name = "flycast_libretro"; break;
  case SystemId::SG1000:
  case SystemId::SMS:
  case SystemId::GG:
  case SystemId::MD: name = "genesis_plus_gx_libretro"; break;
  case SystemId::ARCADE: name = "fbneo_libretro"; break;
  case SystemId::THREEDS: name = "azahar_libretro"; break;
  case SystemId::SATURN: name = "yabasanshiro_libretro"; break;
  case SystemId::PCE: name = "mednafen_pce_fast_libretro"; break;
  }
  if (!name)
    return {};
  // Its version as the core says it, spaces made underscores ("(SVN)_ac0d00a"), as RetroArch sends it: the server's
  // rules about a core's versions read it the same way.
  std::string version;
  for (const char c : library_version)
  {
    if (static_cast<unsigned char>(c) < 0x20 || c == '"')
      continue;
    version += c == ' ' ? '_' : c;
  }
  return version.empty() ? std::string(name) : std::string(name) + "/" + version;
}

// rc_libretro's lists of disallowed settings know some cores by other names than theirs.
std::string LibraryKey(const std::string& name)
{
  if (name.rfind("FinalBurn Neo", 0) == 0)
    return "FinalBurn Neo";  // ("FinalBurn Neo (Arcade subset)")
  if (name.rfind("melonDS", 0) == 0)
    return "melonDS";  // ("melonDS DS")
  return name;
}

// ---- The account file (data_dir/retroachievements.ini) ----
void LoadIni()
{
  const std::string text = ReadTextFile(s_ini);
  size_t start = 0;
  while (start < text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    // (00.78: Casual only - the "hardcore" and "hardcore_refused" of 00.73-00.77 are left out, and go at the next save.)
    if (key == "user")
      s_user = value;
    else if (key == "token")
      s_token = value;
    else if (key == "translate")
      s_translate = value == "1" || value == "on" ? 1 : 0;
    else if (key == "marks")
      s_marks = value == "1" || value == "on";
  }
}

// (With s_mutex held.)
void SaveIni()
{
  std::string text = "# EmuSuNi's RetroAchievements account (Settings -> RetroAchievements): the user name and the token\n"
                     "# the site gave at the login - never the password.\n";
  text += "user=" + s_user + "\n";
  if (!s_token.empty())
    text += "token=" + s_token + "\n";
  if (s_translate >= 0)
    text += std::string("translate=") + (s_translate ? "1" : "0") + "\n";
  if (!s_marks)
    text += "marks=0\n";
  if (!WriteTextFile(s_ini, text))
    LogF("ra: %s couldn't be written", s_ini.c_str());
}

// ---- The connection (00.75) ----
// The seconds to the next try without an answer: 15, 30, 60, then every 120 (with s_mutex held).
int NextRetry()
{
  static const int STEPS[] = {15, 30, 60, 120};
  const int seconds = STEPS[std::min(s_retry_step, 3)];
  s_retry_step++;
#ifndef __PS4__
  // (The PC tests: shorter waits.)
  if (const char* fast = std::getenv("EMUSUNI_RA_RETRY"))
    return std::atoi(fast);
#endif
  return seconds;
}

// Every server call's answer: the connection's state, and a try planned when there was none (Retry: in the main menu
// a login with the token; in a game rc_client's own pings and unlocks say when the server is back).
void NoteAnswer(bool reachable)
{
  std::lock_guard lock(s_mutex);
  if (reachable)
  {
    s_net_down = false;
    return;
  }
  s_net_down = true;
  if (s_retry_at == Clock::time_point{})
    s_retry_at = Clock::now() + std::chrono::seconds(NextRetry());
}

// ---- The history's files (00.75): a line per record, its fields between tabs ----
std::string Clean(const std::string& text)
{
  std::string out = text;
  for (char& c : out)
  {
    if (c == '\t' || c == '\n' || c == '\r')
      c = ' ';
  }
  return out;
}

std::vector<std::string> Fields(const std::string& line)
{
  std::vector<std::string> fields;
  size_t start = 0;
  for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1)
    fields.push_back(line.substr(start, tab - start));
  fields.push_back(line.substr(start));
  return fields;
}

uint32_t Number(const std::string& text)
{
  return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

int64_t Time(const std::string& text)
{
  return static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

std::vector<std::string> Lines(const std::string& text)
{
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size())
  {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty() && line[0] != '#')
      lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

// (With s_mutex held.)
void LoadHistory()
{
  for (const std::string& line : Lines(ReadTextFile(s_history_file)))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "U" && f.size() >= 12)
    {
      HistoryUnlock u;
      u.time = Time(f[2]);
      u.game_id = Number(f[3]);
      u.achievement_id = Number(f[4]);
      u.points = Number(f[5]);
      u.game = f[7];
      u.title = f[8];
      u.description = f[9];
      u.badge = f[10];
      s_unlocks.emplace_back(f[1], std::move(u));
    }
    else if (f[0] == "G" && f.size() >= 10)
    {
      HistoryGame g;
      g.played = Time(f[2]);
      g.game_id = Number(f[3]);
      g.console = Number(f[4]);
      g.total = Number(f[5]);
      g.unlocked = Number(f[6]);
      g.title = f[8];
      g.badge = f[9];
      s_played.emplace_back(f[1], std::move(g));
    }
  }
  for (const std::string& line : Lines(ReadTextFile(s_account_file)))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "S" && f.size() >= 3)
    {
      s_account_user = f[1];
      s_synced = Time(f[2]);
    }
    else if (f[0] == "A" && f.size() >= 8)
    {
      HistoryGame g;
      g.game_id = Number(f[1]);
      g.console = Number(f[2]);
      g.total = Number(f[3]);
      g.unlocked = Number(f[4]);
      g.title = f[6];
      g.badge = f[7];
      s_account_games.push_back(std::move(g));
    }
  }
}

// A file written on the module's thread (in order: the last one wins), never on the emulation's.
void Save(const std::string& path, std::string text)
{
  s_worker.Post([path, text = std::move(text)] {
    if (!WriteTextFile(path, text))
      LogF("ra: %s couldn't be written", path.c_str());
  });
}

// (With s_mutex held.) The PS4's record, written on the module's thread (never the emulation's).
void SaveHistory()
{
  // At most 1000 unlocks and 400 games (every user's): the oldest go.
  if (s_unlocks.size() > 1000)
    s_unlocks.erase(s_unlocks.begin(), s_unlocks.begin() + static_cast<long>(s_unlocks.size() - 1000));
  if (s_played.size() > 400)
    s_played.erase(s_played.begin(), s_played.begin() + static_cast<long>(s_played.size() - 400));
  std::string text = "# EmuSuNi's RetroAchievements history: the achievements unlocked in EmuSuNi and the games played "
                     "(Settings -> RetroAchievements -> My Achievements).\n";
  // (The 0s: the mode columns of 00.73-00.77's files - 1 for Hardcore -, kept so that each version reads the other's.)
  char number[96];
  for (const auto& [user, u] : s_unlocks)
  {
    std::snprintf(number, sizeof(number), "\t%lld\t%u\t%u\t%u\t0\t", static_cast<long long>(u.time), u.game_id,
                  u.achievement_id, u.points);
    text += "U\t" + Clean(user) + number + Clean(u.game) + "\t" + Clean(u.title) + "\t" + Clean(u.description) + "\t" +
            Clean(u.badge) + "\t\n";
  }
  for (const auto& [user, g] : s_played)
  {
    std::snprintf(number, sizeof(number), "\t%lld\t%u\t%u\t%u\t%u\t0\t", static_cast<long long>(g.played), g.game_id,
                  g.console, g.total, g.unlocked);
    text += "G\t" + Clean(user) + number + Clean(g.title) + "\t" + Clean(g.badge) + "\n";
  }
  Save(s_history_file, std::move(text));
}

// (With s_mutex held.) The account's games as the last update brought them.
void SaveAccountGames()
{
  std::string text = "# EmuSuNi: the RetroAchievements account's games with their progress (retroachievements.org; "
                     "My Achievements).\n";
  text += "S\t" + Clean(s_account_user) + "\t" + std::to_string(s_synced) + "\n";
  char number[96];
  for (const HistoryGame& g : s_account_games)
  {
    std::snprintf(number, sizeof(number), "\t%u\t%u\t%u\t%u\t0\t", g.game_id, g.console, g.total, g.unlocked);
    text += "A" + std::string(number) + Clean(g.title) + "\t" + Clean(g.badge) + "\n";
  }
  Save(s_account_file, std::move(text));
}

// ---- The achievements' texts in the app's language (00.76) ----
// (With s_mutex held.) The setting as it applies: on, or unset with the app in Spanish; never for English.
bool TranslateOn()
{
  if (s_language.empty() || s_language == "en")
    return false;
  return s_translate == 1 || (s_translate == -1 && s_language == "es");
}

std::string Today()
{
  const std::time_t now = std::time(nullptr);
  std::tm day = {};
  localtime_r(&now, &day);
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &day);
  return text;
}

// The seconds to the next day (a minute after midnight): MyMemory's day starts again.
int SecondsToTomorrow()
{
  const std::time_t now = std::time(nullptr);
  std::tm day = {};
  localtime_r(&now, &day);
  return std::max(60, (23 - day.tm_hour) * 3600 + (59 - day.tm_min) * 60 + (60 - day.tm_sec) + 60);
}

std::string UrlEncode(const std::string& text)
{
  static const char* HEX = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text)
  {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      out += static_cast<char>(c);
    }
    else
    {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 15];
    }
  }
  return out;
}

void AppendUtf8(std::string* out, uint32_t code)
{
  if (code < 0x80)
  {
    *out += static_cast<char>(code);
  }
  else if (code < 0x800)
  {
    *out += static_cast<char>(0xC0 | (code >> 6));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
  else if (code < 0x10000)
  {
    *out += static_cast<char>(0xE0 | (code >> 12));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
  else
  {
    *out += static_cast<char>(0xF0 | (code >> 18));
    *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// A JSON string's value after "key": (MyMemory's answer), its escapes undone: false when it isn't there.
bool JsonString(const std::string& json, const char* key, std::string* value)
{
  const std::string marker = std::string("\"") + key + "\":";
  size_t at = json.find(marker);
  if (at == std::string::npos)
    return false;
  at += marker.size();
  while (at < json.size() && json[at] == ' ')
    at++;
  if (at >= json.size() || json[at] != '"')
    return false;
  value->clear();
  for (size_t i = at + 1; i < json.size(); i++)
  {
    const char c = json[i];
    if (c == '"')
      return true;
    if (c != '\\')
    {
      *value += c;
      continue;
    }
    if (++i >= json.size())
      return false;
    switch (json[i])
    {
    case 'n':
      *value += '\n';
      break;
    case 't':
      *value += '\t';
      break;
    case 'r':
    case 'b':
    case 'f':
      break;
    case 'u':
    {
      if (i + 4 >= json.size())
        return false;
      uint32_t code = static_cast<uint32_t>(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
      i += 4;
      if (code >= 0xD800 && code < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u')
      {
        const uint32_t low = static_cast<uint32_t>(std::strtoul(json.substr(i + 3, 4).c_str(), nullptr, 16));
        if (low >= 0xDC00 && low < 0xE000)
        {
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
      }
      AppendUtf8(value, code);
      break;
    }
    default:
      *value += json[i];  // (\" \\ \/)
      break;
    }
  }
  return false;
}

// A JSON list of strings (Google's answer: ["...","..."]), their escapes undone: false for anything else.
bool JsonStrings(const std::string& json, std::vector<std::string>* out)
{
  out->clear();
  size_t at = json.find_first_not_of(" \r\n\t");
  if (at == std::string::npos || json[at] != '[')
    return false;
  at++;
  while (true)
  {
    at = json.find_first_not_of(" \r\n\t", at);
    if (at == std::string::npos)
      return false;
    if (json[at] == ']')
      return true;
    if (json[at] != '"')
      return false;
    // (The string read as JsonString does it, behind a key of its own.)
    const size_t close = [&] {
      for (size_t i = at + 1; i < json.size(); i++)
      {
        if (json[i] == '\\')
          i++;
        else if (json[i] == '"')
          return i;
      }
      return std::string::npos;
    }();
    if (close == std::string::npos)
      return false;
    std::string value;
    if (!JsonString("\"v\":" + json.substr(at, close - at + 1), "v", &value))
      return false;
    out->push_back(std::move(value));
    at = json.find_first_not_of(" \r\n\t", close + 1);
    if (at == std::string::npos)
      return false;
    if (json[at] == ',')
      at++;
    else if (json[at] != ']')
      return false;
  }
}

// A number after "key": (MyMemory's responseStatus comes as 200 or as "403").
long JsonNumber(const std::string& json, const char* key)
{
  const std::string marker = std::string("\"") + key + "\":";
  size_t at = json.find(marker);
  if (at == std::string::npos)
    return -1;
  at += marker.size();
  while (at < json.size() && (json[at] == ' ' || json[at] == '"'))
    at++;
  return std::strtol(json.c_str() + at, nullptr, 10);
}

// MyMemory's answer: its machine translation (a match "created-by" "MT!": a neural engine's), else an exact match of
// good quality (MateCat's); "" for none. (Its first answer can be a crowd's segment of no quality: "Finish the game" came
// back as "de la pistolera." from "Public_Corpora", quality 0.) Without a list of matches, its answer.
std::string PickTranslation(const std::string& body)
{
  size_t list = body.find("\"matches\":");
  if (list != std::string::npos)
  {
    list += 10;
    while (list < body.size() && body[list] == ' ')
      list++;
  }
  if (list == std::string::npos || list >= body.size() || body[list] != '[')
  {
    std::string text;
    return JsonString(body, "translatedText", &text) ? text : std::string();
  }
  std::string best;
  size_t at = list + 1;
  while (at < body.size())
  {
    while (at < body.size() && (body[at] == ' ' || body[at] == ','))
      at++;
    if (at >= body.size() || body[at] != '{')
      break;
    // The object's end (its strings skipped).
    size_t end = at;
    int depth = 0;
    bool in_string = false;
    for (; end < body.size(); end++)
    {
      const char c = body[end];
      if (in_string)
      {
        if (c == '\\')
          end++;
        else if (c == '"')
          in_string = false;
        continue;
      }
      if (c == '"')
        in_string = true;
      else if (c == '{')
        depth++;
      else if (c == '}' && --depth == 0)
        break;
    }
    if (end >= body.size())
      break;
    const std::string object = body.substr(at, end - at + 1);
    std::string translation, by;
    JsonString(object, "translation", &translation);
    JsonString(object, "created-by", &by);
    if (by == "MT!" && !translation.empty())
      return translation;
    const size_t match_at = object.find("\"match\":");
    const double match = match_at == std::string::npos ? 0.0 : std::strtod(object.c_str() + match_at + 8, nullptr);
    if (best.empty() && match >= 0.99 && JsonNumber(object, "quality") >= 70 && !translation.empty())
      best = translation;
    at = end + 1;
  }
  return best;
}

// The names kept as they are: replaced by {1}, {2}... (MyMemory's engines keep those) and put back after.
struct Protected
{
  std::string text;
  std::vector<std::string> names;
};

bool HasLetter(const std::string& text)
{
  for (size_t i = 0; i < text.size(); i++)
  {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isalpha(c) || c >= 0x80)
    {
      // (Not inside a placeholder: {12}.)
      return true;
    }
  }
  return false;
}

Protected Protect(const std::string& english, const std::string& game, bool sentence)
{
  Protected p;
  p.text = english;
  auto hold = [&p](size_t at, size_t length) {
    const std::string token = "{" + std::to_string(p.names.size() + 1) + "}";
    p.names.push_back(p.text.substr(at, length));
    p.text.replace(at, length, token);
    return token.size();
  };
  // Quoted names ("Master Sword").
  for (size_t at = 0; (at = p.text.find('"', at)) != std::string::npos;)
  {
    const size_t end = p.text.find('"', at + 1);
    if (end == std::string::npos)
      break;
    at += hold(at, end - at + 1);
  }
  // The game's name, and its part before a subtitle ("Sonic the Hedgehog 2", "The Legend of Zelda: ...").
  std::vector<std::string> names = {game};
  for (const char* cut : {":", " - "})
  {
    const size_t at = game.find(cut);
    if (at != std::string::npos && at >= 4)
      names.push_back(game.substr(0, at));
  }
  for (const std::string& name : names)
  {
    if (name.size() < 4)
      continue;
    for (size_t at = 0; (at = p.text.find(name, at)) != std::string::npos;)
      at += hold(at, name.size());
  }
  if (!sentence)
    return p;
  // A description's names: capitalised words inside a sentence (and the level number after one: "World 8-4"),
  // and words in capitals ("HP"); not when most words are capitalised (a title's way).
  struct Word
  {
    size_t start, end;  // the word without its punctuation
    bool name, number, sentence_start;
  };
  std::vector<Word> words;
  size_t capitalised = 0, counted = 0;
  bool next_starts = true;
  for (size_t at = 0; at < p.text.size();)
  {
    while (at < p.text.size() && p.text[at] == ' ')
      at++;
    if (at >= p.text.size())
      break;
    size_t end = p.text.find(' ', at);
    if (end == std::string::npos)
      end = p.text.size();
    size_t s = at, e = end;
    while (s < e && std::strchr("([\"'", p.text[s]))
      s++;
    while (e > s && std::strchr(".,!?;:)]\"'", p.text[e - 1]))
      e--;
    Word word{s, e, false, false, next_starts};
    const std::string core = p.text.substr(s, e - s);
    bool letters = false, upper = true, digits = false;
    for (const char c : core)
    {
      if (std::isalpha(static_cast<unsigned char>(c)))
      {
        letters = true;
        upper = upper && std::isupper(static_cast<unsigned char>(c));
      }
      digits = digits || std::isdigit(static_cast<unsigned char>(c));
    }
    const bool token = !core.empty() && core.front() == '{' && core.back() == '}';
    word.number = !letters && digits;
    if (letters && !token)
    {
      const bool capital = std::isupper(static_cast<unsigned char>(core[0])) != 0;
      if (!word.sentence_start)
      {
        counted++;
        capitalised += capital ? 1 : 0;
      }
      word.name = (capital && !word.sentence_start && core != "I") || (upper && core.size() >= 2);
    }
    words.push_back(word);
    const char last = p.text[end - 1];
    next_starts = last == '.' || last == '!' || last == '?' || last == ':';
    at = end;
  }
  if (counted == 0 || capitalised * 4 >= counted * 3)
    return p;
  // The runs of names, the last first (the text before them keeps its places).
  std::vector<std::pair<size_t, size_t>> runs;
  for (size_t i = 0; i < words.size();)
  {
    if (!words[i].name)
    {
      i++;
      continue;
    }
    size_t j = i;
    while (j + 1 < words.size() && (words[j + 1].name || words[j + 1].number) && words[j].end == p.text.find(' ', words[j].start))
      j++;
    runs.emplace_back(words[i].start, words[j].end);
    i = j + 1;
  }
  for (auto it = runs.rbegin(); it != runs.rend(); ++it)
    hold(it->first, it->second - it->first);
  return p;
}

// The names put back: false when a placeholder didn't come back once (the translation isn't used).
bool Restore(std::string* text, const std::vector<std::string>& names)
{
  // (Some engines space them: "{ 1 }".)
  for (size_t at = 0; (at = text->find("{ ", at)) != std::string::npos;)
    text->erase(at + 1, 1);
  for (size_t at = 0; (at = text->find(" }", at)) != std::string::npos;)
    text->erase(at, 1);
  for (size_t i = names.size(); i-- > 0;)
  {
    const std::string token = "{" + std::to_string(i + 1) + "}";
    const size_t at = text->find(token);
    if (at == std::string::npos || text->find(token, at + 1) != std::string::npos)
      return false;
    text->replace(at, token.size(), names[i]);
  }
  return true;
}

// (With s_mutex held.) The language's translations from the PS4.
void LoadTranslations()
{
  s_tr.clear();
  s_tr_pending.clear();
  s_tr_failed.clear();
  s_tr_first.clear();
  s_tr_paused_until = {};
  s_tr_pause = Pause::None;
  if (s_language.empty() || s_language == "en" || s_cache.empty())
    return;
  s_tr_file = s_cache + "/translations-" + s_language + ".txt";
  s_tr_usage_file = s_cache + "/translate-usage.txt";
  for (const std::string& line : Lines(ReadTextFile(s_tr_file)))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "T" && f.size() >= 3 && !f[1].empty() && !f[2].empty())
      s_tr[f[1]] = f[2];
  }
  // The day's record: MyMemory's characters sent, Google given up (00.79).
  const std::vector<std::string> lines = Lines(ReadTextFile(s_tr_usage_file));
  const std::vector<std::string> usage = Fields(lines.empty() ? std::string() : lines.front());
  if (usage.size() >= 2 && usage[0] == Today())
  {
    s_tr_day = usage[0];
    s_tr_chars = std::strtoul(usage[1].c_str(), nullptr, 10);
    if (usage.size() >= 3 && usage[2] == "google-off")
      s_tr_google_off = usage[0];
  }
}

void TranslateNext();

// (With s_mutex held.) The texts of these achievements first, in this order (each one's title, then its description).
void MoveFirst(const std::vector<uint32_t>& ids)
{
  if (ids.empty() || s_tr_pending.empty())
    return;
  std::deque<Pending> first, rest;
  for (const uint32_t id : ids)
  {
    for (const Pending& item : s_tr_pending)
    {
      if (item.id == id && id != 0)
        first.push_back(item);
    }
  }
  if (first.empty())
    return;
  for (Pending& item : s_tr_pending)
  {
    if (item.id == 0 || std::find(ids.begin(), ids.end(), item.id) == ids.end())
      rest.push_back(std::move(item));
  }
  first.insert(first.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
  s_tr_pending = std::move(first);
}

// A game's achievements to translate (00.79): each one's title then its description, those not known yet, before the
// texts already waiting (the game asked for last first; the same text waiting further back comes forward) - but after
// the achievements on screen.
struct TextPair
{
  uint32_t id = 0;
  std::string title, description;
};
void QueueTexts(const std::vector<TextPair>& items, const std::string& game)
{
  bool start = false;
  {
    std::lock_guard lock(s_mutex);
    if (!TranslateOn())
      return;
    std::deque<Pending> block;
    std::set<std::string> keys;
    auto add = [&](const std::string& text, bool sentence, uint32_t id) {
      const std::string key = Clean(text);
      if (key.size() < 2 || s_tr.count(key) || s_tr_failed.count(key) || !keys.insert(key).second)
        return;
      block.push_back({key, game, sentence, id});
    };
    for (const TextPair& item : items)
    {
      add(item.title, false, item.id);
      add(item.description, true, item.id);
    }
    if (block.empty())
      return;
    for (Pending& item : s_tr_pending)
    {
      if (!keys.count(item.english))
        block.push_back(std::move(item));
    }
    s_tr_pending = std::move(block);
    MoveFirst(s_tr_first);
    if (!s_tr_running && Clock::now() >= s_tr_paused_until)
    {
      s_tr_running = true;
      start = true;
    }
  }
  if (start)
    s_worker.Post(TranslateNext, true);
}

// Translations waiting after a pause (between Google's requests, without an answer, both services' day over): on again
// once it is over.
void TranslateResume()
{
  {
    std::lock_guard lock(s_mutex);
    if (s_tr_running || s_tr_pending.empty() || !TranslateOn() || Clock::now() < s_tr_paused_until)
      return;
    s_tr_running = true;
  }
  s_worker.Post(TranslateNext, true);
}

// The characters of a text as MyMemory counts them (UTF-8's code points).
size_t Characters(const std::string& text)
{
  size_t count = 0;
  for (const unsigned char c : text)
    count += (c & 0xC0) != 0x80 ? 1 : 0;
  return count;
}

// MyMemory's own wait when its free day is used ("... NEXT AVAILABLE IN 10 HOURS 25 MINUTES 52 SECONDS ..."), in
// seconds; 0 when it doesn't say.
int MyMemoryWait(const std::string& body)
{
  const size_t at = body.find("NEXT AVAILABLE IN");
  if (at == std::string::npos)
    return 0;
  int seconds = 0;
  const char* p = body.c_str() + at + 17;
  for (int parts = 0; parts < 3; parts++)
  {
    while (*p == ' ')
      p++;
    if (!std::isdigit(static_cast<unsigned char>(*p)))
      break;
    char* end = nullptr;
    const long value = std::strtol(p, &end, 10);
    p = end;
    while (*p == ' ')
      p++;
    if (std::strncmp(p, "HOUR", 4) == 0)
      seconds += static_cast<int>(value) * 3600;
    else if (std::strncmp(p, "MINUTE", 6) == 0)
      seconds += static_cast<int>(value) * 60;
    else if (std::strncmp(p, "SECOND", 6) == 0)
      seconds += static_cast<int>(value);
    else
      break;
    while (std::isalpha(static_cast<unsigned char>(*p)))
      p++;
  }
  return seconds > 0 && seconds <= 26 * 3600 ? seconds : 0;
}

// A translation as kept: its form cleaned, and the full stop a machine adds to a text that had none taken away.
std::string Finish(const std::string& english, std::string text)
{
  text = Clean(text);
  if (!english.empty() && english.back() != '.' && text.size() >= 2 && text.back() == '.' && text[text.size() - 2] != '.')
    text.pop_back();
  return text;
}

// (With s_mutex held.) Google is to be asked now: not given up for the day, its pause after an error over.
bool GoogleNow()
{
  return s_tr_google_off != Today() && Clock::now() >= s_tr_google_retry;
}

// (With s_mutex held.) The day's record (translate-usage.txt): MyMemory's characters, Google given up or not.
std::string UsageText()
{
  return s_tr_day + "\t" + std::to_string(s_tr_chars) + "\t" + (s_tr_google_off == s_tr_day ? "google-off" : "") + "\n";
}

// Google's translation (00.79): the web page translator's own endpoint (translate.googleapis.com, client "gtx": no key,
// unofficial - it may change or stop some day), many texts in one request (a "q" each, POSTed, about 24 KB at most: a
// game's 348 texts in one), its answer a list of the texts in order. On the module's thread; false when there was
// nothing for it (MyMemory then).
bool GoogleBatch()
{
  constexpr size_t BODY_BYTES = 24 << 10;
  constexpr size_t TEXTS = 400;
  std::vector<Pending> batch;
  std::string language, tr_file;
  {
    std::lock_guard lock(s_mutex);
    if (!GoogleNow())
      return false;
    // The texts in the queue's order (those on screen and the game asked for last first), not those Google gave
    // back unusable (MyMemory's, one by one).
    size_t bytes = 0;
    for (auto it = s_tr_pending.begin(); it != s_tr_pending.end() && batch.size() < TEXTS;)
    {
      if (it->single)
      {
        ++it;
        continue;
      }
      const size_t size = it->english.size() * 3 / 2 + 3;
      if (!batch.empty() && bytes + size > BODY_BYTES)
        break;
      bytes += size;
      batch.push_back(std::move(*it));
      it = s_tr_pending.erase(it);
    }
    if (batch.empty())
      return false;
    for (const Pending& item : batch)
      s_tr_inflight.push_back(item.game);
    language = s_language == "zh" ? std::string("zh-CN") : s_language;
    tr_file = s_tr_file;
  }
  std::vector<Protected> protect;
  std::string body;
  for (const Pending& item : batch)
  {
    protect.push_back(Protect(item.english, item.game, item.sentence));
    body += (body.empty() ? "q=" : "&q=") + UrlEncode(protect.back().text);
  }
  Net::Request request;
  std::string host = "https://translate.googleapis.com";
#ifndef __PS4__
  if (const char* test = std::getenv("EMUSUNI_GT_HOST"))
    host = test;  // (the PC tests' stand-in)
#endif
  request.url = host + "/translate_a/t?client=gtx&sl=en&tl=" + language;
  request.method = "POST";
  request.body = body;
  request.content_type = "application/x-www-form-urlencoded;charset=utf-8";
  request.user_agent = UserAgent();
  request.ca_file = s_ca_file;
  request.timeout_seconds = 30;
  request.error_body = true;
  request.max_bytes = 2 << 20;
  const auto start = Clock::now();
  const Net::Response answer = Net::Fetch(request, &s_tr_session);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  std::vector<std::string> texts;
  const bool listed = answer.status == 200 && JsonStrings(answer.body, &texts) && texts.size() == batch.size();
  std::string games;
  std::string lines;
  size_t done = 0, back = 0;
  {
    std::lock_guard lock(s_mutex);
    s_tr_inflight.clear();
    for (const Pending& item : batch)
    {
      if (games.find("\"" + item.game + "\"") == std::string::npos && games.size() < 200)
        games += (games.empty() ? "\"" : ", \"") + item.game + "\"";
    }
    if (listed)
    {
      // Each text with its names back; one whose names Google lost goes to MyMemory.
      std::vector<Pending> again;
      for (size_t i = 0; i < batch.size(); i++)
      {
        std::string text = texts[i];
        if (!text.empty() && Restore(&text, protect[i].names))
        {
          const std::string translated = Finish(batch[i].english, text);
          s_tr[batch[i].english] = translated;
          lines += "T\t" + batch[i].english + "\t" + translated + "\n";
          done++;
        }
        else
        {
          batch[i].single = true;
          again.push_back(std::move(batch[i]));
        }
      }
      back = again.size();
      s_tr_pending.insert(s_tr_pending.begin(), std::make_move_iterator(again.begin()),
                          std::make_move_iterator(again.end()));
      s_tr_version++;
      s_tr_google_errors = 0;
      // (Polite: a second between two requests.)
      s_tr_paused_until = Clock::now() + std::chrono::seconds(1);
      s_tr_pause = Pause::Polite;
      LogF("ra: translations: Google: %zu texts of %s in %.0f ms (%zu KB sent)%s", done, games.c_str(), ms,
           body.size() / 1024, back ? (", " + std::to_string(back) + " lost their names: MyMemory's").c_str() : "");
    }
    else
    {
      // Back in the queue, first; MyMemory meanwhile. Blocked (429, its "unusual traffic" page) or an answer of
      // another form: Google is left for the day; no answer or a server's error: again after 1, 5, 15, then 60 minutes.
      s_tr_pending.insert(s_tr_pending.begin(), std::make_move_iterator(batch.begin()),
                          std::make_move_iterator(batch.end()));
      const bool blocked = answer.status == 429 || answer.status == 403 ||
                           answer.body.find("unusual traffic") != std::string::npos ||
                           answer.body.find("<html") != std::string::npos || answer.body.find("<HTML") != std::string::npos;
      const bool other_form = answer.status == 200 && !blocked;
      if (blocked || other_form)
      {
        if (s_tr_day != Today())
        {
          s_tr_day = Today();
          s_tr_chars = 0;
        }
        s_tr_google_off = s_tr_day;
        LogF("ra: translations: Google %s (%s, %.0f ms): MyMemory for the rest of the day", blocked ? "refused" : "answered something else",
             answer.status > 0 ? ("HTTP " + std::to_string(answer.status)).c_str() : answer.error.c_str(), ms);
      }
      else
      {
        static const int MINUTES[] = {1, 5, 15, 60};
        const int minutes = MINUTES[std::min(s_tr_google_errors, 3)];
        s_tr_google_errors++;
        s_tr_google_retry = Clock::now() + std::chrono::minutes(minutes);
        LogF("ra: translations: Google didn't answer (%s, %.0f ms): MyMemory meanwhile, Google again in %d min",
             answer.status > 0 ? ("HTTP " + std::to_string(answer.status)).c_str() : answer.error.c_str(), ms, minutes);
      }
      s_tr_paused_until = {};
      s_tr_pause = Pause::None;
    }
  }
  if (!lines.empty())
  {
    // (Kept: the lines added to the language's file.)
    if (FILE* file = std::fopen(tr_file.c_str(), "a"))
    {
      std::fputs(lines.c_str(), file);
      std::fclose(file);
    }
  }
  if (!listed)
  {
    std::string usage, usage_file;
    {
      std::lock_guard lock(s_mutex);
      usage = UsageText();
      usage_file = s_tr_usage_file;
    }
    WriteTextFile(usage_file, usage);
  }
  bool next;
  {
    std::lock_guard lock(s_mutex);
    next = !s_tr_pending.empty() && TranslateOn() && Clock::now() >= s_tr_paused_until;
    s_tr_running = next;
  }
  if (next)
    s_worker.Post(TranslateNext, true);
  return true;
}

// One text to MyMemory (its terms: a segment a request; the fallback when Google can't), on the module's thread; then
// the next one.
void MyMemoryNext()
{
  // (MyMemory's free day without an e-mail: 5000 characters for the PS4's address, counted as sent - the text with
  // its names taken out. Answers it couldn't use count too, so they are kept in English for good: never asked again.)
  constexpr size_t DAY_CHARACTERS = 5000;
  Pending item;
  std::string language, usage_file, tr_file;
  size_t used = 0;
  {
    std::lock_guard lock(s_mutex);
    if (s_tr_pending.empty() || !TranslateOn())
    {
      s_tr_running = false;
      return;
    }
    item = s_tr_pending.front();
    s_tr_pending.pop_front();
    s_tr_inflight.push_back(item.game);
    used = s_tr_chars;
    language = s_language == "zh" ? std::string("zh-CN") : s_language;
    usage_file = s_tr_usage_file;
    tr_file = s_tr_file;
  }
  const Protected p = Protect(item.english, item.game, item.sentence);
  const size_t chars = Characters(p.text);
  std::string translated;
  enum class Outcome
  {
    Done,     // translated
    Kept,     // MyMemory's answer can't be used (or the text is too long for it): English, for good
    Again,    // no answer: the same text in a minute
    Day,      // MyMemory's free day is used
    Refused,  // an error answer: English this session
  } outcome = Outcome::Refused;
  bool counted = false;
  int wait = 0;
  if (!HasLetter(p.text) || p.text.find_first_not_of("{}0123456789 ") == std::string::npos)
  {
    translated = item.english;  // (nothing but names)
    outcome = Outcome::Done;
  }
  else if (p.text.size() > 480)
  {
    outcome = Outcome::Kept;
  }
  else if (used + chars > DAY_CHARACTERS)
  {
    outcome = Outcome::Day;
  }
  else
  {
    Net::Request request;
    std::string host = "https://api.mymemory.translated.net";
#ifndef __PS4__
    if (const char* test = std::getenv("EMUSUNI_MT_HOST"))
      host = test;  // (the PC tests' stand-in)
#endif
    request.url = host + "/get?q=" + UrlEncode(p.text) + "&langpair=en%7C" + language;
    request.user_agent = UserAgent();
    request.ca_file = s_ca_file;
    request.timeout_seconds = 20;
    request.error_body = true;
    request.max_bytes = 256 << 10;
    const Net::Response answer = Net::Fetch(request, &s_tr_session);
    std::string text;
    const long status = JsonNumber(answer.body, "responseStatus");
    const bool quota = answer.body.find("\"quotaFinished\":true") != std::string::npos || status == 429 ||
                       answer.status == 429 || answer.body.find("MYMEMORY WARNING") != std::string::npos;
    if (answer.status <= 0 || answer.status >= 500)
    {
      outcome = Outcome::Again;
    }
    else if (quota)
    {
      outcome = Outcome::Day;
      wait = MyMemoryWait(answer.body);
    }
    else if (answer.status == 200 && status == 200)
    {
      counted = true;
      if (!(text = PickTranslation(answer.body)).empty() && Restore(&text, p.names))
      {
        translated = Finish(item.english, text);
        outcome = Outcome::Done;
      }
      else
      {
        outcome = Outcome::Kept;
      }
    }
  }
  bool next = false, keep = false;
  {
    std::lock_guard lock(s_mutex);
    s_tr_inflight.clear();
    if (counted)
      s_tr_chars += chars;
    switch (outcome)
    {
    case Outcome::Done:
    case Outcome::Kept:
      if (outcome == Outcome::Kept)
      {
        translated = item.english;
        if (s_tr_failures++ < 5)
          LogF("ra: translations: \"%s\" kept in English (MyMemory's answer can't be used)", item.english.c_str());
      }
      s_tr[item.english] = translated;
      s_tr_version++;
      keep = true;
      // (A line in the log every 25 texts, and when the queue is done.)
      if (++s_tr_mm_count % 25 == 0 || s_tr_pending.empty())
        LogF("ra: translations: MyMemory: %d texts this session (the last of \"%s\"), %zu characters today, %zu waiting",
             s_tr_mm_count, item.game.c_str(), s_tr_chars, s_tr_pending.size());
      break;
    case Outcome::Again:
      s_tr_pending.push_front(item);
      s_tr_paused_until = Clock::now() + std::chrono::seconds(60);
      s_tr_pause = Pause::NoAnswer;
      LogLine("ra: translations: MyMemory didn't answer: again in a minute");
      break;
    case Outcome::Day:
    {
      s_tr_pending.push_front(item);
      const int seconds = wait > 0 ? wait + 60 : SecondsToTomorrow();
      s_tr_mm_until = Clock::now() + std::chrono::seconds(seconds);
      if (wait > 0)
      {
        s_tr_chars = DAY_CHARACTERS;
        s_tr_reset_at = s_tr_mm_until;
      }
      // (Google goes on if it can: TranslateNext decides.)
      LogF("ra: translations: MyMemory's free day is used (%zu characters sent today%s): its rest in %d min (%zu texts)",
           s_tr_chars, wait > 0 ? ", as it says" : "", seconds / 60, s_tr_pending.size());
      break;
    }
    case Outcome::Refused:
      s_tr_failed.insert(item.english);
      if (s_tr_failures++ < 5)
        LogF("ra: translations: \"%s\" left in English (MyMemory refused it)", item.english.c_str());
      break;
    }
    next = !s_tr_pending.empty() && TranslateOn() &&
           (Clock::now() >= s_tr_paused_until || outcome == Outcome::Day);
    s_tr_running = next;
  }
  if (keep)
  {
    // (Kept: a line added to the language's file.)
    if (FILE* file = std::fopen(tr_file.c_str(), "a"))
    {
      std::fprintf(file, "T\t%s\t%s\n", item.english.c_str(), translated.c_str());
      std::fclose(file);
    }
  }
  if (counted || outcome == Outcome::Day)
  {
    std::string usage;
    {
      std::lock_guard lock(s_mutex);
      usage = UsageText();
    }
    WriteTextFile(usage_file, usage);
  }
  if (next)
    s_worker.Post(TranslateNext, true);
}

// The next step of the queue (00.79): Google for many texts at once while it answers, else MyMemory for one, while its
// day lasts; else a pause till the first of them can again.
void TranslateNext()
{
  bool google = false, mymemory = false;
  {
    std::lock_guard lock(s_mutex);
    if (s_tr_pending.empty() || !TranslateOn())
    {
      s_tr_running = false;
      return;
    }
    const std::string today = Today();
    if (s_tr_day != today || (s_tr_reset_at != Clock::time_point{} && Clock::now() >= s_tr_reset_at))
    {
      s_tr_day = today;
      s_tr_chars = 0;
      s_tr_reset_at = {};
      s_tr_mm_until = {};
    }
    google = GoogleNow() && std::any_of(s_tr_pending.begin(), s_tr_pending.end(), [](const Pending& p) { return !p.single; });
    mymemory = Clock::now() >= s_tr_mm_until;
    if (!google && !mymemory)
    {
      s_tr_paused_until = s_tr_google_off == today ? s_tr_mm_until : std::min(s_tr_mm_until, s_tr_google_retry);
      s_tr_pause = Pause::DayOver;
      s_tr_running = false;
      return;
    }
  }
  if (google && GoogleBatch())
    return;
  if (mymemory)
  {
    MyMemoryNext();
    return;
  }
  std::lock_guard lock(s_mutex);
  s_tr_running = false;
}

// ---- rc_client's callbacks ----
uint32_t RC_CCONV ReadMemory(uint32_t address, uint8_t* buffer, uint32_t bytes, rc_client_t*)
{
  // (Only inside rc_client_do_frame and rc_client_idle on the emulation thread: background reads are off.)
  if (!s_regions_ready)
    return 0;
  return rc_libretro_memory_read(&s_regions, address, buffer, bytes);
}

void RC_CCONV ServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t*)
{
  Net::Request http;
  http.url = request->url ? request->url : "";
  if (request->post_data)
  {
    http.method = "POST";
    http.body = request->post_data;
    http.content_type = request->content_type ? request->content_type : "application/x-www-form-urlencoded";
  }
  http.user_agent = UserAgent();
  http.ca_file = s_ca_file;
  http.timeout_seconds = 30;
  http.error_body = true;  // (rcheevos reads the server's error answers too)
  http.max_bytes = 16 << 20;
  const std::string api = ApiName(request->post_data ? request->post_data : request->url);
  s_worker.Post([http = std::move(http), callback, data, api]() mutable {
    const auto start = Clock::now();
    const Net::Response answer = Net::Fetch(http, &s_api_session);
    std::fill(http.body.begin(), http.body.end(), '\0');  // (the password or the token: not kept)
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    // (The connection's state, 00.75: no answer, or the server's front saying it can't reach it.)
    const long status = answer.status;
    NoteAnswer(status > 0 && status != 502 && status != 503 && status != 504 && !(status >= 520 && status <= 527));
    rc_api_server_response_t response = {};
    std::string why;
    if (answer.status > 0)
    {
      response.body = answer.body.data();
      response.body_length = answer.body.size();
      response.http_status_code = static_cast<int>(answer.status);
      LogF("ra: %s: HTTP %ld (%zu bytes, %.0f ms)", api.c_str(), answer.status, answer.body.size(), ms);
    }
    else
    {
      // No answer: rcheevos tries again (an unlock until it reaches the server), its message the plain reason.
      why = answer.plain.empty() ? answer.error : answer.plain;
      response.body = why.c_str();
      response.body_length = why.size();
      response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
      LogF("ra: %s: no answer (%s; %.0f ms)", api.c_str(), answer.error.c_str(), ms);
    }
    callback(&response, data);
  });
}

void RC_CCONV ClientLog(const char* message, const rc_client_t*)
{
  if (!message)
    return;
  // (Some games start and cancel their leaderboards all the time: a few of those lines per game.)
  if (std::strncmp(message, "Leaderboard ", 12) == 0 &&
      (std::strstr(message, " started: ") || std::strstr(message, " canceled: ")))
  {
    std::lock_guard lock(s_mutex);
    if (s_leaderboard_lines++ >= 12)
      return;
  }
  LogF("ra: %s", message);
}

void RC_CCONV HashLog(const char* message, const rc_hash_iterator_t*)
{
  if (message)
    LogF("ra: hash: %s", message);
}

// rc_libretro's lines about the console's memory ("Registered 0x20000 bytes of System RAM at $000000", "Could not map
// region starting at $..."), kept while the regions are made: logged when they are made, and for the first try that
// found no memory (a core may show its memory only after a few frames: not a line per frame).
std::vector<std::string>* s_memory_lines = nullptr;
bool s_memory_failure_logged = false;

void RC_CCONV MemoryLog(const char* message)
{
  if (message && s_memory_lines)
    s_memory_lines->push_back(message);
}

rc_clock_t RC_CCONV Milliseconds(const rc_client_t*)
{
  return static_cast<rc_clock_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

std::string Copy(const char* text)
{
  return text ? std::string(text) : std::string();
}

// ---- The history (00.75) ----
// The game in the history: its progress now, played now (when its achievements load, after each unlock).
void RecordGame(rc_client_t* client)
{
  const rc_client_game_t* game = rc_client_get_game_info(client);
  // (Not a version RetroAchievements' achievements weren't made for: its game id is a virtual one.)
  if (!game || game->id == 0 || game->id >= 1000000000u || !rc_client_is_game_loaded(client))
    return;
  rc_client_user_game_summary_t summary = {};
  rc_client_get_user_game_summary(client, &summary);
  HistoryGame g;
  g.game_id = game->id;
  g.console = game->console_id;
  g.title = Copy(game->title);
  g.badge = Copy(game->badge_url);
  g.total = summary.num_core_achievements;
  g.unlocked = summary.num_unlocked_achievements;
  g.played = static_cast<int64_t>(std::time(nullptr));
  std::lock_guard lock(s_mutex);
  const std::string user = s_user;
  s_played.erase(std::remove_if(s_played.begin(), s_played.end(),
                                [&](const auto& item) { return item.first == user && item.second.game_id == g.game_id; }),
                 s_played.end());
  s_played.emplace_back(user, std::move(g));
  s_lib_version++;  // (the marks' progress, 00.77)
  SaveHistory();
}

void RecordUnlock(rc_client_t* client, const rc_client_achievement_t* a)
{
  const rc_client_game_t* game = rc_client_get_game_info(client);
  HistoryUnlock u;
  u.time = static_cast<int64_t>(std::time(nullptr));
  u.game_id = game ? game->id : 0;
  u.achievement_id = a->id;
  u.points = a->points;
  u.game = game ? Copy(game->title) : std::string();
  u.title = Copy(a->title);
  u.description = Copy(a->description);
  u.badge = Copy(a->badge_url);
  {
    std::lock_guard lock(s_mutex);
    s_unlocks.emplace_back(s_user, std::move(u));
    SaveHistory();
  }
  RecordGame(client);
}

// ---- A game's achievements kept on the PS4 (00.76): My Achievements' games, offline too ----
std::string GameFile(uint32_t game_id)
{
  std::lock_guard lock(s_mutex);
  return s_cache + "/game-" + std::to_string(game_id) + ".txt";
}

// The achievement set as it is now (locked or not, the progress of a measured one, when unlocked), written on the
// module's thread: when the game's achievements load and when the game ends.
void SaveGameCache(rc_client_t* client)
{
  const rc_client_game_t* game = rc_client_get_game_info(client);
  if (!game || game->id == 0 || game->id >= 1000000000u || !rc_client_is_game_loaded(client))
    return;
  rc_client_achievement_list_t* list = rc_client_create_achievement_list(
      client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
  if (!list)
    return;
  std::string text = "# EmuSuNi: a RetroAchievements game's achievements as last seen (My Achievements).\n";
  text += "D\t" + std::to_string(game->id) + "\t" + std::to_string(game->console_id) + "\t" +
          std::to_string(static_cast<long long>(std::time(nullptr))) + "\t" + Clean(Copy(game->title)) + "\t" +
          Clean(Copy(game->badge_url)) + "\n";
  for (uint32_t b = 0; b < list->num_buckets; b++)
  {
    for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++)
    {
      const rc_client_achievement_t* a = list->buckets[b].achievements[i];
      if (a->id >= 101000000)
        continue;  // (the server's warnings)
      char number[160];
      std::snprintf(number, sizeof(number), "A\t%u\t%u\t%d\t0\t%lld\t%d\t", a->id, a->points,
                    a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE ? 1 : 0, static_cast<long long>(a->unlock_time),
                    static_cast<int>(a->rarity * 100.0f));  // (the 0: the mode column, Casual - as SaveHistory)
      text += number + Clean(Copy(a->measured_progress)) + "\t" + Clean(Copy(a->title)) + "\t" +
              Clean(Copy(a->description)) + "\t" + Clean(Copy(a->badge_url)) + "\t" + Clean(Copy(a->badge_locked_url)) +
              "\n";
    }
  }
  rc_client_destroy_achievement_list(list);
  Save(GameFile(game->id), std::move(text));
}

// The game's texts to translate (00.76), each achievement's title and description (00.79), as the in-game page lists
// them (00.78: not its leaderboards' - Hardcore's only, rc_client runs none in Casual).
void QueueGameTexts(rc_client_t* client)
{
  const rc_client_game_t* game = rc_client_get_game_info(client);
  const std::string name = game ? Copy(game->title) : std::string();
  std::vector<TextPair> items;
  if (rc_client_achievement_list_t* list = rc_client_create_achievement_list(
          client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS))
  {
    for (uint32_t b = 0; b < list->num_buckets; b++)
    {
      for (uint32_t i = 0; i < list->buckets[b].num_achievements; i++)
      {
        const rc_client_achievement_t* a = list->buckets[b].achievements[i];
        if (a->id < 101000000)
          items.push_back({a->id, Copy(a->title), Copy(a->description)});
      }
    }
    rc_client_destroy_achievement_list(list);
  }
  QueueTexts(items, name);
}

void RC_CCONV Event(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
  {
    const rc_client_achievement_t* a = event->achievement;
    if (!a)
      break;
    Notice notice;
    // The server's warnings come as achievements of their own (101000001: "Warning: Unknown Emulator", "Outdated
    // Emulator", "Unsupported Game Version"): Hardcore's (an emulator, a core or a version of it RetroAchievements hasn't
    // approved for it), or a version of the game without achievements (LoadDone says so). Nothing to show in Casual
    // (00.78): logged only.
    if (a->id >= 101000000)
    {
      LogF("ra: the server's warning %u \"%s\" (nothing to show in Casual)", a->id, Copy(a->title).c_str());
      break;
    }
    notice.kind = Notice::Kind::Unlocked;
    notice.title = Translated(Copy(a->title));  // (the app's language when its translation came, 00.76)
    notice.text = Translated(Copy(a->description));
    notice.badge = Copy(a->badge_url);
    notice.points = a->points;
    notice.seconds = 5.0f;
    LogF("ra: unlocked %u \"%s\" (%u points; %.1f%% of players have it)", a->id, notice.title.c_str(), a->points,
         a->rarity);
    if (!notice.badge.empty())
      PictureFile(notice.badge);  // (its download starts now: the popup waits a moment for it)
    RecordUnlock(client, a);  // (the history, 00.75)
    Push(std::move(notice));
    break;
  }
  case RC_CLIENT_EVENT_GAME_COMPLETED:
  case RC_CLIENT_EVENT_SUBSET_COMPLETED:
  {
    Notice notice;
    notice.kind = Notice::Kind::Mastered;
    const rc_client_game_t* game = rc_client_get_game_info(client);
    notice.title = event->type == RC_CLIENT_EVENT_SUBSET_COMPLETED && event->subset ? Copy(event->subset->title) :
                   game                                                             ? Copy(game->title) :
                                                                                      std::string();
    notice.badge = event->type == RC_CLIENT_EVENT_SUBSET_COMPLETED && event->subset ? Copy(event->subset->badge_url) :
                   game                                                             ? Copy(game->badge_url) :
                                                                                      std::string();
    notice.text = "Completed";  // (Casual, 00.78: "Mastered" is Hardcore's)
    notice.seconds = 7.0f;
    LogF("ra: %s \"%s\"", notice.text.c_str(), notice.title.c_str());
    Push(std::move(notice));
    break;
  }
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
  {
    const rc_client_leaderboard_t* board = event->leaderboard;
    if (!board)
      break;
    Notice notice;
    notice.kind = event->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED ? Notice::Kind::LeaderboardStarted :
                  event->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED  ? Notice::Kind::LeaderboardFailed :
                                                                       Notice::Kind::LeaderboardSubmitted;
    notice.title = Translated(Copy(board->title));
    notice.text = event->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED ? Copy(board->tracker_value) :
                                                                         Translated(Copy(board->description));
    notice.seconds = 3.0f;
    if (event->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)
      LogF("ra: leaderboard %u \"%s\": %s submitted", board->id, notice.title.c_str(), notice.text.c_str());
    Push(std::move(notice));
    break;
  }
  case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
  {
    const rc_client_leaderboard_scoreboard_t* board = event->leaderboard_scoreboard;
    if (!board)
      break;
    Notice notice;
    notice.kind = Notice::Kind::LeaderboardSubmitted;
    notice.title = event->leaderboard ? Translated(Copy(event->leaderboard->title)) : std::string();
    notice.text = "#" + std::to_string(board->new_rank) + " / " + std::to_string(board->num_entries);
    notice.seconds = 4.0f;
    LogF("ra: leaderboard %u: rank %u of %u (best %s)", board->leaderboard_id, board->new_rank, board->num_entries,
         board->best_score);
    Push(std::move(notice));
    break;
  }
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
  {
    const rc_client_achievement_t* a = event->achievement;
    if (!a)
      break;
    std::lock_guard lock(s_mutex);
    s_challenges.erase(std::remove_if(s_challenges.begin(), s_challenges.end(),
                                      [a](const auto& item) { return item.first == a->id; }),
                       s_challenges.end());
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW)
      s_challenges.emplace_back(a->id, Copy(a->badge_url));
    break;
  }
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
  {
    const rc_client_achievement_t* a = event->achievement;
    std::lock_guard lock(s_mutex);
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE || !a)
    {
      s_overlay.progress.clear();
      s_overlay.progress_badge.clear();
    }
    else
    {
      s_overlay.progress = a->measured_progress;
      s_overlay.progress_badge = Copy(a->badge_url);
      s_overlay.progress_serial++;
    }
    break;
  }
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
  {
    const rc_client_leaderboard_tracker_t* tracker = event->leaderboard_tracker;
    if (!tracker)
      break;
    std::lock_guard lock(s_mutex);
    auto it = std::find_if(s_trackers.begin(), s_trackers.end(), [tracker](const auto& item) { return item.first == tracker->id; });
    if (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE)
    {
      if (it != s_trackers.end())
        s_trackers.erase(it);
    }
    else if (it != s_trackers.end())
    {
      it->second = tracker->display;
    }
    else
    {
      s_trackers.emplace_back(tracker->id, tracker->display);
    }
    break;
  }
  case RC_CLIENT_EVENT_SERVER_ERROR:
  {
    const rc_client_server_error_t* error = event->server_error;
    if (!error)
      break;
    LogF("ra: the server refused %s (%u): %s", Copy(error->api).c_str(), error->related_id,
         Copy(error->error_message).c_str());
    Notice notice;
    notice.kind = Notice::Kind::Message;
    notice.title = "RetroAchievements";
    notice.text = Copy(error->error_message);
    notice.seconds = 5.0f;
    Push(std::move(notice));
    break;
  }
  case RC_CLIENT_EVENT_DISCONNECTED:
  case RC_CLIENT_EVENT_RECONNECTED:
  {
    const bool lost = event->type == RC_CLIENT_EVENT_DISCONNECTED;
    LogF("ra: %s", lost ? "an unlock couldn't reach the server: tried again until it does (while EmuSuNi stays open)" :
                          "the unlocks that waited reached the server");
    {
      std::lock_guard lock(s_mutex);
      s_net_down = lost;
    }
    Notice notice;
    notice.kind = lost ? Notice::Kind::Disconnected : Notice::Kind::Reconnected;
    notice.seconds = lost ? 5.0f : 3.0f;
    Push(std::move(notice));
    break;
  }
  default:
    break;
  }
}

// ---- Logging in ----
// rc_client's reason in plain words for the screen (English, for PS4Lang::T).
std::string PlainLoginError(int result, const char* message)
{
  switch (result)
  {
  case RC_INVALID_CREDENTIALS:
    return "Wrong user name or password";
  case RC_EXPIRED_TOKEN:
    return "The saved login expired: log in again";
  case RC_ACCESS_DENIED:
    return "RetroAchievements refused the login: check the account on retroachievements.org (its email address "
           "verified?)";
  case RC_NO_RESPONSE:
    return message && *message ? std::string(message) : std::string("The server didn't answer");
  default:
    return message && *message ? std::string(message) : std::string(rc_error_str(result));
  }
}

// LoginDone's userdata: what the login was.
constexpr intptr_t LOGIN_TOKEN = 0;     // with the saved token (the start, a try again)
constexpr intptr_t LOGIN_PASSWORD = 1;  // the user's own (Settings -> RetroAchievements -> Log In)
constexpr intptr_t LOGIN_CHECK = 2;     // with the token while logged in: the connection checked (CheckConnection)

void RC_CCONV LoginDone(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  const intptr_t kind = reinterpret_cast<intptr_t>(userdata);
  const bool with_password = kind == LOGIN_PASSWORD;
  if (result == RC_OK)
  {
    const rc_client_user_t* user = rc_client_get_user_info(client);
    std::lock_guard lock(s_mutex);
    const uint32_t points_before = s_points, casual_before = s_casual_points;
    s_login = Login::In;
    s_login_message.clear();
    s_login_offline = false;
    s_checking = false;
    s_checked_at = Clock::now();
    s_net_down = false;
    s_retry_step = 0;
    // A game whose achievements didn't come for want of a connection: asked for again now (Retry); else no try waits.
    s_retry_at = s_game && s_state == GameState::Offline ? Clock::now() : Clock::time_point{};
    if (user)
    {
      s_user = Copy(user->username);
      s_display = Copy(user->display_name);
      s_token = Copy(user->token);
      s_points = user->score;
      s_casual_points = user->score_softcore;
    }
    if (kind == LOGIN_CHECK)
    {
      if (s_points != points_before || s_casual_points != casual_before)
        LogF("ra: the connection checked: %u points, %u casual", s_points, s_casual_points);
      return;
    }
    SaveIni();
    LogF("ra: logged in as %s (%u points, %u casual)%s", s_display.c_str(), s_points, s_casual_points,
         with_password ? ": the token kept in retroachievements.ini" : " with the saved token");
    // (00.77) The account's progress for the library's marks: its games, when the last ones are 10 minutes old (once
    // the login is done - UpdateHistory takes the lock).
    if (s_marks && !s_game)
      s_worker.Post([] { UpdateHistory(false); });
    return;
  }
  std::lock_guard lock(s_mutex);
  s_checking = false;
  // A login stopped by a log out: nothing to say.
  if (result == RC_ABORTED)
    return;
  // A check that couldn't be made (another login on its way): nothing changes.
  if (kind == LOGIN_CHECK && result != RC_NO_RESPONSE && result != RC_INVALID_CREDENTIALS &&
      result != RC_EXPIRED_TOKEN && result != RC_ACCESS_DENIED)
  {
    LogF("ra: the connection's check didn't run (%d): %s", result, Copy(error_message).c_str());
    return;
  }
  s_login = Login::Failed;
  s_login_message = PlainLoginError(result, error_message);
  s_login_offline = result == RC_NO_RESPONSE;
  // No answer and a token kept: tried again by itself (Retry), 15 s, 30 s, 60 s, then every 2 minutes.
  if (s_login_offline && !s_token.empty())
  {
    s_net_down = true;
    const int seconds = NextRetry();
    s_retry_at = Clock::now() + std::chrono::seconds(seconds);
    LogF("ra: no connection to RetroAchievements%s: tried again in %d s",
         kind == LOGIN_CHECK ? " (the connection's check)" : "", seconds);
  }
  // A token the site doesn't take any more: forgotten (the user name stays for the next login).
  if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
  {
    if (!with_password && !s_token.empty())
    {
      s_token.clear();
      SaveIni();
      LogLine("ra: the saved token was refused: forgotten (Settings -> RetroAchievements: log in again)");
    }
  }
  LogF("ra: the login %sfailed (%d): %s", with_password ? "" : "with the saved token ", result,
       Copy(error_message).c_str());
}

// ---- A game ----
void ResetGameState()
{
  // (With s_mutex held.)
  s_game = false;
  s_state = GameState::None;
  s_note.clear();
  s_title.clear();
  s_overlay = {};
  s_challenges.clear();
  s_trackers.clear();
  s_ua_core.clear();
  s_leaderboard_lines = 0;
}

void SetState(GameState state, const std::string& note)
{
  std::lock_guard lock(s_mutex);
  s_state = state;
  s_note = note;
}

void PushNote(const std::string& note)
{
  Notice notice;
  notice.kind = Notice::Kind::Placard;
  notice.title = "RetroAchievements";
  notice.text = note;
  notice.seconds = 5.0f;
  Push(std::move(notice));
}

void RC_CCONV LoadDone(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  const int generation = static_cast<int>(reinterpret_cast<intptr_t>(userdata));
  bool current;
  std::string title;
  {
    std::lock_guard lock(s_mutex);
    current = s_game && generation == s_generation;
    title = s_title;
  }
  if (!current)
  {
    // (The game ended while its achievements were on their way.)
    if (result == RC_OK)
      rc_client_unload_game(client);
    LogLine("ra: a game's achievements came after its end: left out");
    return;
  }
  const rc_client_game_t* game = rc_client_get_game_info(client);
  const std::string hash = game && game->hash ? game->hash : "";
  if (result == RC_OK)
  {
    rc_client_user_game_summary_t summary = {};
    rc_client_get_user_game_summary(client, &summary);
    // (The server's warnings - an unknown emulator, a version of the game its achievements weren't made for - are
    // achievements too, which the summary leaves out.)
    const bool achievements = summary.num_core_achievements > 0;
    const bool boards = rc_client_has_leaderboards(client) != 0;
    const bool presence = rc_client_has_rich_presence(client) != 0;
    LogF("ra: %s is RetroAchievements' game %u \"%s\" (hash %s): %u of %u achievements unlocked (%u of %u points), "
         "%u unsupported, leaderboards %s, rich presence %s",
         title.c_str(), game ? game->id : 0, game ? Copy(game->title).c_str() : "", hash.c_str(),
         summary.num_unlocked_achievements, summary.num_core_achievements, summary.points_unlocked, summary.points_core,
         summary.num_unsupported_achievements, boards ? "yes" : "none", presence ? "yes" : "none");
    // (00.78: leaderboards alone count as none - rc_client runs them only in Hardcore.)
    if (!achievements)
    {
      // RetroAchievements knows this file as another version of a game its achievements weren't made for: its game id
      // then says how (RAWeb's VirtualGameIdService: 1 000 000 000 + the game's id for a version known not to work,
      // 1 100 000 000 + for one not tested, 1 200 000 000 + for one that needs a patch), with only a warning.
      const uint32_t id = game ? game->id : 0;
      const char* note = id > 1200000000u ? "RetroAchievements: this version of the game needs a patch for its achievements" :
                         id > 1100000000u ? "RetroAchievements hasn't tested this version of the game (its site lists the ones that work)" :
                         id > 1000000000u ? "RetroAchievements: this version of the game doesn't work with its achievements" :
                                            "RetroAchievements has no achievements for this game yet";
      LogF("ra: %s", note);
      SetState(id > 1000000000u ? GameState::Unknown : GameState::NoAchievements, note);
      PushNote(note);
      rc_client_unload_game(client);
      return;
    }
    SetState(GameState::Active, "");
    bool retried;
    {
      std::lock_guard lock(s_mutex);
      s_retry_step = 0;
      retried = s_load_retried;
    }
    RecordGame(client);      // (the history, 00.75)
    SaveGameCache(client);   // (My Achievements' copy, 00.76)
    QueueGameTexts(client);  // (the texts in the app's language, 00.76)
    Notice notice;
    notice.kind = Notice::Kind::Placard;
    notice.title = game ? Copy(game->title) : title;
    notice.badge = game ? Copy(game->badge_url) : std::string();
    notice.points = summary.points_unlocked;
    // (The UI builds the line from the summary: "12 of 40 achievements".)
    notice.seconds = 6.0f;
    if (!notice.badge.empty())
      PictureFile(notice.badge);
    Push(std::move(notice));
    // (Asked for again while the game played, 00.75.)
    if (retried)
    {
      Notice back;
      back.kind = Notice::Kind::Message;
      back.title = "Connected to RetroAchievements";
      back.text = "This game's achievements are on";
      back.seconds = 5.0f;
      Push(std::move(back));
    }
    return;
  }
  if (result == RC_NO_GAME_LOADED)
  {
    LogF("ra: RetroAchievements doesn't know %s (hash %s): no achievements, the game plays as always", title.c_str(),
         hash.c_str());
    // (RetroAchievements' Nintendo 3DS achievements are still to come, October 2026.)
    bool threeds;
    {
      std::lock_guard lock(s_mutex);
      threeds = s_system == SystemId::THREEDS;
    }
    const char* note = threeds ? "RetroAchievements has no Nintendo 3DS achievements yet" :
                                 "RetroAchievements doesn't know this file of the game (another version or a hack?)";
    SetState(GameState::Unknown, note);
    PushNote(note);
    return;
  }
  LogF("ra: the achievements of %s didn't load (%d): %s - the game plays without them", title.c_str(), result,
       Copy(error_message).c_str());
  bool offline;
  {
    std::lock_guard lock(s_mutex);
    offline = s_login == Login::Failed && s_login_offline;
  }
  if (result == RC_NO_RESPONSE || (result == RC_LOGIN_REQUIRED && offline))
  {
    // Tried again while the game plays (Retry, 00.75): 15 s, 30 s, 60 s, then every 2 minutes.
    bool first;
    {
      std::lock_guard lock(s_mutex);
      first = !s_load_retried;
      s_net_down = true;
      if (s_retry_at == Clock::time_point{})
        s_retry_at = Clock::now() + std::chrono::seconds(NextRetry());
    }
    SetState(GameState::Offline, "No connection to RetroAchievements: it tries again while you play");
    if (first)
      PushNote("No connection to RetroAchievements: it tries again while you play");
  }
  else if (result == RC_LOGIN_REQUIRED || result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN ||
           result == RC_ACCESS_DENIED)
  {
    SetState(GameState::Failed, "RetroAchievements: log in again in Settings");
    PushNote("RetroAchievements: log in again in Settings");
  }
  else if (result == RC_INVALID_STATE && error_message && std::strstr(error_message, "hash"))
  {
    SetState(GameState::Unsupported, "This game's file couldn't be identified");
    PushNote("This game's file couldn't be identified");
  }
  else
  {
    const std::string note = "RetroAchievements: " + Copy(error_message);
    SetState(GameState::Failed, note);
    PushNote(note);
  }
}

void RC_CCONV DiscDone(int result, const char* error_message, rc_client_t*, void*)
{
  if (result == RC_OK)
  {
    LogLine("ra: the disc put in is one of the game's");
    return;
  }
  LogF("ra: the disc put in couldn't be identified (%d): %s", result, Copy(error_message).c_str());
}

void RC_CCONV CoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info)
{
  info->data = s_get_data ? static_cast<uint8_t*>(s_get_data(id)) : nullptr;
  info->size = s_get_size ? s_get_size(id) : 0;
  // Genesis Plus GX's save RAM (a cartridge's battery RAM, 64 KiB: sram.h's sram[0x10000], null when the cartridge has
  // none): while the game runs the core gives only the part the game has written so far (its saves are kept that
  // short; the whole 64 KiB before the first frame - libretro.c's is_running), and an achievement reading a part not
  // yet written would be "unsupported" for the whole game (seen on the PC: 2 bytes of a Master System game's).
  if (id == RETRO_MEMORY_SAVE_RAM && info->data && s_gpgx)
    info->size = 0x10000;
  if (!info->data)
    info->size = 0;
}

// The console's memory as RetroAchievements numbers it, from the core's map (or its memory, without one): false
// while the core shows none (a core whose memory comes after its first frames).
bool MakeRegions()
{
  rc_libretro_memory_regions_t regions{};
  retro_memory_map map = {};
  map.descriptors = s_descriptors.data();
  map.num_descriptors = static_cast<unsigned>(s_descriptors.size());
  std::vector<std::string> lines;
  s_memory_lines = &lines;
  rc_libretro_init_verbose_message_callback(MemoryLog);
  const bool ok = rc_libretro_memory_init(&regions, s_has_map ? &map : nullptr, CoreMemoryInfo, s_console) != 0;
  rc_libretro_init_verbose_message_callback(nullptr);
  s_memory_lines = nullptr;
  if (!s_regions_ready && (ok || !s_memory_failure_logged))
  {
    for (const std::string& line : lines)
      LogF("ra: memory: %s", line.c_str());
    if (!ok)
    {
      LogLine("ra: memory: the core shows none of the console's memory yet: asked again each frame");
      s_memory_failure_logged = true;
    }
  }
  if (!ok)
    return false;
  // (Again later: a core that moves or grows its memory - Genesis Plus GX's save RAM - is followed.)
  if (s_regions_ready)
  {
    bool same = regions.count == s_regions.count && regions.total_size == s_regions.total_size;
    for (uint32_t i = 0; same && i < regions.count; i++)
      same = regions.data[i] == s_regions.data[i] && regions.size[i] == s_regions.size[i];
    if (same)
      return true;
    LogF("ra: memory: the core's memory changed (%zu bytes in %u pieces, was %zu in %u): followed", regions.total_size,
         regions.count, s_regions.total_size, s_regions.count);
  }
  else
  {
    size_t mapped = 0;
    for (uint32_t i = 0; i < regions.count; i++)
      mapped += regions.data[i] ? regions.size[i] : 0;
    LogF("ra: memory: %zu bytes of the console's memory, %zu of them shown by the core (%s, %u pieces)",
         regions.total_size, mapped, s_has_map ? "its memory map" : "its memory", regions.count);
  }
  s_regions = regions;
  s_regions_total = regions.total_size;
  s_regions_ready = true;
  return true;
}

// The game's hash as rc_client makes it (rc_hash for the console, the app's disc reader): kept (00.75), so its
// achievements can be asked for again without the file - a cartridge's bytes are the core's by then.
bool HashGame(uint32_t console, const std::string& path, const uint8_t* data, size_t size, char hash[33])
{
  rc_hash_iterator_t iterator;
  rc_hash_initialize_iterator(&iterator, path.empty() ? "?" : path.c_str(), data, size);
  iterator.callbacks.verbose_message = HashLog;
  iterator.callbacks.error_message = HashLog;
  AchievementsDisc::GetReader(&iterator.callbacks.cdreader);
  const int ok = rc_hash_generate(hash, console, &iterator);
  rc_hash_destroy_iterator(&iterator);
  return ok != 0;
}

// The hash made: the game's achievements asked for (or why there are none).
void LoadByHash(bool ok, const char* hash, int generation)
{
  {
    std::lock_guard lock(s_mutex);
    if (generation != s_generation)
      return;  // (the game ended meanwhile)
    if (ok)
      s_game_hash = hash;
  }
  if (!ok)
  {
    LogLine("ra: the game's file couldn't be identified: no achievements, the game plays as always");
    SetState(GameState::Unsupported, "This game's file couldn't be identified");
    PushNote("This game's file couldn't be identified");
    return;
  }
  rc_client_begin_load_game(s_client, hash, LoadDone, reinterpret_cast<void*>(static_cast<intptr_t>(generation)));
}

// Without an answer before (00.75): the next try once its time has come - a login with the saved token, a game's
// achievements, or the connection's check in the main menu. Idle (the
// main menu), DoFrame and Paused (a game) call it.
void Retry()
{
  enum class What
  {
    Nothing,
    Login,
    Load,
    Check
  } what = What::Nothing;
  std::string user, token, hash;
  int generation = 0;
  {
    std::lock_guard lock(s_mutex);
    if (!s_client || s_retry_at == Clock::time_point{} || Clock::now() < s_retry_at)
      return;
    s_retry_at = {};
    if (s_login == Login::Failed && s_login_offline && !s_token.empty())
    {
      what = What::Login;
      s_login = Login::Working;
    }
    else if (s_login == Login::In && s_game && s_state == GameState::Offline && !s_game_hash.empty())
    {
      what = What::Load;
      hash = s_game_hash;
      generation = s_generation;
      s_load_retried = true;
      s_state = GameState::Waiting;
      s_note.clear();
    }
    else if (s_login == Login::In && s_net_down && !s_game && !s_checking && !s_syncing)
    {
      what = What::Check;
      s_checking = true;
      s_checked_at = Clock::now();
    }
    user = s_user;
    token = s_token;
  }
  switch (what)
  {
  case What::Login:
    LogLine("ra: no connection before: logging in again with the saved token");
    rc_client_begin_login_with_token(s_client, user.c_str(), token.c_str(), LoginDone,
                                     reinterpret_cast<void*>(LOGIN_TOKEN));
    break;
  case What::Load:
    LogLine("ra: no connection before: the game's achievements asked for again");
    rc_client_begin_load_game(s_client, hash.c_str(), LoadDone, reinterpret_cast<void*>(static_cast<intptr_t>(generation)));
    break;
  case What::Check:
    LogLine("ra: no connection before: checking it again");
    rc_client_begin_login_with_token(s_client, user.c_str(), token.c_str(), LoginDone,
                                     reinterpret_cast<void*>(LOGIN_CHECK));
    break;
  case What::Nothing:
    break;
  }
}
}  // namespace

// ---- The app ----
void Init(const std::string& data_dir, const std::string& assets_dir)
{
  {
    std::lock_guard lock(s_mutex);
    s_ini = data_dir + "/retroachievements.ini";
    s_cache = data_dir + "/cache/ra";
    s_ca_file = assets_dir + "/xmb/cacert.pem";
    s_history_file = data_dir + "/retroachievements-history.txt";
    s_account_file = s_cache + "/account-games.txt";
    LoadIni();
    LoadHistory();
  }
  MakeDirs(s_cache);
  s_client = rc_client_create(ReadMemory, ServerCall);
  if (!s_client)
  {
    LogLine("ra: rc_client couldn't be made: no RetroAchievements");
    return;
  }
  rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_VERBOSE, ClientLog);
#ifndef __PS4__
  // (The PC tests: a stand-in for the server, scratchpad ra-0073/pc.)
  if (const char* host = std::getenv("EMUSUNI_RA_HOST"))
    rc_client_set_host(s_client, host);
#endif
  rc_client_set_event_handler(s_client, Event);
  rc_client_set_get_time_millisecs_function(s_client, Milliseconds);
  // Memory is read inside rc_client_do_frame and rc_client_idle only, on the emulation thread (the core's memory
  // is the emulation's while it runs).
  rc_client_set_allow_background_memory_reads(s_client, 0);
  rc_client_set_unofficial_enabled(s_client, 0);
  rc_hash_callbacks_t callbacks = {};
  callbacks.verbose_message = HashLog;
  callbacks.error_message = HashLog;
  AchievementsDisc::GetReader(&callbacks.cdreader);
  rc_client_set_hash_callbacks(s_client, &callbacks);
  char clause[64] = "";
  rc_client_get_user_agent_clause(s_client, clause, sizeof(clause));
  std::string user, token;
  {
    std::lock_guard lock(s_mutex);
    s_ua_base = "EmuSuNi/" + Version() + " " + SystemPart();
    s_ua_clause = clause;
    user = s_user;
    token = s_token;
    if (!token.empty())
      s_login = Login::Working;
  }
  // (00.78) Casual only (rc_client starts in Hardcore): RetroAchievements hasn't approved EmuSuNi - its Hardcore unlocks
  // would count as Casual. Hardcore's rules (no states loaded, no cheats, no slow motion) never apply.
  rc_client_set_hardcore_enabled(s_client, 0);
  LogF("ra: rcheevos %s, User-Agent \"%s\", Casual; %s", clause, UserAgent().c_str(),
       token.empty()      ? (user.empty() ? "no account (Settings -> RetroAchievements)" : "logged out") :
                            "logging in with the saved token");
  if (!token.empty())
    rc_client_begin_login_with_token(s_client, user.c_str(), token.c_str(), LoginDone,
                                     reinterpret_cast<void*>(LOGIN_TOKEN));
}

void Idle()
{
  if (!s_client)
    return;
  Retry();
  TranslateResume();  // (translations that waited: a minute without an answer, MyMemory's day)
  rc_client_idle(s_client);
}

// ---- The account ----
Account GetAccount()
{
  std::lock_guard lock(s_mutex);
  Account account;
  account.state = s_login;
  account.user = s_display.empty() ? s_user : s_display;
  account.points = s_points;
  account.casual_points = s_casual_points;
  account.message = s_login_message;
  account.saved_token = !s_token.empty();
  return account;
}

void LogIn(const std::string& user, const std::string& password)
{
  if (!s_client || user.empty() || password.empty())
    return;
  {
    std::lock_guard lock(s_mutex);
    s_login = Login::Working;
    s_login_message.clear();
    s_user = user;
    s_display.clear();
  }
  LogF("ra: logging in as %s (the password is sent once, kept nowhere)", user.c_str());
  rc_client_begin_login_with_password(s_client, user.c_str(), password.c_str(), LoginDone,
                                      reinterpret_cast<void*>(LOGIN_PASSWORD));
}

void LogOut()
{
  if (!s_client)
    return;
  rc_client_logout(s_client);
  std::lock_guard lock(s_mutex);
  s_login = Login::Out;
  s_login_message.clear();
  s_token.clear();
  s_display.clear();
  s_points = s_casual_points = 0;
  s_retry_at = {};
  s_retry_step = 0;
  s_net_down = false;
  SaveIni();
  LogLine("ra: logged out: the token forgotten");
}

// ---- The session (00.75) ----
Status GetStatus()
{
  // (The account's points as rc_client has them now: an unlock's answer brings the new ones.)
  bool live;
  {
    std::lock_guard lock(s_mutex);
    live = s_login == Login::In && !s_checking;
  }
  uint32_t points = 0, casual = 0;
  if (live && s_client)
  {
    if (const rc_client_user_t* user = rc_client_get_user_info(s_client))
    {
      points = user->score;
      casual = user->score_softcore;
    }
    else
    {
      live = false;
    }
  }
  std::lock_guard lock(s_mutex);
  Status status;
  status.user = s_display.empty() ? s_user : s_display;
  status.points = live ? points : s_points;
  status.casual_points = live ? casual : s_casual_points;
  switch (s_login)
  {
  case Login::Out:
    status.session = Session::NotLoggedIn;
    break;
  case Login::Working:
    status.session = Session::Connecting;
    break;
  case Login::In:
    status.session = s_net_down ? Session::Offline : Session::Connected;
    break;
  case Login::Failed:
    if (s_login_offline && !s_token.empty())
    {
      status.session = Session::Offline;
    }
    else
    {
      status.session = Session::Failed;
      status.why = s_login_message;
    }
    break;
  }
  if (status.session == Session::Offline && s_retry_at != Clock::time_point{})
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(s_retry_at - Clock::now()).count();
    status.retry_seconds = left <= 0 ? 0 : static_cast<int>((left + 999) / 1000);
  }
  return status;
}

std::string StatusLine(const Status& status)
{
  switch (status.session)
  {
  case Session::NotLoggedIn:
    return "Not logged in";
  case Session::Connecting:
    return "Connecting to RetroAchievements...";
  case Session::Connected:
    // (00.78: the account's points all together - Hardcore's, made elsewhere, and Casual's.)
    return "Connected as " + status.user + " (" + std::to_string(status.points + status.casual_points) + " points)";
  case Session::Offline:
    if (status.retry_seconds > 0)
      return "No connection to RetroAchievements: trying again in " + std::to_string(status.retry_seconds) + " s";
    if (status.retry_seconds == 0)
      return "No connection to RetroAchievements: trying again...";
    return "No connection to RetroAchievements";
  case Session::Failed:
    return status.why.empty() ? std::string("The login failed") : "The login failed: " + status.why;
  }
  return {};
}

void CheckConnection()
{
  std::string user, token;
  {
    std::lock_guard lock(s_mutex);
    if (!s_client || s_game || s_login != Login::In || s_checking || s_syncing || s_token.empty())
      return;
    const auto now = Clock::now();
    if (s_checked_at != Clock::time_point{} && now - s_checked_at < std::chrono::seconds(60))
      return;
    s_checking = true;
    s_checked_at = now;
    user = s_user;
    token = s_token;
  }
  rc_client_begin_login_with_token(s_client, user.c_str(), token.c_str(), LoginDone,
                                   reinterpret_cast<void*>(LOGIN_CHECK));
}

// ---- The history (00.75) ----
namespace
{
// The app's consoles as RetroAchievements numbers them (the Famicom Disk System its own; no 3DS on the site yet).
constexpr uint32_t SYNC_CONSOLES[] = {
    RC_CONSOLE_SUPER_NINTENDO, RC_CONSOLE_NINTENDO,      RC_CONSOLE_FAMICOM_DISK_SYSTEM, RC_CONSOLE_NINTENDO_64,
    RC_CONSOLE_GAMEBOY,        RC_CONSOLE_GAMEBOY_COLOR, RC_CONSOLE_GAMEBOY_ADVANCE,     RC_CONSOLE_PLAYSTATION,
    RC_CONSOLE_NINTENDO_DS,    RC_CONSOLE_PSP,           RC_CONSOLE_DREAMCAST,           RC_CONSOLE_SG1000,
    RC_CONSOLE_MASTER_SYSTEM,  RC_CONSOLE_GAME_GEAR,     RC_CONSOLE_MEGA_DRIVE,          RC_CONSOLE_ARCADE,
    RC_CONSOLE_SATURN,         RC_CONSOLE_PC_ENGINE,  // (00.82)
};
constexpr size_t SYNC_TITLES = 90;  // games' names asked for at once (the server takes 99 at most)

void SyncStep();

void SyncEnd(const std::string& error)
{
  std::lock_guard lock(s_mutex);
  s_syncing = false;
  if (error.empty())
  {
    s_account_games = std::move(s_sync_found);
    s_lib_version++;  // (the marks' progress, 00.77)
    s_account_user = s_user;
    s_synced = static_cast<int64_t>(std::time(nullptr));
    s_sync_error.clear();
    SaveAccountGames();
    LogF("ra: history: %zu of the account's games have achievements unlocked (retroachievements.org)",
         s_account_games.size());
  }
  else
  {
    s_sync_error = error;
    LogF("ra: history: the account's games didn't come: %s", error.c_str());
  }
  s_sync_found.clear();
}

// rc_client's reason in plain words (English, for PS4Lang::T).
std::string SyncError(int result, const char* message)
{
  if (result == RC_NO_RESPONSE)
    return "No connection to RetroAchievements";
  if (result == RC_INVALID_STATE)
    return "Not logged in";
  return message && *message ? std::string(message) : std::string(rc_error_str(result));
}

void RC_CCONV SyncProgressDone(int result, const char* error_message, rc_client_all_user_progress_t* list,
                               rc_client_t*, void* userdata)
{
  const uint32_t console = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userdata));
  if (result != RC_OK || !list)
  {
    if (list)
      rc_client_destroy_all_user_progress(list);
    list = nullptr;
    // No answer, or no longer logged in: the update stops (the last one's games stay). Another error: this console is
    // left out.
    if (result == RC_NO_RESPONSE || result == RC_INVALID_STATE || result == RC_INVALID_CREDENTIALS ||
        result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED || result == RC_OK)
    {
      SyncEnd(SyncError(result == RC_OK ? RC_NO_RESPONSE : result, error_message));
      return;
    }
    LogF("ra: history: console %u left out (%d): %s", console, result, Copy(error_message).c_str());
  }
  else
  {
    std::lock_guard lock(s_mutex);
    for (uint32_t i = 0; i < list->num_entries; i++)
    {
      const rc_client_all_user_progress_entry_t& entry = list->entries[i];
      // (The server lists every game of the console with achievements: those the player has some of.)
      if (entry.num_unlocked_achievements == 0 && entry.num_unlocked_achievements_hardcore == 0)
        continue;
      HistoryGame game;
      game.game_id = entry.game_id;
      game.console = console;
      game.total = entry.num_achievements;
      game.unlocked = std::max(entry.num_unlocked_achievements, entry.num_unlocked_achievements_hardcore);
      s_sync_found.push_back(std::move(game));
    }
  }
  if (list)
    rc_client_destroy_all_user_progress(list);
  {
    std::lock_guard lock(s_mutex);
    s_sync_console++;
  }
  SyncStep();
}

void RC_CCONV SyncTitlesDone(int result, const char* error_message, rc_client_game_title_list_t* list, rc_client_t*,
                             void*)
{
  if (result != RC_OK || !list)
  {
    if (list)
      rc_client_destroy_game_title_list(list);
    SyncEnd(SyncError(result == RC_OK ? RC_NO_RESPONSE : result, error_message));
    return;
  }
  {
    std::lock_guard lock(s_mutex);
    for (uint32_t i = 0; i < list->num_entries; i++)
    {
      const rc_client_game_title_entry_t& entry = list->entries[i];
      for (HistoryGame& game : s_sync_found)
      {
        if (game.game_id == entry.game_id)
        {
          game.title = Copy(entry.title);
          game.badge = Copy(entry.badge_url);
          break;
        }
      }
    }
    s_sync_title += SYNC_TITLES;
  }
  rc_client_destroy_game_title_list(list);
  SyncStep();
}

// The update's next request: each console's progress, then the games' names, a batch at a time (the module's thread,
// one after another).
void SyncStep()
{
  uint32_t console = 0;
  std::vector<uint32_t> ids;
  {
    std::lock_guard lock(s_mutex);
    if (s_sync_console < std::size(SYNC_CONSOLES))
    {
      console = SYNC_CONSOLES[s_sync_console];
    }
    else
    {
      for (size_t i = s_sync_title; i < s_sync_found.size() && i < s_sync_title + SYNC_TITLES; i++)
        ids.push_back(s_sync_found[i].game_id);
    }
  }
  if (console)
    rc_client_begin_fetch_all_user_progress(s_client, console, SyncProgressDone,
                                            reinterpret_cast<void*>(static_cast<uintptr_t>(console)));
  else if (!ids.empty())
    rc_client_begin_fetch_game_titles(s_client, ids.data(), static_cast<uint32_t>(ids.size()), SyncTitlesDone, nullptr);
  else
    SyncEnd("");
}
}  // namespace

History GetHistory()
{
  std::lock_guard lock(s_mutex);
  History history;
  const std::string user = s_user;
  for (auto it = s_unlocks.rbegin(); it != s_unlocks.rend() && history.unlocks.size() < 300; ++it)
  {
    if (user.empty() || it->first == user)
      history.unlocks.push_back(it->second);
  }
  // The account's games (the last update), then those played in EmuSuNi over them: their own numbers when they were
  // played after that update.
  const bool account = !user.empty() && s_account_user == user;
  std::map<uint32_t, HistoryGame> games;
  if (account)
  {
    for (const HistoryGame& game : s_account_games)
      games[game.game_id] = game;
  }
  for (const auto& [owner, played] : s_played)
  {
    if (!user.empty() && owner != user)
      continue;
    auto it = games.find(played.game_id);
    if (it == games.end())
    {
      games[played.game_id] = played;
      continue;
    }
    HistoryGame& game = it->second;
    if (played.played > s_synced)
    {
      // (rc_client counts every unlock in Casual; 00.73-00.77's Hardcore sessions only theirs: the larger count.)
      game.total = played.total;
      game.unlocked = std::max(game.unlocked, played.unlocked);
    }
    game.played = played.played;
    if (game.title.empty())
      game.title = played.title;
    if (game.badge.empty())
      game.badge = played.badge;
  }
  for (auto& [id, game] : games)
    history.games.push_back(std::move(game));
  std::stable_sort(history.games.begin(), history.games.end(), [](const HistoryGame& a, const HistoryGame& b) {
    if (a.played != b.played)
      return a.played > b.played;
    return a.title < b.title;
  });
  history.syncing = s_syncing;
  history.synced = account ? s_synced : 0;
  history.sync_error = s_sync_error;
  return history;
}

void UpdateHistory(bool now)
{
  // (Update Now, 00.77: RetroAchievements' lists of the library's consoles again too.)
  if (now)
  {
    {
      std::lock_guard lock(s_mutex);
      s_lib_force = true;
      if (!s_lib_files.empty())
        s_lib_work = true;
    }
    s_lib_wake.notify_all();
  }
  {
    std::lock_guard lock(s_mutex);
    if (!s_client || s_login != Login::In || s_syncing || s_checking || s_game)
      return;
    const int64_t time = static_cast<int64_t>(std::time(nullptr));
    if (!now && s_account_user == s_user && s_synced > 0 && time - s_synced < 600 && time >= s_synced)
      return;
    s_syncing = true;
    s_sync_error.clear();
    s_sync_found.clear();
    s_sync_console = 0;
    s_sync_title = 0;
  }
  LogLine("ra: history: asking retroachievements.org for the account's games");
  SyncStep();
}

std::string ConsoleName(uint32_t console)
{
  return Copy(rc_console_name(console));
}

// ---- A game's achievements outside a game (00.76) ----
namespace
{
// A game page's texts to translate (00.79), in the page's order: the locked achievements, then the unlocked ones.
std::vector<TextPair> PageTexts(const GameDetail& detail)
{
  std::vector<TextPair> items;
  for (const bool unlocked : {false, true})
  {
    for (const Entry& entry : detail.entries)
    {
      if (entry.unlocked == unlocked)
        items.push_back({entry.id, entry.title, entry.description});
    }
  }
  return items;
}

// The PS4's copy of a game's achievements (SaveGameCache, FetchGame): what it had, else nothing.
GameDetail ReadGameFile(uint32_t game_id)
{
  GameDetail detail;
  detail.game_id = game_id;
  for (const std::string& line : Lines(ReadTextFile(GameFile(game_id))))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "D" && f.size() >= 6)
    {
      detail.console = Number(f[2]);
      detail.updated = Time(f[3]);
      detail.title = f[4];
      detail.badge = f[5];
    }
    else if (f[0] == "A" && f.size() >= 12)
    {
      Entry entry;
      entry.id = Number(f[1]);
      entry.points = Number(f[2]);
      entry.unlocked = f[3] == "1";
      entry.unlock_time = Time(f[5]);
      entry.rarity = static_cast<float>(std::atoi(f[6].c_str())) / 100.0f;
      entry.progress = f[7];
      entry.title = f[8];
      entry.description = f[9];
      entry.badge = entry.unlocked ? f[10] : f[11];
      detail.entries.push_back(std::move(entry));
    }
  }
  return detail;
}

// (Its badges, kept with the entries: the unlocked and the grey one, for the file.)
struct Achieved
{
  Entry entry;
  std::string badge, badge_locked;
};

void WriteGameFile(const GameDetail& detail, const std::vector<Achieved>& all)
{
  std::string text = "# EmuSuNi: a RetroAchievements game's achievements as last seen (My Achievements).\n";
  text += "D\t" + std::to_string(detail.game_id) + "\t" + std::to_string(detail.console) + "\t" +
          std::to_string(static_cast<long long>(detail.updated)) + "\t" + Clean(detail.title) + "\t" +
          Clean(detail.badge) + "\n";
  for (const Achieved& a : all)
  {
    char number[160];
    std::snprintf(number, sizeof(number), "A\t%u\t%u\t%d\t0\t%lld\t%d\t", a.entry.id, a.entry.points,
                  a.entry.unlocked ? 1 : 0, static_cast<long long>(a.entry.unlock_time),
                  static_cast<int>(a.entry.rarity * 100.0f));  // (the 0: the mode column, Casual)
    text += number + Clean(a.entry.progress) + "\t" + Clean(a.entry.title) + "\t" + Clean(a.entry.description) + "\t" +
            Clean(a.badge) + "\t" + Clean(a.badge_locked) + "\n";
  }
  Save(GameFile(detail.game_id), std::move(text));
}

// A call to RetroAchievements made here (rcheevos' request), on the module's thread: false without an answer.
bool Call(const rc_api_request_t& request, std::string* body, long* status)
{
  Net::Request http;
  http.url = request.url ? request.url : "";
  if (request.post_data)
  {
    http.method = "POST";
    http.body = request.post_data;
    http.content_type = request.content_type ? request.content_type : "application/x-www-form-urlencoded";
  }
  http.user_agent = UserAgent();
  http.ca_file = s_ca_file;
  http.timeout_seconds = 30;
  http.error_body = true;
  http.max_bytes = 16 << 20;
  const std::string api = ApiName(request.post_data ? request.post_data : request.url);
  const auto start = Clock::now();
  const Net::Response answer = Net::Fetch(http, &s_api_session);
  std::fill(http.body.begin(), http.body.end(), '\0');
  const long code = answer.status;
  const bool reachable = code > 0 && code != 502 && code != 503 && code != 504 && !(code >= 520 && code <= 527);
  NoteAnswer(reachable);
  LogF("ra: %s: %s (%zu bytes, %.0f ms)", api.c_str(),
       code > 0 ? ("HTTP " + std::to_string(code)).c_str() : ("no answer: " + answer.error).c_str(), answer.body.size(),
       std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  *body = answer.body;
  *status = code;
  return reachable;
}

// (With s_mutex held.) The opened game's state, if it is still the one opened.
void DetailDone(int serial, GameDetail::State state, const std::string& error)
{
  if (serial != s_detail_serial)
    return;
  s_detail.state = state;
  s_detail.error = error;
}

// The game's sets and the account's unlocks from retroachievements.org, merged with the PS4's copy (the progress of
// the last session, the unlocks' dates), kept again; then its texts to translate.
void FetchGame(uint32_t game_id, int serial)
{
  std::string user, token;
  {
    std::lock_guard lock(s_mutex);
    if (serial != s_detail_serial)
      return;
    user = s_user;
    token = s_token;
  }
  rc_api_host_t host = {};
#ifndef __PS4__
  const char* test_host = std::getenv("EMUSUNI_RA_HOST");  // (the PC tests' stand-in)
  host.host = test_host;
#endif
  // The game's achievement sets (core and its subsets).
  rc_api_fetch_game_sets_request_t sets_request = {};
  sets_request.username = user.c_str();
  sets_request.api_token = token.c_str();
  sets_request.game_id = game_id;
  rc_api_request_t request;
  std::string body;
  long status = 0;
  if (rc_api_init_fetch_game_sets_request_hosted(&request, &sets_request, &host) != RC_OK)
    return;
  const bool answered = Call(request, &body, &status);
  rc_api_destroy_request(&request);
  if (!answered)
  {
    std::lock_guard lock(s_mutex);
    DetailDone(serial, GameDetail::State::Offline, "No connection to RetroAchievements");
    return;
  }
  rc_api_server_response_t server = {};
  server.body = body.c_str();
  server.body_length = body.size();
  server.http_status_code = static_cast<int>(status);
  rc_api_fetch_game_sets_response_t sets;
  if (rc_api_process_fetch_game_sets_server_response(&sets, &server) != RC_OK || !sets.response.succeeded)
  {
    const std::string error = sets.response.error_message ? sets.response.error_message : "The server refused";
    rc_api_destroy_fetch_game_sets_response(&sets);
    std::lock_guard lock(s_mutex);
    DetailDone(serial, GameDetail::State::Failed, error);
    return;
  }
  GameDetail fresh;
  fresh.game_id = game_id;
  fresh.console = sets.console_id;
  fresh.title = Copy(sets.title);
  fresh.badge = Copy(sets.image_url);
  std::vector<Achieved> all;
  for (uint32_t s = 0; s < sets.num_sets; s++)
  {
    const rc_api_achievement_set_definition_t& set = sets.sets[s];
    for (uint32_t i = 0; i < set.num_achievements; i++)
    {
      const rc_api_achievement_definition_t& a = set.achievements[i];
      if (a.category != RC_ACHIEVEMENT_CATEGORY_CORE || a.id >= 101000000)
        continue;  // (unofficial ones, the server's warnings)
      Achieved item;
      item.entry.id = a.id;
      item.entry.points = a.points;
      item.entry.title = Copy(a.title);
      item.entry.description = Copy(a.description);
      item.entry.rarity = a.rarity;
      item.badge = Copy(a.badge_url);
      item.badge_locked = Copy(a.badge_locked_url);
      all.push_back(std::move(item));
    }
  }
  rc_api_destroy_fetch_game_sets_response(&sets);
  // Which the account has (h=0: all of them, Casual and Hardcore alike).
  rc_api_fetch_user_unlocks_request_t unlocks_request = {};
  unlocks_request.username = user.c_str();
  unlocks_request.api_token = token.c_str();
  unlocks_request.game_id = game_id;
  if (rc_api_init_fetch_user_unlocks_request_hosted(&request, &unlocks_request, &host) == RC_OK)
  {
    const bool ok = Call(request, &body, &status);
    rc_api_destroy_request(&request);
    if (!ok)
    {
      std::lock_guard lock(s_mutex);
      DetailDone(serial, GameDetail::State::Offline, "No connection to RetroAchievements");
      return;
    }
    server.body = body.c_str();
    server.body_length = body.size();
    server.http_status_code = static_cast<int>(status);
    rc_api_fetch_user_unlocks_response_t unlocks;
    if (rc_api_process_fetch_user_unlocks_server_response(&unlocks, &server) == RC_OK && unlocks.response.succeeded)
    {
      const std::set<uint32_t> ids(unlocks.achievement_ids, unlocks.achievement_ids + unlocks.num_achievement_ids);
      for (Achieved& item : all)
      {
        if (ids.count(item.entry.id))
          item.entry.unlocked = true;
      }
    }
    rc_api_destroy_fetch_user_unlocks_response(&unlocks);
  }
  // The PS4's copy: a locked one's progress, an unlock's date; the history's dates too.
  const GameDetail kept = ReadGameFile(game_id);
  std::map<uint32_t, int64_t> dates;
  {
    std::lock_guard lock(s_mutex);
    for (const auto& [owner, unlock] : s_unlocks)
    {
      if (owner == user && unlock.game_id == game_id)
        dates[unlock.achievement_id] = unlock.time;
    }
  }
  for (Achieved& item : all)
  {
    for (const Entry& old : kept.entries)
    {
      if (old.id != item.entry.id)
        continue;
      if (!item.entry.unlocked)
        item.entry.progress = old.progress;
      if (item.entry.unlocked && old.unlock_time > 0)
        item.entry.unlock_time = old.unlock_time;
    }
    if (item.entry.unlocked && item.entry.unlock_time == 0 && dates.count(item.entry.id))
      item.entry.unlock_time = dates[item.entry.id];
    item.entry.badge = item.entry.unlocked ? item.badge : item.badge_locked;
  }
  fresh.updated = static_cast<int64_t>(std::time(nullptr));
  WriteGameFile(fresh, all);
  for (Achieved& item : all)
    fresh.entries.push_back(std::move(item.entry));
  LogF("ra: My Achievements: game %u \"%s\", %zu achievements", game_id, fresh.title.c_str(), fresh.entries.size());
  const std::vector<TextPair> texts = PageTexts(fresh);
  const std::string name = fresh.title;
  {
    std::lock_guard lock(s_mutex);
    if (serial != s_detail_serial)
      return;
    fresh.state = GameDetail::State::Ready;
    s_detail = std::move(fresh);
  }
  QueueTexts(texts, name);
}
}  // namespace

void OpenGame(uint32_t game_id)
{
  GameDetail detail = ReadGameFile(game_id);
  bool fetch;
  int serial;
  {
    std::lock_guard lock(s_mutex);
    // (Opened again: what the server said this session, newer than the file its writing may not have reached yet.)
    if (s_detail.game_id == game_id && s_detail.state == GameDetail::State::Ready && !s_detail.entries.empty())
      detail = s_detail;
    // (Not seen on this PS4 yet: its name and icon from the history.)
    if (detail.title.empty())
    {
      for (const HistoryGame& game : s_account_games)
      {
        if (game.game_id == game_id)
        {
          detail.title = game.title;
          detail.badge = game.badge;
          detail.console = game.console;
        }
      }
      for (const auto& [owner, game] : s_played)
      {
        if (game.game_id == game_id)
        {
          detail.title = game.title;
          detail.badge = game.badge;
          detail.console = game.console;
        }
      }
    }
    fetch = s_client && s_login == Login::In && !s_token.empty() && !s_game;
    detail.state = fetch ? GameDetail::State::Loading : GameDetail::State::Offline;
    if (!fetch)
      detail.error = s_login == Login::In ? "" : "Not logged in";
    serial = ++s_detail_serial;
    s_detail = detail;
  }
  LogF("ra: My Achievements: game %u (%zu achievements kept on the PS4)%s", game_id, detail.entries.size(),
       fetch ? ": asking retroachievements.org" : "");
  if (fetch)
    s_worker.Post([game_id, serial] { FetchGame(game_id, serial); });
  QueueTexts(PageTexts(detail), detail.title);
}

GameDetail GetGameDetail()
{
  GameDetail detail;
  {
    std::lock_guard lock(s_mutex);
    detail = s_detail;
  }
  // Grouped as rc_client's list does it (Locked, then Unlocked), in the set's order; the counts; the texts in the
  // app's language once translated.
  std::stable_sort(detail.entries.begin(), detail.entries.end(),
                   [](const Entry& a, const Entry& b) { return !a.unlocked && b.unlocked; });
  detail.unlocked = detail.points = detail.total_points = 0;
  for (Entry& entry : detail.entries)
  {
    entry.group = entry.unlocked ? "Unlocked" : "Locked";
    entry.title = Translated(entry.title);
    entry.description = Translated(entry.description);
    detail.total_points += entry.points;
    if (entry.unlocked)
    {
      detail.unlocked++;
      detail.points += entry.points;
    }
  }
  return detail;
}

// ---- The achievements' texts in the app's language (00.76) ----
void SetLanguage(const std::string& code)
{
  {
    std::lock_guard lock(s_mutex);
    if (code == s_language)
      return;
    s_language = code;
    LoadTranslations();
    LogF("ra: the achievements' texts: %s (%zu translations kept)",
         TranslateOn() ? ("translated to " + code).c_str() : "as RetroAchievements has them (English)", s_tr.size());
  }
}

bool TranslateSetting()
{
  std::lock_guard lock(s_mutex);
  return s_translate == 1 || (s_translate == -1 && s_language == "es");
}

void SetTranslateSetting(bool on)
{
  {
    std::lock_guard lock(s_mutex);
    s_translate = on ? 1 : 0;
    SaveIni();
  }
  LogF("ra: translate the achievements: %s", on ? "on" : "off");
}

std::string Translated(const std::string& english)
{
  std::lock_guard lock(s_mutex);
  if (!TranslateOn() || s_tr.empty())
    return english;
  const auto it = s_tr.find(Clean(english));
  return it == s_tr.end() ? english : it->second;
}

int TranslationsVersion()
{
  std::lock_guard lock(s_mutex);
  return s_tr_version;
}

void TranslateFirst(const std::vector<uint32_t>& ids)
{
  std::lock_guard lock(s_mutex);
  if (ids == s_tr_first)
    return;
  s_tr_first = ids;
  MoveFirst(ids);
}

TranslationState GetTranslationState(const std::string& game)
{
  TranslationState state;
  std::lock_guard lock(s_mutex);
  state.on = TranslateOn();
  if (!state.on)
    return state;
  state.waiting = s_tr_pending.size() + s_tr_inflight.size();
  for (const Pending& item : s_tr_pending)
    state.waiting_game += item.game == game ? 1 : 0;
  for (const std::string& name : s_tr_inflight)
    state.waiting_game += name == game ? 1 : 0;
  const auto now = Clock::now();
  if (now < s_tr_paused_until && s_tr_pause != Pause::Polite && s_tr_pause != Pause::None)
  {
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(s_tr_paused_until - now).count();
    state.wait_minutes = static_cast<int>(std::max<long long>(1, minutes));
    state.day_over = s_tr_pause == Pause::DayOver;
  }
  return state;
}

std::string TranslationLine(const TranslationState& state)
{
  if (!state.on || state.waiting_game == 0)
    return {};
  if (state.wait_minutes < 0)
    return "Translating this game's texts: " + std::to_string(state.waiting_game) + " left";
  if (!state.day_over)
    return "Translating this game's texts: " + std::to_string(state.waiting_game) + " left (no answer: again in a "
           "minute)";
  const int hours = (state.wait_minutes + 59) / 60;
  return std::to_string(state.waiting_game) + " of this game's texts stay in English for now: the translators can't "
         "today, the rest in about " + std::to_string(hours) + " h";
}

// ---- A game ----
void GameStarting(const Game& game)
{
  if (!s_client)
    return;
  rc_client_unload_game(s_client);  // (anything left of a game before)
  uint32_t console = ConsoleId(game.system);
  // A Famicom Disk System disk (its own console for RetroAchievements): its header, or its sides' size.
  if (console == RC_CONSOLE_NINTENDO && game.data &&
      ((game.size >= 4 && std::memcmp(game.data, "FDS\x1a", 4) == 0) ||
       (game.size >= 65500 && game.size % 65500 == 0 && std::memcmp(game.data + 1, "*NINTENDO-HVC*", 14) == 0)))
    console = RC_CONSOLE_FAMICOM_DISK_SYSTEM;
  int generation;
  Login login;
  bool token;
  {
    std::lock_guard lock(s_mutex);
    ResetGameState();
    generation = ++s_generation;
    s_game = true;
    s_system = game.system;
    s_console = console;
    s_title = game.title;
    s_library = LibraryKey(game.library_name);
    s_ua_core = CorePart(game.system, game.library_version);
    login = s_login;
    token = !s_token.empty();
    s_game_hash.clear();
    s_load_retried = false;
  }
  s_regions_ready = false;
  s_memory_waits = 0;
  s_memory_failure_logged = false;
  s_frames = 0;
  s_gpgx = IsGpgx(game.system);
  rc_libretro_memory_destroy(&s_regions);
  if (login == Login::Out || (login == Login::Failed && !token))
  {
    LogF("ra: %s: no one logged in to RetroAchievements", game.title.c_str());
    return;
  }
  // A login with the saved token that failed for want of a connection: tried again now (the game's achievements wait
  // for it).
  if (login == Login::Failed && token && !rc_client_get_user_info(s_client))
  {
    std::string user, saved;
    {
      std::lock_guard lock(s_mutex);
      user = s_user;
      saved = s_token;
      s_login = Login::Working;
      s_retry_at = {};  // (tried now)
    }
    LogF("ra: logging in again with the saved token (the last try failed)");
    rc_client_begin_login_with_token(s_client, user.c_str(), saved.c_str(), LoginDone,
                                     reinterpret_cast<void*>(LOGIN_TOKEN));
  }
  if (console == 0)
  {
    LogF("ra: %s: RetroAchievements has no achievements for this console", game.title.c_str());
    SetState(GameState::Unsupported, "RetroAchievements has no achievements for this console");
    return;
  }
  std::string why;
  if (!AchievementsDisc::CanHash(game.system, game.path, &why))
  {
    LogF("ra: %s: %s - the game plays without achievements", game.title.c_str(), why.c_str());
    SetState(GameState::Unsupported, why);
    PushNote(why);
    return;
  }
  SetState(GameState::Waiting, "");
  LogF("ra: identifying %s (console %u, %s%s)", game.title.c_str(), console, game.path.c_str(),
       game.data ? ", from its bytes" : "");
  if (game.data && game.size)
  {
    // A cartridge: hashed now, from the ROM's bytes (the core may free its copy after its load): MD5 at about
    // 300 MB/s, a 64 MiB N64 cartridge in a quarter of a second.
    const auto start = Clock::now();
    char hash[33] = "";
    const bool ok = HashGame(console, game.path, game.data, game.size, hash);
    LogF("ra: %zu bytes hashed in %.0f ms: %s", game.size,
         std::chrono::duration<double, std::milli>(Clock::now() - start).count(), ok ? hash : "no hash");
    LoadByHash(ok, hash, generation);
    return;
  }
  // A disc (or an arcade set's name): hashed on the module's thread, beside the core's own start.
  const std::string path = AchievementsDisc::HashPath(game.path);
  s_worker.Post([console, path, generation] {
    {
      std::lock_guard lock(s_mutex);
      if (generation != s_generation)
        return;  // (the game ended before)
    }
    const auto start = Clock::now();
    char hash[33] = "";
    const bool ok = HashGame(console, path, nullptr, 0, hash);
    LogF("ra: %s hashed in %.0f ms: %s", path.c_str(),
         std::chrono::duration<double, std::milli>(Clock::now() - start).count(), ok ? hash : "no hash");
    LoadByHash(ok, hash, generation);
  });
}

void SetCoreMemory(void* (*get_data)(unsigned), size_t (*get_size)(unsigned))
{
  s_get_data = get_data;
  s_get_size = get_size;
}

void SetMemoryMap(const retro_memory_map* map)
{
  s_descriptors.clear();
  s_addrspaces.clear();
  s_has_map = false;
  s_regions_ready = false;
  if (!map || !map->descriptors || map->num_descriptors == 0)
    return;
  // (A copy: cores pass arrays of their stack, and the address spaces' names with them.)
  s_descriptors.assign(map->descriptors, map->descriptors + map->num_descriptors);
  s_addrspaces.resize(s_descriptors.size());
  for (size_t i = 0; i < s_descriptors.size(); i++)
  {
    if (s_descriptors[i].addrspace)
      s_addrspaces[i] = s_descriptors[i].addrspace;
  }
  for (size_t i = 0; i < s_descriptors.size(); i++)
    s_descriptors[i].addrspace = s_descriptors[i].addrspace ? s_addrspaces[i].c_str() : nullptr;
  s_has_map = true;
  LogF("ra: memory: the core's memory map, %zu pieces", s_descriptors.size());
}

void DoFrame()
{
  if (!s_client)
    return;
  GameState state;
  bool retry;
  {
    std::lock_guard lock(s_mutex);
    state = s_state;
    retry = s_retry_at != Clock::time_point{} && Clock::now() >= s_retry_at;
  }
  // (No connection before, 00.75: the login or the game's achievements tried again.)
  if (retry)
  {
    Retry();
    std::lock_guard lock(s_mutex);
    state = s_state;
  }
  // (00.77) No memory after 10 s: asked again each second; when it comes, the game's achievements go on loading.
  if (state == GameState::NoMemory && !s_regions_ready && ++s_memory_waits % 60 == 0 && MakeRegions())
  {
    LogLine("ra: the emulator shows its memory now: the game's achievements go on");
    SetState(GameState::Waiting, "");
    state = GameState::Waiting;
  }
  if (state != GameState::Waiting && state != GameState::Active)
    return;
  // The console's memory first (the core's first frame has run). No call into rc_client meanwhile: it activates the
  // game's achievements inside rc_client_do_frame or rc_client_idle (background reads are off), checking each of their
  // addresses then, and one it can't read is "unsupported" for the whole game (seen on the PC with idle calls while
  // FinalBurn Neo showed no memory). A core may show its memory only after a few frames: 10 s.
  if (!s_regions_ready && !MakeRegions())
  {
    if (++s_memory_waits < 600)
      return;
    // (00.77) After 10 s: said once, and the session kept - asked again each second (a core whose memory comes
    // late gets its achievements then). Until 00.76 the game was unloaded here, and the session's start that was
    // still on its way failed with "The requested game is no longer active" (Daytona USA on the Saturn).
    LogLine("ra: the emulator shows no memory to read yet: no achievements until it does (asked again each second)");
    SetState(GameState::NoMemory, "The emulator shows no memory to read: no achievements for this game");
    PushNote("The emulator shows no memory to read: no achievements for this game");
    return;
  }
  // Every 5 s: the core's memory again (some move or grow it as the game goes: Genesis Plus GX's save RAM).
  if (++s_frames % 300 == 0)
    MakeRegions();
  if (s_frames % 60 == 0)
    TranslateResume();  // (00.79: the next of Google's requests after its pause, while the game plays)
  rc_client_do_frame(s_client);
}

void Paused()
{
  // (Not before the core showed its memory: the game's activation, its addresses' check, waits for it - DoFrame.)
  if (!s_client)
    return;
  Retry();
  TranslateResume();
  bool game;
  {
    std::lock_guard lock(s_mutex);
    game = s_game && (s_state == GameState::Waiting || s_state == GameState::Active);
  }
  if (!game || s_regions_ready)
    rc_client_idle(s_client);
}

void Reset()
{
  if (!s_client || !rc_client_is_game_loaded(s_client))
    return;
  rc_client_reset(s_client);
  s_regions_ready = false;  // (some cores make their memory again at a reset)
  LogLine("ra: the game was reset: the achievements' progress too");
}

void StateSaved(const std::string& state_path)
{
  const std::string path = state_path + ".ra";
  if (!s_client || !rc_client_is_game_loaded(s_client))
  {
    std::remove(path.c_str());
    return;
  }
  const size_t size = rc_client_progress_size(s_client);
  std::vector<uint8_t> data(size);
  if (size == 0 || rc_client_serialize_progress_sized(s_client, data.data(), size) != RC_OK ||
      !WriteDataFile(path, data.data(), size))
  {
    std::remove(path.c_str());
    return;
  }
  LogF("ra: the achievements' progress kept with the state (%zu bytes)", size);
}

void StateLoaded(const std::string& state_path)
{
  if (!s_client || !rc_client_is_game_loaded(s_client))
    return;
  const std::string text = ReadTextFile(state_path + ".ra");
  const int result = text.empty() ?
                         rc_client_deserialize_progress_sized(s_client, nullptr, 0) :
                         rc_client_deserialize_progress_sized(s_client, reinterpret_cast<const uint8_t*>(text.data()),
                                                              text.size());
  LogF("ra: the achievements' progress %s", text.empty() ? "started afresh (the state has none)" :
                                            result == RC_OK ? "taken from the state" :
                                                              "couldn't be taken from the state: started afresh");
  if (!text.empty() && result != RC_OK)
    rc_client_deserialize_progress_sized(s_client, nullptr, 0);
}

void DiscChanged(const std::string& path)
{
  if (!s_client)
    return;
  int generation;
  {
    std::lock_guard lock(s_mutex);
    if (!s_game || (s_state != GameState::Active && s_state != GameState::Waiting))
      return;
    generation = s_generation;
  }
  const std::string hash_path = AchievementsDisc::HashPath(path);
  LogF("ra: identifying the disc put in: %s", hash_path.c_str());
  s_worker.Post([hash_path, generation] {
    {
      std::lock_guard lock(s_mutex);
      if (generation != s_generation)
        return;
    }
    rc_client_begin_identify_and_change_media(s_client, hash_path.c_str(), nullptr, 0, DiscDone, nullptr);
  });
}

void GameEnded()
{
  {
    std::lock_guard lock(s_mutex);
    if (!s_game)
      return;
    ResetGameState();
    s_generation++;
  }
  if (s_client)
  {
    SaveGameCache(s_client);  // (the progress as the game ends: My Achievements, 00.76)
    rc_client_unload_game(s_client);
  }
  rc_libretro_memory_destroy(&s_regions);
  s_regions_ready = false;
  s_descriptors.clear();
  s_addrspaces.clear();
  s_has_map = false;
  s_get_data = nullptr;
  s_get_size = nullptr;
}

// ---- What the screen shows ----
Summary GetSummary(bool rich_presence)
{
  Summary summary;
  {
    std::lock_guard lock(s_mutex);
    if (!s_game)
      return summary;
    summary.state = s_state;
    summary.note = s_note;
  }
  if (!s_client || (summary.state != GameState::Active && summary.state != GameState::Waiting))
    return summary;
  if (const rc_client_game_t* game = rc_client_get_game_info(s_client); game && rc_client_is_game_loaded(s_client))
  {
    summary.title = Copy(game->title);
    summary.badge = Copy(game->badge_url);
  }
  rc_client_user_game_summary_t user = {};
  rc_client_get_user_game_summary(s_client, &user);
  summary.unlocked = user.num_unlocked_achievements;
  summary.total = user.num_core_achievements;
  summary.unsupported = user.num_unsupported_achievements;
  summary.points = user.points_unlocked;
  summary.total_points = user.points_core;
  summary.mastered = user.num_core_achievements > 0 && user.num_unlocked_achievements == user.num_core_achievements;
  if (rich_presence && s_regions_ready && rc_client_has_rich_presence(s_client))
  {
    char text[256] = "";
    rc_client_get_rich_presence_message(s_client, text, sizeof(text));
    summary.rich_presence = text;
  }
  return summary;
}

std::vector<Entry> List()
{
  std::vector<Entry> entries;
  if (!s_client || !rc_client_is_game_loaded(s_client))
    return entries;
  rc_client_achievement_list_t* list = rc_client_create_achievement_list(
      s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (!list)
    return entries;
  for (uint32_t b = 0; b < list->num_buckets; b++)
  {
    const rc_client_achievement_bucket_t& bucket = list->buckets[b];
    for (uint32_t i = 0; i < bucket.num_achievements; i++)
    {
      const rc_client_achievement_t* a = bucket.achievements[i];
      if (a->id >= 101000000)
        continue;  // (the server's warnings aren't the game's achievements: 00.77)
      Entry entry;
      entry.id = a->id;
      entry.title = Translated(Copy(a->title));  // (the app's language once translated, 00.76)
      entry.description = Translated(Copy(a->description));
      entry.unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
      entry.badge = entry.unlocked ? Copy(a->badge_url) : Copy(a->badge_locked_url);
      entry.points = a->points;
      entry.progress = a->measured_progress;
      entry.percent = a->measured_percent;
      entry.group = Copy(bucket.label);
      entry.unlock_time = static_cast<int64_t>(a->unlock_time);
      entry.rarity = a->rarity;
      entries.push_back(std::move(entry));
    }
  }
  rc_client_destroy_achievement_list(list);
  return entries;
}

std::vector<Notice> TakeNotices()
{
  std::lock_guard lock(s_mutex);
  std::vector<Notice> notices(s_notices.begin(), s_notices.end());
  s_notices.clear();
  return notices;
}

Overlay GetOverlay()
{
  std::lock_guard lock(s_mutex);
  Overlay overlay = s_overlay;
  for (const auto& [id, badge] : s_challenges)
    overlay.challenges.push_back(badge);
  for (const auto& [id, value] : s_trackers)
    overlay.trackers.push_back(value);
  return overlay;
}

std::string PictureFile(const std::string& address)
{
  if (address.empty())
    return {};
  // "https://media.retroachievements.org/Badge/12345_lock.png" -> cache/ra/Badge-12345_lock.png
  const size_t slash = address.rfind('/');
  if (slash == std::string::npos || slash + 1 >= address.size())
    return {};
  const size_t folder_slash = address.rfind('/', slash - 1);
  std::string name = (folder_slash == std::string::npos ? std::string() :
                                                          address.substr(folder_slash + 1, slash - folder_slash - 1) + "-") +
                     address.substr(slash + 1);
  for (char& c : name)
  {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
      c = '_';
  }
  std::string path;
  {
    std::lock_guard lock(s_mutex);
    path = s_cache + "/" + name;
  }
  if (FileExists(path))
    return path;
  {
    std::lock_guard lock(s_mutex);
    const auto now = Clock::now();
    for (auto it = s_pictures_failed.begin(); it != s_pictures_failed.end();)
    {
      if (now - it->second > std::chrono::seconds(60))
      {
        s_pictures_asked.erase(it->first);
        it = s_pictures_failed.erase(it);
      }
      else
      {
        ++it;
      }
    }
    if (!s_pictures_asked.insert(address).second)
      return {};
  }
  s_worker.Post(
      [address, path] {
        Net::Request request;
        request.url = address;
        request.user_agent = UserAgent();
        request.ca_file = s_ca_file;
        request.timeout_seconds = 20;
        const Net::Response answer = Net::Download(request, path, &s_media_session);
        if (!answer.ok())
        {
          LogF("ra: picture %s: %s", address.c_str(), answer.error.empty() ? ("HTTP " + std::to_string(answer.status)).c_str() :
                                                                             answer.error.c_str());
          std::remove(path.c_str());
          std::remove((path + ".part").c_str());
          std::lock_guard lock(s_mutex);
          s_pictures_failed.emplace_back(address, Clock::now());
        }
      },
      true);
  return {};
}

// ---- The library's marks (00.77) ----
namespace
{
constexpr int64_t KNOWN_SECONDS = 7 * 24 * 3600;  // RetroAchievements' lists asked for again after a week

std::string KnownFile(uint32_t console)
{
  return s_cache + "/known-" + std::to_string(console) + ".txt";
}

std::string LibraryHashesFile()
{
  return s_cache + "/library-hashes.txt";
}

std::string Extension(const std::string& path)
{
  const size_t dot = path.rfind('.');
  const size_t slash = path.rfind('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  std::string ext = path.substr(dot + 1);
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// A Famicom Disk System disk (RetroAchievements' console of its own): its header, or its sides' size.
bool IsFds(const uint8_t* data, size_t size)
{
  return (size >= 4 && std::memcmp(data, "FDS\x1a", 4) == 0) ||
         (size >= 65500 && size % 65500 == 0 && std::memcmp(data + 1, "*NINTENDO-HVC*", 14) == 0);
}

// A gzip answer (RetroAchievements sends its lists so: a quarter of their size) made plain again.
bool Gunzip(const std::string& in, std::string* out)
{
  z_stream zs = {};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
    return false;
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  out->clear();
  char buffer[16384];
  int result = Z_OK;
  while (result == Z_OK)
  {
    zs.next_out = reinterpret_cast<Bytef*>(buffer);
    zs.avail_out = sizeof(buffer);
    result = inflate(&zs, Z_NO_FLUSH);
    out->append(buffer, sizeof(buffer) - zs.avail_out);
  }
  inflateEnd(&zs);
  return result == Z_STREAM_END;
}

// A console's list as kept on the PS4 (or as it just came): into the module's tables; its date.
int64_t ApplyKnown(uint32_t console, const std::string& text)
{
  int64_t fetched = 0;
  bool titled = true;
  std::vector<std::pair<std::string, KnownHash>> hashes;
  std::vector<std::pair<uint32_t, KnownGame>> games;
  for (const std::string& line : Lines(text))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "F" && f.size() >= 3)
    {
      fetched = Time(f[2]);
    }
    else if (f[0] == "G" && f.size() >= 4)
    {
      // (00.79: a game with achievements has its name, for Triangle -> Achievements on a file it doesn't know.)
      KnownGame game{Number(f[2]), Number(f[3]), console, f.size() >= 5 ? f[4] : std::string()};
      titled = titled && (game.achievements == 0 || !game.title.empty());
      games.push_back({Number(f[1]), std::move(game)});
    }
    else if ((f[0] == "S" || f[0] == "U") && f.size() >= 3)
    {
      hashes.push_back({f[1], KnownHash{Number(f[2]), f[0] == "S"}});
    }
  }
  // (A list of 00.77-00.78, without the games' names: asked for again - meanwhile it marks the games as before.)
  if (!titled)
    fetched = 0;
  std::lock_guard lock(s_mutex);
  for (auto& [id, game] : games)
    s_known_games[id] = game;
  for (auto& [hash, known] : hashes)
    s_known_hashes[hash] = known;
  s_known_fetched[console] = fetched;
  s_lib_version++;
  return fetched;
}

// RetroAchievements' games of a console (its "systemgames": no account), kept: false when it didn't come. (session: the
// calling thread's own - the library's, or the module's for a file looked at now, 00.80.)
bool FetchKnown(uint32_t console, Net::Session* session = &s_lib_session)
{
  rc_api_fetch_games_list_request_t params = {};
  params.console_id = console;
  rc_api_host_t host = {};
#ifndef __PS4__
  host.host = std::getenv("EMUSUNI_RA_HOST");  // (the PC tests' stand-in)
#endif
  rc_api_request_t request;
  if (rc_api_init_fetch_games_list_request_hosted(&request, &params, &host) != RC_OK)
    return false;
  Net::Request http;
  http.url = request.url ? request.url : "";
  if (request.post_data)
  {
    http.method = "POST";
    http.body = request.post_data;
    http.content_type = request.content_type ? request.content_type : "application/x-www-form-urlencoded";
  }
  rc_api_destroy_request(&request);
  http.headers = {"Accept-Encoding: gzip"};
  http.user_agent = UserAgent();
  http.ca_file = s_ca_file;
  http.timeout_seconds = 90;
  http.error_body = true;
  http.max_bytes = 32 << 20;
  const auto start = Clock::now();
  const Net::Response answer = Net::Fetch(http, session);
  const long code = answer.status;
  NoteAnswer(code > 0 && code != 502 && code != 503 && code != 504 && !(code >= 520 && code <= 527));
  if (!answer.ok())
  {
    LogF("ra: library: console %u's games didn't come (%s)", console,
         code > 0 ? ("HTTP " + std::to_string(code)).c_str() : answer.error.c_str());
    return false;
  }
  std::string body;
  // (gzip's own first bytes: the answer as it came, whatever its headers say.)
  if (answer.body.size() >= 2 && static_cast<unsigned char>(answer.body[0]) == 0x1f &&
      static_cast<unsigned char>(answer.body[1]) == 0x8b)
  {
    if (!Gunzip(answer.body, &body))
    {
      LogF("ra: library: console %u's games came damaged (gzip)", console);
      return false;
    }
  }
  else
  {
    body = answer.body;
  }
  rc_api_server_response_t server = {};
  server.body = body.c_str();
  server.body_length = body.size();
  server.http_status_code = static_cast<int>(code);
  rc_api_fetch_games_list_response_t list;
  if (rc_api_process_fetch_games_list_server_response(&list, &server) != RC_OK || !list.response.succeeded)
  {
    LogF("ra: library: console %u's games: %s", console,
         list.response.error_message ? list.response.error_message : "an answer that isn't the list");
    rc_api_destroy_fetch_games_list_response(&list);
    return false;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  std::string text = "# EmuSuNi: RetroAchievements' games of console " + std::to_string(console) +
                     " (systemgames): G id achievements points name, S a supported file's hash, U another version's.\n";
  text += "F\t" + std::to_string(console) + "\t" + std::to_string(now) + "\n";
  uint32_t with = 0, hashes = 0;
  for (uint32_t i = 0; i < list.num_entries; i++)
  {
    const rc_api_game_list_entry_t& game = list.entries[i];
    text += "G\t" + std::to_string(game.id) + "\t" + std::to_string(game.num_achievements) + "\t" +
            std::to_string(game.points) + (game.num_achievements > 0 ? "\t" + Clean(Copy(game.name)) : std::string()) +
            "\n";
    with += game.num_achievements > 0 ? 1 : 0;
    for (uint32_t h = 0; h < game.num_supported_hashes; h++)
    {
      text += "S\t" + Copy(game.supported_hashes[h]) + "\t" + std::to_string(game.id) + "\n";
      hashes++;
    }
    for (uint32_t h = 0; game.num_achievements > 0 && h < game.num_unsupported_hashes; h++)
      text += "U\t" + Copy(game.unsupported_hashes[h]) + "\t" + std::to_string(game.id) + "\n";
  }
  const uint32_t count = list.num_entries;
  rc_api_destroy_fetch_games_list_response(&list);
  WriteTextFile(KnownFile(console), text);
  ApplyKnown(console, text);
  LogF("ra: library: console %u: %u games, %u with achievements, %u files known (%zu bytes, %.0f ms)", console, count,
       with, hashes, answer.body.size(), std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  return true;
}

// The PS4's copy of the files' hashes, once (the library's thread).
void LoadLibraryHashes()
{
  std::unordered_map<std::string, HashedFile> hashes;
  for (const std::string& line : Lines(ReadTextFile(LibraryHashesFile())))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] != "H" || f.size() < 6)
      continue;
    HashedFile file;
    file.size = std::strtoull(f[1].c_str(), nullptr, 10);
    file.mtime = Time(f[2]);
    file.console = Number(f[3]);
    file.hash = f[4] == "-" ? std::string() : f[4];
    hashes[f[5]] = std::move(file);
  }
  std::lock_guard lock(s_mutex);
  for (auto& [path, file] : hashes)
    s_lib_hashes.emplace(path, std::move(file));
}

void SaveLibraryHashes()
{
  std::string text = "# EmuSuNi: the games' files as rc_hash makes them (size, date, console, hash or -, path).\n";
  {
    std::lock_guard lock(s_mutex);
    for (const auto& [path, file] : s_lib_hashes)
    {
      text += "H\t" + std::to_string(file.size) + "\t" + std::to_string(file.mtime) + "\t" +
              std::to_string(file.console) + "\t" + (file.hash.empty() ? std::string("-") : file.hash) + "\t" + path +
              "\n";
    }
  }
  WriteTextFile(LibraryHashesFile(), text);
}

void RC_CCONV QuietHash(const char*, const rc_hash_iterator_t*) {}

// A file's hash as rc_client makes it for the game (quietly: no line per file in the log). console: its
// RetroAchievements console then (a Famicom Disk System disk's own).
std::string LibraryHash(const LibraryFile& file, uint32_t* console, size_t* read)
{
  *console = ConsoleId(file.system);
  *read = 0;
  if (*console == 0 || *console == RC_CONSOLE_NINTENDO_3DS)
    return {};
  const std::string ext = Extension(file.path);
  std::vector<uint8_t> data;
  std::string path = file.path;
  if (*console != RC_CONSOLE_ARCADE && (ext == "zip" || ext == "7z"))
  {
    // A zipped cartridge: its ROM read as the game's start reads it (at most 48 MiB of zip: no more memory than that
    // in the menu).
    struct stat st;
    if (ext == "7z" || !s_rom_reader || stat(file.path.c_str(), &st) != 0 || st.st_size > (48 << 20))
      return {};
    std::string error;
    if (!s_rom_reader(file.path, file.system, &data, &error) || data.empty())
      return {};
    *read = data.size();
  }
  else if (*console == RC_CONSOLE_PLAYSTATION || *console == RC_CONSOLE_PSP || *console == RC_CONSOLE_DREAMCAST ||
           *console == RC_CONSOLE_SATURN)
  {
    std::string why;
    if (!AchievementsDisc::CanHash(file.system, file.path, &why))
      return {};
    path = AchievementsDisc::HashPath(file.path);
  }
  else if (*console == RC_CONSOLE_NINTENDO)
  {
    // (A disk's header: the first 16 bytes.)
    if (FILE* f = std::fopen(file.path.c_str(), "rb"))
    {
      uint8_t head[16] = {};
      const size_t got = std::fread(head, 1, sizeof(head), f);
      std::fseek(f, 0, SEEK_END);
      const long size = std::ftell(f);
      std::fclose(f);
      if ((got >= 4 && std::memcmp(head, "FDS\x1a", 4) == 0) ||
          (got >= 15 && size >= 65500 && size % 65500 == 0 && std::memcmp(head + 1, "*NINTENDO-HVC*", 14) == 0) ||
          ext == "fds")
        *console = RC_CONSOLE_FAMICOM_DISK_SYSTEM;
    }
  }
  if (*console == RC_CONSOLE_NINTENDO && !data.empty() && IsFds(data.data(), data.size()))
    *console = RC_CONSOLE_FAMICOM_DISK_SYSTEM;
  rc_hash_iterator_t iterator;
  rc_hash_initialize_iterator(&iterator, path.c_str(), data.empty() ? nullptr : data.data(), data.size());
  iterator.callbacks.verbose_message = QuietHash;
  iterator.callbacks.error_message = QuietHash;
  AchievementsDisc::GetReader(&iterator.callbacks.cdreader);
  char hash[33] = "";
  const int ok = rc_hash_generate(hash, *console, &iterator);
  rc_hash_destroy_iterator(&iterator);
  return ok ? std::string(hash) : std::string();
}

// The run stops (between files) when a game starts or the menu brings another list.
bool LibraryInterrupted()
{
  std::lock_guard lock(s_mutex);
  return s_lib_paused || s_lib_work;
}

void LibraryRun(const std::vector<LibraryFile>& files, bool force)
{
  const auto start = Clock::now();
  static bool loaded = false;  // (the library's thread's own)
  static std::set<uint32_t> known_loaded;
  static std::set<std::string> checked;  // the files whose date was looked at this session
  if (!loaded)
  {
    LoadLibraryHashes();
    loaded = true;
  }
  // RetroAchievements' lists of the library's consoles: the PS4's copy, asked for again after a week.
  std::set<uint32_t> consoles;
  for (const LibraryFile& file : files)
  {
    const uint32_t console = ConsoleId(file.system);
    if (console == 0 || console == RC_CONSOLE_NINTENDO_3DS)
      continue;
    consoles.insert(console);
    if (console == RC_CONSOLE_NINTENDO)
      consoles.insert(RC_CONSOLE_FAMICOM_DISK_SYSTEM);
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (const uint32_t console : consoles)
  {
    if (LibraryInterrupted())
      return;
    int64_t fetched = 0;
    if (known_loaded.insert(console).second)
    {
      const std::string text = ReadTextFile(KnownFile(console));
      if (!text.empty())
        fetched = ApplyKnown(console, text);
    }
    else
    {
      std::lock_guard lock(s_mutex);
      fetched = s_known_fetched.count(console) ? s_known_fetched[console] : 0;
    }
    if (force || fetched == 0 || now - fetched > KNOWN_SECONDS || fetched > now)
      FetchKnown(console);
  }
  // The files not hashed yet (or changed since).
  size_t hashed = 0, unchanged = 0;
  uint64_t bytes = 0;
  double slowest = 0.0;
  std::string slowest_path;
  for (const LibraryFile& file : files)
  {
    if (LibraryInterrupted())
      break;
    if (checked.count(file.path))
    {
      unchanged++;
      continue;
    }
    struct stat st;
    if (stat(file.path.c_str(), &st) != 0)
      continue;
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    const int64_t mtime = static_cast<int64_t>(st.st_mtime);
    {
      std::lock_guard lock(s_mutex);
      const auto it = s_lib_hashes.find(file.path);
      if (it != s_lib_hashes.end() && it->second.size == size && it->second.mtime == mtime)
      {
        checked.insert(file.path);
        unchanged++;
        continue;
      }
    }
    const auto file_start = Clock::now();
    uint32_t console = 0;
    size_t read = 0;
    const std::string hash = LibraryHash(file, &console, &read);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - file_start).count();
    if (ms > slowest)
    {
      slowest = ms;
      slowest_path = file.path;
    }
    bytes += read ? read : size;
    hashed++;
    checked.insert(file.path);
    {
      std::lock_guard lock(s_mutex);
      HashedFile& kept = s_lib_hashes[file.path];
      kept.size = size;
      kept.mtime = mtime;
      kept.console = console;
      kept.hash = hash;
      s_lib_version++;
    }
    if (hashed % 50 == 0)
      SaveLibraryHashes();
  }
  if (hashed > 0)
    SaveLibraryHashes();
  size_t known = 0;
  {
    std::lock_guard lock(s_mutex);
    for (const LibraryFile& file : files)
    {
      const auto it = s_lib_hashes.find(file.path);
      if (it == s_lib_hashes.end() || it->second.hash.empty())
        continue;
      const auto k = s_known_hashes.find(it->second.hash);
      if (k != s_known_hashes.end() && k->second.supported)
      {
        const auto g = s_known_games.find(k->second.game_id);
        known += g != s_known_games.end() && g->second.achievements > 0 ? 1 : 0;
      }
    }
  }
  LogF("ra: library: %zu games, %zu hashed now (%.0f MB in %.1f s; the slowest %.0f ms: %s), %zu as before; %zu with "
       "achievements", files.size(), hashed, bytes / 1048576.0,
       std::chrono::duration<double>(Clock::now() - start).count(), slowest, slowest_path.c_str(), unchanged, known);
}

void* LibraryMain(void*)
{
#ifdef __PS4__
  // Off the emulation's cores, at the lowest of the usual priorities: the menu and the module's own thread go first.
  emusuni_ra_set_affinity(scePthreadSelf(), (1ull << 0) | (1ull << 1) | (1ull << 4) | (1ull << 5));
  scePthreadSetprio(scePthreadSelf(), 760);
#endif
  for (;;)
  {
    std::vector<LibraryFile> files;
    bool force;
    {
      std::unique_lock lock(s_mutex);
      s_lib_wake.wait(lock, [] { return s_lib_work && !s_lib_paused; });
      s_lib_work = false;
      files = s_lib_files;
      force = s_lib_force;
      s_lib_force = false;
    }
    LibraryRun(files, force);
  }
  return nullptr;
}
}  // namespace

void SetLibrary(const std::vector<LibraryFile>& files)
{
  bool start = false;
  {
    std::lock_guard lock(s_mutex);
    s_lib_files = files;
    s_lib_work = true;
    if (!s_lib_started)
    {
      s_lib_started = true;
      start = true;
    }
  }
  if (start)
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t thread;
    const bool made = pthread_create(&thread, &attr, LibraryMain, nullptr) == 0;
    pthread_attr_destroy(&attr);
    if (made)
      pthread_detach(thread);
    else
      LogLine("ra: library: its thread couldn't start: no marks");
  }
  s_lib_wake.notify_all();
}

void PauseLibrary(bool pause)
{
  {
    std::lock_guard lock(s_mutex);
    s_lib_paused = pause;
    if (!pause && !s_lib_files.empty())
      s_lib_work = true;  // (the run goes on where it stopped)
  }
  s_lib_wake.notify_all();
}

std::map<std::string, LibraryInfo> GetLibraryInfos()
{
  std::map<std::string, LibraryInfo> out;
  std::lock_guard lock(s_mutex);
  // The account's progress by game: its games (the last update), and those played here after it.
  std::unordered_map<uint32_t, uint32_t> progress;  // (unlocked)
  if (!s_user.empty() && s_account_user == s_user)
  {
    for (const HistoryGame& game : s_account_games)
      progress[game.game_id] = game.unlocked;
  }
  for (const auto& [owner, game] : s_played)
  {
    if (owner != s_user || (s_account_user == s_user && game.played <= s_synced && progress.count(game.game_id)))
      continue;
    uint32_t& unlocked = progress[game.game_id];
    unlocked = std::max(unlocked, game.unlocked);
  }
  for (const LibraryFile& file : s_lib_files)
  {
    LibraryInfo info;
    const auto hashed = s_lib_hashes.find(file.path);
    if (hashed != s_lib_hashes.end())
    {
      const uint32_t console = hashed->second.console;
      const auto known = hashed->second.hash.empty() ? s_known_hashes.end() : s_known_hashes.find(hashed->second.hash);
      if (known == s_known_hashes.end())
      {
        // (Not known: once the console's list is there - or a file that couldn't be hashed.)
        if (hashed->second.hash.empty() || s_known_fetched.count(console))
          info.state = LibraryInfo::State::NotKnown;
      }
      else
      {
        info.game_id = known->second.game_id;
        const auto game = s_known_games.find(info.game_id);
        info.achievements = game != s_known_games.end() ? game->second.achievements : 0;
        info.points = game != s_known_games.end() ? game->second.points : 0;
        info.state = !known->second.supported ? LibraryInfo::State::OtherVersion :
                     info.achievements == 0    ? LibraryInfo::State::NoAchievements :
                                                 LibraryInfo::State::Known;
        if (const auto p = progress.find(info.game_id); p != progress.end())
        {
          info.progress = true;
          info.unlocked = std::min(p->second, info.achievements);
        }
        else if (!s_user.empty() && s_account_user == s_user)
        {
          info.progress = true;  // (the account's games came: none unlocked in this one)
        }
      }
    }
    out[file.key] = info;
  }
  return out;
}

int LibraryVersion()
{
  std::lock_guard lock(s_mutex);
  return s_lib_version;
}

void SetRomReader(bool (*read)(const std::string& path, SystemId system, std::vector<uint8_t>* data,
                               std::string* error))
{
  s_rom_reader = read;
}

bool MarksSetting()
{
  std::lock_guard lock(s_mutex);
  return s_marks;
}

void SetMarksSetting(bool on)
{
  {
    std::lock_guard lock(s_mutex);
    s_marks = on;
    SaveIni();
  }
  LogF("ra: marks on the games: %s", on ? "on" : "off");
}

// ---- The files RetroAchievements knows of a game (00.79): Triangle -> Achievements on a file it doesn't know ----
namespace
{
Versions s_versions;
int s_versions_serial = 0;
Net::Session& s_names_session = *new Net::Session;
constexpr int64_t NAMES_SECONDS = 30 * 24 * 3600;  // the files' names asked for again after a month

// A name compared with another (RetroAchievements' and the menu's): small letters and digits only, the parts in
// brackets ("(USA)", "[!]") and the word "the" left out ("Legend of Zelda, The - A Link to the Past" is "The Legend of
// Zelda: A Link to the Past"); letters past ASCII kept as they are.
std::string TitleKey(const std::string& title)
{
  std::string spaced;
  int depth = 0;
  for (const char c : title)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '(' || c == '[')
      depth++;
    else if (c == ')' || c == ']')
      depth = std::max(0, depth - 1);
    else if (depth > 0)
      continue;
    else if (std::isalnum(u) || u >= 0x80)
      spaced += static_cast<char>(std::tolower(u));
    else if (c == '&')
      spaced += " and ";
    else if (c != '\'')
      spaced += ' ';
  }
  std::string key;
  for (size_t start = 0; start < spaced.size();)
  {
    size_t end = spaced.find(' ', start);
    if (end == std::string::npos)
      end = spaced.size();
    const std::string word = spaced.substr(start, end - start);
    if (!word.empty() && word != "the")
      key += (key.empty() ? "" : " ") + word;
    start = end + 1;
  }
  return key;
}

// No-Intro's list of a console's cartridges in libretro's database (their files' MD5, which is RetroAchievements'
// hash for these consoles: a NES file's without its header, the list's ".unh"), "" for the others (discs: hashes of
// RetroAchievements' own; the arcade: sets' names; the DS: its own hash).
const char* NoIntroList(uint32_t console)
{
  switch (console)
  {
  case RC_CONSOLE_MEGA_DRIVE: return "Sega - Mega Drive - Genesis";
  case RC_CONSOLE_NINTENDO_64: return "Nintendo - Nintendo 64";
  case RC_CONSOLE_SUPER_NINTENDO: return "Nintendo - Super Nintendo Entertainment System";
  case RC_CONSOLE_GAMEBOY: return "Nintendo - Game Boy";
  case RC_CONSOLE_GAMEBOY_ADVANCE: return "Nintendo - Game Boy Advance";
  case RC_CONSOLE_GAMEBOY_COLOR: return "Nintendo - Game Boy Color";
  case RC_CONSOLE_NINTENDO: return "Nintendo - Nintendo Entertainment System";
  case RC_CONSOLE_MASTER_SYSTEM: return "Sega - Master System - Mark III";
  case RC_CONSOLE_GAME_GEAR: return "Sega - Game Gear";
  case RC_CONSOLE_SG1000: return "Sega - SG-1000";
  case RC_CONSOLE_FAMICOM_DISK_SYSTEM: return "Nintendo - Family Computer Disk System";
  default: return "";
  }
}

std::string NamesFile(uint32_t console)
{
  return s_cache + "/names-" + std::to_string(console) + ".txt";
}

// The names of these files (by hash) on a console: from the PS4's copy (made once a month from No-Intro's list, the
// names of every file RetroAchievements knows there), else from the list now (a download of 0.5 to 3 MB, compressed
// to a fraction). The module's thread.
std::unordered_map<std::string, std::string> FileNames(uint32_t console, const std::set<std::string>& wanted)
{
  std::unordered_map<std::string, std::string> out;
  const std::string list = NoIntroList(console);
  if (list.empty() || wanted.empty())
    return out;
  std::string text = ReadTextFile(NamesFile(console));
  int64_t made = 0;
  for (const std::string& line : Lines(text))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "F" && f.size() >= 2)
      made = Time(f[1]);
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (made == 0 || now - made > NAMES_SECONDS || made > now)
  {
    std::string host = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/";
#ifndef __PS4__
    if (const char* test = std::getenv("EMUSUNI_DAT_HOST"))
      host = test;  // (the PC tests' stand-in)
#endif
    Net::Request http;
    std::string path;
    for (const char c : list)
      path += c == ' ' ? std::string("%20") : std::string(1, c);
    http.url = host + path + ".dat";
    http.headers = {"Accept-Encoding: gzip"};
    http.user_agent = UserAgent();
    http.ca_file = s_ca_file;
    http.timeout_seconds = 60;
    http.max_bytes = 12 << 20;
    const auto start = Clock::now();
    const Net::Response answer = Net::Fetch(http, &s_names_session);
    std::string dat;
    if (answer.ok() && answer.body.size() >= 2 && static_cast<unsigned char>(answer.body[0]) == 0x1f &&
        static_cast<unsigned char>(answer.body[1]) == 0x8b)
    {
      if (!Gunzip(answer.body, &dat))
        dat.clear();
    }
    else if (answer.ok())
    {
      dat = answer.body;
    }
    if (dat.find("rom ( name \"") == std::string::npos)
    {
      LogF("ra: versions: No-Intro's list for console %u didn't come (%s)", console,
           answer.status > 0 ? ("HTTP " + std::to_string(answer.status)).c_str() : answer.error.c_str());
    }
    else
    {
      // Every file RetroAchievements knows on the console (its games' supported files and other versions).
      std::set<std::string> known;
      {
        std::lock_guard lock(s_mutex);
        for (const auto& [hash, file] : s_known_hashes)
        {
          const auto game = s_known_games.find(file.game_id);
          if (game != s_known_games.end() && game->second.console == console)
            known.insert(hash);
        }
      }
      text = "# EmuSuNi: No-Intro's names (libretro's database) of the files RetroAchievements knows on console " +
             std::to_string(console) + ": N hash name.\nF\t" + std::to_string(now) + "\n";
      size_t named = 0;
      for (size_t at = 0; (at = dat.find("rom ( name \"", at)) != std::string::npos;)
      {
        at += 12;
        const size_t quote = dat.find('"', at);
        const size_t eol = dat.find('\n', at);
        if (quote == std::string::npos)
          break;
        std::string name = dat.substr(at, quote - at);
        const size_t md5 = dat.find(" md5 ", quote);
        if (md5 == std::string::npos || (eol != std::string::npos && md5 > eol) || md5 + 5 + 32 > dat.size())
          continue;
        std::string hash = dat.substr(md5 + 5, 32);
        for (char& c : hash)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!known.count(hash))
          continue;
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && name.size() - dot <= 5)
          name.resize(dot);
        text += "N\t" + hash + "\t" + Clean(name) + "\n";
        named++;
      }
      WriteTextFile(NamesFile(console), text);
      LogF("ra: versions: No-Intro's names of console %u: %zu of the %zu files RetroAchievements knows (%zu KB, %.0f ms)",
           console, named, known.size(), answer.body.size() / 1024,
           std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
  }
  for (const std::string& line : Lines(text))
  {
    const std::vector<std::string> f = Fields(line);
    if (f[0] == "N" && f.size() >= 3 && wanted.count(f[1]))
      out[f[1]] = f[2];
  }
  return out;
}

std::string BaseName(const std::string& path)
{
  const size_t slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0 && name.size() - dot <= 5)
    name.resize(dot);
  return name;
}
// (The module's thread, 00.80) The console's list (systemgames): the PS4's copy, or asked for when there is none or it
// is a week old - for a file looked at before the library's run reached it, or with the marks off. False: no list.
bool EnsureKnown(uint32_t console)
{
  {
    std::lock_guard lock(s_mutex);
    const auto it = s_known_fetched.find(console);
    if (it != s_known_fetched.end() && it->second != 0)
      return true;
  }
  const std::string text = ReadTextFile(KnownFile(console));
  const int64_t fetched = text.empty() ? 0 : ApplyKnown(console, text);
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  if (fetched == 0 || now - fetched > KNOWN_SECONDS || fetched > now)
    FetchKnown(console, &s_names_session);
  std::lock_guard lock(s_mutex);
  return s_known_fetched.count(console) > 0;
}

// A game's Triangle -> Achievements (00.80: every game of a console RetroAchievements has, its file checked now when the
// library's run hasn't reached it): the file's hash (the PS4's copy when its size and date are the same, else made now),
// the console's list, the file's state; for a file it doesn't take, its game and the files it does (00.79).
void CheckVersions(const LibraryFile& file, const std::string& title, int serial)
{
  const auto start = Clock::now();
  uint32_t console = ConsoleId(file.system);
  std::string hash;
  struct stat st;
  const bool exists = stat(file.path.c_str(), &st) == 0;
  bool hash_now = false;
  {
    std::lock_guard lock(s_mutex);
    const auto it = s_lib_hashes.find(file.path);
    if (exists && it != s_lib_hashes.end() && it->second.size == static_cast<uint64_t>(st.st_size) &&
        it->second.mtime == static_cast<int64_t>(st.st_mtime))
    {
      hash = it->second.hash;
      console = it->second.console ? it->second.console : console;
    }
    else
    {
      hash_now = exists;
    }
  }
  if (hash_now)
  {
    // (Kept for the marks too - the library's run then leaves the file; written with the others by that run.)
    size_t read = 0;
    hash = LibraryHash(file, &console, &read);
    std::lock_guard lock(s_mutex);
    s_lib_hashes[file.path] =
        HashedFile{static_cast<uint64_t>(st.st_size), static_cast<int64_t>(st.st_mtime), console, hash};
    s_lib_version++;
  }
  const bool listed = console != 0 && EnsureKnown(console);
  Versions found;
  found.file_name = BaseName(file.path);
  std::set<std::string> wanted;
  {
    std::lock_guard lock(s_mutex);
    const auto known = hash.empty() ? s_known_hashes.end() : s_known_hashes.find(hash);
    if (known != s_known_hashes.end())
    {
      found.game_id = known->second.game_id;
      found.other_version = !known->second.supported;
      const auto game = s_known_games.find(found.game_id);
      const uint32_t achievements = game != s_known_games.end() ? game->second.achievements : 0;
      found.file_state = found.other_version ? LibraryInfo::State::OtherVersion :
                         achievements == 0    ? LibraryInfo::State::NoAchievements :
                                                LibraryInfo::State::Known;
    }
    else
    {
      // (Without the console's list - no connection the first time -: not known yet.)
      found.file_state = listed || hash.empty() ? LibraryInfo::State::NotKnown : LibraryInfo::State::Unchecked;
    }
    // A file RetroAchievements doesn't know: its game by the name (the one with the most achievements; never a
    // subset's own entry).
    if (found.file_state == LibraryInfo::State::NotKnown)
    {
      const std::string key = TitleKey(title);
      uint32_t best = 0;
      for (const auto& [id, game] : s_known_games)
      {
        if (game.console != console || game.achievements == 0 || game.title.empty() ||
            game.title.find("[Subset") != std::string::npos || TitleKey(game.title) != key)
          continue;
        if (game.achievements > best)
        {
          best = game.achievements;
          found.game_id = id;
        }
      }
    }
    if (const auto game = s_known_games.find(found.game_id); found.game_id && game != s_known_games.end())
    {
      // (The lists keep the names of the games with achievements only: else the menu's own.)
      found.title = game->second.title.empty() ? title : game->second.title;
      found.achievements = game->second.achievements;
      found.points = game->second.points;
      for (const auto& [other, known_file] : s_known_hashes)
      {
        if (known_file.game_id == found.game_id && known_file.supported)
          wanted.insert(other);
      }
      // The player's own files it takes for that game.
      for (const LibraryFile& other : s_lib_files)
      {
        const auto h = s_lib_hashes.find(other.path);
        const auto k = h == s_lib_hashes.end() || h->second.hash.empty() ? s_known_hashes.end() :
                                                                             s_known_hashes.find(h->second.hash);
        if (k != s_known_hashes.end() && k->second.game_id == found.game_id && k->second.supported)
          found.yours.push_back(BaseName(other.path));
      }
      std::sort(found.yours.begin(), found.yours.end());
      found.yours.erase(std::unique(found.yours.begin(), found.yours.end()), found.yours.end());
    }
  }
  found.unnamed = static_cast<uint32_t>(wanted.size());
  // The names of the files it takes (No-Intro's list), and this one's own, for a file it doesn't take.
  const bool other = found.file_state == LibraryInfo::State::NotKnown || found.other_version;
  if (other && !wanted.empty() && *NoIntroList(console))
  {
    std::set<std::string> lookup = wanted;
    if (!hash.empty())
      lookup.insert(hash);  // (its own name too, when No-Intro's list has it)
    const std::unordered_map<std::string, std::string> names = FileNames(console, lookup);
    if (const auto own = hash.empty() ? names.end() : names.find(hash); own != names.end())
      found.file_name = own->second;
    uint32_t named = 0;
    for (const std::string& supported : wanted)
    {
      if (const auto name = names.find(supported); name != names.end())
      {
        found.names.push_back(name->second);
        named++;
      }
    }
    std::sort(found.names.begin(), found.names.end());
    found.names.erase(std::unique(found.names.begin(), found.names.end()), found.names.end());
    found.unnamed = static_cast<uint32_t>(wanted.size()) - named;
  }
  found.state = Versions::State::Ready;
  static const char* STATES[] = {"not checked (no list)", "not known", "known, no achievements yet",
                                 "another version", "known"};
  LogF("ra: versions: %s (console %u%s, %.0f ms): %s%s", file.path.c_str(), console, hash_now ? ", hashed now" : "",
       std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
       STATES[static_cast<int>(found.file_state)],
       found.game_id ? (", RetroAchievements' game " + std::to_string(found.game_id) + " \"" + found.title + "\", " +
                        std::to_string(wanted.size()) + " files it supports")
                           .c_str() :
                       other ? ", no game of that name" : "");
  std::lock_guard lock(s_mutex);
  if (serial == s_versions_serial)
    s_versions = std::move(found);
}
}  // namespace

void OpenVersions(const LibraryFile& file, const std::string& title)
{
  int serial;
  {
    std::lock_guard lock(s_mutex);
    serial = ++s_versions_serial;
    s_versions = {};
    s_versions.state = Versions::State::Loading;
    s_versions.file_name = BaseName(file.path);
  }
  s_worker.Post([file, title, serial] { CheckVersions(file, title, serial); }, true);
}

Versions GetVersions()
{
  std::lock_guard lock(s_mutex);
  return s_versions;
}

std::string UserAgent()
{
  std::lock_guard lock(s_mutex);
  return s_ua_base + (s_ua_core.empty() ? std::string() : " " + s_ua_core) + " " + s_ua_clause;
}

uint32_t ConsoleId(SystemId system)
{
  switch (system)
  {
  case SystemId::SNES: return RC_CONSOLE_SUPER_NINTENDO;
  case SystemId::NES: return RC_CONSOLE_NINTENDO;
  case SystemId::N64: return RC_CONSOLE_NINTENDO_64;
  case SystemId::GB: return RC_CONSOLE_GAMEBOY;
  case SystemId::GBC: return RC_CONSOLE_GAMEBOY_COLOR;
  case SystemId::GBA: return RC_CONSOLE_GAMEBOY_ADVANCE;
  case SystemId::PS1: return RC_CONSOLE_PLAYSTATION;
  case SystemId::NDS: return RC_CONSOLE_NINTENDO_DS;
  case SystemId::PSP: return RC_CONSOLE_PSP;
  case SystemId::DC: return RC_CONSOLE_DREAMCAST;
  case SystemId::SG1000: return RC_CONSOLE_SG1000;
  case SystemId::SMS: return RC_CONSOLE_MASTER_SYSTEM;
  case SystemId::GG: return RC_CONSOLE_GAME_GEAR;
  case SystemId::MD: return RC_CONSOLE_MEGA_DRIVE;
  // The arcade boards: RetroAchievements' Arcade, a set known by its name (FinalBurn Neo's, Flycast's NAOMI and
  // Atomiswave sets alike: "mvsc2" is RetroAchievements' game 18732).
  case SystemId::ARCADE:
  case SystemId::NAOMI:
  case SystemId::ATOMISWAVE: return RC_CONSOLE_ARCADE;
  case SystemId::THREEDS: return RC_CONSOLE_NINTENDO_3DS;
  case SystemId::SATURN: return RC_CONSOLE_SATURN;
  case SystemId::PCE: return RC_CONSOLE_PC_ENGINE;  // (00.82)
  }
  return 0;
}
}  // namespace Achievements
