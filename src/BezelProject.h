// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The Bezel Project's game bezels (00.80): github.com/thebezelproject - a bezel made for each game (its art around a
// TV's or a handheld's screen), for RetroArch, one GitHub project per console ("bezelproject-SNES": its PNGs in
// retroarch/overlay/GameBezels/SNES, named after the No-Intro names of the games; the arcade's after the MAME sets in
// retroarch/overlay/ArcadeBezels). The project publishes no licence, so none of its bezels is in the app or its
// repository: Settings -> Game Bezels downloads, on the player's request, the bezels of the player's own games from the
// project's GitHub - as its own RetroPie script and Windows utility do -, into data_dir/bezels/<console>/<the game's
// file name>.png, where Video -> Bezel's "Your Own" and "Per Game (The Bezel Project)" look (Emulator.cpp BezelPath).
//
// - Each project's list: one request to GitHub's API (git/trees/master:<folder>: the folder's files with their sizes
//   and Git hashes, gzip; unauthenticated, 60 requests an hour for the connection), kept in data_dir/cache/bezels/
//   <project>.txt and asked for again a day later with its ETag ("the same", 304: nothing downloaded again).
// - The games matched as their covers are (CoverDownload::Match: the names' words, their parts, close spellings, a
//   list's numbering in front left out, the region preferred); an arcade set by its name, then its parent's.
// - The files from raw.githubusercontent.com, one at a time through <file>.part (Net::Download: a cut download goes on
//   from where it stopped, also at the next start), checked against the list's Git hash; recorded in
//   data_dir/bezels/<console>/bezelproject.txt (Delete Downloaded Bezels deletes those files only: never the player's
//   own). The free space is checked before the first file; nothing is downloaded while a game runs (Pause).

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include "Image.h"
#include "Library.h"

namespace Net
{
class Session;
}

namespace BezelProject
{
// A console's project at The Bezel Project and its bezels' folder there.
struct Source
{
  const char* repo;    // "SNES": github.com/thebezelproject/bezelproject-SNES
  const char* folder;  // "retroarch/overlay/GameBezels/SNES"
};
// A console's projects, by its key (Systems.h), the first one's names first: the Super Nintendo's and the Super
// Famicom's, the NES's, the Famicom's and the Famicom Disk System's... None for a console without per-game bezels
// there (the PSP: only one bezel for the console).
std::vector<Source> Sources(const std::string& console);
inline bool Has(const std::string& console)
{
  return !Sources(console).empty();
}

// A bezel in a project's list.
struct Entry
{
  std::string name;  // the file's name without ".png" ("Super Mario World (USA)", "mslug")
  int64_t size = 0;  // bytes
  std::string sha;   // its Git blob hash (SHA-1 of "blob <size>\0" and the file)
  int source = 0;    // its project: an index into the console's Sources()
};
// The .png files of a GitHub tree listing (git/trees: a folder's files, not recursive). *truncated: GitHub cut it.
std::vector<Entry> ParseTree(const std::string& json, bool* truncated);
// A console's bezels: every project's list (its games are matched with all of them or none), each from the copy kept
// on the PS4 (a day old at most; any age when !fresh), else from GitHub's API. False when one didn't come: *error says
// why in plain words (English, for PS4Lang::T), *reset when GitHub's hour of requests starts again (Unix time) when
// its limit was the reason. cancel: stops a request (optional).
bool List(const std::string& data_dir, const std::string& console, bool fresh, std::vector<Entry>* entries,
          std::string* error, int64_t* reset, Net::Session* session, std::atomic<bool>* cancel);
// A file's Git blob hash, as GitHub's lists give it (40 hex digits).
std::string GitBlobSha(const std::string& path);

// A console's lists made ready for matching (the names indexed as the covers' are).
class Matcher
{
public:
  explicit Matcher(std::vector<Entry> entries);
  ~Matcher();
  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;
  // The game's bezel (nullptr: none). romset: a game known by its set's name (the Arcade, the NAOMI, the Atomiswave):
  // that name, then `others` (its parent's), letter case aside; any other by its names as its cover is found.
  // *how: how it was found (for the log).
  const Entry* Find(const Library::Game& game, bool romset, const std::vector<std::string>& others, std::string* how) const;
  size_t Size() const { return m_entries.size(); }

private:
  struct Index;
  std::vector<Entry> m_entries;
  std::vector<std::string> m_names;  // (the entries' names, in their order: the covers' matcher indexes these)
  Index* m_index = nullptr;
};

// ---- What was downloaded ----
// The record of a console's downloaded bezels: data_dir/bezels/<console>/bezelproject.txt.
std::string RecordPath(const std::string& data_dir, const std::string& console);
struct Downloaded
{
  int files = 0;
  int64_t bytes = 0;
};
// The console's downloaded bezels still there (as recorded).
Downloaded Count(const std::string& data_dir, const std::string& console);
// Deletes every console's downloaded bezels (only the files the downloads wrote, still as they were written, and the
// downloads' .part files) and their records. What was deleted.
Downloaded DeleteAll(const std::string& data_dir, const std::vector<std::string>& consoles);

// ---- The download (one at a time, on a thread of its own) ----
class Downloader
{
public:
  enum class Step
  {
    Waiting,      // not reached yet
    Listing,      // its lists asked for, its games matched
    Downloading,  // its bezels coming
    Done,
    Failed,  // its lists didn't come (Console::error)
  };
  struct Console
  {
    std::string key, name;  // "snes", "Super Nintendo"
    Step step = Step::Waiting;
    int games = 0;       // the player's games of it
    int matched = 0;     // those The Bezel Project has a bezel for
    int already = 0;     // downloaded before (still there)
    int own = 0;         // a bezel of the player's own there, of the same name: left as it is
    int downloaded = 0;  // this time
    int failed = 0;      // this time
    int64_t bytes = 0;   // the bezels to download this time
    int64_t done = 0;    // what came of them
    std::string error;   // Failed: why, in plain words (English, for PS4Lang::T)
  };
  struct Progress
  {
    std::vector<Console> consoles;
    std::string game;     // the game whose bezel is coming
    std::string error;    // why the whole download stopped, in plain words (not enough space...)
    bool running = false;
    bool paused = false;  // a game runs: it waits
    bool stopped = false; // stopped by the player
    unsigned serial = 0;  // changes with every change of the rest
  };

  ~Downloader() { Stop(false); }
  // The bezels of `games` (the player's) for these consoles (keys; those without a source are left out). False when a
  // download is at work already or no thread could be made. resumed: the start of a download a cut stopped (Pending).
  bool Start(const std::string& data_dir, const std::vector<Library::Game>& games,
             const std::vector<std::string>& consoles, bool resumed);
  // While set (a game runs) nothing is asked for: a file coming stops (its part kept) and goes on afterwards.
  void Pause(bool pause) { m_pause = pause; }
  // Stops it (waits for its thread). forget: the player's Stop - not resumed at the next start either; else (the app
  // closing) the consoles not done are.
  void Stop(bool forget);
  bool Busy() const { return m_busy; }
  Progress Now() const;
  unsigned Serial() const { return m_serial; }
  // Once per download, when it has ended: true.
  bool Finished() { return m_ended.exchange(false); }

  // The consoles of a download that didn't finish (the app closed, the connection cut): data_dir/cache/bezels/pending.txt.
  static std::vector<std::string> Pending(const std::string& data_dir);

private:
  struct Job
  {
    Library::Game game;
    std::vector<std::string> others;  // a romset's other names (its parent's)
  };
  static void* Main(void* self);
  void Run();
  void Changed();
  bool Wait();  // while paused; false when stopped

  pthread_t m_thread{};
  bool m_started = false;
  std::string m_data_dir;
  std::vector<std::pair<std::string, std::vector<Job>>> m_work;  // console key, its games
  bool m_resumed = false;
  std::atomic<bool> m_busy{false}, m_cancel{false}, m_pause{false}, m_ended{false}, m_forget{false};
  std::atomic<unsigned> m_serial{0};
  mutable std::mutex m_mutex;
  Progress m_progress;  // (under m_mutex)
};

// ---- A bezel's window (Emulator.cpp LoadBezel) ----
struct Window
{
  bool found = false;
  std::string why;                 // not found: why, for the log after the file's name (" isn't see-through...")
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the see-through window found (the PNG's pixels, inclusive)
  // Where the picture goes, in the PNG's pixels (left, top inclusive; right, bottom exclusive): the window, or (fitted)
  // the biggest rectangle of the picture's shape, centred in it, whose pixels are all see-through.
  float left = 0, top = 0, right = 0, bottom = 0;
};
// The bezel's see-through window (alpha under 128, from the middle out along the middle row, then along the window's
// middle column), and its see-through pixels there made fully see-through (alpha 0: the soft edges' faint colour never
// lies over the picture). ratio > 0 (a bezel of the data folder, 00.80): the window as the picture's shape fits in it
// (width / height on the screen, which the PNG fills: screen_w x screen_h), for a window that isn't a rectangle (The
// Bezel Project's TV screens have rounded, bulging edges): no pixel of the bezel over the picture; only that rectangle
// is cleared, the rest of the window's soft shading stays.
Window FindWindow(ImageRGBA* image, float ratio, float screen_w, float screen_h);
// How big a picture of that shape is in the window (fitted) against the whole screen: 1 as big, 0.5 half as wide and
// tall. A bezel made for another shape of picture (the DS's screens side by side in a bezel made for them one above
// the other, a 16:9 picture in a 4:3 TV) makes it much smaller.
float FitScale(const Window& window, float ratio, uint32_t image_w, uint32_t image_h, float screen_w, float screen_h);
// Below it, Per Game shows the console's own bezel instead.
constexpr float MIN_FIT = 0.78f;
}  // namespace BezelProject
