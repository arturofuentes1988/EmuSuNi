// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// Star ratings (00.73): the stars under each game's cover - the LaunchBox Games Database's community rating (the
// average of its players' own ratings, 0 to 5 stars) in half stars, with how many voted.
//
// - LaunchBox's data isn't in the package (it has no licence that lets an app hand it on): each PS4 downloads it from
//   LaunchBox itself, as LaunchBox's own app and others do - https://gamesdb.launchbox-app.com/Metadata.zip, about
//   110 MB, once, and again after 60 days or when asked (Settings -> Star Ratings -> Download Again) - and keeps of it
//   only what the app needs: data_dir/cache/ratings/launchbox.tsv, each rated game of the app's consoles (its id,
//   platform, rating in half stars, votes, name and other names; a few MB). The zip is deleted after. No key, no
//   account: a plain download. After that the stars need no network.
// - The package has the app's own index of names (xmb/ratings/<console key>.tsv, scripts/make-ratings.py): each
//   console's No-Intro / Redump names - libretro-database's, the names of libretro's thumbnails, which the covers
//   are matched with - and each arcade set's name, with the LaunchBox game they are ("name<TAB>LaunchBox id").
// - A game is matched once, as its cover is (src/CoverDownload.h: the name given it with Rename, else its file's -
//   a list's numbering in front, the region and the tags as the covers take them; only a sure match counts), against
//   its console's index names and LaunchBox's own names for the console's games (those 3 players rated at least: a
//   hack's name rated by one player would be some other game's closest name), on a thread of this module's (256 KiB of
//   stack) that ends when the games are matched; an arcade, NAOMI or Atomiswave game by its set's name, else its
//   parent's. A game of no line, or one LaunchBox's players didn't rate, has no stars.

#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Library.h"

namespace Ratings
{
struct Rating
{
  int half_stars = -1;  // 0 (no star) to 10 (five stars); -1: the game has none
  int votes = 0;        // how many players rated it
};

// Where the ratings come from (Information says it).
constexpr const char* SOURCE = "LaunchBox Games Database";
constexpr const char* URL = "https://gamesdb.launchbox-app.com/Metadata.zip";

// ---- The ratings downloaded ----
// data_dir/cache/ratings/launchbox.tsv.
std::string CopyPath(const std::string& data_dir);
struct Copy
{
  bool there = false;
  int64_t made = 0;   // when it was written (seconds since 1970)
  std::string date;   // the date of LaunchBox's data in it ("2026-10-22"), else empty
  int games = 0;      // its games
  std::string platforms;  // the LaunchBox platforms it was made for (00.80: "a|b|c"), else empty
};
Copy ReadCopy(const std::string& data_dir);
// No copy yet, one older than 60 days, or one made for fewer consoles (00.80: before the Sega CD and the 32X; a copy
// made for more - 00.80's to 00.82's - is kept in 00.83).
bool CopyOld(const Copy& copy);
// LaunchBox's Metadata.zip (its Metadata.xml, read as it is unpacked, a line at a time) into the copy at `path`: the
// rated games of the app's consoles' platforms. False with *error (for the log) when the zip can't be read or holds
// no such game. `cancel` stops it; while `pause` is set it waits.
bool Extract(const std::string& zip, const std::string& path, std::string* error, int* games,
             const std::atomic<bool>* cancel = nullptr, const std::atomic<bool>* pause = nullptr);

// Downloads and extracts the copy on a thread (256 KiB of stack), one run at a time.
class Downloader
{
public:
  enum class State
  {
    Idle,
    Downloading,
    Reading,  // the zip downloaded, Metadata.xml being read
    Done,
    Failed,
  };
  ~Downloader() { Stop(); }
  // False when a run is at work already, or there is no thread for it.
  bool Start(const std::string& data_dir);
  // While set (a game runs), the download and the reading wait.
  void Pause(bool pause) { m_pause = pause; }
  State Now() const { return m_state; }
  bool Busy() const { return m_state == State::Downloading || m_state == State::Reading; }
  int64_t Bytes() const { return m_bytes; }
  int64_t Total() const { return m_total; }  // -1 until the server says it
  // Why the last run failed, in plain words (English, for PS4Lang::T).
  std::string Reason() const;
  // Once per run, when it has ended: true (and *ok: a new copy is there).
  bool Finished(bool* ok);
  void Stop();

private:
  static void* Main(void* self);
  void Run();

  pthread_t m_thread{};
  bool m_started = false;
  std::string m_data_dir;
  std::atomic<State> m_state{State::Idle};
  std::atomic<bool> m_cancel{false}, m_pause{false}, m_ended{false};
  std::atomic<int64_t> m_bytes{0}, m_total{-1};
  mutable std::mutex m_mutex;
  std::string m_reason;
};

// ---- A game's rating ----
// The ratings of the games (by game key; a game without one too, half_stars -1): its console's index in `index_dir`
// (xmb/ratings) and the copy at `copy` (CopyPath). The thread's work, and the PC tests'. `cancel` set: it stops (what
// it has so far); `pause` set: it waits between games. `matched` (the tests'): each rated game's name it matched.
std::unordered_map<std::string, Rating> Match(const std::vector<Library::Game>& games, const std::string& index_dir,
                                              const std::string& copy, const std::atomic<bool>* cancel = nullptr,
                                              const std::atomic<bool>* pause = nullptr,
                                              std::unordered_map<std::string, std::string>* matched = nullptr);
// A console's names with their ratings, as Match() takes them (the PC tests compare them with the script's).
std::vector<std::pair<std::string, Rating>> Names(SystemId system, const std::string& index_dir,
                                                  const std::string& copy);

// A name made plain for the matching: Latin-1's accented letters (and a few others: the macron vowels of Japanese
// names, the ligatures) without their accents, the typographic quotes and dashes as ASCII ones, the trademark signs
// left out ("Pokémon" -> "Pokemon", "Dracula’s" -> "Dracula's"). The index's names are written so
// (scripts/make-ratings.py has the same table).
std::string Plain(const std::string& name);

// Match() on a thread of its own, one run at a time.
class Matcher
{
public:
  ~Matcher();
  // Matches the games (copied) on the thread; a run still at work is stopped first. False: no thread (nothing
  // starts; the log says why).
  bool Start(const std::vector<Library::Game>& games, const std::string& index_dir, const std::string& copy);
  // The last run's ratings once it is done: true then (out gets them), else false.
  bool Take(std::unordered_map<std::string, Rating>* out);
  // While set (a game runs), the run waits between games.
  void Pause(bool pause) { m_pause = pause; }
  bool Busy() const { return m_started && !m_done; }

private:
  static void* Main(void* self);
  void Stop();

  pthread_t m_thread{};
  bool m_started = false;
  std::atomic<bool> m_done{false}, m_cancel{false}, m_pause{false};
  std::vector<Library::Game> m_games;
  std::string m_index_dir, m_copy;
  std::unordered_map<std::string, Rating> m_results;
};
}  // namespace Ratings
